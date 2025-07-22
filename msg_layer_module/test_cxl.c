/**
 * test_cxl.c - Test module for CXL shared memory functionality
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>

/* External CXL SHM function prototypes */
extern struct cxl_kmsg_message *cxl_kmsg_get(size_t size);
extern void cxl_kmsg_put(struct cxl_kmsg_message *msg);
extern int cxl_kmsg_send_message(int dest_nid, struct cxl_kmsg_message *msg, size_t size);
extern int cxl_kmsg_broadcast_message(struct cxl_kmsg_message *msg, size_t size);
extern int cxl_kmsg_poll_all_rx(struct cxl_kmsg_message **msg, int *from_nid);

/* Test message structure - must match CXL module */
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

static int test_node_id = 0;
module_param(test_node_id, int, 0444);
MODULE_PARM_DESC(test_node_id, "Test node ID for messaging");

static int __init test_cxl_init(void)
{
    struct cxl_kmsg_message *msg;
    struct cxl_kmsg_message *recv_msg;
    int from_nid;
    int ret;
    
    printk(KERN_INFO "CXL Test: Starting test on node %d\n", test_node_id);
    
    /* Test 1: Message allocation */
    msg = cxl_kmsg_get(32);
    if (!msg) {
        printk(KERN_ERR "CXL Test: Failed to allocate message\n");
        return -ENOMEM;
    }
    printk(KERN_INFO "CXL Test: Message allocation successful\n");
    
    /* Test 2: Message content setup */
    msg->header.type = 1;
    msg->header.size = 32;
    msg->header.from_nid = test_node_id;
    msg->header.to_nid = (test_node_id + 1) % 4;
    snprintf(msg->payload, sizeof(msg->payload), "Test from node %d", test_node_id);
    
    printk(KERN_INFO "CXL Test: Message prepared: '%s'\n", (char*)msg->payload);
    
    /* Test 3: Point-to-point send test */
    if (test_node_id < 3) {
        ret = cxl_kmsg_send_message(test_node_id + 1, msg, 32);
        printk(KERN_INFO "CXL Test: Send to node %d result: %d\n", test_node_id + 1, ret);
    }
    
    /* Test 4: Poll for incoming messages */
    ret = cxl_kmsg_poll_all_rx(&recv_msg, &from_nid);
    if (ret == 0) {
        printk(KERN_INFO "CXL Test: Received message from node %d: '%s'\n", 
               from_nid, (char*)recv_msg->payload);
        kfree(recv_msg);
    } else if (ret == -EAGAIN) {
        printk(KERN_INFO "CXL Test: No messages waiting (expected)\n");
    } else {
        printk(KERN_ERR "CXL Test: Poll error: %d\n", ret);
    }
    
    /* Test 5: Broadcast test */
    snprintf(msg->payload, sizeof(msg->payload), "Broadcast from node %d", test_node_id);
    ret = cxl_kmsg_broadcast_message(msg, 32);
    printk(KERN_INFO "CXL Test: Broadcast result: %d\n", ret);
    
    /* Clean up */
    cxl_kmsg_put(msg);
    printk(KERN_INFO "CXL Test: All tests completed successfully\n");
    
    return 0;
}

static void __exit test_cxl_exit(void)
{
    printk(KERN_INFO "CXL Test: Module unloaded\n");
}

module_init(test_cxl_init);
module_exit(test_cxl_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CXL Test");
MODULE_DESCRIPTION("Test module for CXL shared memory");
MODULE_VERSION("1.0");
