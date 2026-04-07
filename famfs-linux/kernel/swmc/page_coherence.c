/*
 * Page Coherence Management for CXL Shared Memory
 *
 * This file implements page coherence functionality for managing 
 * replica pages in CXL shared memory environments.
 */

#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/slab.h>
#include <linux/pfn_t.h>
#include <swmc/page_coherence.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/iomap.h>
#include <linux/gfp.h>
#include <linux/printk.h>
#include <linux/memcontrol.h>
#include <linux/hugetlb.h>    /* pfn_pmd, pmd_mkdirty, set_pmd_at */
#include <swmc/page_replication_info.h>
#include <linux/xarray.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <swmc/swmc_kmsg.h>
#include "wait_station.h"
#include <linux/rmap.h>
#include <linux/pagewalk.h>
#include <linux/mm_inline.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/shmem_fs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/pagemap.h>
#include <linux/delay.h>
#include <linux/ktime.h>

#ifdef CONFIG_PAGE_COHERENCE

/*
 * Page Coherence Management for CXL Shared Memory
 *
 * This file implements page coherence functionality for managing
 * replica pages in CXL shared memory environments.
 */

// dummy base PA for CXL HDM 
static unsigned long cxl_hdm_base = 0x1e80000000; // Default value, can be set by external module

/**
 * set_cxl_hdm_base - Set the CXL HDM base address
 * @base_addr: CXL HDM base physical address
 *
 * This function allows external modules to set the CXL HDM base address
 * during their initialization phase.
 */
void set_cxl_hdm_base(unsigned long base_addr)
{
    cxl_hdm_base = base_addr;
    pr_info("[Info]%s: CXL HDM base address set to 0x%lx\n", __func__, base_addr);
}
EXPORT_SYMBOL(set_cxl_hdm_base);

/**
 * get_cxl_hdm_base - Get the current CXL HDM base address
 *
 * Returns: Current CXL HDM base address
 */
unsigned long get_cxl_hdm_base(void)
{
    return cxl_hdm_base;
}
EXPORT_SYMBOL(get_cxl_hdm_base);

/* =============================================================================
 * SYSFS INTERFACE FOR PAGE COHERENCE FAULT STATISTICS
 * ============================================================================= */

/* Page coherence fault statistics */
static atomic64_t page_coherence_fault_count = ATOMIC64_INIT(0);
static atomic64_t page_coherence_fault_read_count = ATOMIC64_INIT(0);
static atomic64_t page_coherence_fault_write_count = ATOMIC64_INIT(0);
static atomic64_t page_coherence_replica_found_count = ATOMIC64_INIT(0);
static atomic64_t page_coherence_replica_created_count = ATOMIC64_INIT(0);
static atomic64_t page_coherence_total_page_fault_handling_time_ns = ATOMIC64_INIT(0);
static atomic64_t page_coherence_total_async_transaction_wait_time_ns = ATOMIC64_INIT(0);
static atomic64_t page_coherence_total_coherence_transaction_time_ns = ATOMIC64_INIT(0);
static atomic64_t page_coherence_total_page_replication_time_ns = ATOMIC64_INIT(0);
static atomic64_t page_coherence_total_metadata_update_time_ns = ATOMIC64_INIT(0);

/* Sysfs show functions for fault statistics */
static ssize_t fault_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_fault_count));
}

static ssize_t fault_read_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_fault_read_count));
}

static ssize_t fault_write_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_fault_write_count));
}

static ssize_t replica_found_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_replica_found_count));
}

static ssize_t replica_created_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_replica_created_count));
}

static ssize_t total_page_fault_handling_time_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_total_page_fault_handling_time_ns));
}

static ssize_t total_async_transaction_wait_time_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_total_async_transaction_wait_time_ns));
}

static ssize_t total_coherence_transaction_time_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_total_coherence_transaction_time_ns));
}

static ssize_t total_page_replication_time_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_total_page_replication_time_ns));
}

static ssize_t total_metadata_update_time_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_total_metadata_update_time_ns));
}

/* Sysfs store function for resetting counters */
static ssize_t reset_counters_store(struct kobject *kobj, struct kobj_attribute *attr,
                                  const char *buf, size_t count)
{
    int reset_value;
    
    if (kstrtoint(buf, 10, &reset_value) < 0)
        return -EINVAL;
    
    if (reset_value == 1) {
        atomic64_set(&page_coherence_fault_count, 0);
        atomic64_set(&page_coherence_fault_read_count, 0);
        atomic64_set(&page_coherence_fault_write_count, 0);
        atomic64_set(&page_coherence_replica_found_count, 0);
        atomic64_set(&page_coherence_replica_created_count, 0);
        atomic64_set(&page_coherence_total_page_fault_handling_time_ns, 0);
        atomic64_set(&page_coherence_total_async_transaction_wait_time_ns, 0);
        atomic64_set(&page_coherence_total_coherence_transaction_time_ns, 0);
        atomic64_set(&page_coherence_total_page_replication_time_ns, 0);
        atomic64_set(&page_coherence_total_metadata_update_time_ns, 0);
        pr_info("[Info]%s: All fault counters reset\n", __func__);
    }
    
    return count;
}

/* Define sysfs attributes */
static struct kobj_attribute fault_count_attr = __ATTR_RO(fault_count);
static struct kobj_attribute fault_read_count_attr = __ATTR_RO(fault_read_count);
static struct kobj_attribute fault_write_count_attr = __ATTR_RO(fault_write_count);
static struct kobj_attribute replica_found_count_attr = __ATTR_RO(replica_found_count);
static struct kobj_attribute replica_created_count_attr = __ATTR_RO(replica_created_count);
static struct kobj_attribute total_page_fault_handling_time_attr = __ATTR_RO(total_page_fault_handling_time);
static struct kobj_attribute total_async_transaction_wait_time_attr = __ATTR_RO(total_async_transaction_wait_time);
static struct kobj_attribute total_coherence_transaction_time_attr = __ATTR_RO(total_coherence_transaction_time);
static struct kobj_attribute total_page_replication_time_attr = __ATTR_RO(total_page_replication_time);
static struct kobj_attribute total_metadata_update_time_attr = __ATTR_RO(total_metadata_update_time);
static struct kobj_attribute reset_counters_attr = __ATTR_WO(reset_counters);

/* Array of attributes for the attribute group */
static struct attribute *page_coherence_attrs[] = {
    &fault_count_attr.attr,
    &fault_read_count_attr.attr,
    &fault_write_count_attr.attr,
    &replica_found_count_attr.attr,
    &replica_created_count_attr.attr,
    &total_page_fault_handling_time_attr.attr,
    &total_async_transaction_wait_time_attr.attr,
    &total_coherence_transaction_time_attr.attr,
    &total_page_replication_time_attr.attr,
    &total_metadata_update_time_attr.attr,
    &reset_counters_attr.attr,
    NULL,
};

static struct attribute_group page_coherence_attr_group = {
    .name = "page_coherence",
    .attrs = page_coherence_attrs,
};

static struct kobject *page_coherence_kobj;

/* =============================================================================
 * MESSAGE HANDLING FUNCTIONS
 * ============================================================================= */

#define FAULT_HASH_SIZE 31
#define FH_ACTION_MAX_FOLLOWER 8

atomic64_t __local_acked_fault_count = ATOMIC64_INIT(0); // Local ACK count incremented when local handling gets an ACK. lower ACK count means higher priority.
struct hlist_head faults[FAULT_HASH_SIZE];
spinlock_t faults_lock[FAULT_HASH_SIZE];
static struct kmem_cache *__fault_handle_cache = NULL;

enum fault_handle_state {
    FH_STATE_REMOTE = 0x04,
    FH_STATE_WRITE = 0x02,
    FH_STATE_NEED_REDO = 0x01,
};

// To serialize concurrent fault handling caused by multiple processes from the same/other nodes processes.
struct fault_handle {
    struct hlist_node list;

    unsigned long original_pfn_val;
    unsigned long fh_flags;

    struct completion *complete;
};

static inline int __fault_hash_key(unsigned long pfn)
{
    return pfn % FAULT_HASH_SIZE;
}

static struct fault_handle *__alloc_fault_handle(unsigned long pfn)
{
    // pr_info("[Info]%s: Allocating fault handle for pfn=0x%lx\n", __func__, pfn);
	struct fault_handle *fh = kmem_cache_alloc(__fault_handle_cache, GFP_ATOMIC);
	int fk = __fault_hash_key(pfn);
	
	if (!fh)
		return NULL;

	INIT_HLIST_NODE(&fh->list);

	fh->original_pfn_val = pfn;
	fh->fh_flags = 0;

    fh->complete = NULL;

	hlist_add_head(&fh->list, &faults[fk]);
	return fh;
}

static inline bool has_lower_priority(unsigned long fh_flags, bool is_write, long remote_acked_fault_count, int remote_node_id, int local_node_id)
{
    bool local_is_write = (fh_flags & FH_STATE_WRITE) != 0;
    long local_acked_count = atomic64_read(&__local_acked_fault_count);
    
    /* READ vs WRITE: WRITE always has higher priority */
    if (!is_write && local_is_write) {
        pr_info("[Info]%s: Remote READ has lower priority than local WRITE\n", __func__);
        return true;  /* Remote READ has lower priority than local WRITE */
    }
    
    /* Both are WRITE faults: Compare ACK counts first */
    if (is_write && local_is_write) {
        if (remote_acked_fault_count < local_acked_count) {
            pr_info("[Info]%s: Remote WRITE has higher priority than local WRITE\n", __func__);
            return false;   /* Remote has higher priority (lower ACK count) */
        }
        
        if (remote_acked_fault_count > local_acked_count) {
            pr_info("[Info]%s: Remote WRITE has lower priority than local WRITE\n", __func__);
            return true;  /* Local has higher priority (lower ACK count) */
        }
        
        /* ACK counts are equal: Use node ID as tiebreaker */
        pr_info("[Info]%s: Remote WRITE and local WRITE have equal ACK counts, comparing node IDs (remote: %d, local: %d)\n", __func__, remote_node_id, local_node_id);
        return (local_node_id < remote_node_id);  /* Lower node ID wins */
    }
    
    /* All other cases: Remote has higher or equal priority */
    pr_info("[Info]%s: Remote fault has higher or equal priority\n", __func__);
    return false;
}

/**
 * __start_local_fault_handling - Handle local fault processing
 * @pid: current task pid
 * @original_pfn: fault pfn
 * @vm_fault_flags: fault flags
 * @leader: output parameter indicating if this is the leader
 *
 * Returns: fault_handle pointer or NULL
 */
static struct fault_handle *__start_local_fault_handling(pid_t pid, 
							 pfn_t original_pfn, 
                             bool is_write)
{
    // pr_info("[Info]%s: Starting local fault handling for pid=%d, pfn=0x%lx, is_write=%s\n", __func__, pid, pfn_t_to_pfn(original_pfn), is_write ? "true" : "false");
	unsigned long flags;
	struct fault_handle *fh;
	bool found = false;
    unsigned long original_pfn_val;
    original_pfn_val = pfn_t_to_pfn(original_pfn);
    int fk = __fault_hash_key(original_pfn_val);

	spin_lock_irqsave(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Acquired lock for fault hash bucket %d\n", __func__, fk);

    /* Search for existing fault handle */
    hlist_for_each_entry(fh, &faults[fk], list) {
        if (fh->original_pfn_val == original_pfn_val) {
            found = true;
            break;
        }
    }

    if (found) {
        // pr_info("[Info]%s: Found existing fault handle for pfn=0x%lx, PID=%d, %s.", __func__, original_pfn_val, pid, fh->fh_flags & FH_STATE_REMOTE ? "REMOTE" : "LOCAL");
        DECLARE_COMPLETION_ONSTACK(complete);
        fh->complete = &complete;

        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        wait_for_completion(&complete);
        // pr_info("[Info]%s: Waked up from existing fault handle for pfn=0x%lx, PID=%d\n", __func__, original_pfn_val, pid);

        if (fh->fh_flags & FH_STATE_WRITE) {
            pr_info("[Info]%s: Fault handling for pfn=0x%lx needs to be redone to release DAX entry lock\n",
                    __func__, original_pfn_val);
            hlist_del_init(&fh->list);
            kmem_cache_free(__fault_handle_cache, fh);
            return NULL;
        }
        spin_lock_irqsave(&faults_lock[fk], flags);
        fh->fh_flags = is_write ? FH_STATE_WRITE : 0;
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        return fh;
    }

    /* Allocate new fault handle */
    fh = __alloc_fault_handle(original_pfn_val);
    if (!fh) {
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        return NULL;
    }
    
    /* Update flags */
    fh->fh_flags = is_write ? FH_STATE_WRITE : 0;

    spin_unlock_irqrestore(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);

    return fh;
}

/**
 * __finish_local_fault_handling - Complete local fault processing
 * @fh: fault handle
 *
 * Returns: true if fault handling need to be redone
 */
static bool __finish_local_fault_handling(struct fault_handle *fh)
{
    // pr_info("[Info]%s: Finishing local fault handling for original_pfn=0x%lx\n", __func__, fh->original_pfn_val);
    unsigned long flags;
    bool need_redo = false;
    int fk = __fault_hash_key(fh->original_pfn_val);

    spin_lock_irqsave(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Acquired lock for fault hash bucket %d\n", __func__, fk);

    /* If this local fault need to be redone, return true */
    if (fh->fh_flags & FH_STATE_NEED_REDO) {
        pr_info("[Info]%s: Fault handling for pfn=0x%lx needs to be redone\n",
                __func__, fh->original_pfn_val);
        need_redo = true;
    }

    // pr_info("[Info]%s: Completed local fault handling for pfn=0x%lx, deleting fault handle.\n", __func__, fh->original_pfn_val);
    hlist_del_init(&fh->list);
    spin_unlock_irqrestore(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
    kmem_cache_free(__fault_handle_cache, fh);
    return need_redo;
}

/**
 * __start_remote_fault_handling - Handle remote fault processing
 * @original_pfn: fault pfn
 * @is_write: true for write fault, false for read fault
 *
 * Returns: fault_handle pointer for ACK or NULL for NACK 
 */
static struct fault_handle *__start_remote_fault_handling(pfn_t original_pfn, bool is_write, long remote_acked_fault_count, int remote_node_id, int local_node_id)
{
    // pr_info("[Info]%s: Starting remote fault handling for pfn=0x%lx, is_write=%s, remote_acked_fault_count=%ld, remote_node_id=%d, local_node_id=%d\n", 
    //         __func__, pfn_t_to_pfn(original_pfn), is_write ? "true" : "false", remote_acked_fault_count, remote_node_id, local_node_id);
    unsigned long flags;
    struct fault_handle *fh;
    bool found = false;
    unsigned long original_pfn_val;
    original_pfn_val = pfn_t_to_pfn(original_pfn);
    
    int fk = __fault_hash_key(original_pfn_val);
    
    
    spin_lock_irqsave(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Acquired lock for fault hash bucket %d\n", __func__, fk);

    /* Search for existing fault handle */
    hlist_for_each_entry(fh, &faults[fk], list) {
        if (fh->original_pfn_val == original_pfn_val) {
            found = true;
            break;
        }
    }

    /* Conditions for NACK:
     * 1. remote fault in-flight
     * 2. Local fault in-flight with higher priority

     * Condition for ACK:
     * 1. No fault in-flight
     * 2. Local fault in-flight with lower priority
     * 3. Local fault in-flight with same priority (i.e. both fault are for I->S)
     */

    if (found) {
        // pr_info("[Info]%s: Found existing fault handle for pfn=0x%lx \n",
        //         __func__, original_pfn_val);
        if (fh->fh_flags & FH_STATE_REMOTE) {
            /* Another remote fault is already being processed */
            spin_unlock_irqrestore(&faults_lock[fk], flags);
            // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
            return NULL;
        }

        if (has_lower_priority(fh->fh_flags, is_write, remote_acked_fault_count, remote_node_id, local_node_id)) {
            spin_unlock_irqrestore(&faults_lock[fk], flags);
            // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
            return NULL;
        }

        /* Local fault is being processed, but remote fault has higher or equal priority */
        if (is_write) {
            fh->fh_flags |= FH_STATE_NEED_REDO;
        }
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        return fh;
    }

    /* Allocate new fault handle for remote processing */
    fh = __alloc_fault_handle(original_pfn_val); 
    if (!fh) {
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        return NULL;
    }

    /* Update flags for remote handling */
    fh->fh_flags = FH_STATE_REMOTE | (is_write ? FH_STATE_WRITE : 0);

    spin_unlock_irqrestore(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);

    return fh;
}

/**
 * __finish_remote_fault_handling - Complete remote fault processing
 * @fh: fault handle
 *
 * Returns: true if fault handle is freed.
 */
static bool __finish_remote_fault_handling(struct fault_handle *fh)
{
    // pr_info("[Info]%s: Finishing remote fault handling for pfn=0x%lx\n", __func__, fh->original_pfn_val);
    unsigned long flags;
    int fk = __fault_hash_key(fh->original_pfn_val);

    spin_lock_irqsave(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Acquired lock for fault hash bucket %d\n", __func__, fk);

    if (fh->complete) {
        // pr_info("[Info]%s: There is a local fault waiting for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        complete(fh->complete);
        fh->complete = NULL;
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        return false;
    }

    if (fh->fh_flags & FH_STATE_REMOTE) {
        // pr_info("[Info]%s: No local fault waiting, deleting fault handle for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        hlist_del_init(&fh->list);
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
    
        kmem_cache_free(__fault_handle_cache, fh);
        return true;
    }

    // pr_info("[Info]%s: Completed remote fault handling without freeing fault handle for pfn=0x%lx\n", __func__, fh->original_pfn_val);
    spin_unlock_irqrestore(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);

    return false;
}

// Fetch/Invalidate message handling
// M-> S, S -> S, I -> I
// S -> I, I -> I (M -> I is violated)
static int swmc_kmsg_handle_fetch_or_invalidate(struct swmc_kmsg_message *msg)
{
    int ret = 0;
    struct payload_data *payload = &msg->payload;
    // Validate message
    if (!msg || (msg->header.type != SWMC_KMSG_TYPE_FETCH && msg->header.type != SWMC_KMSG_TYPE_INVALIDATE)) {
        pr_err("[Info]%s: Invalid fetch/invalidate message\n", __func__);
        return -EINVAL;
    }

    
    // calculate original pfn from payload->cxl_hdm_offset
    unsigned long original_phys_addr = cxl_hdm_base + payload->cxl_hdm_offset;
    pfn_t original_pfn;
    
    if (payload->page_order == 0 || payload->page_order == PMD_ORDER) {
        original_pfn = pfn_to_pfn_t(original_phys_addr >> PAGE_SHIFT);
    }
    else {
        pr_err("[Error]%s: Invalid page order: %d\n", __func__, payload->page_order);
        return -EINVAL;
    }
    // pr_info("[Info]%s: Handling fetch/invalidate message for offset 0x%lx, page order=%d, original PFN=0x%lx.\n", __func__, payload->cxl_hdm_offset, payload->page_order, pfn_t_to_pfn(original_pfn));

    // find fault handle for remote fault handling
    struct fault_handle *fh;
    struct fault_handle *_tmp_fh;
    bool exist = false;
    long remote_acked_fault_count = payload->acked_fault_count;
    unsigned long flags;
    bool is_write = (msg->header.type == SWMC_KMSG_TYPE_INVALIDATE);
    struct page *page_replica;
    int fk;

    fh = __start_remote_fault_handling(original_pfn, is_write, remote_acked_fault_count, msg->header.from_nid, msg->header.to_nid);
    if (!fh) {
        pr_info("[Info]%s: NACK remote fault handling\n", __func__);
        ret = swmc_kmsg_unicast((is_write ? SWMC_KMSG_TYPE_INVALIDATE_NACK : SWMC_KMSG_TYPE_FETCH_NACK), msg->header.ws_id, msg->header.from_nid, payload);
        return ret;
    }

    // pr_info("[Info]%s: ACK remote fault handling\n", __func__);

    // find page replica from original pfn
    page_replica = get_page_replica_with_ref(original_pfn, payload->page_order);

    if (!page_replica) { // I -> I
        // pr_info("[Info]%s: No replica page found (I->I transition)\n", __func__);
    } else if (!is_write) {
        // M -> S, S -> S
        // pr_info("[Info]%s: Page replica found (M->S or S->S transition)\n", __func__);

        /* For M->S transition: remove write permissions from all mappings
         * For S->S transition: already read-only, so writeback will check if dirty
         * In both cases, writeback_page_replica() will handle it.
         */
        ret = writeback_page_replica(page_replica);
        // if (ret == REPLICA_SUCCESS) {
        //     pr_info("[Info]%s: M->S transition, successfully wrote back page relica.\n", __func__);
        // } else if (ret == REPLICA_SHARED_STATE) {
        //     pr_info("[Info]%s: S->S transition, no writeback needed\n", __func__);
        // } else {
        //     pr_info("[Info]%s: Failed to write-protect page replica: \n", __func__);
        // }
        /* Release reference obtained from get_page_replica_with_ref() */
        put_page_replica_ref(page_replica);
    } else {
        // S -> I (M -> I is not allowed)
        // pr_info("[Info]%s: Page replica found (S->I transition)\n", __func__);

        /* For M->I or S->I transitions, clean up the page replica
         * This do not includes flushing dirty data back to original.
         * Just free the replica.
         * So M->I transition is not allowed.
         */

        // Just make page_replica to invalid
        // make_page_replica_invalid(page_replica);
        put_page_replica_ref(page_replica);
        // struct task_struct *tsk;
        // tsk = kthread_run((int (*)(void *))destroy_page_replica, page_replica, "destroy_replica");
        // if (IS_ERR(tsk)) {
        //     pr_err("[Error]%s: Failed to create kernel thread to destroy page replica %p\n", __func__, page_replica);
        // } else {
        //     pr_info("[Info]%s: Created kernel thread %s to destroy page replica %p\n", __func__, tsk->comm, page_replica);
        // }
        destroy_page_replica(page_replica);
    }

    ret = swmc_kmsg_unicast((is_write ? SWMC_KMSG_TYPE_INVALIDATE_ACK : SWMC_KMSG_TYPE_FETCH_ACK), msg->header.ws_id, msg->header.from_nid, payload);

    fk = __fault_hash_key(pfn_t_to_pfn(original_pfn));

    spin_lock_irqsave(&faults_lock[fk], flags);
    hlist_for_each_entry(_tmp_fh, &faults[fk], list) {
        if (_tmp_fh->original_pfn_val == pfn_t_to_pfn(original_pfn)) {
            exist = true;
            break;
        }
    }
    spin_unlock_irqrestore(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);

    if(!exist) {
        pr_info("[Info]%s: Fault handle already deleted for pfn=0x%lx\n", __func__, pfn_t_to_pfn(original_pfn));
        return 0;
    }

    __finish_remote_fault_handling(fh);

    return 0;
}


// Invalidate_ack message handling
static int swmc_kmsg_handle_ack_or_nack(struct swmc_kmsg_message *msg)
{
    // check if the message is valid
    if (!msg || (msg->header.type != SWMC_KMSG_TYPE_INVALIDATE_ACK && msg->header.type != SWMC_KMSG_TYPE_INVALIDATE_NACK && msg->header.type != SWMC_KMSG_TYPE_FETCH_ACK && msg->header.type != SWMC_KMSG_TYPE_FETCH_NACK)) {
        pr_err("[Err]%s: Invalid ACK/NACK message\n", __func__);
        return -EINVAL;
    }

    // pr_info("[Info]%s: Handling ACK/NACK message for offset 0x%lx\n", __func__, msg->payload.cxl_hdm_offset);

    // find the wait station by ID
    struct wait_station *ws = wait_station(msg->header.ws_id);
    if (!ws) {
        pr_err("[Err]%s: Invalid wait station ID: %d\n", __func__, msg->header.ws_id);
        return -EINVAL;
    }

    if (msg->header.type == SWMC_KMSG_TYPE_INVALIDATE_NACK || msg->header.type == SWMC_KMSG_TYPE_FETCH_NACK) {
        // pr_info("[Info]%s: Received NACK for wait station %d\n", __func__, msg->header.ws_id);
        ws->private = (void *)-1; // Indicate NACK received
    }

    // Decrease pending count atomically
    if (atomic_dec_and_test(&ws->pendings_count)) {
        // All invalidate ACKs received, wake up the wait station
        // pr_info("[Info]%s: All ACKs/NACKs received for wait station %d\n", __func__, msg->header.ws_id);
        atomic64_inc(&__local_acked_fault_count); // Increment remote ACK count
        complete(&ws->pendings);
    } else {
        // pr_info("[Info]%s: ACK/NACK received, pending count: %d\n", __func__,
        //         atomic_read(&ws->pendings_count));
    }

    return 0;
}

// Error message handling
static int swmc_kmsg_handle_error(struct swmc_kmsg_message *msg)
{
    // Validate message
    if (!msg || msg->header.type != SWMC_KMSG_TYPE_ERROR) {
        pr_err("[Err]%s: Invalid error message\n", __func__);
        return -EINVAL;
    }

    pr_err("[Err]%s: Received error message from node %d for offset 0x%lx\n", __func__,
           msg->header.from_nid, msg->payload.cxl_hdm_offset);

    // TODO: Handle error appropriately
    return 0;
}


/* =============================================================================
 * PAGE COHERENCE FAULT HANDLING
 * ============================================================================= */
/* Helper function to handle messaging operations */
static int broadcast_message_and_wait(enum swmc_kmsg_type msg_type, 
                                     int node_count, struct payload_data *payload,
                                     unsigned long cxl_hdm_offset)
{
    struct wait_station *ws;
    int ret;

    // register wait station for this fault
    ws = get_wait_station_multiple(current, node_count - 1);
    if (!ws) {
        pr_info("[Info]%s: Failed to get wait station\n", __func__);
        return -ENOMEM;
    }
    
    // broadcast message
    ret = swmc_kmsg_broadcast(msg_type, ws->id, payload);
    if (ret) {
        pr_info("[Info]%s: Failed to send %s message: %d\n", __func__, 
               msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate", ret);
        // Continue anyway for now - could implement fallback
    }

    void *wait_result = wait_at_station(ws);
    
    if (wait_result == (void *)-1) {
        pr_info("[Info]%s: Received NACK for %s message, aborting operation\n", __func__,
        msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate");
        return -EAGAIN; // Indicate operation should be retried or aborted
    } else if (IS_ERR(wait_result)) {
        ret = PTR_ERR(wait_result);
        pr_info("[Info]%s: Failed to wait at station: %d\n", __func__, ret);
        return ret;
    }

    // pr_info("[Info]%s: Waiting done, received response for %s message\n", __func__,
    //         msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate");
    return 0;
}

/**
 * page_coherence_fault - Handle page coherence faults
 * @vmf: Fault information structure
 * @iter: IOMAP iterator
 * @size: Size of the fault (PAGE_SIZE or PMD_SIZE)
 * @kaddr: Kernel virtual address of the original page
 * @pfn: Pointer to the page frame number, will be updated to replica PFN
 *
 * Returns: 0 on success, negative error code on failure
 */
int page_coherence_fault(struct vm_fault *vmf, const struct iomap_iter *iter,
                         size_t size, void *kaddr, pfn_t *pfn, pfn_t *pfnp)
{
    struct page *page_replica;
    unsigned int order;
    pfn_t original_pfn = *pfn;
    pfn_t replica_pfn;
    int ret;
    bool write = iter->flags & IOMAP_WRITE;
    struct payload_data payload;

    int node_count;
    unsigned long cxl_hdm_offset;
    bool need_invalidate;
    struct fault_handle *fh;
    unsigned long flags;
    bool is_currently_writable;
    int fk;

    // TODO: change this with a proper check with dax name
    // early return if it's not a fault to famfs
    pfn_t cxl_hdm_base_pfn = pfn_to_pfn_t(cxl_hdm_base >> PAGE_SHIFT);
    if (pfn_t_to_pfn(original_pfn) < pfn_t_to_pfn(cxl_hdm_base_pfn)) {
        pr_info("[Info]%s: Not a CXL HDM fault, skipping page coherence handling\n", __func__);
        return 0;
    }

    // check if file path contains ".meta", if yes, skip page coherence handling
    // char *buff = (char *)__get_free_page(GFP_KERNEL);
    // char *path = d_path(&vmf->vma->vm_file->f_path, buff, PAGE_SIZE);
    // if (strstr(path, ".meta")) {
    //     pr_info("[Info]%s: Meta file access, skipping page coherence handling for %s\n", __func__, path);
    //     free_page((unsigned long)buff);
    //     return 0;
    // }
    // free_page((unsigned long)buff);

    struct file *file = vmf->vma->vm_file;
    const char *filename;
    filename = file->f_path.dentry->d_name.name;
    if (strstr(filename, ".log")) {
        pr_info("[Info]%s: .log Meta file access, skipping page coherence handling for %s\n", __func__, filename);
        return 0; 
    }
    if (strstr(filename, ".superblock")) {
        pr_info("[Info]%s: .superblock Meta file access, skipping page coherence handling for %s\n", __func__, filename);
        return 0; 
    }

    // Validate original page exists for devdax
    struct page *original_page = pfn_to_page(pfn_t_to_pfn(original_pfn));
    if (!original_page) {
        pr_err("[Err]%s: Invalid original page pointer\n", __func__);
        return -EINVAL;
    }
    // print_page_info(original_page, "original_page");
    // print_page_info(original_page + 1, "original_page + 1");
    // print_page_info(original_page + 2, "original_page + 2");
    
    /* Increment total fault counter */
    atomic64_inc(&page_coherence_fault_count);
    
    /* Increment read/write specific counters */
    if (write) {
        atomic64_inc(&page_coherence_fault_write_count);
    } else {
        atomic64_inc(&page_coherence_fault_read_count);
    }

    /* start timestamp */
    ktime_t start_page_fault, start_coherence_transaction, start_page_replication, start_metadata_update;
    ktime_t end_page_fault, end_coherence_transaction, end_page_replication, end_metadata_update;
    ktime_t coherence_transaction, page_replication, metadata_update, page_fault_latency;
    start_page_fault = ktime_get();

    if (!vmf || !iter || !kaddr || !pfn) {
        pr_err("[Err]%s: Invalid parameters\n", __func__);
        return -EINVAL;
    }

    /* Determine page order */
    if (size == PMD_SIZE)
        order = PMD_ORDER;
    else if (size == PAGE_SIZE)
        order = 0;
    else {
        pr_err("[Err]%s: Unsupported size: %zu\n", __func__, size);
        return -EINVAL;
    }

    // pr_info("[Info]%s: Fault at 0x%lx: size=%zu order=%u original pfn=0x%lx\n",
    //     __func__, vmf->address, size, order, pfn_t_to_pfn(original_pfn));

    // Get CXL HDM offset for this fault
    cxl_hdm_offset = pfn_t_to_pfn(original_pfn) * PAGE_SIZE - cxl_hdm_base;

    payload.cxl_hdm_offset = cxl_hdm_offset;
    payload.page_order = order;
    payload.acked_fault_count = atomic64_read(&__local_acked_fault_count);

    node_count = swmc_kmsg_node_count();
    if (node_count <= 0) {
        pr_err("[Err]%s: Invalid node count: %d\n", __func__, node_count);
        return -EINVAL;
    }

redo:

    fh = __start_local_fault_handling(current->pid, original_pfn, write);
    if (!fh) {
        pr_err("[Err]%s: Failed to allocate new fault handle\n", __func__);
        return -ENOMEM;
    }

    /* =======================================================================
     * STEP 1: Check if replica already exists for this original PFN
     * ======================================================================= */
    
    page_replica = get_page_replica_with_ref(original_pfn, order);

    if (page_replica) {
        /* Replica exists - handle different fault scenarios */
        atomic64_inc(&page_coherence_replica_found_count);
        // pr_info("[Info]%s: Replica found: %p\n", __func__, page_replica);
            
        if (!write) {
            /* READ FAULT on existing replica:
             * Simply map the existing replica without any protocol messages.
             * This handles the case where multiple processes read the same page.
             */
            //  pr_info("[Info]%s: Read fault on existing replica - direct mapping\n", __func__);
             put_page_replica_ref(page_replica);
             goto map_pfn;
            }

        /* WRITE FAULT on existing replica:
        * Need to check if replica is already writable by ANY process in the system.
        * If already writable somewhere, this is M->M (just map it writable here too).
        * If only readable, this is S->M transition (need invalidation to others).
        */
        // pr_info("[Info]%s: Write fault on existing replica\n", __func__);

        /* Check if the replica page is currently mapped as writable anywhere in the system.
        * We use page_mapped() and check if any PTE/PMD has write permissions.
        * This tells us the current coherence state:
        * - If writable mappings exist: page is in M state
        * - If only read-only mappings exist: page is in S state
        */
        is_currently_writable = false;

        if (page_replica->mapping) {

            /* Check if this page is mapped writable anywhere in the system.
             * Check if page is dirty, which indicates recent writes         
             */
            if (check_page_replica_dirty(page_replica)) {
                is_currently_writable = true;
                // pr_info("[Info]%s: Replica page is dirty - M state\n", __func__);
            } else {
                /* Page is clean - likely in S state, but could be M with clean data */
                // pr_info("[Info]%s: Replica page is clean - S state\n", __func__);
                is_currently_writable = false;
            }
        } else {
            /* Page not mapped anywhere - shouldn't happen for existing replica */
            // pr_info("[Info]%s: Replica exists but not mapped anywhere\n", __func__);
            is_currently_writable = false;
        }
        
        if (is_currently_writable) {
            /* Replica is already in M state - just map it writable here too (M->M) */
            // pr_info("[Info]%s: Replica already writable elsewhere - M->M transition\n", __func__);
            put_page_replica_ref(page_replica);
            goto map_pfn;
        }

        /* Replica is in S state - need S->M transition */
        // pr_info("[Info]%s: S->M transition needed for existing replica\n", __func__);
        start_coherence_transaction = ktime_get();
        /* Send invalidation to other nodes for S->M transition */
        ret = broadcast_message_and_wait(SWMC_KMSG_TYPE_INVALIDATE, node_count, 
                                        &payload, cxl_hdm_offset);
        end_coherence_transaction = ktime_get();
        coherence_transaction = ktime_sub(end_coherence_transaction, start_coherence_transaction);
        atomic64_add(ktime_to_ns(coherence_transaction), &page_coherence_total_coherence_transaction_time_ns);

        if (ret == -EAGAIN) {
            /* Invalidate was NACKed - likely due to concurrent write fault elsewhere
             * Retry entire fault handling
             */
            pr_info("[Info]%s: Invalidate NACKed - retrying fault handling, original PFN: %lx\n", __func__, fh->original_pfn_val);
            put_page_replica_ref(page_replica);
            __finish_local_fault_handling(fh);
            // wait for a second before redo
            msleep(1);
            return VM_FAULT_RETRY;
            // goto redo;
        } else if (ret) {
            put_page_replica_ref(page_replica);
            pr_err("[Err]%s: Failed to invalidate page replica, VM_FAULT_RETRY, original PFN: %lx, error: %d\n", __func__, fh->original_pfn_val, ret);
            __finish_local_fault_handling(fh);
            return VM_FAULT_RETRY;
        }
        put_page_replica_ref(page_replica);
        goto map_pfn;
    }
    
    /* =======================================================================
     * STEP 2: No replica exists - create new replica (I->S or I->S->M)
     * ======================================================================= */

    // pr_info("[Info]%s: No replica found, original PFN: %lx - creating new replica\n", __func__, fh->original_pfn_val);

    fk = __fault_hash_key(fh->original_pfn_val);

    /* Determine what protocol messages are needed for new replica creation */
    if (write) {
        /* I->M transition: Need both fetch and invalidate */
        // pr_info("[Info]%s: I->M transition: need fetch + invalidate\n", __func__);
        need_invalidate = true;
        // Set FH_STATE_WRITE flag to 0 indicate write state after I->S transition
        spin_lock_irqsave(&faults_lock[fk], flags);
        fh->fh_flags &= ~FH_STATE_WRITE;
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
    } else {
        /* I->S transition: Only need fetch */
        // pr_info("[Info]%s: I->S transition: need fetch only\n", __func__);
        need_invalidate = false;
    }

    start_coherence_transaction = ktime_get();
    /* Send fetch message to other nodes */
    // pr_info("[Info]%s: Broadcasting fetch message for I->S transition\n", __func__);
    ret = broadcast_message_and_wait(SWMC_KMSG_TYPE_FETCH, node_count, 
                                   &payload, cxl_hdm_offset);
    end_coherence_transaction = ktime_get();
    coherence_transaction = ktime_sub(end_coherence_transaction, start_coherence_transaction);
    atomic64_add(ktime_to_ns(coherence_transaction), &page_coherence_total_coherence_transaction_time_ns);    
    if (ret == -EAGAIN) {
        /* Fetch was NACKed - likely due to concurrent write fault elsewhere
            * Retry entire fault handling
            */
        pr_info("[Info]%s: Fetch NACKed - retrying fault handling, original PFN: %lx\n", __func__, fh->original_pfn_val);
        __finish_local_fault_handling(fh);
        msleep(1);
        return VM_FAULT_RETRY;
        // goto redo;
    } else if (ret) {
        __finish_local_fault_handling(fh);
        pr_err("[Err]%s: Failed to fetch page replica, VM_FAULT_RETRY, original PFN: %lx, error: %d\n", __func__, fh->original_pfn_val, ret);
        return VM_FAULT_RETRY;
    }
    
    /* Send invalidate message if needed (for I->M transition) */
    if (need_invalidate) {
        // pr_info("[Info]%s: Broadcasting invalidate message for S->M transition\n", __func__);
        // Set FH_STATE_WRITE flag to 1 indicate write state after I->S transition
        spin_lock_irqsave(&faults_lock[fk], flags);
        fh->fh_flags |= FH_STATE_WRITE;
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        start_coherence_transaction = ktime_get();
        ret = broadcast_message_and_wait(SWMC_KMSG_TYPE_INVALIDATE, node_count, 
                                       &payload, cxl_hdm_offset);
        end_coherence_transaction = ktime_get();
        coherence_transaction = ktime_sub(end_coherence_transaction, start_coherence_transaction);
        atomic64_add(ktime_to_ns(coherence_transaction), &page_coherence_total_coherence_transaction_time_ns);
        if (ret == -EAGAIN) {
            /* Invalidate was NACKed - likely due to concurrent write fault elsewhere
             * Retry entire fault handling
             */
            pr_info("[Info]%s: Invalidate NACKed - retrying fault handling, original PFN: %lx\n", __func__, fh->original_pfn_val);
            __finish_local_fault_handling(fh);
            msleep(1);
            return VM_FAULT_RETRY;
            // goto redo;
        } else if (ret) {
            __finish_local_fault_handling(fh);
            pr_err("[Err]%s: Failed to invalidate page replica, VM_FAULT_RETRY, original PFN: %lx, error: %d\n", __func__, fh->original_pfn_val, ret);
            return VM_FAULT_RETRY;
        }
    }
    
    start_page_replication = ktime_get();
    /* Create replica page using page_replication.c API */
    page_replica = create_page_replica(order, original_pfn, kaddr);
    end_page_replication = ktime_get();
    page_replication = ktime_sub(end_page_replication, start_page_replication);
    atomic64_add(ktime_to_ns(page_replication), &page_coherence_total_page_replication_time_ns);


    if (IS_ERR(page_replica)) {
        ret = PTR_ERR(page_replica);
        pr_info("[Info]%s: Failed to create page replica: %d\n", __func__, ret);
        __finish_local_fault_handling(fh);
        return VM_FAULT_RETRY;
    }
    
    /* Increment replica created counter */
    atomic64_inc(&page_coherence_replica_created_count);

    // map virtual address to page replica
    // pr_info("[Info]%s: Created new page replica: %p\n", __func__, page_replica);
    
map_pfn:
    // pr_info("[Info]%s: Original page: %p, page replica: %p\n", __func__, original_page, page_replica);
    start_metadata_update = ktime_get();
    if (write)
        make_page_replica_dirty(page_replica);

    /* Build new PFN with preserved flags and update */
    replica_pfn.val = page_to_pfn(page_replica) |
                      (original_pfn.val & PFN_FLAGS_MASK);

    // pr_info("[COHERENCE_MAP] PID=%d maps pfn=0x%lx to vaddr=0x%lx (write=%d)\n",
    //     current->pid, pfn_t_to_pfn(replica_pfn), vmf->address, write);
    *pfn = replica_pfn;

    // pr_info("[Info]%s: Replicated pfn=0x%lx at 0x%lx\n",
    //         __func__, pfn_t_to_pfn(replica_pfn), vmf->address);

    /* Finish local fault handling */
    if (__finish_local_fault_handling(fh)) {
        /* If we need to redo, go back to redo label */
        pr_info("[Info]%s: We should redo local fault handling\n", __func__);
        msleep(1);
        return VM_FAULT_RETRY;
        // goto redo;
    }
    end_metadata_update = ktime_get();
    metadata_update = ktime_sub(end_metadata_update, start_metadata_update);
    atomic64_add(ktime_to_ns(metadata_update), &page_coherence_total_metadata_update_time_ns);
    end_page_fault = ktime_get();
    page_fault_latency = ktime_sub(end_page_fault, start_page_fault);
    atomic64_add(ktime_to_ns(page_fault_latency), &page_coherence_total_page_fault_handling_time_ns);
    return 0;
}

/**
 * page_coherence_init - Initialize page coherence subsystem
 *
 * Returns: 0 on success, negative error code on failure
 */
int __init page_coherence_init(void)
{
    int ret;
    int i;
    
    pr_info("[Info]%s: Initializing page coherence subsystem\n", __func__);

    // Initialize fault handle cache
    for (i = 0; i < FAULT_HASH_SIZE; i++) {
        spin_lock_init(&faults_lock[i]);
        INIT_HLIST_HEAD(&faults[i]);
    }
    __fault_handle_cache = kmem_cache_create("fault_handle",
                    sizeof(struct fault_handle),
                    0, SLAB_PANIC, NULL);
    if (!__fault_handle_cache) {
        pr_err("[Err]%s: Failed to create fault handle cache\n", __func__);
        return -ENOMEM;
    }
    
    // Register message handlers
    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_FETCH, swmc_kmsg_handle_fetch_or_invalidate);
    if (ret) {
        pr_err("[Err]%s: Failed to register fetch handler: %d\n", __func__, ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_INVALIDATE, swmc_kmsg_handle_fetch_or_invalidate);
    if (ret) {
        pr_err("[Err]%s: Failed to register invalidate handler: %d\n", __func__, ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_FETCH_ACK, swmc_kmsg_handle_ack_or_nack);
    if (ret) {
        pr_err("[Err]%s: Failed to register fetch_ack handler: %d\n", __func__, ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_FETCH_NACK, swmc_kmsg_handle_ack_or_nack);
    if (ret) {
        pr_err("[Err]%s: Failed to register fetch_ack handler: %d\n", __func__, ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_INVALIDATE_ACK, swmc_kmsg_handle_ack_or_nack);
    if (ret) {
        pr_err("[Err]%s: Failed to register invalidate_ack handler: %d\n", __func__, ret);
        return ret;
    }
    
    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_INVALIDATE_NACK, swmc_kmsg_handle_ack_or_nack);
    if (ret) {
        pr_err("[Err]%s: Failed to register invalidate_ack handler: %d\n", __func__, ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_ERROR, swmc_kmsg_handle_error);
    if (ret) {
        pr_err("[Err]%s: Failed to register error handler: %d\n", __func__, ret);
        return ret;
    }

    /* Create sysfs interface for fault statistics */
    page_coherence_kobj = kobject_create_and_add("swmc", kernel_kobj);
    if (!page_coherence_kobj) {
        pr_err("[Err]%s: Failed to create kobject\n", __func__);
        return -ENOMEM;
    }

    ret = sysfs_create_group(page_coherence_kobj, &page_coherence_attr_group);
    if (ret) {
        pr_err("[Err]%s: Failed to create sysfs group: %d\n", __func__, ret);
        kobject_put(page_coherence_kobj);
        return ret;
    }

    pr_info("[Info]%s: Page coherence subsystem initialized successfully\n", __func__);
    pr_info("[Info]%s: Sysfs interface available at /sys/kernel/swmc/page_coherence/\n", __func__);
    return 0;
}

subsys_initcall(page_coherence_init);

#endif /* CONFIG_PAGE_COHERENCE */
