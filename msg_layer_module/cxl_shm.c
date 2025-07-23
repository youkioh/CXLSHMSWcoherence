/**
 * cxl_shm.c
 * CXL Shared Memory Messaging Layer
 *
 * A kernel module for CXL shared memory communication between nodes.
 * Provides ring buffer-based message passing with cache coherency management.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <asm/cacheflush.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/types.h>

/* =============================================================================
 * MODULE CONFIGURATION
 * ============================================================================= */

#define MODULE_NAME "shm_cxl"
#define CL_SIZE 64                    /* Cache line size */
#define CXL_KMSG_RBUF_SIZE 128        /* Ring buffer size */
#define CXL_KMSG_PAYLOAD_SIZE 36        /* Maximum message size */
#define CXL_KMSG_WINDOW_OFFSET 0x4000 /* Offset for shared memory windows */

/* Module parameters */
static char *dax_name = "dax0.0";
module_param(dax_name, charp, 0444);
MODULE_PARM_DESC(dax_name, "DAX device name (e.g., dax0.0)");

static int node_id = 0;
module_param(node_id, int, 0444);
MODULE_PARM_DESC(node_id, "CXL node ID (0-3)");

/* Multi-node configuration */
#define MAX_NODES 4
#define WINDOWS_PER_NODE 4
#define TOTAL_WINDOWS (MAX_NODES * WINDOWS_PER_NODE)

/* =============================================================================
 * DATA STRUCTURES
 * ============================================================================= */

/* CXL message header */
struct cxl_kmsg_hdr {
    int type;
    int size;
    int from_nid;
    int to_nid;
} __attribute__((packed));

/* CXL message structure */
struct cxl_kmsg_message {
    struct cxl_kmsg_hdr header;
    unsigned char payload[CXL_KMSG_PAYLOAD_SIZE];
    volatile int ready;
    unsigned long last_ticket;
} __attribute__((packed, aligned(CL_SIZE)));

/* CXL shared memory window */
struct cxl_kmsg_window {
    volatile unsigned long head;
    volatile unsigned long tail;
    volatile unsigned char int_enabled;
    volatile struct cxl_kmsg_message buffer[CXL_KMSG_RBUF_SIZE];
} __attribute__((packed));

/* CXL handle structure */
struct cxl_handle {
    int nid;
    struct cxl_kmsg_message *msg;
    /* TX windows: this node sends to other nodes (win_tx[dest_nid]) */
    struct cxl_kmsg_window *win_tx[MAX_NODES];
    /* RX windows: this node receives from other nodes (win_rx[src_nid]) */
    struct cxl_kmsg_window *win_rx[MAX_NODES];
    struct task_struct *recv_handler;
};

/* =============================================================================
 * GLOBAL VARIABLES
 * ============================================================================= */

static struct cxl_handle *cxl_handler = NULL;
static unsigned long insurance_recv = 0, insurance_send = 0;

/* =============================================================================
 * FUNCTION PROTOTYPES
 * ============================================================================= */

/* Public API */
struct cxl_kmsg_message *cxl_kmsg_get(size_t size);
void cxl_kmsg_put(struct cxl_kmsg_message *msg);
int cxl_kmsg_send_message(int dest_nid, struct cxl_kmsg_message *msg, size_t size);
int cxl_kmsg_broadcast_message(struct cxl_kmsg_message *msg, size_t size);
int cxl_kmsg_poll_all_rx(struct cxl_kmsg_message **msg, int *from_nid);
int cxl_kmsg_register_processor(void (*processor)(struct cxl_kmsg_message *msg));
void cxl_kmsg_unregister_processor(void);

/* Cache management */
static inline void __flush_processor_cache(const void *addr, size_t len);
static inline void flush_processor_cache(const void *addr, size_t len);
static inline void invalidate_processor_cache(const void *addr, size_t len);
static inline void hard_flush_processor_cache(const void *addr, size_t len);

/* Ring buffer operations */
static inline unsigned long win_inuse(struct cxl_kmsg_window *win);
static inline int win_put(struct cxl_kmsg_window *win, struct cxl_kmsg_message *msg, size_t size);
static inline int win_get(struct cxl_kmsg_window *win, struct cxl_kmsg_message **msg);
static inline int cxl_kmsg_window_init(struct cxl_kmsg_window *window);

/* Message handling */
static void cxl_kmsg_process(struct cxl_kmsg_message *msg);
static int cxl_kmsg_receive(struct cxl_handle *ch);
static int recv_handler(void *arg);
static int cxl_kmsg_send(struct cxl_handle *ch, int dest_nid, size_t size);

/* Utility functions */
static phys_addr_t get_dax_physical_range(const char *name);

/* =============================================================================
 * CACHE MANAGEMENT FUNCTIONS
 * ============================================================================= */

/**
 * __flush_processor_cache() - Low-level cache line flush
 */
static inline void __flush_processor_cache(const void *addr, size_t len)
{
    int64_t i;
    volatile char *buffer = (volatile char *)addr;

    /* Flush the processor cache for the target range */
    for (i = 0; i < len; i += CL_SIZE)
        clflush((volatile void *)&buffer[i]);
}

/**
 * flush_processor_cache() - Flush data written by this host
 */
static inline void flush_processor_cache(const void *addr, size_t len)
{
    /* Barrier before clflush to guarantee all prior memory mutations are flushed */
    smp_mb();
    __flush_processor_cache(addr, len);
}

/**
 * invalidate_processor_cache() - Invalidate cache to see data from other hosts
 */
static inline void invalidate_processor_cache(const void *addr, size_t len)
{
    __flush_processor_cache(addr, len);
    smp_mb();
    /* Barrier after flush to guarantee subsequent memory accesses see fresh data */
}

/**
 * hard_flush_processor_cache() - Bidirectional cache flush
 */
static inline void hard_flush_processor_cache(const void *addr, size_t len)
{
    smp_mb();
    __flush_processor_cache(addr, len);
    smp_mb();
}

/* CXL-specific cache management wrappers */
static inline void cxl_flush_cache_range(volatile void *addr, size_t size)
{
    flush_processor_cache((const void *)addr, size);
}

static inline void cxl_invalidate_cache_range(volatile void *addr, size_t size)
{
    invalidate_processor_cache((const void *)addr, size);
}

static inline void cxl_flush_message_buffer(volatile struct cxl_kmsg_message *msg)
{
    cxl_flush_cache_range((volatile void *)msg, sizeof(struct cxl_kmsg_message));
}

static inline void cxl_invalidate_message_buffer(volatile struct cxl_kmsg_message *msg)
{
    cxl_invalidate_cache_range((volatile void *)msg, sizeof(struct cxl_kmsg_message));
}

static inline void cxl_flush_window_metadata(struct cxl_kmsg_window *win)
{
    cxl_flush_cache_range(&win->head, sizeof(win->head));
    cxl_flush_cache_range(&win->tail, sizeof(win->tail));
    cxl_flush_cache_range(&win->int_enabled, sizeof(win->int_enabled));
}

static inline void cxl_invalidate_window_metadata(struct cxl_kmsg_window *win)
{
    cxl_invalidate_cache_range(&win->head, sizeof(win->head));
    cxl_invalidate_cache_range(&win->tail, sizeof(win->tail));
    cxl_invalidate_cache_range(&win->int_enabled, sizeof(win->int_enabled));
}

/* =============================================================================
 * RING BUFFER OPERATIONS
 * ============================================================================= */

/**
 * win_inuse() - Get number of messages in ring buffer
 */
static inline unsigned long win_inuse(struct cxl_kmsg_window *win) 
{
    return win->head - win->tail;
}

/**
 * win_put() - Put message into ring buffer
 */
static inline int win_put(struct cxl_kmsg_window *win, 
                         struct cxl_kmsg_message *msg,
                         size_t size) 
{
    unsigned long ticket;
    
    /* Validate message */
    if (msg->header.type < 0 || msg->header.size <= 0) {
        printk(KERN_ERR "%s: Invalid message type %d or size %d\n", 
               MODULE_NAME, msg->header.type, msg->header.size);
        return -EINVAL;
    }

    /* Check buffer space */
    if (win_inuse(win) >= CXL_KMSG_RBUF_SIZE - 1) {
        printk(KERN_WARNING "%s: Window full, dropping message\n", MODULE_NAME);
        return -EAGAIN;
    }

    /* Copy message to ring buffer */
    ticket = insurance_send % CXL_KMSG_RBUF_SIZE;
    memcpy((void*)&win->buffer[ticket].payload, (void*)&msg->payload, size);
    memcpy((void*)&win->buffer[ticket].header, (void*)&msg->header, sizeof(struct cxl_kmsg_hdr));
    
    /* Ensure message visibility across CXL */
    cxl_flush_message_buffer(&win->buffer[ticket]);
    
    win->buffer[ticket].ready = 1;
    smp_mb();
    
    /* Update ring buffer head */
    ticket = __sync_fetch_and_add(&win->head, 1);
    insurance_send = win->head;
    win->buffer[ticket % CXL_KMSG_RBUF_SIZE].last_ticket = insurance_send;

    /* Ensure metadata visibility */
    cxl_flush_window_metadata(win);

    return 0;
}

/**
 * win_get() - Get message from ring buffer
 */
static inline int win_get(struct cxl_kmsg_window *win,
                         struct cxl_kmsg_message **msg)
{
    struct cxl_kmsg_message *rcvd;
    
    if (!win_inuse(win)) {
        return -1;
    }
    
    /* Invalidate cache to see latest data from other CXL nodes */
    cxl_invalidate_window_metadata(win);
    
    rcvd = (struct cxl_kmsg_message*)&(win->buffer[win->tail % CXL_KMSG_RBUF_SIZE]);
    
    /* Invalidate message buffer cache to get fresh data */
    cxl_invalidate_message_buffer(rcvd);
    
    if (!rcvd->ready) {
        return -1;
    }
    
    /* Update ring buffer tail */
    insurance_recv = win->buffer[win->tail % CXL_KMSG_RBUF_SIZE].last_ticket;
    __sync_fetch_and_add(&win->tail, 1);
    
    /* Make tail update visible to other nodes */
    cxl_flush_cache_range(&win->tail, sizeof(win->tail));
    
    smp_mb();
    *msg = rcvd;

    return 0;
}

/**
 * cxl_kmsg_window_init() - Initialize ring buffer window
 */
static inline int cxl_kmsg_window_init(struct cxl_kmsg_window *window)
{
    window->head = 0;
    window->tail = 0;
    memset((void*)window->buffer, 0, sizeof(window->buffer));
    window->int_enabled = 1;
    
    /* Ensure initialization is visible across CXL */
    hard_flush_processor_cache(window, sizeof(struct cxl_kmsg_window));
    
    return 0;
}

/* =============================================================================
 * MESSAGE HANDLING
 * ============================================================================= */

/* External message processor function pointer */
static void (*external_msg_processor)(struct cxl_kmsg_message *msg) = NULL;
static DEFINE_SPINLOCK(processor_lock);

/**
 * cxl_kmsg_register_processor() - Register external message processor
 */
int cxl_kmsg_register_processor(void (*processor)(struct cxl_kmsg_message *msg))
{
    unsigned long flags;
    
    spin_lock_irqsave(&processor_lock, flags);
    external_msg_processor = processor;
    spin_unlock_irqrestore(&processor_lock, flags);
    
    printk(KERN_INFO "%s: External message processor registered\n", MODULE_NAME);
    return 0;
}
EXPORT_SYMBOL(cxl_kmsg_register_processor);

/**
 * cxl_kmsg_unregister_processor() - Unregister external message processor
 */
void cxl_kmsg_unregister_processor(void)
{
    unsigned long flags;
    
    spin_lock_irqsave(&processor_lock, flags);
    external_msg_processor = NULL;
    spin_unlock_irqrestore(&processor_lock, flags);
    
    printk(KERN_INFO "%s: External message processor unregistered\n", MODULE_NAME);
}
EXPORT_SYMBOL(cxl_kmsg_unregister_processor);

/**
 * cxl_kmsg_process() - Process received message
 */
static void cxl_kmsg_process(struct cxl_kmsg_message *msg)
{
    unsigned long flags;
    void (*processor)(struct cxl_kmsg_message *msg);
    
    spin_lock_irqsave(&processor_lock, flags);
    processor = external_msg_processor;
    spin_unlock_irqrestore(&processor_lock, flags);
    
    if (processor) {
        processor(msg);
    } else {
        /* Default processing if no external processor registered */
        printk(KERN_INFO "%s: Processing message type=%d, size=%d, from_nid=%d\n",
               MODULE_NAME, msg->header.type, msg->header.size, msg->header.from_nid);
        printk(KERN_INFO "%s: Message payload: %.*s\n", MODULE_NAME, msg->header.size, msg->payload);
        kfree(msg);
    }
}

/**
 * cxl_kmsg_receive() - Receive and process messages from all nodes
 */
static int cxl_kmsg_receive(struct cxl_handle *ch)
{
    struct cxl_kmsg_window *win;
    struct cxl_kmsg_message *msg;
    struct cxl_kmsg_hdr header;
    struct cxl_kmsg_message *data;
    int from_nid;

    /* Poll all RX windows for incoming messages */
    for (from_nid = 0; from_nid < MAX_NODES; from_nid++) {
        if (from_nid == ch->nid) continue; /* Skip self */
        
        win = ch->win_rx[from_nid];
        if (!win) continue;

        while (!win_get(win, &msg)) {
            header = msg->header;
            
            /* Validate message */
            if (header.type < 0 || header.size <= 0 || header.size > CL_SIZE) {
                printk(KERN_ERR "%s: Invalid message header from node %d\n", MODULE_NAME, from_nid);
                msg->ready = 0;
                cxl_flush_cache_range(&msg->ready, sizeof(msg->ready));
                continue;
            }
            
            /* Allocate and copy message */
            data = kmalloc(sizeof(struct cxl_kmsg_message), GFP_KERNEL);
            if (!data) {
                printk(KERN_ERR "%s: Unable to allocate message from node %d\n", MODULE_NAME, from_nid);
                msg->ready = 0;
                cxl_flush_cache_range(&msg->ready, sizeof(msg->ready));
                continue;
            }
            
            memcpy(&data->payload, &msg->payload, header.size);
            memcpy(&data->header, &msg->header, sizeof(header));
            
            /* Process message */
            cxl_kmsg_process(data);
            
            /* Mark slot as available */
            smp_mb();
            msg->ready = 0;
            cxl_flush_cache_range(&msg->ready, sizeof(msg->ready));
        }
    }
    
    return 0;
}

/**
 * recv_handler() - Message receive thread
 */
static int recv_handler(void *arg)
{
    struct cxl_handle *ch = (struct cxl_handle *)arg;
    
    printk(KERN_INFO "%s: Receive handler for node %d started\n", MODULE_NAME, ch->nid);
    
    while (!kthread_should_stop()) {
        msleep(1); /* Polling interval: 1ms */
        cxl_kmsg_receive(ch);
    }
    
    printk(KERN_INFO "%s: Receive handler for node %d stopped\n", MODULE_NAME, ch->nid);
    return 0;
}

/**
 * cxl_kmsg_send() - Send message to specific destination node
 */
static int cxl_kmsg_send(struct cxl_handle *ch, int dest_nid, size_t size)
{
    int rc;
    struct cxl_kmsg_window *dest_window;
    struct cxl_kmsg_message *msg = ch->msg;
    
    if (dest_nid < 0 || dest_nid >= MAX_NODES) {
        printk(KERN_ERR "%s: Invalid destination node ID %d\n", MODULE_NAME, dest_nid);
        return -EINVAL;
    }

    if (dest_nid == ch->nid) {
        printk(KERN_WARNING "%s: Cannot send message to self (node %d)\n", MODULE_NAME, dest_nid);
        return -EINVAL;
    }

    dest_window = ch->win_tx[dest_nid];
    if (unlikely(!dest_window)) {
        printk(KERN_ERR "%s: No TX window for destination node %d\n", MODULE_NAME, dest_nid);
        return -EINVAL;
    }

    if (unlikely(!msg)) {
        printk(KERN_ERR "%s: Null message pointer\n", MODULE_NAME);
        return -EINVAL;
    }

    /* Set message header */
    msg->header.from_nid = ch->nid;
    msg->header.to_nid = dest_nid;

    rc = win_put(dest_window, msg, size);
    if (rc) {
        printk(KERN_ERR "%s: Failed to send message to node %d, rc=%d\n", MODULE_NAME, dest_nid, rc);
        return rc;
    }

    printk(KERN_DEBUG "%s: Message sent from node %d to node %d\n", MODULE_NAME, ch->nid, dest_nid);
    return 0;
}

/* =============================================================================
 * PUBLIC API FUNCTIONS
 * ============================================================================= */

/**
 * cxl_kmsg_get() - Allocate a new message
 */
struct cxl_kmsg_message *cxl_kmsg_get(size_t size)
{
    struct cxl_kmsg_message *msg;
    
    msg = kmalloc(sizeof(struct cxl_kmsg_message), GFP_KERNEL);
    if (!msg) {
        printk(KERN_ERR "%s: Failed to allocate message\n", MODULE_NAME);
    }
    
    return msg;
}
EXPORT_SYMBOL(cxl_kmsg_get);

/**
 * cxl_kmsg_put() - Free a message
 */
void cxl_kmsg_put(struct cxl_kmsg_message *msg)
{
    kfree(msg);
}
EXPORT_SYMBOL(cxl_kmsg_put);

/**
 * cxl_kmsg_send_message() - Send message to destination node
 */
int cxl_kmsg_send_message(int dest_nid, struct cxl_kmsg_message *msg, size_t size)
{
    if (!cxl_handler) {
        printk(KERN_ERR "%s: Handler not initialized\n", MODULE_NAME);
        return -ENODEV;
    }
    
    if (dest_nid < 0 || dest_nid >= MAX_NODES) {
        printk(KERN_ERR "%s: Invalid destination node ID %d\n", MODULE_NAME, dest_nid);
        return -EINVAL;
    }
    
    cxl_handler->msg = msg;
    return cxl_kmsg_send(cxl_handler, dest_nid, size);
}
EXPORT_SYMBOL(cxl_kmsg_send_message);

/**
 * cxl_kmsg_broadcast_message() - Broadcast message to all nodes
 */
int cxl_kmsg_broadcast_message(struct cxl_kmsg_message *msg, size_t size)
{
    int dest_nid, rc, errors = 0;
    
    if (!cxl_handler) {
        printk(KERN_ERR "%s: Handler not initialized\n", MODULE_NAME);
        return -ENODEV;
    }
    
    printk(KERN_INFO "%s: Broadcasting message from node %d\n", MODULE_NAME, cxl_handler->nid);
    
    for (dest_nid = 0; dest_nid < MAX_NODES; dest_nid++) {
        if (dest_nid == cxl_handler->nid) continue; /* Skip self */
        
        cxl_handler->msg = msg;
        rc = cxl_kmsg_send(cxl_handler, dest_nid, size);
        if (rc) {
            printk(KERN_ERR "%s: Failed to broadcast to node %d, rc=%d\n", MODULE_NAME, dest_nid, rc);
            errors++;
        }
    }
    
    if (errors > 0) {
        printk(KERN_WARNING "%s: Broadcast completed with %d errors\n", MODULE_NAME, errors);
        return -EAGAIN;
    }
    
    printk(KERN_INFO "%s: Broadcast completed successfully\n", MODULE_NAME);
    return 0;
}
EXPORT_SYMBOL(cxl_kmsg_broadcast_message);

/**
 * cxl_kmsg_poll_all_rx() - Poll all RX windows for messages (non-blocking)
 */
int cxl_kmsg_poll_all_rx(struct cxl_kmsg_message **msg, int *from_nid)
{
    struct cxl_kmsg_window *win;
    struct cxl_kmsg_message *rcv_msg;
    int src_nid;
    
    if (!cxl_handler) {
        printk(KERN_ERR "%s: Handler not initialized\n", MODULE_NAME);
        return -ENODEV;
    }
    
    /* Poll all RX windows */
    for (src_nid = 0; src_nid < MAX_NODES; src_nid++) {
        if (src_nid == cxl_handler->nid) continue; /* Skip self */
        
        win = cxl_handler->win_rx[src_nid];
        if (!win) continue;
        
        if (!win_get(win, &rcv_msg)) {
            /* Message found */
            *msg = kmalloc(sizeof(struct cxl_kmsg_message), GFP_KERNEL);
            if (!*msg) {
                printk(KERN_ERR "%s: Failed to allocate message\n", MODULE_NAME);
                rcv_msg->ready = 0;
                cxl_flush_cache_range(&rcv_msg->ready, sizeof(rcv_msg->ready));
                return -ENOMEM;
            }
            
            /* Copy message */
            memcpy(*msg, rcv_msg, sizeof(struct cxl_kmsg_message));
            *from_nid = src_nid;
            
            /* Mark slot as available */
            smp_mb();
            rcv_msg->ready = 0;
            cxl_flush_cache_range(&rcv_msg->ready, sizeof(rcv_msg->ready));
            
            return 0; /* Success */
        }
    }
    
    return -EAGAIN; /* No messages available */
}
EXPORT_SYMBOL(cxl_kmsg_poll_all_rx);


/* =============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================= */

/**
 * get_dax_physical_range() - Get physical address of DAX device
 */
static phys_addr_t get_dax_physical_range(const char *name)
{
    struct file *filp;
    char sysfs_path[256];
    char buffer[256];
    loff_t pos = 0;
    ssize_t bytes_read;
    phys_addr_t start = 0;

    /* Read physical start address from sysfs */
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/resource", name);
    filp = filp_open(sysfs_path, O_RDONLY, 0);
    if (!IS_ERR(filp)) {
        bytes_read = kernel_read(filp, buffer, sizeof(buffer) - 1, &pos);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            sscanf(buffer, "0x%llx", &start);
        }
        filp_close(filp, NULL);
    }

    return start; /* 0 means failure */
}

/* =============================================================================
 * MODULE INITIALIZATION AND CLEANUP
 * ============================================================================= */

/**
 * init_cxl_shm() - Module initialization
 */
static int __init init_cxl_shm(void)
{
    struct cxl_kmsg_window *shm_window;
    struct task_struct *tsk_recv;
    int ret = 0, i;
    phys_addr_t start_addr, window_addr;
    
    printk(KERN_INFO "%s: Loading CXL Shared Memory messaging layer...\n", MODULE_NAME);
    printk(KERN_INFO "%s: Using DAX device: %s, Node ID: %d\n", MODULE_NAME, dax_name, node_id);
    
    if (node_id < 0 || node_id >= MAX_NODES) {
        printk(KERN_ERR "%s: Invalid node_id %d (must be 0-%d)\n", MODULE_NAME, node_id, MAX_NODES-1);
        return -EINVAL;
    }
    
    /* Get DAX device physical address */
    start_addr = get_dax_physical_range(dax_name);
    if (!start_addr) {
        printk(KERN_ERR "%s: Failed to get DAX device physical address\n", MODULE_NAME);
        return -ENODEV;
    }

    /* Allocate handler structure */
    cxl_handler = kmalloc(sizeof(struct cxl_handle), GFP_KERNEL);
    if (!cxl_handler) {
        printk(KERN_ERR "%s: Failed to allocate handler\n", MODULE_NAME);
        return -ENOMEM;
    }
    
    /* Initialize handler */
    cxl_handler->nid = node_id;
    cxl_handler->msg = NULL;
    
    /* Initialize all window pointers to NULL */
    for (i = 0; i < MAX_NODES; i++) {
        cxl_handler->win_tx[i] = NULL;
        cxl_handler->win_rx[i] = NULL;
    }

    /* Map TX windows: where this node sends to other nodes
     * Layout: TX[src_nid][dest_nid] at offset (src_nid * MAX_NODES + dest_nid) * WINDOW_OFFSET
     */
    for (i = 0; i < MAX_NODES; i++) {
        if (i == node_id) continue; /* Skip self */
        
        window_addr = start_addr + ((node_id * MAX_NODES + i) * CXL_KMSG_WINDOW_OFFSET);
        shm_window = (struct cxl_kmsg_window*)memremap(window_addr, 
                                          sizeof(struct cxl_kmsg_window), 
                                          MEMREMAP_WB);
        if (!shm_window) {
            printk(KERN_ERR "%s: Failed to map TX window for dest node %d\n", MODULE_NAME, i);
            ret = -ENOMEM;
            goto out_unmap;
        }
        
        cxl_handler->win_tx[i] = shm_window;
        cxl_kmsg_window_init(shm_window);
        
        printk(KERN_INFO "%s: Mapped TX window [%d->%d] at phys 0x%llx\n", 
               MODULE_NAME, node_id, i, window_addr);
    }

    /* Map RX windows: where this node receives from other nodes
     * Layout: RX[dest_nid][src_nid] at offset (src_nid * MAX_NODES + dest_nid) * WINDOW_OFFSET
     */
    for (i = 0; i < MAX_NODES; i++) {
        if (i == node_id) continue; /* Skip self */
        
        window_addr = start_addr + ((i * MAX_NODES + node_id) * CXL_KMSG_WINDOW_OFFSET);
        shm_window = (struct cxl_kmsg_window*)memremap(window_addr, 
                                          sizeof(struct cxl_kmsg_window), 
                                          MEMREMAP_WB);
        if (!shm_window) {
            printk(KERN_ERR "%s: Failed to map RX window from src node %d\n", MODULE_NAME, i);
            ret = -ENOMEM;
            goto out_unmap;
        }
        
        cxl_handler->win_rx[i] = shm_window;
        /* Note: Don't initialize RX windows - they're initialized by the sender */
        
        printk(KERN_INFO "%s: Mapped RX window [%d->%d] at phys 0x%llx\n", 
               MODULE_NAME, i, node_id, window_addr);
    }
    
    /* Start receive handler thread */
    tsk_recv = kthread_run(recv_handler, cxl_handler, "cxl_recv_%d", node_id);
    if (IS_ERR(tsk_recv)) {
        printk(KERN_ERR "%s: Cannot create receive handler\n", MODULE_NAME);
        ret = PTR_ERR(tsk_recv);
        goto out_unmap;
    }
    cxl_handler->recv_handler = tsk_recv;
    
    printk(KERN_INFO "%s: Ready on CXL Shared Memory (Node ID: %d, %d TX + %d RX windows)\n", 
           MODULE_NAME, node_id, MAX_NODES-1, MAX_NODES-1);
    return 0;

out_unmap:
    if (cxl_handler) {
        for (i = 0; i < MAX_NODES; i++) {
            if (cxl_handler->win_tx[i])
                memunmap(cxl_handler->win_tx[i]);
            if (cxl_handler->win_rx[i])
                memunmap(cxl_handler->win_rx[i]);
        }
        kfree(cxl_handler);
        cxl_handler = NULL;
    }
    return ret;
}

/**
 * exit_cxl_shm() - Module cleanup
 */
static void __exit exit_cxl_shm(void)
{
    int i;
    
    printk(KERN_INFO "%s: Unloading CXL Shared Memory messaging layer...\n", MODULE_NAME);
    
    if (cxl_handler) {
        if (cxl_handler->recv_handler) {
            kthread_stop(cxl_handler->recv_handler);
        }
        
        /* Unmap all TX and RX windows */
        for (i = 0; i < MAX_NODES; i++) {
            if (cxl_handler->win_tx[i]) {
                memunmap(cxl_handler->win_tx[i]);
            }
            if (cxl_handler->win_rx[i]) {
                memunmap(cxl_handler->win_rx[i]);
            }
        }
        
        kfree(cxl_handler);
        cxl_handler = NULL;
    }
    
    printk(KERN_INFO "%s: Successfully unloaded\n", MODULE_NAME);
}

module_init(init_cxl_shm);
module_exit(exit_cxl_shm);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CXL SHM Dev");
MODULE_DESCRIPTION("CXL Shared Memory Messaging Layer");
MODULE_VERSION("1.0");
