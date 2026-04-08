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
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/iomap.h>
#include <linux/gfp.h>
#include <linux/printk.h>
#include <linux/memcontrol.h>
#include <linux/hugetlb.h>    /* pfn_pmd, pmd_mkdirty, set_pmd_at */
#include <linux/xarray.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/rmap.h>
#include <linux/pagewalk.h>
#include <linux/mm_inline.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/shmem_fs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/delay.h>
#include <linux/page-flags.h>
#include <swmc/page_coherence.h>
#include <swmc/swmc_kmsg.h>
#include "wait_station.h"
#include <linux/syscalls.h>
#include <linux/ktime.h>
#include <linux/refcount.h>

#ifdef CONFIG_PAGE_COHERENCE

/*
 * Page Coherence Management for CXL Shared Memory
 *
 * This file implements page coherence functionality for managing
 * replica pages in CXL shared memory environments.
 */

// dummy base PA for CXL HDM 
static unsigned long cxl_hdm_base;
pfn_t cxl_hdm_base_pfn;
static int page_coherence_enabled = 0;

SYSCALL_DEFINE0(enable_page_coherence)
{
    page_coherence_enabled = 1;
    pr_info("[Info]%s: Page coherence enabled\n", __func__);
    return 0;
}

SYSCALL_DEFINE0(disable_page_coherence)
{
    page_coherence_enabled = 0;
    pr_info("[Info]%s: Page coherence disabled\n", __func__);
    return 0;
}

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
    cxl_hdm_base_pfn = pfn_to_pfn_t(cxl_hdm_base >> PAGE_SHIFT);
    pr_info("[Info]%s: CXL HDM base address set to 0x%lx\n", __func__, base_addr);
}
EXPORT_SYMBOL(set_cxl_hdm_base);

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
static atomic64_t page_coherence_total_async_transaction_count = ATOMIC64_INIT(0);
static atomic64_t nr_in_flight_transactions = ATOMIC64_INIT(0);

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

static ssize_t total_async_transaction_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", atomic64_read(&page_coherence_total_async_transaction_count));
}

static ssize_t in_flight_transaction_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%lld\n", atomic64_read(&nr_in_flight_transactions));
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
        atomic64_set(&page_coherence_total_async_transaction_count, 0);
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
static struct kobj_attribute total_async_transaction_count_attr = __ATTR_RO(total_async_transaction_count);
static struct kobj_attribute in_flight_transaction_count_attr = __ATTR_RO(in_flight_transaction_count);
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
    &total_async_transaction_count_attr.attr,
    &in_flight_transaction_count_attr.attr,
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

/* Fault handle */
enum fault_handle_state {
    FH_STATE_RETRY = 0x020,
    FH_STATE_REMOTE = 0x010,
    FH_STATE_REPLICATED = 0x08,
    FH_STATE_NEEDWRITE = 0x04,
    FH_STATE_MODIFIED = 0x02,
    FH_STATE_SHARED = 0x01
};

// To serialize concurrent fault handling caused by multiple processes from the same/other nodes processes.
struct fault_handle {
    struct hlist_node list;

    unsigned long original_pfn_val;
    struct page *original_page;
    unsigned long fh_flags;
    unsigned long fh_action_local;
    unsigned long fh_action_remote;
    refcount_t refs;

    struct completion *complete;
};

#define FH_SET_FLAG(name) \
static inline void set_##name(struct fault_handle *fh) \
{ (fh)->fh_flags |= FH_STATE_##name; } \

#define FH_CLEAR_FLAG(name) \
static inline void clear_##name(struct fault_handle *fh) \
{ (fh)->fh_flags &= ~FH_STATE_##name; }

#define FH_IS_FLAG(name) \
static inline bool is_##name(struct fault_handle *fh) \
{ return (fh)->fh_flags & FH_STATE_##name; }

#define FH_INIT_FLAG(name) \
    FH_SET_FLAG(name) \
    FH_CLEAR_FLAG(name) \
    FH_IS_FLAG(name)

FH_INIT_FLAG(RETRY)
FH_INIT_FLAG(REMOTE)
FH_INIT_FLAG(REPLICATED)
FH_INIT_FLAG(NEEDWRITE)
FH_INIT_FLAG(MODIFIED)
FH_INIT_FLAG(SHARED)

#define FH_CLEAR_FLAG_ALL(fh) \
    clear_RETRY(fh); \
    clear_REMOTE(fh); \
    clear_REPLICATED(fh); \
    clear_NEEDWRITE(fh); \
    clear_MODIFIED(fh); \
    clear_SHARED(fh);

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
    fh->original_page = pfn_to_page(pfn);
	fh->fh_flags = 0;
    fh->fh_action_local = 0;
    fh->fh_action_remote = 0;
    refcount_set(&fh->refs, 1); /* list ownership */

    fh->complete = NULL;

	hlist_add_head(&fh->list, &faults[fk]);
	return fh;
}

static inline void fault_handle_get(struct fault_handle *fh)
{
    refcount_inc(&fh->refs);
}

static inline void fault_handle_put(struct fault_handle *fh)
{
    if (refcount_dec_and_test(&fh->refs)) {
        unlock_page(fh->original_page);
        kmem_cache_free(__fault_handle_cache, fh);
    }
}

static inline bool has_lower_priority(struct fault_handle *fh, bool is_write, long remote_acked_fault_count, int remote_node_id, int local_node_id)
{
    bool local_is_write = is_NEEDWRITE(fh);
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


static void check_metadata(struct fault_handle *fh)
{    
    struct page *page_replica;
    if (PageShared(fh->original_page)) {
        set_SHARED(fh);
    } else {
        clear_SHARED(fh);
    }

    if (PageModified(fh->original_page)) {
        set_MODIFIED(fh);
    } else {
        clear_MODIFIED(fh);
    }

    page_replica = get_replica_opt(fh->original_page);
    if (page_replica) {
        set_REPLICATED(fh);
    } else {
        clear_REPLICATED(fh);
    }
}

enum {
    FH_ACTION_NONE = 0x00,
    FH_ACTION_INVALID = 0x01,
    FH_ACTION_UPDATE_METADATA=0x02,
    /* For local fault */
    FH_ACTION_ISSUE_SYNC_TRANSACTION = 0x04,
    FH_ACTION_ISSUE_ASYNC_TRANSACTION = 0x08,
    FH_ACTION_WAIT_FOR_ASYNC_TRANSACTION = 0x10,
    FH_ACTION_MAP_VPN_TO_PFN = 0x20,
    FH_ACTION_FETCH_REPLICA = 0x40,
    /* For remote fault */
    FH_ACTION_WRITEBACK = 0x80,
    FH_ACTION_INVALIDATE = 0x100,
    FH_ACTION_RESPOND = 0x200,
};

#ifdef CONFIG_DE_STIJL
static const unsigned long fh_action_table_local[16] = {

    /*
     * R = replicated
     * W = Write fault
     * M = Modified
     * S = Shared
     * M S means stale Shared
     */
    
    /* Local Fault */
#ifdef CONFIG_DE_STIJL_NO_ASYNC
    /* - - - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA,
    /* - - - S */ FH_ACTION_NONE,
    /* - - M - */ FH_ACTION_NONE,
    /* - - M S */ FH_ACTION_NONE,
    /* - W - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA,
    /* - W - S */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA,
    /* - W M - */ FH_ACTION_NONE,
    /* - W M S */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA,
    /* R - - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN | FH_ACTION_FETCH_REPLICA,
    /* R - - S */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R - M - */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R - M S */ FH_ACTION_INVALID,
    /* R W - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN | FH_ACTION_FETCH_REPLICA,
    /* R W - S */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN,
    /* R W M - */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R W M S */ FH_ACTION_INVALID,
#else
    /* - - - - */ FH_ACTION_ISSUE_ASYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA,
    /* - - - S */ FH_ACTION_NONE,
    /* - - M - */ FH_ACTION_NONE,
    /* - - M S */ FH_ACTION_NONE,
    /* - W - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA,
    /* - W - S */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA,
    /* - W M - */ FH_ACTION_NONE,
    /* - W M S */ FH_ACTION_WAIT_FOR_ASYNC_TRANSACTION | FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA,
    /* R - - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN | FH_ACTION_FETCH_REPLICA,
    /* R - - S */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R - M - */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R - M S */ FH_ACTION_INVALID,
    /* R W - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN | FH_ACTION_FETCH_REPLICA,
    /* R W - S */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN,
    /* R W M - */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R W M S */ FH_ACTION_INVALID,
#endif

};
static const unsigned long fh_action_table_remote[16] = {
    /* - - - - */ FH_ACTION_RESPOND,
    /* - - - S */ FH_ACTION_RESPOND,
    /* - - M - */ FH_ACTION_RESPOND | FH_ACTION_WRITEBACK | FH_ACTION_UPDATE_METADATA,
    /* - - M S */ FH_ACTION_RESPOND,
    /* - W - - */ FH_ACTION_RESPOND,
    /* - W - S */ FH_ACTION_RESPOND | FH_ACTION_INVALIDATE | FH_ACTION_UPDATE_METADATA,
    /* - W M - */ FH_ACTION_RESPOND | FH_ACTION_WRITEBACK | FH_ACTION_INVALIDATE | FH_ACTION_UPDATE_METADATA,
    /* - W M S */ FH_ACTION_RESPOND | FH_ACTION_INVALIDATE | FH_ACTION_UPDATE_METADATA,
    /* R - - - */ FH_ACTION_RESPOND,
    /* R - - S */ FH_ACTION_RESPOND,
    /* R - M - */ FH_ACTION_RESPOND | FH_ACTION_WRITEBACK | FH_ACTION_UPDATE_METADATA,
    /* R - M S */ FH_ACTION_INVALID,
    /* R W - - */ FH_ACTION_RESPOND,
    /* R W - S */ FH_ACTION_RESPOND | FH_ACTION_INVALIDATE | FH_ACTION_UPDATE_METADATA,
    /* R W M - */ FH_ACTION_RESPOND | FH_ACTION_INVALIDATE | FH_ACTION_WRITEBACK | FH_ACTION_UPDATE_METADATA,
    /* R W M S */ FH_ACTION_INVALID,
};
#else
static const unsigned long fh_action_table_local[16] = {

    /*
     * R = replicated
     * W = Write fault
     * M = Modified
     * S = Shared
     * M S means stale Shared
     */
    
    /* Local Fault */
    
    /* - - - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_FETCH_REPLICA | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN,
    /* - - - S */ FH_ACTION_INVALID,
    /* - - M - */ FH_ACTION_INVALID,
    /* - - M S */ FH_ACTION_INVALID,
    /* - W - - */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_FETCH_REPLICA | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN,
    /* - W - S */ FH_ACTION_INVALID,
    /* - W M - */ FH_ACTION_INVALID,
    /* - W M S */ FH_ACTION_INVALID,
    /* R - - - */ FH_ACTION_INVALID,
    /* R - - S */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R - M - */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R - M S */ FH_ACTION_INVALID,
    /* R W - - */ FH_ACTION_INVALID,
    /* R W - S */ FH_ACTION_ISSUE_SYNC_TRANSACTION | FH_ACTION_UPDATE_METADATA | FH_ACTION_MAP_VPN_TO_PFN,
    /* R W M - */ FH_ACTION_MAP_VPN_TO_PFN,
    /* R W M S */ FH_ACTION_INVALID,

};
static const unsigned long fh_action_table_remote[16] = {
    /* - - - - */ FH_ACTION_RESPOND,
    /* - - - S */ FH_ACTION_INVALID,
    /* - - M - */ FH_ACTION_INVALID,
    /* - - M S */ FH_ACTION_INVALID,
    /* - W - - */ FH_ACTION_RESPOND,
    /* - W - S */ FH_ACTION_INVALID,
    /* - W M - */ FH_ACTION_INVALID,
    /* - W M S */ FH_ACTION_INVALID,
    /* R - - - */ FH_ACTION_INVALID,
    /* R - - S */ FH_ACTION_RESPOND,
    /* R - M - */ FH_ACTION_RESPOND | FH_ACTION_WRITEBACK | FH_ACTION_UPDATE_METADATA,
    /* R - M S */ FH_ACTION_INVALID,
    /* R W - - */ FH_ACTION_RESPOND,
    /* R W - S */ FH_ACTION_RESPOND | FH_ACTION_INVALIDATE | FH_ACTION_UPDATE_METADATA,
    /* R W M - */ FH_ACTION_RESPOND | FH_ACTION_INVALIDATE | FH_ACTION_WRITEBACK | FH_ACTION_UPDATE_METADATA,
    /* R W M S */ FH_ACTION_INVALID,
};
#endif

static void set_fh_action_local(struct fault_handle *fh)
{
    unsigned long fh_action;
    unsigned int index;

    index = fh->fh_flags & 0x0F; // Use R/W/M/S bits only

    // pr_info("[Info]%s: Determining action for FH flags=0x%lx (index=%u)\n", __func__, fh->fh_flags, index);

    fh_action = fh_action_table_local[index];
    fh->fh_action_local = fh_action;
}

static void set_fh_action_remote(struct fault_handle *fh)
{
    unsigned long fh_action;
    unsigned int index;

    index = fh->fh_flags & 0x0F; // Use R/W/M/S bits only
    fh_action = fh_action_table_remote[index];
    fh->fh_action_remote = fh_action;
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
static struct fault_handle *__start_local_fault_handling(pfn_t original_pfn, bool is_write)
{
    // pr_info("[Info]%s: Starting local fault handling for pid=%d, pfn=0x%lx, is_write=%s\n", __func__, current->pid, pfn_t_to_pfn(original_pfn), is_write ? "true" : "false");
	
    unsigned long flags;
	struct fault_handle *fh;
	bool found;
    unsigned long original_pfn_val;
    int fk;
    struct page *original_page;
    int retry_count = 0;

    original_pfn_val = pfn_t_to_pfn(original_pfn);
    original_page = pfn_to_page(original_pfn_val);
    found = false;
    fk = __fault_hash_key(original_pfn_val);

retry:
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
        pr_info("[Info]%s: Found existing fault handle for pfn=0x%lx, PID=%d, %s.", __func__, original_pfn_val, current->pid, fh->fh_flags & FH_STATE_REMOTE ? "REMOTE" : "LOCAL");
        fault_handle_get(fh);
        DECLARE_COMPLETION_ONSTACK(complete);
        fh->complete = &complete;

        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        wait_for_completion(&complete);
        pr_info("[Info]%s: Waked up from existing fault handle for pfn=0x%lx, PID=%d\n", __func__, original_pfn_val, current->pid);

        if (is_NEEDWRITE(fh)) {
            pr_info("[Info]%s: Fault handling for pfn=0x%lx needs to be redone to release DAX entry lock\n",
                    __func__, original_pfn_val);
            spin_lock_irqsave(&faults_lock[fk], flags);
            if (!hlist_unhashed(&fh->list)) {
                hlist_del_init(&fh->list);
                fault_handle_put(fh); /* drop list ownership */
            }
            spin_unlock_irqrestore(&faults_lock[fk], flags);
            fault_handle_put(fh);
            return NULL;
        }
        spin_lock_irqsave(&faults_lock[fk], flags);
    } else {
        /* Allocate new fault handle */
        if (!trylock_page(original_page)) {
            spin_unlock_irqrestore(&faults_lock[fk], flags);
            pr_info("[info]%s: Failed to lock page before allocating fault handle for pfn=0x%lx.\n", __func__, original_pfn_val);
            if (retry_count > 3) {
                pr_err("[Err]%s: Exceeded maximum retry count for locking page of pfn=0x%lx. Aborting fault handling.\n", __func__, original_pfn_val);
                return NULL;
            } else {
                retry_count++;
                pr_info("[Info]%s: Retrying to lock page for pfn=0x%lx (retry count: %d)\n", __func__, original_pfn_val, retry_count);
                usleep_range(100, 200); // Sleep for a short time before retrying
                goto retry;
            }
        }
        fh = __alloc_fault_handle(original_pfn_val);   
    }
    
    if (unlikely(!fh)) {
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        pr_err("[Err]%s: Failed to allocate fault handle for pfn=0x%lx.\n", __func__, original_pfn_val);
        return NULL;
    }
    
    /* Clear & update flags */
    FH_CLEAR_FLAG_ALL(fh);
    is_write ? set_NEEDWRITE(fh) : clear_NEEDWRITE(fh);
    check_metadata(fh);
    set_fh_action_local(fh);
    if (!found)
        fault_handle_get(fh);
    
    spin_unlock_irqrestore(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Fault handle action is 0x%lx for pfn=0x%lx\n", __func__, fh->fh_action, original_pfn_val);
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
    bool retry = false;
    int fk = __fault_hash_key(fh->original_pfn_val);

    spin_lock_irqsave(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Acquired lock for fault hash bucket %d\n", __func__, fk);

    /* If this local fault need to be redone, return true */
    if (is_RETRY(fh)) {
        pr_info("[Info]%s: Fault handling for pfn=0x%lx needs to be redone\n",
                __func__, fh->original_pfn_val);
        retry = true;
    }

    // pr_info("[Info]%s: Completed local fault handling for pfn=0x%lx, deleting fault handle.\n", __func__, fh->original_pfn_val);
    if (!hlist_unhashed(&fh->list)) {
        hlist_del_init(&fh->list);
        fault_handle_put(fh); /* drop list ownership */
    }
    spin_unlock_irqrestore(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
    fault_handle_put(fh); /* drop caller ownership */

    return retry;
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
    struct page *original_page;

    original_pfn_val = pfn_t_to_pfn(original_pfn);
    original_page = pfn_to_page(original_pfn_val);
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
        pr_info("[Info]%s: Found existing fault handle for pfn=0x%lx \n",
            __func__, original_pfn_val);
        fault_handle_get(fh);
        if (is_REMOTE(fh)) {
            /* Another remote fault is already being processed */
            spin_unlock_irqrestore(&faults_lock[fk], flags);
            // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
            return NULL;
        }

        if (has_lower_priority(fh, is_write, remote_acked_fault_count, remote_node_id, local_node_id)) {
            spin_unlock_irqrestore(&faults_lock[fk], flags);
            // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
            return NULL;
        }

        /* Local fault is being processed, but remote fault has higher or equal priority */
        if (is_write) {
            set_RETRY(fh);
            set_NEEDWRITE(fh);
        }
        set_fh_action_remote(fh);
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        return fh;
    }

    
    /* Allocate new fault handle for remote processing */
    if (!trylock_page(original_page)) {
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        pr_err("[Err]%s: Failed to lock page before allocating fault handle for pfn=0x%lx.\n", __func__, original_pfn_val);
        return NULL;
    }
    fh = __alloc_fault_handle(original_pfn_val); 
    if (!fh) {
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        pr_err("[Err]%s: Failed to allocate fault handle for pfn=0x%lx.\n", __func__, original_pfn_val);
        return NULL;
    }

    /* Update flags for remote handling */
    FH_CLEAR_FLAG_ALL(fh);
    set_REMOTE(fh); // allocated by remote fault handling
    is_write ? set_NEEDWRITE(fh) : clear_NEEDWRITE(fh);
    check_metadata(fh);
    set_fh_action_remote(fh);
    fault_handle_get(fh);

    // pr_info("[Info]%s: Fault handle action is 0x%lx for pfn=0x%lx\n", __func__, fh->fh_action, original_pfn_val);

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
        pr_info("[Info]%s: There is a local fault waiting for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        complete(fh->complete);
        fh->complete = NULL;
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        fault_handle_put(fh); /* drop caller ownership */
        return false;
    }

    if (is_REMOTE(fh)) {
        // pr_info("[Info]%s: No local fault waiting, deleting fault handle for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        if (!hlist_unhashed(&fh->list)) {
            hlist_del_init(&fh->list);
            fault_handle_put(fh); /* drop list ownership */
        }
        spin_unlock_irqrestore(&faults_lock[fk], flags);
        // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
        fault_handle_put(fh); /* drop caller ownership */
        return true;
    }

    // pr_info("[Info]%s: Completed remote fault handling without freeing fault handle for pfn=0x%lx\n", __func__, fh->original_pfn_val);
    spin_unlock_irqrestore(&faults_lock[fk], flags);
    // pr_info("[Info]%s: Released lock for fault hash bucket %d.\n", __func__, fk);
    fault_handle_put(fh); /* drop caller ownership */

    return false;
}

static int broadcast_message_and_wait(enum swmc_kmsg_type msg_type, pfn_t original_pfn, unsigned int order)
{
    struct wait_station *ws;
    int ret;
    struct payload_data payload;
    int node_count;
    unsigned long cxl_hdm_offset;

    // Get CXL HDM offset for this fault
    cxl_hdm_offset = pfn_t_to_pfn(original_pfn) * PAGE_SIZE - cxl_hdm_base;
    node_count = swmc_kmsg_node_count();

    payload.cxl_hdm_offset = cxl_hdm_offset;
    payload.page_order = order;
    payload.acked_fault_count = atomic64_read(&__local_acked_fault_count);

    // register wait station for this fault
retry_get_ws_bmw:
    ws = get_wait_station_multiple(current, node_count - 1);
    if (!ws) {
        pr_info("[Info]%s: Failed to get wait station\n", __func__);
        msleep(10);
        goto retry_get_ws_bmw;
    }
    
    // broadcast message
retry_broadcast_bmw:
    ret = swmc_kmsg_broadcast(msg_type, ws->id, &payload);
    if (ret) {
        pr_info("[Info]%s: Failed to send %s message: %d\n", __func__, 
               msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate", ret);
        // Continue anyway for now - could implement fallback
        msleep(10);
        goto retry_broadcast_bmw;
    }

    void *wait_result = wait_at_station(ws);
    // pr_info("[Info]%s: Waiting done, received response for %s message\n", __func__,
    //         msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate");
    
    if (wait_result == (void *)-1) {
        // pr_info("[Info]%s: Received NACK for %s message, aborting operation\n", __func__,
        // msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate");
        return -EAGAIN; // Indicate operation should be retried or aborted
    } else if (IS_ERR(wait_result)) {
        ret = PTR_ERR(wait_result);
        // pr_info("[Info]%s: Failed to wait at station: %d\n", __func__, ret);
        return ret;
    }

    return 0;
}

static struct wait_station *broadcast_message(enum swmc_kmsg_type msg_type, pfn_t original_pfn, unsigned int order)
{
    struct wait_station *ws;
    int ret;
    struct payload_data payload;
    int node_count;
    unsigned long cxl_hdm_offset;

    // Get CXL HDM offset for this fault
    cxl_hdm_offset = pfn_t_to_pfn(original_pfn) * PAGE_SIZE - cxl_hdm_base;
    node_count = swmc_kmsg_node_count();

    payload.cxl_hdm_offset = cxl_hdm_offset;
    payload.page_order = order;
    payload.acked_fault_count = atomic64_read(&__local_acked_fault_count);

retry_get_ws_bm:
    // register wait station for this fault
    ws = get_wait_station_multiple(current, node_count - 1);
    if (!ws) {
        pr_info("[Info]%s: Failed to get wait station\n", __func__);
        msleep(10);
        goto retry_get_ws_bm;
    }
    
    // broadcast message
retry_broadcast_bm:
    ret = swmc_kmsg_broadcast(msg_type, ws->id, &payload);
    if (ret) {
        pr_info("[Info]%s: Failed to send %s message: %d\n", __func__, 
               msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate", ret);
        msleep(10);
        goto retry_broadcast_bm;
    }

    return ws;
}

static void wait_for_async_transaction_completion(struct fault_handle *fh)
{
    // TODO: Implement actual wait logic using wait stations or completions
    msleep(100); // Simulate wait for async transaction completion
}

// 여기서 transaction을 완료하고, coherence를 맞추기 위한 cache나 page에 대한 flush, fetch 등의 작업까지 담당한다.
static int issue_page_coherence_transaction(struct fault_handle *fh, void *kaddr)
{
    int ret = 0;
    
    /* broadcast fetch/invalidate message and wait for ACKs */
    // Get Shared
    if (!is_NEEDWRITE(fh)) {
        ret = broadcast_message_and_wait(SWMC_KMSG_TYPE_FETCH, pfn_to_pfn_t(fh->original_pfn_val), 0);
        // pr_info("[Info]%s: Issuing GetS transaction for pfn=0x%lx\n", __func__, fh->original_pfn_val);
    } else { // Get Modified
        ret = broadcast_message_and_wait(SWMC_KMSG_TYPE_INVALIDATE, pfn_to_pfn_t(fh->original_pfn_val), 0);
        // pr_info("[Info]%s: Issuing GetM/Upgrade transaction for pfn=0x%lx\n", __func__, fh->original_pfn_val);
    }

    // if NACK recieved, return -EAGAIN to indicate retry is needed
    if (ret == -EAGAIN) {
        pr_info("[Info]%s: Transaction for pfn=0x%lx needs to be retried due to NACK\n", __func__, fh->original_pfn_val);
        return -EAGAIN;
    } else if (ret) {
        pr_err("[Err]%s: Transaction for pfn=0x%lx failed with error %d\n", __func__, fh->original_pfn_val, ret);
        return ret;
    }

    return 0;
}


static int issue_page_coherence_transaction_async(struct fault_handle *fh)
{   
    struct wait_station *ws;

    //broadcast fetch message without waiting for ACKs
    ws = broadcast_message(SWMC_KMSG_TYPE_FETCH, pfn_to_pfn_t(fh->original_pfn_val), 0);
    if (!ws) {
        pr_err("[Err]%s: Failed to broadcast fetch message for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        return -ENOMEM;
    }

    ws->async_page = fh->original_page;

    return 0;
}

static void update_metadata(struct fault_handle *fh, bool is_async)
{
    // if replicated or not, same action.
    if (is_REMOTE(fh)) {
        if (is_NEEDWRITE(fh)) { // invalidation
            ClearPageModified(fh->original_page);
            ClearPageShared(fh->original_page);
        } else { // downgrade from M to S
            SetPageShared(fh->original_page);
            ClearPageModified(fh->original_page);
        }
    } else {
        if (is_NEEDWRITE(fh)) {
            SetPageModified(fh->original_page);
            ClearPageShared(fh->original_page);
        } else { // Shared state
#ifdef CONFIG_DE_STIJL_NO_ASYNC
            SetPageShared(fh->original_page);
            ClearPageModified(fh->original_page);
#else
            if (is_async) { // stale shared
                SetPageShared(fh->original_page);
                SetPageModified(fh->original_page);
            } else { // fresh shared
                SetPageShared(fh->original_page);
                ClearPageModified(fh->original_page);
            }
#endif
        }
    }
    // print_page_info(fh->original_page, "update_metadata");
}

static void map_vpn_to_pfn(struct fault_handle *fh, pfn_t *pfn)
{
    pfn_t pfn_to_map;
    pfn_t original_pfn = *pfn;
    struct page *page_replica;

    page_replica = get_replica_opt(fh->original_page);
    pfn_to_map.val = page_to_pfn(page_replica) | 
                        (original_pfn.val & PFN_FLAGS_MASK);
    *pfn = pfn_to_map;
    // pr_info("[Info]%s: page_to_pfn(page_replica)=0x%lx, original_pfn.val & PFN_FLAGS_MASK=0x%llx\n", __func__, page_to_pfn(page_replica), original_pfn.val & PFN_FLAGS_MASK);
    // pr_info("[Info]%s: Mapped VPN to PFN=0x%lx for original_pfn=0x%lx\n", __func__, pfn_t_to_pfn(pfn_to_map), pfn_t_to_pfn(original_pfn));
}

// Ring buffer to handle async transaction completions
#define ASYNC_TRANSACTION_RING_SIZE 65536 // This size should match message ring buffer.
struct async_transaction_work {
    struct page *original_page;
    bool nacked;
};

static struct async_transaction_work async_transaction_workqueue[ASYNC_TRANSACTION_RING_SIZE];
atomic_t async_transaction_workqueue_head = ATOMIC_INIT(0);
atomic_t async_transaction_workqueue_tail = ATOMIC_INIT(0);

static void async_transaction_daemon(void)
{
    pr_info("[Info]%s: Async transaction daemon started\n", __func__);
    while (!kthread_should_stop()) {
        int head = atomic_read(&async_transaction_workqueue_head);
        int tail = atomic_read(&async_transaction_workqueue_tail);

        if (head != tail) {
            struct async_transaction_work *work = &async_transaction_workqueue[tail % ASYNC_TRANSACTION_RING_SIZE];
            struct page *work_page = work->original_page;
            if (work->nacked) {
                // TODO: resend fetch message
            }
            // Process completion
            
            // flush cache lines of the page to eliminate stale data in CPU caches
            volatile char *kaddr = kmap(work_page);
            for (int i = 0; i < PAGE_SIZE; i += CL_SIZE) {
                clflush((volatile void *)&kaddr[i]);
            }
            kunmap(work_page);
            
            // clear modified flag to change state from Shared stale to Shared
            ClearPageModified(work_page);
            
            // Decrement in flight transactions number
            atomic64_dec(&nr_in_flight_transactions);
            
            // Advance tail
            atomic_inc(&async_transaction_workqueue_tail);

            // pr_info("[Info]%s: Processed async transaction completion for original_pfn=0x%lx, queue size: %d\n", __func__, page_to_pfn(work_page), (atomic_read(&async_transaction_workqueue_head) - atomic_read(&async_transaction_workqueue_tail) + ASYNC_TRANSACTION_RING_SIZE) % ASYNC_TRANSACTION_RING_SIZE);
        } else {
            // Sleep for a while
            msleep(1);
        }
    }
}

static void put_work_to_workqueue(struct page *async_page, struct wait_station *ws)
{
    int head = atomic_read(&async_transaction_workqueue_head);
    int tail = atomic_read(&async_transaction_workqueue_tail);

    if (((head + 1) % ASYNC_TRANSACTION_RING_SIZE) == (tail % ASYNC_TRANSACTION_RING_SIZE)) {
        pr_err("[Err]%s: Async transaction workqueue is full, dropping work for page with original_pfn=0x%lx\n", __func__, page_to_pfn(async_page));
        return;
    }

    struct async_transaction_work *work = &async_transaction_workqueue[head % ASYNC_TRANSACTION_RING_SIZE];
    work->original_page = async_page;
    if (ws->private == (void * )-1) {
        work->nacked = true;
    } else {
        work->nacked = false;
    }

    atomic_inc(&async_transaction_workqueue_head);
    // pr_info("[Info]%s: Added async transaction work for original_pfn=0x%lx to workqueue\n", __func__, page_to_pfn(async_page));

    put_wait_station(ws);
}

static void writeback_page(struct fault_handle *fh)
{
    if (is_REPLICATED(fh)) {
        struct page *page_replica = get_replica_opt(fh->original_page);
        writeback_page_replica(page_replica);
    } else {
        // For non-replicated page, just flush cache lines and clear modified flag
        volatile char *kaddr = kmap(fh->original_page);
        for (int i = 0; i < PAGE_SIZE; i += CL_SIZE) {
            clflush((volatile void *)&kaddr[i]);
        }
        kunmap(fh->original_page);
    }
    struct vm_area_struct *vma;
    unsigned long pfn_to_clean;
    struct address_space *mapping;
    unsigned long index, end;
    struct page *page_replica = NULL;
    if (is_REPLICATED(fh)) {
        page_replica = get_replica_opt(fh->original_page);
        pfn_to_clean = page_to_pfn(page_replica);
        index = page_replica->index;
        mapping = page_replica->mapping;
    } else {
        pfn_to_clean = fh->original_pfn_val;
        index = fh->original_page->index;
        mapping = fh->original_page->mapping;
    }
    end = index + 1;
    i_mmap_lock_read(mapping);
    
    vma_interval_tree_foreach(vma, &mapping->i_mmap, index, end) {
        pfn_mkclean_range(pfn_to_clean, 1, index, vma);
        cond_resched();
    }
    i_mmap_unlock_read(mapping);
}

static void invalidate_page(struct fault_handle *fh)
{
    struct address_space *mapping;
    unsigned long index;
    struct page *page_replica = NULL;
    if (is_REPLICATED(fh)) {
        page_replica = get_replica_opt(fh->original_page);
        index = page_replica->index;
        mapping = page_replica->mapping;
    } else {
        index = fh->original_page->index;
        mapping = fh->original_page->mapping;
    }

    // TODO: 여기 앞뒤로 dax folio에 대한 lock 없어도 제대로 동작하는지 확인 필요함.
#ifdef CONFIG_DE_STIJL
    if (!mapping) {
        pr_err("[Error]%s: No mapping found for page replica of pfn=0x%lx\n", __func__, fh->original_pfn_val);
        return;
    }
    unmap_mapping_pages(mapping, index, 1, false);
#else
    int ret = flush_page_replica_locked(page_replica);
    if (ret) {
        pr_err("[Error]%s: Failed to flush page replica for pfn=0x%lx\n", __func__, fh->original_pfn_val);
    }
#endif
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
    long remote_acked_fault_count = payload->acked_fault_count;
    bool is_write = (msg->header.type == SWMC_KMSG_TYPE_INVALIDATE);

    fh = __start_remote_fault_handling(original_pfn, is_write, remote_acked_fault_count, msg->header.from_nid, msg->header.to_nid);
    
    if (!fh) {
        // pr_info("[Info]%s: NACK remote fault handling\n", __func__);
        ret = swmc_kmsg_unicast((is_write ? SWMC_KMSG_TYPE_INVALIDATE_NACK : SWMC_KMSG_TYPE_FETCH_NACK), msg->header.ws_id, msg->header.from_nid, payload);
        return ret;
    }

    if (!fh->fh_action_remote) {
        pr_err("[Error]%s: Invalid fault handle action for pfn=0x%lx\n", __func__, pfn_t_to_pfn(original_pfn));
        ret = swmc_kmsg_unicast((is_write ? SWMC_KMSG_TYPE_INVALIDATE_ACK : SWMC_KMSG_TYPE_FETCH_ACK), msg->header.ws_id, msg->header.from_nid, payload);
        // Clean up fault handle
        __finish_remote_fault_handling(fh);
        return ret;
    }

    if (fh->fh_action_remote & FH_ACTION_WRITEBACK) {
        // pr_info("[Info]%s: Fault action includes WRITEBACK for pfn=0x%lx\n", __func__, pfn_t_to_pfn(original_pfn));
        writeback_page(fh);
    }

    if (fh->fh_action_remote & FH_ACTION_INVALIDATE) {
        // pr_info("[Info]%s: Fault action includes INVALIDATE for pfn=0x%lx\n", __func__, pfn_t_to_pfn(original_pfn));
        invalidate_page(fh);
    }

    if (fh->fh_action_remote & FH_ACTION_UPDATE_METADATA) {
        // pr_info("[Info]%s: Fault action includes UPDATE_METADATA for pfn=0x%lx\n", __func__, pfn_t_to_pfn(original_pfn));
        update_metadata(fh, false);
    }

    // pr_info("[Info]%s: ACK remote fault handling\n", __func__);
    ret = swmc_kmsg_unicast((is_write ? SWMC_KMSG_TYPE_INVALIDATE_ACK : SWMC_KMSG_TYPE_FETCH_ACK), msg->header.ws_id, msg->header.from_nid, payload);

    __finish_remote_fault_handling(fh);

    return 0;
}

// Invalidate_ack message handling
static int swmc_kmsg_handle_ack_or_nack(struct swmc_kmsg_message *msg)
{

    // pr_info("[Info]%s: Handling ACK/NACK message for offset 0x%lx\n", __func__, msg->payload.cxl_hdm_offset);

    // find the wait station by ID
    struct wait_station *ws = wait_station(msg->header.ws_id);
    if (!ws) {
        pr_err("[Err]%s: Invalid wait station ID: %d\n", __func__, msg->header.ws_id);
        return -EINVAL;
    }

    if (msg->header.type == SWMC_KMSG_TYPE_INVALIDATE_NACK || msg->header.type == SWMC_KMSG_TYPE_FETCH_NACK) {
        pr_info("[Info]%s: Received NACK for wait station %d\n", __func__, msg->header.ws_id);
        ws->private = (void *)-1; // Indicate NACK received
    }

    // Decrease pending count atomically
    if (atomic_dec_and_test(&ws->pendings_count)) {
        // All invalidate ACKs received, wake up the wait station
        // pr_info("[Info]%s: All ACKs/NACKs received for wait station %d\n", __func__, msg->header.ws_id);
        atomic64_inc(&__local_acked_fault_count); // Increment remote ACK count
        if (ws->async_page) {
            // Decrement in-flight transaction will be handled by async transaction daemon.
            put_work_to_workqueue(ws->async_page, ws); // put work to workqueue for daemon to complete async transaction
        } else {
            atomic64_dec(&nr_in_flight_transactions); // Decrement in-flight transaction count
            complete(&ws->pendings);
        }
    } else {
        pr_info("[Info]%s: ACK/NACK received, pending count: %d\n", __func__,
                atomic_read(&ws->pendings_count));
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

    return 0;
}


/* =============================================================================
 * PAGE COHERENCE FAULT HANDLING
 * ============================================================================= */


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
    // declaration
    struct fault_handle *fh;
    pfn_t original_pfn;
    int ret;
    bool write;
    struct file *file;
    const char *filename;

    /* Initialization */
    original_pfn = *pfn;
    write = iter->flags & IOMAP_WRITE;
    file = vmf->vma->vm_file;
    filename = file->f_path.dentry->d_name.name;

    /* Early return conditions */
    if (pfn_t_to_pfn(original_pfn) < pfn_t_to_pfn(cxl_hdm_base_pfn)) {
        pr_info("[Info]%s: Not a CXL HDM fault, skipping page coherence handling\n", __func__);
        return 0;
    }
    if (strstr(filename, ".log")) {
        pr_info("[Info]%s: .log Meta file access, skipping page coherence handling for %s\n", __func__, filename);
        return 0; 
    }
    if (strstr(filename, ".superblock")) {
        pr_info("[Info]%s: .superblock Meta file access, skipping page coherence handling for %s\n", __func__, filename);
        return 0; 
    }

    if (page_coherence_enabled == 0) {
        // pr_info("[Info]%s: Page coherence handling is disabled, skipping\n", __func__);
        // pr_info("[Info]%s: Page coherence handling is disabled, check every accessed paged in CXL shared memory to Shared State without coherence transaction issuing\n", __func__);
#ifdef CONFIG_DE_STIJL
        SetPageCoherence(pfn_to_page(pfn_t_to_pfn(original_pfn)));
        SetPageShared(pfn_to_page(pfn_t_to_pfn(original_pfn)));
        ClearPageModified(pfn_to_page(pfn_t_to_pfn(original_pfn)));
#else
        // /* Check Metadata & get fault handle */
        // fh = __start_local_fault_handling(original_pfn, write);
        // SetPageCoherence(fh->original_page);
        // if (fh->fh_action & FH_ACTION_FETCH_REPLICA) {
        //     // pr_info("[Info]%s: Fetching page replica for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        //     ret = create_page_replica(fh->original_page, 0);
        //     if (ret) {
        //         pr_err("[Err]%s: Failed to fetch page replica for pfn=0x%lx, error %d\n", __func__, fh->original_pfn_val, ret);
        //         __finish_local_fault_handling(fh);
        //         return ret;
        //     }
        // }
        // /* Update metadata */
        // if (fh->fh_action & FH_ACTION_UPDATE_METADATA) {
        //     // pr_info("[Info]%s: Updating metadata for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        //     update_metadata(fh);
        // }
        
        // // pr_info("[Info]%s: Mapping PFN for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        // /* Map VPN to PFN */
        // if (fh->fh_action & FH_ACTION_MAP_VPN_TO_PFN) {
        //     // pr_info("[Info]%s: Mapping VPN to replica PFN for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        //     map_vpn_to_pfn(fh, pfn);
        // }
        
        // /* Finish local fault handling */
        // if (__finish_local_fault_handling(fh)) {
        //     pr_info("[Info]%s: We should retry local fault handling\n", __func__);
        //     msleep(1);
        //     return VM_FAULT_RETRY;
        // }
#endif
        return 0;
    }


    /* 
     * For statistics
     */
    /* Increment fault counters */
    atomic64_inc(&page_coherence_fault_count);
    if (write) {
        atomic64_inc(&page_coherence_fault_write_count);
    } else {
        atomic64_inc(&page_coherence_fault_read_count);
    }
    /* start timestamp */
    ktime_t start_page_fault, start_async_transaction_wait, start_coherence_transaction, start_page_replication, start_metadata_update, end_page_fault, async_transaction_wait, coherence_transaction, page_replication, metadata_update, page_fault_latency;
    start_page_fault = ktime_get();

    /* Check Metadata & get fault handle */
    fh = __start_local_fault_handling(original_pfn, write);

    // pr_info("[Info]%s: PG_coherence = %d for original_page=%p\n", __func__, PageCoherence(fh->original_page), fh->original_page);

    if (!fh) {
        pr_info("[Info]%s: Need to retry local fault handling for pfn=0x%llx\n", __func__, original_pfn.val);
        // msleep(1);
        return VM_FAULT_RETRY;
    }

    if (fh->fh_action_local == FH_ACTION_INVALID) {
        pr_err("[Err]%s: Invalid fault action for local fault\n", __func__);
        __finish_local_fault_handling(fh);
        return -EINVAL;
    }

    // print_page_info(fh->original_page, "__page_coherence_fault: after __start_local_fault_handling");

    SetPageCoherence(fh->original_page);

    start_async_transaction_wait = ktime_get();
    if (fh->fh_action_local & FH_ACTION_WAIT_FOR_ASYNC_TRANSACTION) {
        // pr_info("[Info]%s: Waiting for async transaction completion for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        wait_for_async_transaction_completion(fh);
    }

    int nr_ift = atomic64_read(&nr_in_flight_transactions);

    /* Issue Transaction */
    bool is_async = false;
    // if (nr_ift > WAIT_STATION_THRESHOLD) { // for debugging
    //     pr_info("[Info]%s: Number of in-flight transactions (%lld) exceeds threshold, issuing synchronous transaction for pfn=0x%lx\n", __func__, nr_ift, fh->original_pfn_val);
    // }
    start_coherence_transaction = ktime_get();
    // Synchronous transaction if requested or if over threshold
    if (fh->fh_action_local & FH_ACTION_ISSUE_SYNC_TRANSACTION || nr_ift > WAIT_STATION_THRESHOLD) {
        // pr_info("[Info]%s: Issuing synchronous page coherence transaction for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        ret = issue_page_coherence_transaction(fh, kaddr);
        if (ret) {
            pr_err("[Err]%s: Failed to issue page coherence transaction\n", __func__);
            __finish_local_fault_handling(fh);
            return ret;
        }
        atomic64_inc(&nr_in_flight_transactions);
    } else if (fh->fh_action_local & FH_ACTION_ISSUE_ASYNC_TRANSACTION) {
        is_async = true;
        // pr_info("[Info]%s: Issuing asynchronous page coherence transaction for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        ret = issue_page_coherence_transaction_async(fh);
        if (ret) {
            pr_err("[Err]%s: Failed to issue async page coherence transaction\n", __func__);
            __finish_local_fault_handling(fh);
            return ret;
        }
        atomic64_inc(&nr_in_flight_transactions);
    }

    start_page_replication = ktime_get();
    if (fh->fh_action_local & FH_ACTION_FETCH_REPLICA) {
        // pr_info("[Info]%s: Fetching page replica for pfn=0x%lx\n", __func__, fh->original_pfn_val);
#ifdef CONFIG_DE_STIJL
        ret = fetch_page_replica(fh->original_page);
#else
        ret = create_page_replica(fh->original_page, 0);
#endif
        if (ret) {
            pr_err("[Err]%s: Failed to fetch page replica for pfn=0x%lx, error %d\n", __func__, fh->original_pfn_val, ret);
            __finish_local_fault_handling(fh);
            return ret;
        }
    }

    /* Update metadata */
    start_metadata_update = ktime_get();
    if (fh->fh_action_local & FH_ACTION_UPDATE_METADATA) {
        // pr_info("[Info]%s: Updating metadata for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        update_metadata(fh, is_async);
    }

    // pr_info("[Info]%s: Mapping PFN for pfn=0x%lx\n", __func__, fh->original_pfn_val);
    /* Map VPN to PFN */
    if (fh->fh_action_local & FH_ACTION_MAP_VPN_TO_PFN) {
        // pr_info("[Info]%s: Mapping VPN to PFN for pfn=0x%lx\n", __func__, fh->original_pfn_val);
        map_vpn_to_pfn(fh, pfn);
    }

    /* Finish local fault handling */
    if (__finish_local_fault_handling(fh)) {
        pr_info("[Info]%s: We should retry local fault handling\n", __func__);
        // msleep(1);
        return VM_FAULT_RETRY;
    }

    // pr_info("[Info]%s: Page coherence fault handling completed successfully for pfn=0x%lx, mapped pfn=0x%lx\n", __func__, original_pfn.val, pfn_t_to_pfn(*pfn));
    /* end timestamp and update total handling time */
    end_page_fault = ktime_get();
    async_transaction_wait = ktime_sub(start_coherence_transaction, start_async_transaction_wait);
    coherence_transaction = ktime_sub(start_page_replication, start_coherence_transaction);
    page_replication = ktime_sub(start_metadata_update, start_page_replication);
    metadata_update = ktime_sub(end_page_fault, start_metadata_update);
    page_fault_latency = ktime_sub(end_page_fault, start_page_fault);
    atomic64_add(ktime_to_ns(async_transaction_wait), &page_coherence_total_async_transaction_wait_time_ns);
    atomic64_add(ktime_to_ns(coherence_transaction), &page_coherence_total_coherence_transaction_time_ns);
    atomic64_add(ktime_to_ns(page_replication), &page_coherence_total_page_replication_time_ns);
    atomic64_add(ktime_to_ns(metadata_update), &page_coherence_total_metadata_update_time_ns);
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
    
    cxl_hdm_base = 0x1e80000000; // Default value, can be set by external module
    cxl_hdm_base_pfn = pfn_to_pfn_t(cxl_hdm_base >> PAGE_SHIFT);

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

    // Start async transaction daemon
    struct task_struct *tsk;
    tsk = kthread_run((int (*)(void *))async_transaction_daemon, NULL, "async_transaction_daemon");
    if (IS_ERR(tsk)) {
        pr_err("[Err]%s: Failed to create async transaction daemon thread\n", __func__);
        return PTR_ERR(tsk);
    }
    pr_info("[Info]%s: Started async transaction daemon thread %s\n", __func__, tsk->comm);

    pr_info("[Err]%s: Page coherence subsystem initialized successfully\n", __func__);
    pr_info("[Err]%s: Sysfs interface available at /sys/kernel/swmc/page_coherence/\n", __func__);
    return 0;
}

subsys_initcall(page_coherence_init);

#endif /* CONFIG_PAGE_COHERENCE */
