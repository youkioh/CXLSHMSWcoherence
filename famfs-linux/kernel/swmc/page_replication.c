// SPDX-License-Identifier: GPL-2.0

#include <linux/list.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/xarray.h>
#include <linux/jiffies.h>
#include <linux/mm_types.h>
#include <linux/pagemap.h>
#include <linux/ratelimit.h>
#include <linux/shrinker.h>
#include <linux/mmu_notifier.h>
#include <linux/delay.h>
#include <linux/refcount.h>
#include <swmc/page_coherence.h>
#include <swmc/page_replication_info.h>
#include <linux/mmdebug.h>
#include <linux/pagewalk.h>
#include <linux/dax.h>
#include <asm/cacheflush.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>

/* =============================================================================
 * SYSFS INTERFACE FOR PAGE REPLICATION STATISTICS
 * ============================================================================= */

/* Page allocation tracking */
static atomic64_t page_replica_allocated_pages = ATOMIC64_INIT(0);

static ssize_t allocated_pages_show(struct kobject *kobj, 
                                   struct kobj_attribute *attr, 
                                   char *buf)
{
    return sprintf(buf, "%lld\n", atomic64_read(&page_replica_allocated_pages));
}

static struct kobj_attribute allocated_pages_attribute = 
    __ATTR(allocated_pages, 0444, allocated_pages_show, NULL);

static struct attribute *page_replica_attrs[] = {
    &allocated_pages_attribute.attr,
    NULL,
};

static struct attribute_group page_replica_attr_group = {
    .attrs = page_replica_attrs,
};

static struct kobject *page_replica_kobj;

static int __init page_replica_sysfs_init(void)
{
    int ret;
    
    page_replica_kobj = kobject_create_and_add("page_replica", kernel_kobj);
    if (!page_replica_kobj)
        return -ENOMEM;
    
    ret = sysfs_create_group(page_replica_kobj, &page_replica_attr_group);
    if (ret) {
        kobject_put(page_replica_kobj);
        return ret;
    }
    
    pr_info("[%s] page_replica sysfs initialized\n", __func__);
    return 0;
}

static void __exit page_replica_sysfs_exit(void)
{
    if (page_replica_kobj) {
        sysfs_remove_group(page_replica_kobj, &page_replica_attr_group);
        kobject_put(page_replica_kobj);
    }
}

/* Helper functions to track page allocation/deallocation */
static inline void track_page_alloc(unsigned int order)
{
    long pages = 1L << order;
    atomic64_add(pages, &page_replica_allocated_pages);
    pr_debug("[%s] Allocated 2^%u = %ld pages, total: %lld\n", 
             __func__, order, pages, atomic64_read(&page_replica_allocated_pages));
}

static inline void track_page_free(unsigned int order)
{
    long pages = 1L << order;
    atomic64_sub(pages, &page_replica_allocated_pages);
    pr_debug("[%s] Freed 2^%u = %ld pages, total: %lld\n", 
             __func__, order, pages, atomic64_read(&page_replica_allocated_pages));
}




void print_page_info(struct page *page, const char *context)
{
    pr_info("[%s] page_info in '%s': page=%p, flags=0x%lx, mapping=%p, index=%lu, refcount=%d\n",
            __func__, context, page, page->flags, page->mapping, page->index,
            atomic_read(&page->_refcount));
    pr_info("[%s] more info with flags: PG_head=%d, PG_dirty=%d, PG_writeback=%d, PG_locked=%d\n",
            __func__, PageHead(page), PageDirty(page), PageWriteback(page), PageLocked(page));
    //print raw dump of fields of struct page with hex code
    
    pr_info("[%s] page[%d-%d]: %lx %lx %lx %lx\n", __func__, 0, 3, ((unsigned long *)page)[0], ((unsigned long *)page)[1], ((unsigned long *)page)[2], ((unsigned long *)page)[3]);
    pr_info("[%s] page[%d-%d]: %lx %lx %lx %lx\n", __func__, 4, 7, ((unsigned long *)page)[4], ((unsigned long *)page)[5], ((unsigned long *)page)[6], ((unsigned long *)page)[7]);
}
EXPORT_SYMBOL(print_page_info);

static LIST_HEAD(replica_active_lru);
static LIST_HEAD(replica_inactive_lru);
static DEFINE_SPINLOCK(replica_lru_lock);
static DEFINE_XARRAY(replica_meta_xa);

/* XArrays for page mapping */
static DEFINE_XARRAY(original_to_replica_xa);
static DEFINE_XARRAY(replica_to_original_xa);

/* Constants for replica management */
#define MAX_ALLOCATE_RETRIES             3
#define REPLICA_DEFAULT_SCAN_PAGES      1024
#define REPLICA_INACTIVE_THRESHOLD_MULT 2
#define REPLICA_AGING_MULT              4
#define REPLICA_ACTIVE_TO_INACTIVE_RATIO 4  /* 1/4 of active pages count for shrinking */
#define REPLICA_MAX_LIST_COUNT          (1UL << 20)
#define CL_SIZE                        64 /* Cache line size for flushing */



static int print_replica_error(enum replica_error err)
{
    switch (err) {
        case REPLICA_SUCCESS:
            pr_info("[replica] Operation succeeded\n");
            break;
        case REPLICA_SHARED_STATE:
            pr_info("[replica] Shared state detected, operation skipped\n");
            break;
        case REPLICA_ERROR_NOMEM:
            pr_err("[replica] Memory allocation failed\n");
            break;
        case REPLICA_ERROR_INVAL:
            pr_err("[replica] Invalid parameters\n");
            break;
        case REPLICA_ERROR_EXIST:
            pr_info("[replica] Replica already exists\n");
            break;
        case REPLICA_ERROR_NOENT:
            pr_info("[replica] Replica not found\n");
            break;
        case REPLICA_ERROR_LOCK:
            pr_err("[replica] Failed to acquire lock\n");
            break;
        case REPLICA_ERROR_ANY:
            pr_err("[replica] Replica error any\n");
            break;
        default:
            pr_err("[replica] Unknown error code: %d\n", err);
    }
    return err;
}

/* ========================================================================
 * Walk page mapping helpers
 * ======================================================================== */
static int pte_entry_young_and_clear(pte_t *pte, unsigned long addr, unsigned long next, struct mm_walk *walk)
{
    unsigned long *reference_count = walk->private;
    
    pr_info("[%s] VMA: %p, addr: 0x%lx, next: 0x%lx, PTE: 0x%lx\n",
            __func__, walk->vma, addr, next, pte_val(*pte));
    
    if (ptep_test_and_clear_young(walk->vma, addr, pte)) {
        pr_info("-> Young: Yes\n");
        ++(*reference_count);
    }

    return 0;
}

static int pmd_entry_young_and_clear(pmd_t *pmd, unsigned long addr, unsigned long next, struct mm_walk *walk)
{
    unsigned long *reference_count = walk->private;
    
    pr_info("[%s] VMA: %p, addr: 0x%lx, next: 0x%lx, PMD: 0x%lx\n",
            __func__, walk->vma, addr, next, pmd_val(*pmd));
    if (pmd_trans_huge(*pmd) || pmd_devmap(*pmd)) {
        pr_info("[%s] THP/Devmap PMD: 0x%lx\n", __func__, pmd_val(*pmd));
        
        if (pmdp_test_and_clear_young(walk->vma, addr, pmd)) {
            pr_info("-> Young: Yes\n");
            ++(*reference_count);
        }
        return 1;
    }

    return 0;
}

static const struct mm_walk_ops young_and_clear_ops = {
    .pte_entry = pte_entry_young_and_clear,
    .pmd_entry = pmd_entry_young_and_clear,
};

/* ========================================================================
 * page replica meta management
 * ======================================================================== */

struct page_replica_meta {
    struct page *page;
    unsigned int order;
    struct list_head lru;
    refcount_t refcount;
    bool on_lru;
    bool dirty;
    bool invalidated;
};

/* Helper to get meta with reference under lock */
static struct page_replica_meta *get_page_replica_meta(struct page *page_replica)
{
    struct page_replica_meta *meta;
    unsigned long flags;
    
    spin_lock_irqsave(&replica_lru_lock, flags);
    meta = xa_load(&replica_meta_xa, (unsigned long)page_replica);
    if (meta && refcount_inc_not_zero(&meta->refcount)) {
        spin_unlock_irqrestore(&replica_lru_lock, flags);
        // pr_info("[%s] Found page replica meta: page=%p, order=%u, refcount=%d\n",
        //         __func__, meta->page, meta->order, refcount_read(&meta->refcount));
        return meta;
    }
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    // pr_info("[%s] No meta for page=%p\n", __func__, page_replica);
    return NULL;
}

/* Reference counting helpers for safe lock dropping */
static void put_page_replica_meta(struct page_replica_meta *meta)
{
    // pr_info("[%s] put_page_replica_meta: page=%p, order=%u, refcount=%d\n",
    //         __func__, meta->page, meta->order, refcount_read(&meta->refcount));
    if (refcount_dec_and_test(&meta->refcount)){
        pr_info("[%s] Freeing page replica meta: page=%p, order=%u, refcount=%d\n",
                __func__, meta->page, meta->order, refcount_read(&meta->refcount));
        kfree(meta);
    }
}

int make_page_replica_invalid(struct page *page_replica)
{
    struct address_space *mapping = page_replica->mapping;
    unsigned long index = page_replica->index;
    struct page_replica_meta *meta = get_page_replica_meta(page_replica);
    unsigned int order;

    if (!meta) {
        pr_info("[%s] No meta found for page=%p, cannot mark invalidated\n", __func__, page_replica);
        return -ENOENT;
    }

    unmap_mapping_pages(mapping, index, 1 << order, false);

    meta->invalidated = true;
    order = meta->order;
    meta->dirty = false; // clear dirty flag
    put_page_replica_meta(meta);
    pr_info("[%s] Page replica invalidated: page=%p\n", __func__, page_replica);

    return 0;
}
EXPORT_SYMBOL(make_page_replica_invalid);

bool is_page_replica_invalid(struct page *page_replica)
{
    struct page_replica_meta *meta = get_page_replica_meta(page_replica);
    bool invalidated = false;

    if (meta) {
        invalidated = meta->invalidated;
        put_page_replica_meta(meta);
    } else {
        pr_info("[%s] No meta found for page=%p\n", __func__, page_replica);
    }
    return invalidated;
}
EXPORT_SYMBOL(is_page_replica_invalid);

/* ========================================================================
 * XArray mapping management helpers
 * ======================================================================== */

/**
 * replica_to_original_pfn - Get original PFN from page replica
 * @page_replica: Page replica to look up
 *
 * Returns: Original PFN key, or 0 if not found
 */
static unsigned long replica_to_original_pfn(struct page *page_replica)
{
    void *original_pfn_val = xa_load(&replica_to_original_xa, (unsigned long)page_replica);
    return original_pfn_val ? xa_to_value(original_pfn_val) : 0;
}

/* Helper to map original<->replica */
static int establish_bidir_mapping(struct page *page_replica, unsigned long pfn_key)
{
    void *xa_ret;
    int err;

    xa_ret = xa_store(&original_to_replica_xa, pfn_key, page_replica, GFP_KERNEL);
    if (xa_is_err(xa_ret)) {
        err = xa_err(xa_ret);
        pr_info("[%s] Failed to store original->replica mapping: %d\n", __func__, err);
        return REPLICA_ERROR_ANY;
    }
    
    xa_ret = xa_store(&replica_to_original_xa, (unsigned long)page_replica, 
                     xa_mk_value(pfn_key), GFP_KERNEL);
    if (xa_is_err(xa_ret)) {
        err = xa_err(xa_ret);
        xa_erase(&original_to_replica_xa, pfn_key);
        pr_info("[%s] Failed to store replica->original mapping: %d\n", __func__, err);
        return REPLICA_ERROR_ANY;
    }
    
    return REPLICA_SUCCESS;
}

/**
 * remove_bidir_mapping - Remove bidirectional mapping between original and replica
 * @page_replica: Page replica
 * @pfn_key: Original PFN key
 */
static void remove_bidir_mapping(struct page *page_replica, unsigned long pfn_key)
{
    xa_erase(&original_to_replica_xa, pfn_key);
    xa_erase(&replica_to_original_xa, (unsigned long)page_replica);
}

/* ========================================================================
 * LRU management utilities
 * ======================================================================== */

static void __replica_lru_add_active(struct page_replica_meta *m)
{
    // pr_info("[%s] ADD to ACTIVE: page=%p order=%u\n", __func__, m->page, m->order);
    list_add(&m->lru, &replica_active_lru);
}

static void __replica_lru_move_to_active_mru(struct page_replica_meta *m)
{
    // pr_info("[%s] MOVE to ACTIVE: page=%p\n", __func__, m->page);
    list_move(&m->lru, &replica_active_lru);
}

static void __replica_lru_move_to_inactive_mru(struct page_replica_meta *m)
{
    // pr_info("[%s] MOVE to INACTIVE: page=%p\n", __func__, m->page);
    list_move(&m->lru, &replica_inactive_lru);
}

static void __replica_lru_del(struct page_replica_meta *m)
{
    // pr_info("[%s] DEL: page=%p\n", __func__, m->page);
    list_del_init(&m->lru);
    m->on_lru = false;
}

static int insert_replica_lru(struct page *page, unsigned int order)
{
    struct page_replica_meta *m;
    void *ret;
    unsigned long flags;

    m = kzalloc(sizeof(*m), GFP_KERNEL);
    if (!m)
        return -ENOMEM;

    m->page = page;
    m->order = order;
    refcount_set(&m->refcount, 1);
    m->on_lru = true;
    m->dirty = false;
    INIT_LIST_HEAD(&m->lru);

    spin_lock_irqsave(&replica_lru_lock, flags);
    ret = xa_store(&replica_meta_xa, (unsigned long)page, m, GFP_ATOMIC);
    if (xa_is_err(ret)) {
        spin_unlock_irqrestore(&replica_lru_lock, flags);
        kfree(m);
        return xa_err(ret);
    }
    __replica_lru_add_active(m);
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    return 0;
}

/* Helper to remove page from LRU during error cleanup */
static void remove_replica_lru(struct page *page)
{
    struct page_replica_meta *meta;
    unsigned long flags;
    
    spin_lock_irqsave(&replica_lru_lock, flags);
    meta = xa_load(&replica_meta_xa, (unsigned long)page);
    if (meta) {
        __replica_lru_del(meta);
        xa_erase(&replica_meta_xa, (unsigned long)page);
        // pr_info("[%s] Removed page %p from LRU during error cleanup\n", __func__, page);
        put_page_replica_meta(meta);
    }
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    
}

static bool check_page_replica_referenced_and_clear(struct page *page_replica)
{
    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return false;
    }
    unsigned long reference_count = 0;
    // pr_info("[%s] Checking if page replica %p is referenced\n", __func__, page_replica);

    // TODO: We need to ensure for concurrency safety later. for now, we just remove it from LRU and mapping XArrays.
    // TODO: More understanding with reference counting needed. Why does it needed? Can we just get it from argument?
    struct page_replica_meta *m = get_page_replica_meta(page_replica);
    if (!m) {
        pr_err("[%s] Failed to get page replica meta for %p\n", __func__, page_replica);
        return false;
    }
    // pr_info("[%s] page_replica_meta: %p, order: %d\n", __func__, m, m->order);
    int order = m->order;

    put_page_replica_meta(m); // Decrement reference count

    struct address_space *mapping = page_replica->mapping;
    pgoff_t start_index = page_replica->index;
    pgoff_t count = 1UL << order;

    // pr_info("[%s] Checking page replica %p in mapping %p, start_index: %lu, count: %lu\n",
    //         __func__, page_replica, mapping, start_index, count);

    if (!mapping) {
        pr_err("[%s] Invalid mapping for page replica %p\n", __func__, page_replica);
        return false;
    }

    struct rw_semaphore *sem = &mapping->i_mmap_rwsem;
    // pr_info("[%s] Semaphore ptr: %p, mapping: %p\n", __func__, sem, mapping);
    i_mmap_lock_read(mapping);

    // 락을 잡은 후 매핑 재검사
    if (page_replica->mapping != mapping) {
        pr_warn("[%s] Mapping changed during processing, unlocking and returning\n", __func__);
        i_mmap_unlock_read(mapping);
        return false;
    }

    int ret = walk_page_mapping(mapping, start_index, count, &young_and_clear_ops, &reference_count);

    i_mmap_unlock_read(mapping);

    if (ret < 0) { // if ret is negetive, it's an error code
        pr_err("[%s] Failed to walk page mapping for page replica %p: %d\n", __func__, page_replica, ret);
        return false;
    }

    if ((unsigned long)reference_count > 0) {
        // pr_info("[%s] Page replica %p is referenced, count=%lu\n", __func__, page_replica, (unsigned long)reference_count);
        return true;
    } else {
        // pr_info("[%s] Page replica %p is not referenced\n", __func__, page_replica);
        return false;
    }
}

bool check_page_replica_dirty(struct page *page_replica)
{
    if (unlikely(!page_replica)) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return false;
    }
    
    // pr_info("[%s] Checking if page replica %p is dirty\n", __func__, page_replica);
    bool is_dirty = false;
    struct page_replica_meta *m = get_page_replica_meta(page_replica);
    if (!m) {
        pr_err("[%s] Failed to get page replica meta for %p\n", __func__, page_replica);
        return false;
    }
    is_dirty = m->dirty;
    // if (is_dirty) {
    //     pr_info("[%s] Page replica %p is dirty\n", __func__, page_replica);
    // } else {
    //     pr_info("[%s] Page replica %p is clean\n", __func__, page_replica);
    // }
    put_page_replica_meta(m); // Decrement reference count
    return is_dirty;
}

static bool check_page_replica_dirty_and_clean(struct page *page_replica)
{
    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return false;
    }
    
    // pr_info("[%s] Checking if page replica %p is dirty\n", __func__, page_replica);
    bool is_dirty = false;
    struct page_replica_meta *m = get_page_replica_meta(page_replica);
    if (!m) {
        pr_err("[%s] Failed to get page replica meta for %p\n", __func__, page_replica);
        return false;
    }
    is_dirty = m->dirty;
    // if (is_dirty) {
    //     pr_info("[%s] Page replica %p is dirty\n", __func__, page_replica);
    //     m->dirty = false;
    // } else {
    //     pr_info("[%s] Page replica %p is clean\n", __func__, page_replica);
    // }
    m->dirty = false; // clear dirty flag
    put_page_replica_meta(m); // Decrement reference count
    return is_dirty;
}

/* ========================================================================
 * Linux-style LRU implementation 
 * ======================================================================== */

/**
 * replica_reclaim_from_inactive - Reclaim pages from inactive list (Linux vmscan style)
 * @nr: Number of pages to attempt to reclaim
 *
 * This function implements Linux-style reclaim from inactive list:
 * - Takes pages from TAIL (LRU) of inactive list
 * - Referenced pages get moved back to active list MRU
 * - Non-referenced pages get unmapped and freed
 */
static unsigned long replica_reclaim_from_inactive(unsigned long nr)
{
    unsigned long flags;
    unsigned long collected = 0, freed = 0;
    struct page_replica_meta *m, *tmp;
    struct list_head process_list;
    
    INIT_LIST_HEAD(&process_list);
    
    /* First pass: collect pages from tail (LRU) of inactive list */
    spin_lock_irqsave(&replica_lru_lock, flags);
    list_for_each_entry_safe_reverse(m, tmp, &replica_inactive_lru, lru) {
        if (collected >= nr)
            break;
        
        /* Get reference and move to processing list */
        if (refcount_inc_not_zero(&m->refcount)) {
            list_move(&m->lru, &process_list);
            m->on_lru = false;
            collected++;
        }
    }
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    
    // pr_info("[%s] Collected %lu pages from inactive list for reclaim\n", 
    //         __func__, collected);
    
    /* Second pass: process pages - check references and reclaim */
    list_for_each_entry_safe(m, tmp, &process_list, lru) {
        struct page *page = m->page;
        bool refd;
        int ret;
        
        /* Check if referenced (last chance for inactive pages) */
        refd = check_page_replica_referenced_and_clear(page);
        
        if (refd) {
            /* Referenced - promote back to active list MRU */
            spin_lock_irqsave(&replica_lru_lock, flags);
            __replica_lru_move_to_active_mru(m);
            m->on_lru = true;
            spin_unlock_irqrestore(&replica_lru_lock, flags);
            put_page_replica_meta(m);
            // pr_info("[%s] Promoted referenced page %p back to active\n", 
            //     __func__, m->page);
                continue;
            }
        
        ret = __flush_page_replica(m->page);
        
        if (ret == REPLICA_SUCCESS) {
            freed++;
            pr_debug("[%s] Successfully reclaimed page %p (pfn=0x%lx)\n", 
                    __func__, m->page, page_to_pfn(m->page));
        } else {
            pr_err("[%s] Failed to flush page replica %p: %d\n", 
                    __func__, m->page, ret);
            /* On failure, reinsert to inactive list MRU */
            spin_lock_irqsave(&replica_lru_lock, flags);
            __replica_lru_move_to_inactive_mru(m);
            m->on_lru = true;
            spin_unlock_irqrestore(&replica_lru_lock, flags);
        }
        put_page_replica_meta(m);
    }
    
    pr_info("[%s] Reclaimed %lu pages from inactive list\n", __func__, freed);
    return freed;
}

/**
 * replica_age_active_to_inactive - Age pages from active to inactive list
 * @nr: Number of pages to scan for aging
 *
 * Linux-style aging:
 * - Takes pages from TAIL (LRU) of active list  
 * - Referenced pages stay in active list MRU
 * - Non-referenced pages move to inactive list MRU
 */
static unsigned int replica_age_active_to_inactive(unsigned long nr)
{
    unsigned long flags;
    unsigned long collected = 0, aged = 0;
    struct page_replica_meta *m, *tmp;
    struct list_head process_list;
    
    INIT_LIST_HEAD(&process_list);
    
    /* First pass: collect pages from tail (LRU) of active list */
    spin_lock_irqsave(&replica_lru_lock, flags);
    list_for_each_entry_safe_reverse(m, tmp, &replica_active_lru, lru) {
        if (collected >= nr)
            break;
        
        /* Get reference and move to processing list */
        if (refcount_inc_not_zero(&m->refcount)) {
            list_move(&m->lru, &process_list);
            m->on_lru = false;
            collected++;
        }
    }
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    
    pr_info("[%s] Collected %lu pages from active list for aging\n", 
            __func__, collected);
    
    /* Second pass: check references and age appropriately */
    list_for_each_entry_safe(m, tmp, &process_list, lru) {
        struct page *page = m->page;
        bool refd;
        
        /* Check if referenced (last chance for inactive pages) */
        refd = check_page_replica_referenced_and_clear(page);
        
        
        if (refd) {
            /* Still referenced - keep in active list MRU */
            spin_lock_irqsave(&replica_lru_lock, flags);
            __replica_lru_move_to_active_mru(m);
            m->on_lru = true;
            // pr_info("[%s] Keeping referenced page %p in active\n", 
            //     __func__, m->page);
            } else {
            /* Not referenced - move to inactive list MRU */
            spin_lock_irqsave(&replica_lru_lock, flags);
            __replica_lru_move_to_inactive_mru(m);
            m->on_lru = true;
            aged++;
            // pr_info("[%s] Aged page %p to inactive\n", __func__, m->page);
        }
        spin_unlock_irqrestore(&replica_lru_lock, flags);
        put_page_replica_meta(m);
    }
    
    pr_info("[%s] Aged %lu pages from active to inactive\n", __func__, aged);
    return aged;
}

/* ========================================================================
 * Shrinker integration (single, final)
 * ======================================================================== */

static unsigned long __replica_list_len(struct list_head *head)
{
    unsigned long n = 0;
    struct page_replica_meta *m;
    list_for_each_entry(m, head, lru) {
        if (++n > REPLICA_MAX_LIST_COUNT)
            break;
    }
    return n;
}

static unsigned long replica_shrink_count(struct shrinker *s,
                                          struct shrink_control *sc)
{
    unsigned long flags, n;
    spin_lock_irqsave(&replica_lru_lock, flags);
    n  = __replica_list_len(&replica_inactive_lru);
    pr_info("[%s] shrink_count: inactive_len=%lu\n", __func__, n);
    n += __replica_list_len(&replica_active_lru) / REPLICA_ACTIVE_TO_INACTIVE_RATIO;
    spin_unlock_irqrestore(&replica_lru_lock, flags);

    pr_info("[%s] shrink_count: returning %lu pages\n", __func__, n);
    return n;
}

static unsigned long replica_shrink_scan(struct shrink_control *sc)
{
    unsigned long nr_to_scan = sc->nr_to_scan ? sc->nr_to_scan : REPLICA_DEFAULT_SCAN_PAGES;
    unsigned long flags;
    unsigned long inactive_len;
    unsigned long active_len;
    bool age_again = true;
    unsigned long freed = 0;
    unsigned int aged = 0;
    unsigned int age_mult = 1;
    unsigned int free_mult = 1;
    
    pr_info("[%s] nr_to_scan=%lu\n", __func__, nr_to_scan);
    
    while (freed < nr_to_scan) {

        /* Step 1: Check if inactive list has enough pages for direct reclaim */
        spin_lock_irqsave(&replica_lru_lock, flags);
        inactive_len = __replica_list_len(&replica_inactive_lru);
        active_len = __replica_list_len(&replica_active_lru);
        spin_unlock_irqrestore(&replica_lru_lock, flags);

        if (!inactive_len && !active_len) {
            pr_info("[%s] Both inactive and active lists are empty, nothing to reclaim\n", __func__);
            break;
        }
        
        if (inactive_len >= nr_to_scan * REPLICA_INACTIVE_THRESHOLD_MULT) {
            /* Step 1-1: Direct reclaim from inactive list */
            freed += replica_reclaim_from_inactive(nr_to_scan * free_mult);
            pr_info("[%s] Reclaim result: inactive_len=%lu, freed=%lu\n", 
                    __func__, inactive_len, freed);
            free_mult *= 2; // double the reclaim size next time
            continue;
        }
        
        /* Step 2: Not enough inactive pages, need to age active pages first */
        pr_info("[%s] Not enough inactive pages (%lu < %lu), aging active pages\n",
                __func__, inactive_len, nr_to_scan * REPLICA_INACTIVE_THRESHOLD_MULT);
        
        while (aged < nr_to_scan * REPLICA_INACTIVE_THRESHOLD_MULT) {
            aged += replica_age_active_to_inactive(nr_to_scan * REPLICA_AGING_MULT * age_mult);
            spin_lock_irqsave(&replica_lru_lock, flags);
            active_len = __replica_list_len(&replica_active_lru);
            spin_unlock_irqrestore(&replica_lru_lock, flags);
            if (!active_len) {
                pr_info("[%s] Active list is empty, cannot age more\n", __func__);
                break;
            }
            age_mult *= 2; // double the aging size next time
            pr_info("[%s] Aged %u pages so far, active_len=%lu\n", 
                    __func__, aged, active_len);
        }
        
        /* Step 3: Try reclaim again after aging */
        spin_lock_irqsave(&replica_lru_lock, flags);
        inactive_len = __replica_list_len(&replica_inactive_lru);
        spin_unlock_irqrestore(&replica_lru_lock, flags);
        
        if (inactive_len >= nr_to_scan * REPLICA_INACTIVE_THRESHOLD_MULT) {
            freed += replica_reclaim_from_inactive(nr_to_scan * free_mult);
            free_mult *= 2; // double the reclaim size next time
        }
    }

    pr_info("[%s] Final result: aged=%u, inactive_len=%lu, freed=%lu\n",
            __func__, aged, inactive_len, freed);
    
    return freed;
}

static unsigned long replica_shrink_scan_wrapper(struct shrinker *s,
                                                struct shrink_control *sc)
{
    return replica_shrink_scan(sc);
}

static struct shrinker *replica_shrinker;

/* Manual shrinker trigger function */
static void replica_trigger_shrink(unsigned long nr_to_free)
{
    struct shrink_control sc = {
        .nr_to_scan = nr_to_free,
        .gfp_mask = GFP_KERNEL,
    };
    
    unsigned long freed = replica_shrink_scan(&sc);
    pr_info("[%s] Manual shrink: requested=%lu, freed=%lu\n",
            __func__, nr_to_free, freed);
}

static int __init replica_shrinker_init(void)
{
    replica_shrinker = shrinker_alloc(0, "replica_shrinker");
    if (!replica_shrinker) {
        pr_err("[%s] failed to allocate shrinker\n", __func__);
        return -ENOMEM;
    }

    replica_shrinker->count_objects = replica_shrink_count;
    replica_shrinker->scan_objects = replica_shrink_scan_wrapper;
    replica_shrinker->seeks = DEFAULT_SEEKS;

    // TODO: Register the shrinker later. now is for testing purpose.
    shrinker_register(replica_shrinker);
    pr_info("[%s] shrinker registered\n", __func__);
    return 0;
}

subsys_initcall(replica_shrinker_init);

SYSCALL_DEFINE0(flush_replicas)
{
    pr_info("[syscall] flush_replicas called\n");
    int n;
    unsigned long flags;
    unsigned int aged;
    unsigned long freed;

    pr_info("[syscall] flush_replicas: aging active to inactive\n");
    aged = replica_age_active_to_inactive(REPLICA_MAX_LIST_COUNT);
    pr_info("[syscall] flush_replicas: aged %u pages\n", aged);

    spin_lock_irqsave(&replica_lru_lock, flags);
    n = __replica_list_len(&replica_inactive_lru);
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    pr_info("[syscall] flush_replicas: reclaiming for %d pages\n", n);
    freed = replica_reclaim_from_inactive(n);  
    pr_info("[syscall] flush_replicas: reclaimed %lu pages\n", freed); 
    return 0;
}

/* ========================================================================
 * Public API for page coherence integration
 * ======================================================================== */

/* Simple and unified copy function - caller handles all mapping */
static int copy_data(void *src_kaddr, void *dst_kaddr, size_t size)
{
    if (!src_kaddr || !dst_kaddr) {
        pr_err("[%s] NULL address provided: src=%p, dst=%p\n", 
               __func__, src_kaddr, dst_kaddr);
        return REPLICA_ERROR_INVAL;
    }
    
    memcpy(dst_kaddr, src_kaddr, size);
    // pr_info("[%s] Copied %zu bytes from %p to %p\n", 
    //         __func__, size, src_kaddr, dst_kaddr);
    
    return REPLICA_SUCCESS;
}

/* Helper function to safely map pages*/
static void *kmap_page_safe(struct page *page, unsigned int order)
{
    if (order > 0) {
        void *kaddr = kmap(page);
        if (!kaddr) {
            pr_err("%s: Failed to kmap pages for order %u\n", __func__, order);
            return NULL;
        }

        // pr_info("[%s] Mapped pages (order=%u) using kmap\n", __func__, order);
        return kaddr;
    } else {
        void *kaddr = kmap_local_page(page);
        if (!kaddr) {
            pr_err("%s: Failed to kmap_local_page for order %u\n", __func__, order);
            return NULL;
        }
        // pr_info("[%s] Mapped pages (order=%u) using kmap_local_page\n", __func__, order);
        return kaddr;
    }
}

static void *kunmap_page_safe(struct page *page, void *kaddr, unsigned int order)
{
    if (order > 0) {
        kunmap(page);
        // pr_info("[%s] Unmapped pages (order=%u) using kunmap\n", __func__, order);
    } else {
        kunmap_local(kaddr);
        // pr_info("[%s] Unmapped pages (order=%u) using kunmap_local\n", __func__, order);
    }
    return NULL;
}

static int unmap_page_replica(struct page *page_replica, unsigned int order)
{
    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return REPLICA_ERROR_INVAL;
    }

    pr_info("[%s] Unmapping page replica %p (order=%u)\n", __func__, page_replica, order);

    struct address_space *mapping = page_replica->mapping;
    // TODO: Check if this works well.
    if (!mapping) {
        pr_err("[%s] Page %p has no mapping, which is already cleaned because of process termination\n", __func__, page_replica);
        return REPLICA_SUCCESS; // No mapping, nothing to unmap
    }

    // if (unlikely(!mapping)) {
    //     pr_err("[%s] Page %p has no mapping, cannot unmap\n", __func__, page_replica);
    //     return REPLICA_ERROR_INVAL;
    // }

    unsigned long index = page_replica->index; // page_replica->index is identical with linear_page_index(vma, address & ~(size - 1)) and also with vmf->pgoff, which is the key of Xarray(mapping)
    int ret;


    // temporarliy casting to folio to use dax folio helpers
    struct folio *folio_replica = page_folio(page_replica);
    dax_entry_t cookie;
    cookie = dax_lock_folio(folio_replica);
    if (!cookie) {
        pr_err("[%s] Failed to lock folio %p for unmapping\n", __func__, page_replica);
        return REPLICA_ERROR_LOCK;
    }

    // print_page_info(page_replica, "Before unmap_mapping_pages");
    // print_page_info(page_replica + 1, "Before unmap_mapping_pages + 1");
    // print_page_info(page_replica + 2, "Before unmap_mapping_pages + 2");
    
    unmap_mapping_pages(mapping, index, 1 << order, false);
    
    // print_page_info(page_replica, "Before dax_delete_mapping_entry replica");
    // print_page_info(page_replica + 1, "Before dax_delete_mapping_entry + 1");
    // print_page_info(page_replica + 2, "Before dax_delete_mapping_entry + 2");

    dax_unlock_folio(folio_replica, cookie);

    // Remove the mapping entry from the XArray
    pr_info("[%s] Removing mapping entry for page %p (index=%lu)\n", __func__, page_replica, index);

    // return 1 if sucessed
    ret = dax_delete_mapping_entry(mapping, index);

    // print_page_info(page_replica, "After unmap_mapping_pages and dax_delete_mapping_entry");
    // print_page_info(page_replica + 1, "After unmap_mapping_pages and dax_delete_mapping_entry + 1");
    // print_page_info(page_replica + 2, "After unmap_mapping_pages and dax_delete_mapping_entry + 2");

    return ret ? REPLICA_SUCCESS : REPLICA_ERROR_ANY;
}

/* Helper to allocate pages with retry and shrinking */
static struct page *allocate_page_replica_with_retry(unsigned int order)
{
    struct page *page_replica;
    gfp_t gfp_flags = GFP_HIGHUSER_MOVABLE | __GFP_ZERO;
    int retry_count= 0;

retry_alloc:
    page_replica = alloc_pages(gfp_flags, order);
    
    if (unlikely(!page_replica)) {
        if (retry_count < MAX_ALLOCATE_RETRIES) {
            /* Calculate how many pages to free */
            unsigned long pages_to_free = (order==0) ? 1 : 16; // to get as fast as possible

            pr_info("[%s] Allocation failed (retry %d/%d), triggering manual shrink of %lu pages\n",
                    __func__, retry_count + 1, MAX_ALLOCATE_RETRIES, pages_to_free);
            replica_trigger_shrink(pages_to_free);
            msleep(10);  /* Brief delay for shrinking to complete */
            
            retry_count++;
            goto retry_alloc;
        }
        pr_err("[%s] Failed to allocate page replica after %d retries (order=%u)\n",
            __func__, MAX_ALLOCATE_RETRIES, order);
        return NULL;
    }

    if (retry_count > 0) {
        pr_info("[%s] Allocation succeeded after %d retries and manual shrinking\n", 
                __func__, retry_count);
    }

    // print_page_info(page_replica, "Allocated page replica");
    // print_page_info(page_replica + 1, "Allocated page replica + 1");
    // print_page_info(page_replica + 2, "Allocated page replica + 2");
    track_page_alloc(order);
    return page_replica;
}

/**
 * create_page_replica - Create a new page replica
 * @order: order (0 for single page, PMD_ORDER for page 512 pages)
 * @original_pfn: Original page PFN to replicate
 * @src_kaddr: Source kernel virtual address for data copy
 *
 * Creates a new page replica, adds it to LRU management, and establishes
 * bidirectional mapping with the original page.
 *
 * Returns: Replica page pointer on success, ERR_PTR on failure
 */
struct page *create_page_replica(unsigned int order, pfn_t original_pfn, void *src_kaddr)
{
    struct page *page_replica;
    unsigned long pfn_key = pfn_t_to_pfn(original_pfn);
    int err;
    size_t size = PAGE_SIZE << order; // Calculate size based on order

    // Check for duplicate replica, existance of replica should be checked before call create_page_replica
    if (xa_load(&original_to_replica_xa, pfn_key)) {
        pr_err("[Err]%s: Replica already exists for pfn %lu\n", __func__, pfn_key);
        return print_replica_error(REPLICA_ERROR_EXIST);
    }

    /* Step 1: Allocate page replica with retry and manual shrinking */
    page_replica = allocate_page_replica_with_retry(order);
    if (!page_replica) {
        pr_err("[%s] Failed to allocate replica page (order=%u)\n", __func__, order);
        return ERR_PTR(REPLICA_ERROR_NOMEM);
    }

    /* Step 2: Copy data from source to replica using unified helper */
    void *dst_kaddr = kmap_page_safe(page_replica, order);
    if (!dst_kaddr) {
        pr_err("[%s] Failed to kmap page replica for copy\n", __func__);
        err = REPLICA_ERROR_ANY;
        goto free_pages;
    }

    err = copy_data(src_kaddr, dst_kaddr, size);

    kunmap_page_safe(page_replica, dst_kaddr, order);
    if (err != REPLICA_SUCCESS) {
        pr_err("[%s] Data copy failed: %d\n", __func__, err);
        goto free_pages;
    }

    // TODO: Step 3 and 4 should be done in atomic context. but later wee need to change lru later in LRU management.
    // for now, we just want to see for basic functionality.
    /* Step 3: Add to LRU management */
    err = insert_replica_lru(page_replica, order);
    if (err) {
        pr_err("[%s] LRU insertion failed: %d\n", __func__, err);
        goto free_pages;
    }

    /* Step 4: Establish bidirectional mapping */
    err = establish_bidir_mapping(page_replica, pfn_key);
    if (err) {
        pr_err("[%s] Mapping establishment failed: %d\n", __func__, err);
        goto remove_from_lru;
    }

    pr_info("[%s] Created page replica (order=%u, pfn=0x%lx, original_pfn=0x%lx)\n",
            __func__, order, page_to_pfn(page_replica), pfn_key);

    return page_replica;

remove_from_lru:
    remove_replica_lru(page_replica);
    pr_err("[%s] Removed page replica from LRU due to mapping failure\n", __func__);

free_pages:
    __free_pages(page_replica, order);
    track_page_free(order);
    return ERR_PTR(err);
} 
EXPORT_SYMBOL(create_page_replica);

// No writeback for page replica, just destroy it
int destroy_page_replica(struct page *page_replica)
{
    int err;
    // pr_info("[%s] Destroying page replica %p\n", __func__, page_replica);

    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return REPLICA_ERROR_INVAL;
    }

    // TODO: We need to ensure for concurrency safety later. for now, we just remove it from LRU and mapping XArrays.
    // TODO: More understanding with reference counting needed. Why does it needed? Can we just get it from argument?
    struct page_replica_meta *m = get_page_replica_meta(page_replica);
    if (!m) {
        pr_err("[%s] Failed to get page replica meta for %p\n", __func__, page_replica);
        return REPLICA_ERROR_NOMEM;
    }
    int order = m->order;
    if (order < 0) {
        pr_err("[%s] Invalid order for page replica %p: %d\n", __func__, page_replica, order);
        return REPLICA_ERROR_INVAL;
    }
    put_page_replica_meta(m); // Decrement reference count

    /* Step 1: Remove from LRU */
    remove_replica_lru(page_replica);

    /* Step 2: Remove bidirectional mapping */
    unsigned long pfn_key = replica_to_original_pfn(page_replica);
    remove_bidir_mapping(page_replica, replica_to_original_pfn(page_replica));

    /* Step 3: Unmap replica pages */
    err = unmap_page_replica(page_replica, order);
    if (err != REPLICA_SUCCESS) {
        pr_err("[%s] Failed to unmap replica page %p: %d\n", __func__, page_replica, err);
        // restore mapping if unmap failed
        insert_replica_lru(page_replica, order);
        establish_bidir_mapping(page_replica, pfn_key);
        return err;
    }
    
    /* Step 4: Free pages */
    __free_pages(page_replica, order);
    track_page_free(order);
    pr_info("[%s] Successfully destroyed page replica %p (order=%u)\n",
            __func__, page_replica, order);

    return REPLICA_SUCCESS;
} 
EXPORT_SYMBOL(destroy_page_replica);


// Writeback the page replica to original page if dirty, otherwise do nothing
// Return REPLICA_SUCCESS if writeback done, REPLICA_SHARED_STATE if no write
int writeback_page_replica(struct page *page_replica)
{
    int ret;

    // pr_info("[%s] Checking page replica is dirty before writeback\n", __func__);
    if (!check_page_replica_dirty_and_clean(page_replica)) {
        pr_info("[%s] Page replica %p is not dirty, no writeback needed\n", __func__, page_replica);
        return REPLICA_SHARED_STATE; // No writeback needed
    }

    // pr_info("[%s] Writing back page replica %p\n", __func__, page_replica);

    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return REPLICA_ERROR_INVAL;
    }

    // TODO: We need to ensure for concurrency safety later. for now, we just remove it from LRU and mapping XArrays.
    // TODO: More understanding with reference counting needed. Why does it needed? Can we just get it from argument?
    struct page_replica_meta *m = get_page_replica_meta(page_replica);
    if (!m) {
        pr_err("[%s] Failed to get page replica meta for %p\n", __func__, page_replica);
        return REPLICA_ERROR_NOMEM;
    }
    int order = m->order;
    if (order < 0) {
        pr_err("[%s] Invalid order for page replica %p: %d\n", __func__, page_replica, order);
        return REPLICA_ERROR_INVAL;
    }
    put_page_replica_meta(m); // Decrement reference count

    /* Step 1: Get original pfn with Xarray */
    unsigned long pfn_key = replica_to_original_pfn(page_replica);
    if (!pfn_key) {
        pr_err("[Err]%s: No original PFN mapping found for replica page %p\n", __func__, page_replica);
        return REPLICA_ERROR_NOENT;
    }

    // temporarliy casting to folio to use dax folio helpers
    struct folio *folio_replica = page_folio(page_replica);
    dax_entry_t cookie;
    

    /* Step 2: Clean R/W bit of all PTE/PMD */
    struct address_space *mapping = page_replica->mapping;
    // TODO: Check if this works well.
    if (!mapping) {
        pr_info("[%s] Page %p has no mapping, so skip cleaning R/W bit\n", __func__, page_replica);
        // for test
        goto skip_rw_clean;
    }

    cookie = dax_lock_folio(folio_replica);
    if (!cookie) {
        pr_err("[%s] Failed to lock folio %p for unmapping\n", __func__, page_replica);
        return REPLICA_ERROR_LOCK;
    }

    struct vm_area_struct *vma;
    pfn_t pfn = page_to_pfn_t(page_replica);
    unsigned long count = 1UL << order;
    unsigned long index = page_replica->index;
    unsigned long end = index + count - 1;
    i_mmap_lock_read(mapping);
    vma_interval_tree_foreach(vma, &mapping->i_mmap, index, end) {
        pfn_mkclean_range(pfn_t_to_pfn(pfn), count, index, vma);
        cond_resched();
    }
    i_mmap_unlock_read(mapping);

    dax_unlock_folio(folio_replica, cookie);

skip_rw_clean:
    /* Step 3: kmap for copy */
    void *src_kaddr = kmap_page_safe(page_replica, order);
    if (!src_kaddr) {
        pr_err("[%s] Failed to kmap page replica for writeback\n", __func__);
        return REPLICA_ERROR_ANY;
    }

    void *dst_kaddr = kmap_page_safe(pfn_to_page(pfn_key), order);
    if (!dst_kaddr) {
        pr_err("[%s] Failed to kmap original page for writeback\n", __func__);
        kunmap_page_safe(page_replica, src_kaddr, order);
        return REPLICA_ERROR_ANY;
    }

    /* Step 4: copy */
    size_t size = PAGE_SIZE << order; // Calculate size based on order
    ret = copy_data(src_kaddr, dst_kaddr, size);
    if (ret != REPLICA_SUCCESS) {
        pr_err("[%s] Data copy failed: %d\n", __func__, ret);
        kunmap_page_safe(pfn_to_page(pfn_key), dst_kaddr, order);
        kunmap_page_safe(page_replica, dst_kaddr, order);
        return ret;
    }

    /* Step 5: Flush cache to make shure writeback */
    volatile char *buffer = (volatile char *)dst_kaddr;
    for (unsigned long i = 0; i < size; i += CL_SIZE) {
        clflush((volatile void *)&buffer[i]);
    }

    /* Step 6: kunmap */
    kunmap_page_safe(page_replica, src_kaddr, order);
    kunmap_page_safe(pfn_to_page(pfn_key), dst_kaddr, order);
    
    pr_info("[%s] Successfully wrote back replica page %p to original pfn %lu\n",
            __func__, page_replica, pfn_key);

    /* Step 7?: Clean dirty mark in DAX entry? Do we need this? -> move to the first of this function */
    return REPLICA_SUCCESS;
} 
EXPORT_SYMBOL(writeback_page_replica);

int fetch_page_replica(struct page *page_replica, unsigned int order, void *src_kaddr)
{
    int err;
    size_t size = PAGE_SIZE << order;
    struct page_replica_meta *meta = get_page_replica_meta(page_replica);
    
    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return -1;
    }

    /* Step 2: Copy data from source to replica using unified helper */
    void *dst_kaddr = kmap_page_safe(page_replica, order);
    if (!dst_kaddr) {
        pr_err("[%s] Failed to kmap page replica for copy\n", __func__);
        err = REPLICA_ERROR_ANY;
    }

    err = copy_data(src_kaddr, dst_kaddr, size);

    kunmap_page_safe(page_replica, dst_kaddr, order);
    if (err != REPLICA_SUCCESS) {
        pr_err("[%s] Data copy failed: %d\n", __func__, err);

    }

    meta->invalidated = false; // Mark as valid
    put_page_replica_meta(meta); // Decrement reference count

    return 0;
}
EXPORT_SYMBOL(fetch_page_replica);

/**
 * get_page_replica_with_ref - Get existing page replica by original PFN and order
 * @original_pfn: Original page PFN to look up
 * @order: Order of the page (0 for single page, PMD_ORDER for huge page)
 *
 * Returns: Replica page pointer if found, NULL if not found
 * 
 * Note: Caller must call put_page_replica_ref() when done with the returned page
 * to release the reference and avoid memory leaks.
 */
struct page *get_page_replica_with_ref(pfn_t original_pfn, unsigned int order)
{
    unsigned long pfn_key = pfn_t_to_pfn(original_pfn);
    struct page *page_replica;
    struct page_replica_meta *meta;
    
    /* First: get replica page pointer (may become stale) */
    page_replica = xa_load(&original_to_replica_xa, pfn_key);
    if (!page_replica)
        return NULL;
    
    /* Second: get reference to prevent freeing using unified helper */
    meta = get_page_replica_meta(page_replica);
    if (meta) {
        if (meta->order != order) {
            pr_err("[%s] Mismatched order for replica page %p: expected %u, got %u\n",
                   __func__, page_replica, order, meta->order);
            return NULL; // Order mismatch, return NULL
        }
        pr_info("[%s] Found page replica %p for original pfn %lu (with reference)\n", 
                __func__, page_replica, pfn_key);
        return page_replica;
    } else {
        /* Failed: page was freed or being freed */
        return NULL;
    }
} 
EXPORT_SYMBOL(get_page_replica_with_ref);

/**
 * put_page_replica_ref - Release reference obtained from find_page_replica_with_ref()
 * @page_replica: Page replica to release reference for
 *
 * This function must be called for every page replica returned by
 * find_page_replica_with_ref() to release the reference and prevent memory leaks.
 */
void put_page_replica_ref(struct page *page_replica)
{
    struct page_replica_meta *meta;
    
    if (!page_replica)
        return;

    meta = xa_load(&replica_meta_xa, (unsigned long)page_replica);

    if (!meta) {
        pr_err("[%s] No metadata found for page replica %p during put\n", __func__, page_replica);
        return; // No metadata, nothing to do
    }
    
    if (unlikely(refcount_dec_and_test(&meta->refcount))) {
        pr_err("[%s]: Freeing metadata while handling coherency request should not happen!\n", __func__);
        kfree(meta);
        pr_err("[%s] Freed metadata for page replica %p\n", __func__, page_replica);
    } else {
        // pr_info("[%s] Release reference for page replica %p, remaining refcount=%d\n", 
        //         __func__, page_replica, refcount_read(&meta->refcount));
    }
} 
EXPORT_SYMBOL(put_page_replica_ref);

int make_page_replica_dirty(struct page *page_replica)
{
    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return REPLICA_ERROR_INVAL;
    }

    pr_info("[%s] Making page replica %p dirty\n", __func__, page_replica);

    struct page_replica_meta *m = get_page_replica_meta(page_replica);
    if (!m) {
        pr_err("[%s] Failed to get page replica meta for %p\n", __func__, page_replica);
        return REPLICA_ERROR_NOMEM;
    }
    m->dirty = true; // Mark as dirty
    put_page_replica_meta(m); // Decrement reference count
    return REPLICA_SUCCESS;
}
EXPORT_SYMBOL(make_page_replica_dirty);

int __flush_page_replica(struct page *page_replica)
{
    int ret;
    pr_info("[%s] Flushing page replica %p\n", __func__, page_replica);

    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return REPLICA_ERROR_INVAL;
    }

    ret = writeback_page_replica(page_replica);
    if ((ret != REPLICA_SUCCESS) && (ret != REPLICA_SHARED_STATE)) {
        pr_err("[%s] Writeback failed for page replica %p: %d\n", __func__, page_replica, ret);
        return ret;
    }

    ret = destroy_page_replica(page_replica);
    if (ret != REPLICA_SUCCESS) {
        pr_err("[%s] Destroy failed for page replica %p: %d\n", __func__, page_replica, ret);
        return ret;
    }

    pr_info("[%s] Successfully flushed page replica %p\n", __func__, page_replica);
    return REPLICA_SUCCESS;
}

static int __init page_replication_init(void)
{
    return page_replica_sysfs_init();
}

subsys_initcall(page_replication_init);
