/**
 * test_cxl_sender.c - Test module for sending messages to specific CXL nodes
 * 
 * This module demonstrates how to use the CXL shared memory public API
 * to send messages to specific nodes' RX buffers.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <linux/string.h>

/* =============================================================================
 * MODULE PARAMETERS
 * ============================================================================= */

static int target_node = 1;
module_param(target_node, int, 0444);
MODULE_PARM_DESC(target_node, "Target node ID to send messages to (0-3)");

static int send_interval = 5;
module_param(send_interval, int, 0444);
MODULE_PARM_DESC(send_interval, "Message sending interval in seconds (default: 5)");

static int message_count = 10;
module_param(message_count, int, 0444);
MODULE_PARM_DESC(message_count, "Number of messages to send (default: 10)");

static bool enable_broadcast = false;
module_param(enable_broadcast, bool, 0444);
MODULE_PARM_DESC(enable_broadcast, "Enable broadcast mode (default: false)");

/* =============================================================================
 * EXTERNAL API DECLARATIONS
 * ============================================================================= */

/* CXL SHM public API function prototypes */
extern struct cxl_kmsg_message *cxl_kmsg_get(size_t size);
extern void cxl_kmsg_put(struct cxl_kmsg_message *msg);
extern int cxl_kmsg_send_message(int dest_nid, struct cxl_kmsg_message *msg, size_t size);
extern int cxl_kmsg_broadcast_message(struct cxl_kmsg_message *msg, size_t size);
extern int cxl_kmsg_poll_all_rx(struct cxl_kmsg_message **msg, int *from_nid);
extern int cxl_kmsg_register_processor(void (*processor)(struct cxl_kmsg_message *msg));
extern void cxl_kmsg_unregister_processor(void);

/* Message structure - must match CXL module */
struct cxl_kmsg_hdr {
    int type;
    int size;
    int from_nid;
    int to_nid;
} __attribute__((packed));

struct cxl_kmsg_message {
    struct cxl_kmsg_hdr header;
    unsigned char payload[36];
    volatile int ready;
    unsigned long last_ticket;
} __attribute__((packed, aligned(64)));

/* =============================================================================
 * MESSAGE TYPES
 * ============================================================================= */

#define MSG_TYPE_PING       1
#define MSG_TYPE_DATA       2
#define MSG_TYPE_STATUS     3
#define MSG_TYPE_ECHO       4
#define MSG_TYPE_BROADCAST  5

/* =============================================================================
 * GLOBAL VARIABLES
 * ============================================================================= */

static struct task_struct *sender_task = NULL;
static struct task_struct *receiver_task = NULL;
static atomic_t messages_sent = ATOMIC_INIT(0);
static atomic_t messages_received = ATOMIC_INIT(0);
static bool module_running = true;

/* =============================================================================
 * HELPER FUNCTIONS
 * ============================================================================= */

/**
 * create_test_message() - Create a test message with specific content
 */
static struct cxl_kmsg_message *create_test_message(int msg_type, const char *content)
{
    struct cxl_kmsg_message *msg;
    int content_len;
    
    msg = cxl_kmsg_get(32);
    if (!msg) {
        printk(KERN_ERR "CXL_SENDER: Failed to allocate message\n");
        return NULL;
    }
    
    /* Initialize message header */
    msg->header.type = msg_type;
    msg->header.from_nid = -1;  /* Will be set by sender */
    msg->header.to_nid = target_node;
    
    /* Set payload */
    memset(msg->payload, 0, sizeof(msg->payload));
    if (content) {
        content_len = min(strlen(content), sizeof(msg->payload) - 1);
        memcpy(msg->payload, content, content_len);
        msg->header.size = content_len;
    } else {
        msg->header.size = 0;
    }
    
    return msg;
}

/**
 * test_kmsg_process() - External message processor function
 */
void test_kmsg_process(struct cxl_kmsg_message *msg)
{
    if (!msg) {
        printk(KERN_ERR "CXL_SENDER: Received NULL message\n");
        return;
    }
    
    atomic_inc(&messages_received);
    
    printk(KERN_INFO "CXL_SENDER: [EXTERNAL PROCESSOR] Received message type=%d, size=%d, from_nid=%d\n",
           msg->header.type, msg->header.size, msg->header.from_nid);
    
    /* Handle different message types */
    switch (msg->header.type) {
        case MSG_TYPE_PING:
            printk(KERN_INFO "CXL_SENDER: PING message received: %.*s\n", 
                   msg->header.size, msg->payload);
            break;
        case MSG_TYPE_DATA:
            printk(KERN_INFO "CXL_SENDER: DATA message received: %.*s\n", 
                   msg->header.size, msg->payload);
            break;
        case MSG_TYPE_STATUS:
            printk(KERN_INFO "CXL_SENDER: STATUS message received: %.*s\n", 
                   msg->header.size, msg->payload);
            break;
        case MSG_TYPE_ECHO:
            printk(KERN_INFO "CXL_SENDER: ECHO message received: %.*s\n", 
                   msg->header.size, msg->payload);
            /* Send echo response */
            if (msg->header.from_nid >= 0) {
                struct cxl_kmsg_message *echo_reply;
                char reply_content[64];
                
                snprintf(reply_content, sizeof(reply_content), "ECHO_REPLY: %.*s", 
                        msg->header.size, msg->payload);
                
                echo_reply = create_test_message(MSG_TYPE_ECHO, reply_content);
                if (echo_reply) {
                    int ret = cxl_kmsg_send_message(msg->header.from_nid, echo_reply, echo_reply->header.size);
                    if (ret == 0) {
                        printk(KERN_INFO "CXL_SENDER: Sent ECHO reply to node %d\n", 
                               msg->header.from_nid);
                    } else {
                        printk(KERN_ERR "CXL_SENDER: Failed to send ECHO reply: %d\n", ret);
                    }
                    cxl_kmsg_put(echo_reply);
                }
            }
            break;
        case MSG_TYPE_BROADCAST:
            printk(KERN_INFO "CXL_SENDER: BROADCAST message received: %.*s\n", 
                   msg->header.size, msg->payload);
            break;
        default:
            printk(KERN_WARNING "CXL_SENDER: Unknown message type %d received\n", 
                   msg->header.type);
            break;
    }
    
    /* Note: Do not call kfree here - the CXL module manages message memory */
}

/**
 * send_ping_message() - Send a ping message to target node
 */
static int send_ping_message(void)
{
    struct cxl_kmsg_message *msg;
    char content[32];
    int ret;
    
    snprintf(content, sizeof(content), "PING-%d", atomic_read(&messages_sent));
    
    msg = create_test_message(MSG_TYPE_PING, content);
    if (!msg)
        return -ENOMEM;
    
    ret = cxl_kmsg_send_message(target_node, msg, msg->header.size);
    if (ret == 0) {
        atomic_inc(&messages_sent);
        printk(KERN_INFO "CXL_SENDER: Sent PING to node %d: '%s'\n", 
               target_node, content);
    } else {
        printk(KERN_ERR "CXL_SENDER: Failed to send PING to node %d: %d\n", 
               target_node, ret);
    }
    
    cxl_kmsg_put(msg);
    return ret;
}

/**
 * send_data_message() - Send a data message to target node
 */
static int send_data_message(const char *data)
{
    struct cxl_kmsg_message *msg;
    int ret;
    
    msg = create_test_message(MSG_TYPE_DATA, data);
    if (!msg)
        return -ENOMEM;
    
    ret = cxl_kmsg_send_message(target_node, msg, msg->header.size);
    if (ret == 0) {
        atomic_inc(&messages_sent);
        printk(KERN_INFO "CXL_SENDER: Sent DATA to node %d: '%s'\n", 
               target_node, data);
    } else {
        printk(KERN_ERR "CXL_SENDER: Failed to send DATA to node %d: %d\n", 
               target_node, ret);
    }
    
    cxl_kmsg_put(msg);
    return ret;
}

/**
 * send_broadcast_message() - Send a broadcast message to all nodes
 */
static int send_broadcast_message(void)
{
    struct cxl_kmsg_message *msg;
    char content[32];
    int ret;
    
    snprintf(content, sizeof(content), "BROADCAST-%d", atomic_read(&messages_sent));
    
    msg = create_test_message(MSG_TYPE_BROADCAST, content);
    if (!msg)
        return -ENOMEM;
    
    ret = cxl_kmsg_broadcast_message(msg, msg->header.size);
    if (ret == 0) {
        atomic_inc(&messages_sent);
        printk(KERN_INFO "CXL_SENDER: Broadcasted message: '%s'\n", content);
    } else {
        printk(KERN_ERR "CXL_SENDER: Failed to broadcast message: %d\n", ret);
    }
    
    cxl_kmsg_put(msg);
    return ret;
}

/* =============================================================================
 * SENDER THREAD
 * ============================================================================= */

/**
 * sender_thread() - Main sender thread function
 */
static int sender_thread(void *data)
{
    int count = 0;
    char content[32];
    
    printk(KERN_INFO "CXL_SENDER: Sender thread started (target=%d, interval=%ds, count=%d)\n",
           target_node, send_interval, message_count);
    
    while (!kthread_should_stop() && module_running && count < message_count) {
        if (enable_broadcast) {
            /* Send broadcast message */
            send_broadcast_message();
        } else {
            /* Send only one message per iteration */
            snprintf(content, sizeof(content), "MSG-%d", count);
            send_data_message(content);
        }
        
        count++;
        
        /* Wait for next interval */
        if (count < message_count) {
            ssleep(send_interval);
        }
    }
    
    printk(KERN_INFO "CXL_SENDER: Sender thread completed (%d messages sent)\n", 
           atomic_read(&messages_sent));
    
    /* Wait a bit for any remaining receives, then signal completion */
    ssleep(2);
    module_running = false;
    
    return 0;
}

/* =============================================================================
 * RECEIVER THREAD
 * ============================================================================= */

/**
 * process_received_message() - Process incoming message
 */
static void process_received_message(struct cxl_kmsg_message *msg, int from_nid)
{
    const char *msg_type_str;
    
    switch (msg->header.type) {
    case MSG_TYPE_PING:
        msg_type_str = "PING";
        break;
    case MSG_TYPE_DATA:
        msg_type_str = "DATA";
        break;
    case MSG_TYPE_STATUS:
        msg_type_str = "STATUS";
        break;
    case MSG_TYPE_ECHO:
        msg_type_str = "ECHO";
        break;
    case MSG_TYPE_BROADCAST:
        msg_type_str = "BROADCAST";
        break;
    default:
        msg_type_str = "UNKNOWN";
        break;
    }
    
    printk(KERN_INFO "CXL_SENDER: Received %s from node %d: '%.*s'\n",
           msg_type_str, from_nid, msg->header.size, msg->payload);
    
    atomic_inc(&messages_received);
}

/**
 * receiver_thread() - Message receiver thread
 */
static int receiver_thread(void *data)
{
    struct cxl_kmsg_message *msg;
    int from_nid;
    int ret;
    
    printk(KERN_INFO "CXL_SENDER: Receiver thread started\n");
    
    while (!kthread_should_stop() && module_running) {
        ret = cxl_kmsg_poll_all_rx(&msg, &from_nid);
        if (ret == 0) {
            /* Message received */
            process_received_message(msg, from_nid);
            kfree(msg);  /* Free the allocated message */
        } else if (ret == -EAGAIN) {
            /* No messages available, continue polling */
        } else {
            /* Error occurred */
            printk(KERN_ERR "CXL_SENDER: Poll error: %d\n", ret);
        }
        
        /* Short sleep to avoid busy waiting */
        msleep(100);
    }
    
    printk(KERN_INFO "CXL_SENDER: Receiver thread completed (%d messages received)\n",
           atomic_read(&messages_received));
    return 0;
}

/* =============================================================================
 * MODULE INIT/EXIT
 * ============================================================================= */

/**
 * test_cxl_sender_init() - Module initialization
 */
static int __init test_cxl_sender_init(void)
{
    printk(KERN_INFO "CXL_SENDER: Loading CXL message sender test module\n");
    printk(KERN_INFO "CXL_SENDER: Parameters - target_node=%d, send_interval=%ds, message_count=%d, broadcast=%s\n",
           target_node, send_interval, message_count, enable_broadcast ? "enabled" : "disabled");
    
    /* Validate parameters */
    if (target_node < 0 || target_node > 3) {
        printk(KERN_ERR "CXL_SENDER: Invalid target_node %d (must be 0-3)\n", target_node);
        return -EINVAL;
    }
    
    if (send_interval < 1 || send_interval > 60) {
        printk(KERN_ERR "CXL_SENDER: Invalid send_interval %d (must be 1-60)\n", send_interval);
        return -EINVAL;
    }
    
    if (message_count < 1 || message_count > 100) {
        printk(KERN_ERR "CXL_SENDER: Invalid message_count %d (must be 1-100)\n", message_count);
        return -EINVAL;
    }

    /* Register external message processor */
    cxl_kmsg_register_processor(test_kmsg_process);
    printk(KERN_INFO "CXL_SENDER: External message processor registered\n");
    
    /* Start receiver thread */
    receiver_task = kthread_run(receiver_thread, NULL, "cxl_test_receiver");
    if (IS_ERR(receiver_task)) {
        printk(KERN_ERR "CXL_SENDER: Failed to create receiver thread\n");
        return PTR_ERR(receiver_task);
    }
    
    /* Start sender thread */
    sender_task = kthread_run(sender_thread, NULL, "cxl_test_sender");
    if (IS_ERR(sender_task)) {
        printk(KERN_ERR "CXL_SENDER: Failed to create sender thread\n");
        kthread_stop(receiver_task);
        return PTR_ERR(sender_task);
    }
    
    printk(KERN_INFO "CXL_SENDER: Test module loaded successfully\n");
    return 0;
}

/**
 * test_cxl_sender_exit() - Module cleanup
 */
static void __exit test_cxl_sender_exit(void)
{
    printk(KERN_INFO "CXL_SENDER: Unloading CXL message sender test module\n");
    
    module_running = false;

    /* Unregister external message processor */
    cxl_kmsg_unregister_processor();
    printk(KERN_INFO "CXL_SENDER: External message processor unregistered\n");
    
    /* Stop threads */
    if (sender_task) {
        kthread_stop(sender_task);
    }
    
    if (receiver_task) {
        kthread_stop(receiver_task);
    }
    
    /* Give threads time to finish */
    msleep(500);
    
    printk(KERN_INFO "CXL_SENDER: Final statistics - Sent: %d, Received: %d\n",
           atomic_read(&messages_sent), atomic_read(&messages_received));
    
    printk(KERN_INFO "CXL_SENDER: Module unloaded successfully\n");
}

module_init(test_cxl_sender_init);
module_exit(test_cxl_sender_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CXL Test Developer");
MODULE_DESCRIPTION("CXL Shared Memory Message Sender Test Module");
MODULE_VERSION("1.0");
