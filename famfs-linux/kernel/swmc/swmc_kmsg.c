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
#include <linux/workqueue.h>

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

struct swmc_work_item {
    struct work_struct work;        // 커널 워크큐 관리용 구조체
    struct swmc_kmsg_message msg;   // 실제 처리할 메시지 데이터 (복사본)
};

/* 전역 워크큐 포인터 */
static struct workqueue_struct *swmc_fetch_inv_wq;
static struct workqueue_struct *swmc_ack_wq;

/* 2. Worker 함수 (Consumer) */
// 워커 스레드가 큐에서 작업을 하나 꺼냈을 때 실행되는 함수입니다.
static void swmc_work_handler(struct work_struct *work)
{
    // work_struct 포인터를 감싸고 있는 swmc_work_item 포인터를 얻어옵니다.
    struct swmc_work_item *item = container_of(work, struct swmc_work_item, work);
    swmc_kmsg_cbftn callback;
    
    // 콜백 함수 가져오기
    callback = swmc_kmsg_cbftns[item->msg.header.type];

    if (callback) {
        // 실제 콜백 실행
        callback(&item->msg);
    } else {
        pr_err("swmc_kmsg: No callback for type %d\n", item->msg.header.type);
    }

    // [중요] 동적으로 할당했던 작업 아이템 메모리 해제
    kfree(item);
}

int swmc_kmsg_process_message(struct swmc_kmsg_message *message)
{
    struct swmc_work_item *item;

    if (!message) return -EINVAL;
    if (message->header.type < 0 || message->header.type >= SWMC_KMSG_TYPE_MAX)
        return -EINVAL;

    // 콜백이 있는지 먼저 확인
    if (!swmc_kmsg_cbftns[message->header.type]) {
        pr_err("swmc_kmsg: No callback registered for type %d\n", message->header.type);
        return -EINVAL;
    }

    item = kmalloc(sizeof(struct swmc_work_item), GFP_ATOMIC);
    if (!item) {
        pr_err("swmc_kmsg: Failed to allocate work item\n");
        return -ENOMEM;
    }

    // 데이터 복사
    memcpy(&item->msg, message, sizeof(struct swmc_kmsg_message));

        // Work 구조체 초기화 (이 작업이 실행될 때 swmc_work_handler가 호출됨)
    INIT_WORK(&item->work, swmc_work_handler);

    // 큐에 작업 등록 (즉시 리턴됨)
    // 성공 시 true, 이미 큐에 있으면 false 반환
    if (message->header.type == SWMC_KMSG_TYPE_FETCH ||
        message->header.type == SWMC_KMSG_TYPE_INVALIDATE) {
        queue_work(swmc_fetch_inv_wq, &item->work);
    } else {
        queue_work(swmc_ack_wq, &item->work);
    }

    return 0;
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

static int __init swmc_kmsg_init(void)
{
    swmc_fetch_inv_wq = alloc_ordered_workqueue("swmc_kmsg_fetch_inv_worker", 0);
    swmc_ack_wq = alloc_ordered_workqueue("swmc_kmsg_ack_worker", 0);
    if (!swmc_fetch_inv_wq || !swmc_ack_wq)
        return -ENOMEM;
    return 0;
}

subsys_initcall(swmc_kmsg_init);
