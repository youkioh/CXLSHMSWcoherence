/*
 * pgcoherence_kmsg.c - CXL Page Coherence Messaging Interface
 *
 * This file implements the messaging interface for CXL page coherence.
 * It provides an abstraction layer between page coherence logic and
 * the actual messaging implementation (cxl_shm.c).
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <swmc/swmc_kmsg.h>
#include <linux/kthread.h>

static swmc_kmsg_cbftn swmc_kmsg_cbftns[SWMC_KMSG_TYPE_MAX] = {NULL};

/* Global ops structure - will be set by messaging layer module */
static struct swmc_kmsg_ops *registered_ops = NULL;
static DEFINE_SPINLOCK(ops_lock);

int swmc_kmsg_register_callback(enum swmc_kmsg_type type, swmc_kmsg_cbftn callback)
{
    BUG_ON(type < 0 || type >= SWMC_KMSG_TYPE_MAX);

    swmc_kmsg_cbftns[type] = callback;
    return 0;
}
EXPORT_SYMBOL(swmc_kmsg_register_callback);

int swmc_kmsg_unregister_callback(enum swmc_kmsg_type type)
{
    return swmc_kmsg_register_callback(type, (swmc_kmsg_cbftn)NULL);
}
EXPORT_SYMBOL(swmc_kmsg_unregister_callback);

int swmc_kmsg_process_message(struct swmc_kmsg_message *message)
{
    swmc_kmsg_cbftn callback;

    /* Validate message pointer */
    if (!message) {
        pr_err("swmc_kmsg: NULL message pointer\n");
        return -EINVAL;
    }

    /* Validate message type range */
    if (message->header.type < 0 || message->header.type >= SWMC_KMSG_TYPE_MAX) {
        pr_err("swmc_kmsg: Invalid message type %d (max: %d)\n", 
               message->header.type, SWMC_KMSG_TYPE_MAX - 1);
        return -EINVAL;
    }

    callback = swmc_kmsg_cbftns[message->header.type];

    if (callback != NULL) {
        // make kthread to process the message
        struct task_struct *tsk;
        tsk =  kthread_run((int (*)(void *))callback, message, "swmc_kmsg_msg_processer");
        
        if (IS_ERR(tsk))
            return PTR_ERR(tsk);

        return 0;
        
        // return callback(message);
    } else {
        pr_err("No callback registered for message type %d\n", message->header.type);
        return -1; // Indicate error if no callback is registered, can be changed later
    }
}
EXPORT_SYMBOL(swmc_kmsg_process_message);

/* =============================================================================
 * MESSAGING OPERATIONS REGISTRATION
 * ============================================================================= */

int swmc_kmsg_register_ops(struct swmc_kmsg_ops *ops)
{
    unsigned long flags;
    
    if (!ops) {
        pr_err("swmc_kmsg: Cannot register NULL ops\n");
        return -EINVAL;
    }
    
    spin_lock_irqsave(&ops_lock, flags);
    if (registered_ops) {
        spin_unlock_irqrestore(&ops_lock, flags);
        pr_err("swmc_kmsg: Ops already registered (%s)\n", registered_ops->name);
        return -EBUSY;
    }
    
    registered_ops = ops;
    spin_unlock_irqrestore(&ops_lock, flags);
    
    pr_info("swmc_kmsg: Registered messaging ops: %s\n", ops->name);
    return 0;
}
EXPORT_SYMBOL(swmc_kmsg_register_ops);

void swmc_kmsg_unregister_ops(void)
{
    unsigned long flags;
    
    spin_lock_irqsave(&ops_lock, flags);
    if (registered_ops) {
        pr_info("swmc_kmsg: Unregistered messaging ops: %s\n", registered_ops->name);
        registered_ops = NULL;
    }
    spin_unlock_irqrestore(&ops_lock, flags);
}
EXPORT_SYMBOL(swmc_kmsg_unregister_ops);

/* =============================================================================
 * MESSAGING INTERFACE FUNCTIONS
 * ============================================================================= */

int swmc_kmsg_unicast(enum swmc_kmsg_type type, int ws_id, int dest_nid, struct payload_data *payload)
{
    unsigned long flags;
    int ret;
    
    spin_lock_irqsave(&ops_lock, flags);
    if (!registered_ops || !registered_ops->unicast) {
        spin_unlock_irqrestore(&ops_lock, flags);
        return SWMC_KMSG_ERR_NO_IMPL;
    }
    
    ret = registered_ops->unicast(type, ws_id, dest_nid, payload);
    spin_unlock_irqrestore(&ops_lock, flags);
    
    return ret;
}
EXPORT_SYMBOL(swmc_kmsg_unicast);

int swmc_kmsg_broadcast(enum swmc_kmsg_type type, int ws_id, struct payload_data *payload)
{
    unsigned long flags;
    int ret;
    
    spin_lock_irqsave(&ops_lock, flags);
    if (!registered_ops || !registered_ops->broadcast) {
        spin_unlock_irqrestore(&ops_lock, flags);
        return SWMC_KMSG_ERR_NO_IMPL;
    }
    
    ret = registered_ops->broadcast(type, ws_id, payload);
    spin_unlock_irqrestore(&ops_lock, flags);
    
    return ret;
}
EXPORT_SYMBOL(swmc_kmsg_broadcast);

void swmc_kmsg_done(struct swmc_kmsg_message *message)
{
    unsigned long flags;
    
    spin_lock_irqsave(&ops_lock, flags);
    if (registered_ops && registered_ops->done) {
        registered_ops->done(message);
    }
    spin_unlock_irqrestore(&ops_lock, flags);
}
EXPORT_SYMBOL(swmc_kmsg_done);

int swmc_kmsg_node_count(void)
{
    unsigned long flags;
    int count = 0;

    spin_lock_irqsave(&ops_lock, flags);
    if (registered_ops && registered_ops->node_count) {
        count = registered_ops->node_count();
    }
    spin_unlock_irqrestore(&ops_lock, flags);

    return count;
}
EXPORT_SYMBOL(swmc_kmsg_node_count);

