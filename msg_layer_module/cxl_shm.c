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
#include <swmc/swmc_kmsg.h>
#include <swmc/page_coherence.h>

/* =============================================================================
 * MODULE CONFIGURATION
 * ============================================================================= */

#define MODULE_NAME "shm_cxl"
#define CXL_KMSG_RBUF_SIZE 65536        /* Ring buffer size */

/* Multi-node configuration */
// #define MAX_NODES 4
#define MAX_NODES 2

/* Module parameters */
static char *dax_name = NULL;  /* Default to NULL - must be specified */
module_param(dax_name, charp, 0644);  /* Allow runtime modification */
MODULE_PARM_DESC(dax_name, "DAX device name (e.g., dax0.0) - REQUIRED");

static int node_id = -1;  /* Default to -1 - must be specified */
module_param(node_id, int, 0644);  /* Allow runtime modification */
MODULE_PARM_DESC(node_id, "CXL node ID (0-3) - REQUIRED");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CXL SHM Dev");
MODULE_DESCRIPTION("CXL Shared Memory Messaging Layer");
MODULE_VERSION("1.0");

/* =============================================================================
 * PROTOTYPES
 * ============================================================================= */

 int cxl_kmsg_unicast(enum swmc_kmsg_type type, int ws_id, int dest_nid, struct payload_data *payload);
 int cxl_kmsg_broadcast(enum swmc_kmsg_type type, int ws_id, struct payload_data *payload);
 void cxl_kmsg_done(struct swmc_kmsg_message *message);
 int cxl_kmsg_node_count(void);

/* =============================================================================
 * CACHE MANAGEMENT
 * ============================================================================= */

 /**
 * __flush_processor_cache() - Low-level cache line flush
 */
static inline void __flush_processor_cache(const volatile void *addr, size_t len)
{
    int64_t i;
    volatile char *buffer = (volatile char *)addr;

    for (i = 0; i < len; i += CL_SIZE)
        clflush((volatile void *)&buffer[i]);
}

/**
 * cxl_cache_operation() - Unified cache operation
 */
typedef enum {
    CXL_CACHE_FLUSH,
    CXL_CACHE_INVALIDATE, 
    CXL_CACHE_HARD_FLUSH
} cxl_cache_op_t;

static inline void cxl_cache_operation(const volatile void *addr, size_t len, cxl_cache_op_t op)
{
    switch (op) {
    case CXL_CACHE_FLUSH:
        smp_mb();
        __flush_processor_cache(addr, len);
        break;
    case CXL_CACHE_INVALIDATE:
        __flush_processor_cache(addr, len);
        smp_mb();
        break;
    case CXL_CACHE_HARD_FLUSH:
        smp_mb();
        __flush_processor_cache(addr, len);
        smp_mb();
        break;
    }
}

/* Simplified cache management interface */
#define cxl_flush_cache(addr, len)      cxl_cache_operation(addr, len, CXL_CACHE_FLUSH)
#define cxl_invalidate_cache(addr, len) cxl_cache_operation(addr, len, CXL_CACHE_INVALIDATE)
#define cxl_hard_flush_cache(addr, len) cxl_cache_operation(addr, len, CXL_CACHE_HARD_FLUSH)

/* =============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================= */

 static inline int __validate_kmsg(struct swmc_kmsg_message *msg)
 {
     if (!msg) {
         pr_err("%s: Null message pointer\n", MODULE_NAME);
         return -EINVAL;
     }
     
     if (msg->header.type < 0 || msg->header.type >= SWMC_KMSG_TYPE_MAX) {
         pr_err("%s: Invalid message type %d\n", MODULE_NAME, msg->header.type);
         return -EINVAL;
     }

    if (msg->header.ws_id < 0 || msg->header.from_nid < 0 || msg->header.to_nid < 0) {
         pr_err("%s: Invalid message header fields\n", MODULE_NAME);
         return -EINVAL;
     }
     
     return 0;
 }

static inline int __build_kmsg(struct swmc_kmsg_message *msg, 
                                enum swmc_kmsg_type type, 
                                int ws_id, 
                                int dest_nid, 
                                struct payload_data *payload)
{
    msg->header.type = type;
    msg->header.ws_id = ws_id;
    msg->header.from_nid = node_id; // Set to current node ID
    msg->header.to_nid = dest_nid;

    if (__validate_kmsg(msg)) {
        pr_err("%s: Invalid message header\n", MODULE_NAME);
        return -EINVAL;
    }

    if (payload) {
        msg->payload = *payload;
    } else {
        memset(&msg->payload, 0, sizeof(msg->payload));
    }

    return 0;
}

/* =============================================================================
 * RING BUFFER DATA STRUCTURES
 * ============================================================================= */

 /* CXL shared memory window */
struct cxl_kmsg_window {
    volatile unsigned long head;
    volatile unsigned long tail;
    volatile unsigned char int_enabled;
    volatile struct swmc_kmsg_message buffer[CXL_KMSG_RBUF_SIZE];
} __attribute__((packed));

/* Calculate window offset based on actual structure size
 * Round up to 4KB page boundary for better performance and alignment
 */
#define SWMC_KMSG_WINDOW_OFFSET \
    (((sizeof(struct cxl_kmsg_window) + 0xFFF) >> 12) << 12)  /* Round up to 4KB */

/* CXL kmsg handle structure */
struct cxl_kmsg_handle {
    int nid;
    /* TX windows: this node sends to other nodes (win_tx[dest_nid]) */
    struct cxl_kmsg_window *win_tx[MAX_NODES];
    /* RX windows: this node receives from other nodes (win_rx[src_nid]) */
    struct cxl_kmsg_window *win_rx[MAX_NODES];
    struct task_struct *recv_handler;
};

static struct cxl_kmsg_handle *cxl_kmsg_handler = NULL;
static unsigned long insurance_recv = 0, insurance_send = 0;


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
                         struct swmc_kmsg_message *msg) 
{
    unsigned long ticket;
    int ret;
    
    /* Validate message */
    ret = __validate_kmsg(msg);
    if (ret)
        return ret;

    /* Check buffer space */
    if (win_inuse(win) >= CXL_KMSG_RBUF_SIZE - 1) {
        pr_warn("%s: Window full, dropping message\n", MODULE_NAME);
        return -EAGAIN;
    }

    /* Get ticket for message placement */
    ticket = win->head % CXL_KMSG_RBUF_SIZE;
    
    /* Copy message to ring buffer */
    memcpy((void*)&win->buffer[ticket], msg, sizeof(struct swmc_kmsg_message));
    
    /* Ensure message data is visible */
    cxl_flush_cache(&win->buffer[ticket], sizeof(struct swmc_kmsg_message));
    
    /* Update ring buffer head with atomic operation */
    __sync_fetch_and_add(&win->head, 1);
    insurance_send = win->head;

    /* Ensure metadata visibility */
    cxl_flush_cache(&win->head, sizeof(win->head));

    return 0;
}

/**
 * win_get() - Get message from ring buffer
 */
static inline int win_get(struct cxl_kmsg_window *win,
                         struct swmc_kmsg_message **msg)
{
    struct swmc_kmsg_message *rcvd;
    
    if (!win_inuse(win))
        return -1;
    
    /* Invalidate cache to see latest data from other CXL nodes */
    cxl_invalidate_cache(win, sizeof(struct cxl_kmsg_window));
    
    rcvd = (struct swmc_kmsg_message*)&win->buffer[win->tail % CXL_KMSG_RBUF_SIZE];
    
    /* Invalidate message buffer cache to get fresh data */
    cxl_invalidate_cache(rcvd, sizeof(struct swmc_kmsg_message));
    
    /* Update ring buffer tail */
    insurance_recv = win->tail + 1;
    __sync_fetch_and_add(&win->tail, 1);
    
    /* Make tail update visible to other nodes */
    cxl_flush_cache(&win->tail, sizeof(win->tail));
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
    window->int_enabled = 1;
    memset((void*)window->buffer, 0, sizeof(window->buffer));
    
    /* Ensure initialization is visible across CXL */
    cxl_hard_flush_cache(window, sizeof(struct cxl_kmsg_window));
    return 0;
}

/* =============================================================================
 * WINDOW MAPPING UTILITIES
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
            if (sscanf(buffer, "0x%llx", &start) != 1) {
                start = 0; /* Failed to parse */
            }
        }
        filp_close(filp, NULL);
    }

    return start; /* 0 means failure */
}

/**
 * cxl_map_window() - Map a single CXL window
 */
static struct cxl_kmsg_window *cxl_map_window(phys_addr_t base_addr, 
                                             int src_nid, int dest_nid, 
                                             const char *type)
{
    phys_addr_t window_addr;
    struct cxl_kmsg_window *window;
    
    window_addr = base_addr + ((src_nid * MAX_NODES + dest_nid) * SWMC_KMSG_WINDOW_OFFSET);
    window = (struct cxl_kmsg_window*)memremap(window_addr, 
                                              sizeof(struct cxl_kmsg_window), 
                                              MEMREMAP_WB);
    if (!window) {
        pr_info(KERN_ERR "%s: Failed to map %s window [%d->%d]\n", 
               MODULE_NAME, type, src_nid, dest_nid);
        return NULL;
    }
    
    pr_info(KERN_INFO "%s: Mapped %s window [%d->%d] at phys 0x%llx\n", 
           MODULE_NAME, type, src_nid, dest_nid, window_addr);
    
    return window;
}

/**
 * cxl_unmap_windows() - Unmap all windows in handler
 */
static void cxl_unmap_windows(struct cxl_kmsg_handle *handler)
{
    int i;
    
    if (!handler)
        return;
        
    for (i = 0; i < MAX_NODES; i++) {
        if (handler->win_tx[i]) {
            memunmap(handler->win_tx[i]);
            handler->win_tx[i] = NULL;
        }
        if (handler->win_rx[i]) {
            memunmap(handler->win_rx[i]);
            handler->win_rx[i] = NULL;
        }
    }
}

/* =============================================================================
 * SWMC_KMSG OPERATIONS
 * ============================================================================= */

int cxl_kmsg_unicast(enum swmc_kmsg_type type, int ws_id, int dest_nid, struct payload_data *payload)
{
    // Implementation of unicast messaging
    struct swmc_kmsg_message *message;
    int ret;

    if (!cxl_kmsg_handler) {
        pr_err("%s: Messaging handler not initialized\n", MODULE_NAME);
        return -ENODEV;
    }

    if (dest_nid < 0 || dest_nid >= MAX_NODES || dest_nid == node_id) {
        pr_err("%s: Invalid destination node ID: %d\n", MODULE_NAME, dest_nid);
        return -EINVAL;
    }

    if (!cxl_kmsg_handler->win_tx[dest_nid]) {
        pr_err("%s: TX window to node %d not available\n", MODULE_NAME, dest_nid);
        return -ENODEV;
    }

    // use win_put to send message
    message = kmalloc(sizeof(struct swmc_kmsg_message), GFP_KERNEL);
    if (!message) {
        pr_err("%s: Failed to allocate message\n", MODULE_NAME);
        return -ENOMEM;
    }

    ret = __build_kmsg(message, type, ws_id, dest_nid, payload);
    if (ret) {
        pr_err("%s: Failed to build message: %d\n", MODULE_NAME, ret);
        kfree(message);
        return ret;
    }

    ret = win_put(cxl_kmsg_handler->win_tx[dest_nid], message);
    if (ret) {
        pr_err("%s: Failed to send message: %d\n", MODULE_NAME, ret);
        kfree(message);
        return ret;
    }

    pr_info(KERN_INFO "%s: Unicast message sent: type=%d, ws_id=%d, dest_nid=%d\n", 
            MODULE_NAME, type, ws_id, dest_nid);
    
    kfree(message);

    return 0;
}

int cxl_kmsg_broadcast(enum swmc_kmsg_type type, int ws_id, struct payload_data *payload)
{
    // Implementation of broadcast messaging
    // use unicast function to send to all nodes except self
    for (int i = 0; i < MAX_NODES; i++) {
        if (i == node_id) continue; // Skip self

        int ret = cxl_kmsg_unicast(type, ws_id, i, payload);
        if (ret) {
            pr_err("%s: Failed to broadcast message to node %d: %d\n", MODULE_NAME, i, ret);
            return ret;
        }
    }

    pr_info(KERN_INFO "%s: Broadcast message sent: type=%d, ws_id=%d\n", 
            MODULE_NAME, type, ws_id);

    return 0;
}

// used to process acknowledgeement or completion messages
void cxl_kmsg_done(struct swmc_kmsg_message *message)
{
    // Mark message as done, can be used to free resources or notify completion
    kfree(message);
}

int cxl_kmsg_node_count(void)
{
    // Return the number of nodes in the system
    return MAX_NODES;
}

/* CXL messaging operations structure */
static struct swmc_kmsg_ops cxl_shm_ops = {
    .name = MODULE_NAME,
    .node_count = cxl_kmsg_node_count,
    .unicast = cxl_kmsg_unicast,
    .broadcast = cxl_kmsg_broadcast,
    // .multicast = cxl_kmsg_multicast, // TODO: Implement later
    .done = cxl_kmsg_done,
};

/* =============================================================================
 * INCOMING MESSAGE HANDLING
 * ============================================================================= */

/**
 * cxl_kmsg_receive() - Receive and process messages from all nodes
 */
static int cxl_kmsg_receive(struct cxl_kmsg_handle *ckh)
{
    struct cxl_kmsg_window *win;
    struct swmc_kmsg_message *msg;
    int from_nid, ret;
    bool found_message = true;

    while (found_message) {
        found_message = false;
        /* Poll all RX windows for incoming messages */
        for (from_nid = 0; from_nid < MAX_NODES; from_nid++) {
            if (from_nid == ckh->nid) continue; /* Skip self */
    
            win = ckh->win_rx[from_nid];
            if (!win) continue;
    
            if (win_get(win, &msg) == 0) {  /* Changed: check return value properly */
                /* Validate message before processing */
                if (!msg) {
                    pr_err("%s: Received NULL message from node %d\n", MODULE_NAME, from_nid);
                    continue;
                }
                pr_info("%s: Received message from node %d: type=%d, ws_id=%d\n", 
                        MODULE_NAME, from_nid, msg->header.type, msg->header.ws_id);
                
                /* Additional validation for message header */
                if (msg->header.type < 0 || msg->header.type >= SWMC_KMSG_TYPE_MAX) {
                    pr_err("%s: Invalid message type %d from node %d (hex: 0x%x)\n", 
                           MODULE_NAME, msg->header.type, from_nid, msg->header.type);
                    continue;
                }
                found_message = true;
                ret = swmc_kmsg_process_message(msg);
                
                smp_mb();
                
                if (ret) {
                    pr_info(KERN_WARNING "%s: Failed to process message from node %d: %d\n", 
                           MODULE_NAME, from_nid, ret);
                }
            }
        }
    }
    
    return 0;
}

/**
 * recv_handler() - Message receive thread
 */
static int recv_handler(void *arg)
{
    struct cxl_kmsg_handle *ckh = (struct cxl_kmsg_handle *)arg;

    pr_info(KERN_INFO "%s: Receive handler for node %d started\n", MODULE_NAME, ckh->nid);

    while (!kthread_should_stop()) {
        msleep(1); /* Polling interval: 1ms */
        // usleep_range(100, 200); /* Polling interval: 100-200us */
        cxl_kmsg_receive(ckh);
    }
    
    pr_info(KERN_INFO "%s: Receive handler for node %d stopped\n", MODULE_NAME, ckh->nid);
    return 0;
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
    phys_addr_t start_addr;
    
    pr_info(KERN_INFO "%s: Loading CXL Shared Memory messaging layer...\n", MODULE_NAME);
    pr_info(KERN_INFO "%s: Using DAX device: %s, Node ID: %d\n", MODULE_NAME, dax_name, node_id);
    pr_info(KERN_INFO "%s: Ring buffer size: %d messages\n", MODULE_NAME, CXL_KMSG_RBUF_SIZE);
    pr_info(KERN_INFO "%s: Window structure size: %lu bytes (0x%lx)\n", 
            MODULE_NAME, sizeof(struct cxl_kmsg_window), sizeof(struct cxl_kmsg_window));
    pr_info(KERN_INFO "%s: Window offset (aligned): %d bytes (0x%x)\n", 
            MODULE_NAME, SWMC_KMSG_WINDOW_OFFSET, SWMC_KMSG_WINDOW_OFFSET);
    
    if (node_id < 0 || node_id >= MAX_NODES) {
        pr_info(KERN_ERR "%s: Invalid node_id %d (must be 0-%d)\n", MODULE_NAME, node_id, MAX_NODES-1);
        return -EINVAL;
    }
    
    /* Get DAX device physical address */
    start_addr = get_dax_physical_range(dax_name);
    if (!start_addr) {
        pr_info(KERN_ERR "%s: Failed to get DAX device physical address for %s\n", MODULE_NAME, dax_name);
        return -ENODEV;
    }
    
    pr_info(KERN_INFO "%s: DAX device %s mapped at physical address 0x%llx\n", 
           MODULE_NAME, dax_name, start_addr);

    // cxl_hdm_base_addr initialization
    unsigned long cxl_hdm_base = start_addr;

    pr_info("[%s] Setting CXL HDM base address to 0x%lx\n", __func__, cxl_hdm_base);

    set_cxl_hdm_base(cxl_hdm_base);

    pr_info("[%s] CXL HDM base set to: 0x%lx\n", __func__, get_cxl_hdm_base());
    
    /* Allocate handler structure */
    cxl_kmsg_handler = kmalloc(sizeof(struct cxl_kmsg_handle), GFP_KERNEL);
    if (!cxl_kmsg_handler) {
        pr_info(KERN_ERR "%s: Failed to allocate handler\n", MODULE_NAME);
        return -ENOMEM;
    }
    
    /* Initialize handler */
    cxl_kmsg_handler->nid = node_id;

    /* Initialize all window pointers to NULL */
    for (i = 0; i < MAX_NODES; i++) {
        cxl_kmsg_handler->win_tx[i] = NULL;
        cxl_kmsg_handler->win_rx[i] = NULL;
    }

    // ADD 94 GB to start_addr to account for the CXL shared memory region
    // TODO: Should change daxctl or ndctl or FamFS allocator later.
    start_addr += 94UL << 30; // 94 GB

    /* Map TX windows: where this node sends to other nodes */
    for (i = 0; i < MAX_NODES; i++) {
        if (i == node_id) continue; /* Skip self */
        
        shm_window = cxl_map_window(start_addr, node_id, i, "TX");
        if (!shm_window) {
            ret = -ENOMEM;
            goto out_unmap;
        }

        cxl_kmsg_handler->win_tx[i] = shm_window;
        cxl_kmsg_window_init(shm_window);
    }

    /* Map RX windows: where this node receives from other nodes */
    for (i = 0; i < MAX_NODES; i++) {
        if (i == node_id) continue; /* Skip self */
        
        shm_window = cxl_map_window(start_addr, i, node_id, "RX");
        if (!shm_window) {
            ret = -ENOMEM;
            goto out_unmap;
        }

        cxl_kmsg_handler->win_rx[i] = shm_window;
        /* Note: Don't initialize RX windows - they're initialized by the sender */
    }
    
    /* Register messaging operations with page coherence subsystem */
    ret = swmc_kmsg_register_ops(&cxl_shm_ops);
    if (ret) {
        pr_info(KERN_ERR "%s: Failed to register messaging ops: %d\n", MODULE_NAME, ret);
        kthread_stop(tsk_recv);
        goto out_unmap;
    }
    
    /* Start receive handler thread */
    tsk_recv = kthread_run(recv_handler, cxl_kmsg_handler, "cxl_recv_%d", node_id);
    if (IS_ERR(tsk_recv)) {
        pr_info(KERN_ERR "%s: Cannot create receive handler\n", MODULE_NAME);
        ret = PTR_ERR(tsk_recv);
        goto out_unmap;
    }
    cxl_kmsg_handler->recv_handler = tsk_recv;

    
    pr_info(KERN_INFO "%s: Ready on CXL Shared Memory (Node ID: %d, %d TX + %d RX windows)\n", 
           MODULE_NAME, node_id, MAX_NODES-1, MAX_NODES-1);
    pr_info(KERN_INFO "%s: Messaging operations registered with page coherence subsystem\n", MODULE_NAME);
    return 0;

out_unmap:
    cxl_unmap_windows(cxl_kmsg_handler);
    kfree(cxl_kmsg_handler);
    cxl_kmsg_handler = NULL;
    return ret;
}


/**
 * exit_cxl_shm() - Module cleanup
 */
static void __exit exit_cxl_shm(void)
{
    pr_info(KERN_INFO "%s: Unloading CXL Shared Memory messaging layer...\n", MODULE_NAME);
    
    /* Unregister messaging operations first */
    swmc_kmsg_unregister_ops();
    pr_info(KERN_INFO "%s: Messaging operations unregistered\n", MODULE_NAME);
    
    if (cxl_kmsg_handler) {
        if (cxl_kmsg_handler->recv_handler) {
            kthread_stop(cxl_kmsg_handler->recv_handler);
        }
        
        /* Unmap all windows */
        cxl_unmap_windows(cxl_kmsg_handler);
        kfree(cxl_kmsg_handler);
        cxl_kmsg_handler = NULL;
    }
    
    pr_info(KERN_INFO "%s: Successfully unloaded\n", MODULE_NAME);
}

module_init(init_cxl_shm);
module_exit(exit_cxl_shm);