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
#include <linux/vmstat.h>
#include <linux/slab.h>

// For PEBS
#include <linux/perf_event.h>
#include "../kernel/events/internal.h"

static LIST_HEAD(replica_active_lru);
static LIST_HEAD(replica_inactive_lru);
static DEFINE_SPINLOCK(replica_lru_lock);

/* Constants for replica management */
#define MAX_ALLOCATE_RETRIES             3
#define REPLICA_DEFAULT_SCAN_PAGES      1024
#define REPLICA_INACTIVE_THRESHOLD_MULT 2
#define REPLICA_AGING_MULT              4
#define REPLICA_ACTIVE_TO_INACTIVE_RATIO 4  /* 1/4 of active pages count for shrinking */
#define REPLICA_MAX_LIST_COUNT          (1UL << 20)
#define CL_SIZE                        64 /* Cache line size for flushing */


/* =============================================================================
 * sysfs interface for page replication statistics
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

/* Utility function for debugging */
void print_page_info(struct page *page, const char *context)
{
    phys_addr_t phys_addr;
    phys_addr = __pa(page);
    pr_info("%s: Printing page info for struct page at physical address: 0x%llx\n",
            __func__, (unsigned long long)phys_addr);
    pr_info("%s: page_info in '%s': page=%p, flags=0x%lx, mapping=%p, index=%lu, refcount=%d\n",
            __func__, context, page, page->flags, page->mapping, page->index,
            atomic_read(&page->_refcount));
    pr_info("%s: more info with flags: PG_head=%d, PG_dirty=%d, PG_writeback=%d, PG_locked=%d\n",
            __func__, PageHead(page), PageDirty(page), PageWriteback(page), PageLocked(page));
    //print raw dump of fields of struct page with hex code
    
    pr_info("%s: page[%d-%d]: %lx %lx %lx %lx\n", __func__, 0, 3, ((unsigned long *)page)[0], ((unsigned long *)page)[1], ((unsigned long *)page)[2], ((unsigned long *)page)[3]);
    pr_info("%s: page[%d-%d]: %lx %lx %lx %lx\n", __func__, 4, 7, ((unsigned long *)page)[4], ((unsigned long *)page)[5], ((unsigned long *)page)[6], ((unsigned long *)page)[7]);
}
EXPORT_SYMBOL(print_page_info);



/* ========================================================================
 * Page reference checking utilities
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
 * LRU management utilities
 * ======================================================================== */

static void __replica_lru_add_active(struct page *page_replica)
{
    list_add(&page_replica->lru, &replica_active_lru);
}

static void __replica_lru_move_to_active_mru(struct page *page_replica)
{
    list_move(&page_replica->lru, &replica_active_lru);
}

static void __replica_lru_move_to_inactive_mru(struct page *page_replica)
{
    list_move(&page_replica->lru, &replica_inactive_lru);
}

static void __replica_lru_del(struct page *page_replica)
{
    list_del_init(&page_replica->lru);
}

static int insert_replica_lru(struct page *page_replica)
{
    unsigned long flags;

    INIT_LIST_HEAD(&page_replica->lru);

    spin_lock_irqsave(&replica_lru_lock, flags);
    __replica_lru_add_active(page_replica);
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    return 0;
}

/* Helper to remove page from LRU during error cleanup */
static void remove_replica_lru(struct page *page_replica)
{
    unsigned long flags;
    
    spin_lock_irqsave(&replica_lru_lock, flags);
    __replica_lru_del(page_replica);
    spin_unlock_irqrestore(&replica_lru_lock, flags);
}

static bool check_page_replica_referenced_and_clear(struct page *page_replica)
{
    if (!page_replica) {
        pr_err("[%s] Invalid page replica pointer\n", __func__);
        return false;
    }
    unsigned long reference_count = 0;

    struct address_space *mapping = page_replica->mapping;
    pgoff_t start_index = page_replica->index;

    if (!mapping) {
        pr_err("[%s] Invalid mapping for page replica %p\n", __func__, page_replica);
        return false;
    }

    i_mmap_lock_read(mapping);

    // 락을 잡은 후 매핑 재검사
    if (page_replica->mapping != mapping) {
        pr_warn("[%s] Mapping changed during processing, unlocking and returning\n", __func__);
        i_mmap_unlock_read(mapping);
        return false;
    }

    int ret = walk_page_mapping(mapping, start_index, 1, &young_and_clear_ops, &reference_count);

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

/* ========================================================================
 * Linux-style LRU implementation 
 * ======================================================================== */

int flush_page_replica(struct page *page_replica);

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
    unsigned long collected = 0, aged = 0;
    unsigned long freed = 0;
    struct page *page_replica, *tmp_page;
    struct list_head process_list;
    
    INIT_LIST_HEAD(&process_list);
    
    /* First pass: collect pages from tail (LRU) of inactive list */
    spin_lock_irqsave(&replica_lru_lock, flags);
    list_for_each_entry_safe_reverse(page_replica, tmp_page, &replica_inactive_lru, lru) {
        if (collected >= nr)
            break;
        
        list_move(&page_replica->lru, &process_list);
        collected++;
    }
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    
    pr_info("[%s] Collected %lu pages from inactive list for reclaim\n", 
            __func__, collected);
    
    /* Second pass: process pages - check references and reclaim */
    list_for_each_entry_safe(page_replica, tmp_page, &process_list, lru) {
        bool refd;
        int ret;
        
        /* Check if referenced (last chance for inactive pages) */
        refd = check_page_replica_referenced_and_clear(page_replica);
        
        if (refd) {
            /* Referenced - promote back to active list MRU */
            spin_lock_irqsave(&replica_lru_lock, flags);
            __replica_lru_move_to_active_mru(page_replica);
            spin_unlock_irqrestore(&replica_lru_lock, flags);
            continue;
        }
        
        ret = flush_page_replica(page_replica);

        if (ret < 0) {
            pr_err("[Err]%s: Failed to flush page replica %p: %d\n", 
                    __func__, page_replica, ret);
            /* On failure, reinsert to inactive list MRU */
            spin_lock_irqsave(&replica_lru_lock, flags);
            __replica_lru_move_to_inactive_mru(page_replica);
            spin_unlock_irqrestore(&replica_lru_lock, flags);
            continue;
        }
        freed++;
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
    struct page *page_replica, *tmp_page;
    struct list_head process_list;
    
    INIT_LIST_HEAD(&process_list);
    
    /* First pass: collect pages from tail (LRU) of active list */
    spin_lock_irqsave(&replica_lru_lock, flags);
    list_for_each_entry_safe_reverse(page_replica, tmp_page, &replica_active_lru, lru) {
        if (collected >= nr)
            break;

        list_move(&page_replica->lru, &process_list);
        collected++;
    }
    spin_unlock_irqrestore(&replica_lru_lock, flags);
    
    pr_info("[%s] Collected %lu pages from active list for aging\n", 
            __func__, collected);
    
    /* Second pass: check references and age appropriately */
    list_for_each_entry_safe(page_replica, tmp_page, &process_list, lru) {
        bool refd;
        
        /* Check if referenced (last chance for inactive pages) */
        refd = check_page_replica_referenced_and_clear(page_replica);
        
        
        if (refd) {
            /* Still referenced - keep in active list MRU */
            spin_lock_irqsave(&replica_lru_lock, flags);
            __replica_lru_move_to_active_mru(page_replica);
            // pr_info("[%s] Keeping referenced page %p in active\n", 
            //     __func__, m->page);
            } else {
            /* Not referenced - move to inactive list MRU */
            spin_lock_irqsave(&replica_lru_lock, flags);
            __replica_lru_move_to_inactive_mru(page_replica);
            aged++;
            // pr_info("[%s] Aged page %p to inactive\n", __func__, m->page);
        }
        spin_unlock_irqrestore(&replica_lru_lock, flags);
    }
    
    pr_info("[%s] Aged %lu pages from active to inactive\n", __func__, aged);
    return aged;
}

/* ========================================================================
 * Shrinker integration
 * ======================================================================== */

static unsigned long __replica_list_len(struct list_head *head)
{
    unsigned long n = 0;
    struct page *page_replica;
    list_for_each_entry(page_replica, head, lru) {
        n++;
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
        aged = 0;

        /* Step 1: Check if inactive list has enough pages for direct reclaim */
        spin_lock_irqsave(&replica_lru_lock, flags);
        inactive_len = __replica_list_len(&replica_inactive_lru);
        active_len = __replica_list_len(&replica_active_lru);
        spin_unlock_irqrestore(&replica_lru_lock, flags);

        if ( (active_len + inactive_len) < (nr_to_scan * REPLICA_INACTIVE_THRESHOLD_MULT) ) {
            pr_info("[%s] Both inactive and active are not enough\n", __func__);
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

    shrinker_register(replica_shrinker);
    pr_info("[%s] shrinker registered\n", __func__);
    return 0;
}

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

/* ============================================================================= 
 * Page Replication Utility Functions
 * ============================================================================= */

/* 하위 2비트 태그 */
#define SWMC_TAG_MASK          0x3UL
#define SWMC_TAG_PTR           0x0UL  /* replica 포인터 저장됨 */
#define SWMC_TAG_ACCESS        0x1UL  /* 상32: access_count, 하32: flags */
#define SWMC_TAG_REPLICA_SELF  0x2UL  /* 이 page 자체가 replica */
#define SWMC_TAG_RSVD          0x3UL  /* 예약/에러 */

static inline struct page *swmc_decode_replica_ptr(unsigned long v)
{
    return (struct page *)(v & ~SWMC_TAG_MASK);
}

static inline unsigned int swmc_access_flags(unsigned long v) { return (u32)(v & 0xffffffffUL); }
static inline unsigned int swmc_access_count(unsigned long v) { return (u32)(v >> 32); }
static inline unsigned short swmc_last_accessed_age(unsigned long v) { return (unsigned short)((v & 0xffff0000UL) >> 16); }

struct page *get_replica_opt(struct page *orig)
{
    unsigned long v = READ_ONCE(orig->private);

    // print_page_info(orig, "original_page in get_replica");
    // pr_info("[Info]%s: original_page=%px, private(raw)=0x%lx\n",
    //         __func__, orig, v);

    if (!v) {
        // pr_info("[Info]%s: not replicated (private==0)\n", __func__);
        return NULL;
    }

    switch (v & SWMC_TAG_MASK) {
    case SWMC_TAG_PTR: {
        // pr_info("[Info]%s: tag=PTR\n", __func__);
        struct page *rep = swmc_decode_replica_ptr(v);
        // pr_info("[Info]%s: replica pointer -> %px\n", __func__, rep);
        return rep;
    }
    case SWMC_TAG_ACCESS:
        // pr_info("[Info]%s: access-mode (flags=0x%x, access_count=%u) => no replica\n",
        //         __func__, swmc_access_flags(v), swmc_access_count(v));
        return NULL;

    case SWMC_TAG_REPLICA_SELF:
        // pr_info("[Info]%s: this page is a REPLICA itself => no replica-of\n", __func__);
        return NULL;

    default: /* SWMC_TAG_RSVD */
        // pr_warn("[Warn]%s: invalid tag(11b), private=0x%lx\n", __func__, v);
        return NULL;
    }
}

struct page *get_original_opt(struct page *page_replica)
{
    struct page *original;
    if (!page_replica->memcg_data){
        pr_err("[Error]%s: page_replica->memcg_data is NULL for page_replica=0x%lx\n", __func__, (unsigned long)page_replica);
    }
    original = page_replica->memcg_data;
    return original;
}

/* Copy one (possibly compound) page to another.
 * order == 0 : use kmap_local_page/kunmap_local (fast, per-CPU)
 * order > 0  : use kmap/kunmap (for high-order physically contiguous pages)
 * Returns: 0 on success, negative errno on failure.
 */
static int copy_data_page(struct page *src_page, struct page *dst_page, unsigned int order)
{
    void *src_kaddr = NULL, *dst_kaddr = NULL;
    size_t bytes;

    if (!src_page || !dst_page) {
        pr_err("[%s] NULL page: src=%px dst=%px\n", __func__, src_page, dst_page);
        return -EINVAL;
    }

    bytes = (size_t)PAGE_SIZE << order;

    if (order > 0) {
        /* High-order: use kmap (non-local) */
        src_kaddr = kmap(src_page);
        if (!src_kaddr) {
            pr_err("[%s] kmap(src) failed (order=%u)\n", __func__, order);
            return -ENOMEM;
        }

        dst_kaddr = kmap(dst_page);
        if (!dst_kaddr) {
            pr_err("[%s] kmap(dst) failed (order=%u)\n", __func__, order);
            kunmap(src_page);
            return -ENOMEM;
        }
    } else {
        /* order==0: faster per-CPU local mapping */
        src_kaddr = kmap_local_page(src_page);
        if (!src_kaddr) {
            pr_err("[%s] kmap_local(src) failed\n", __func__);
            return -ENOMEM;
        }

        dst_kaddr = kmap_local_page(dst_page);
        if (!dst_kaddr) {
            pr_err("[%s] kmap_local(dst) failed\n", __func__);
            kunmap_local(src_kaddr);
            return -ENOMEM;
        }
    }

    memcpy(dst_kaddr, src_kaddr, bytes);

    if (order > 0) {
        kunmap(dst_page);
        kunmap(src_page);
    } else {
        kunmap_local(dst_kaddr);
        kunmap_local(src_kaddr);
    }

    return 0;
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
 *
 * Returns: 0 on success, negative errno on failure.
 */
int create_page_replica(struct page *page_original, unsigned int order)
{
    struct page *page_replica;
    int err;
    size_t size = PAGE_SIZE << order; // Calculate size based on order

    pr_info("[Info]%s: Creating page replica for original page %p (order=%u)\n",
            __func__, page_original, order);

    if (get_replica_opt(page_original)) {
        pr_err("[%s] Page %p is already a replica\n", __func__, page_original);
        return -EINVAL;
    }

    /* Step 1: Allocate page replica with retry and manual shrinking */
    page_replica = allocate_page_replica_with_retry(order);
    if (!page_replica) {
        pr_err("[%s] Failed to allocate replica page (order=%u)\n", __func__, order);
        return -ENOMEM;
    }

    /* Step 2: Copy data from original to replica */
    err = copy_data_page(page_original, page_replica, order);
    if (err) {
        pr_err("[%s] Data copy failed: %d\n", __func__, err);
        goto free_pages;
    }

    struct address_space *mapping = page_original->mapping;
    pgoff_t index = page_original->index;

    if (PageModified(page_original) && PageShared(page_original)) {
        pr_info("[Info]%s: Original page 0x%lx is stale shared page, skip replication\n",
                __func__, page_to_pfn(page_original));
        goto free_pages;
    }

    /* step 3: Add replica page to LRU*/
    insert_replica_lru(page_replica);

    /* Step 4: Unmap original page */
    if (mapping)
        unmap_mapping_pages(mapping, index, 1 << order, false);

    /* Step 5: Set struct page infomation */
    page_replica->memcg_data = page_original;
    page_original->private = page_replica;

    page_replica->mapping = mapping;
    page_replica->index = index;
    page_replica->private = page_original->private & ~SWMC_TAG_MASK; // copy private data except tag bits
    page_replica->private = page_replica->private | SWMC_TAG_REPLICA_SELF | SWMC_TAG_ACCESS;

    pr_info("[Info]%s: Created page replica (order=%u, pfn=0x%lx, original_pfn=0x%lx)\n",
            __func__, order, page_to_pfn(page_replica), page_to_pfn(page_original));

    return 0;

free_pages:
    __free_pages(page_replica, order);
    track_page_free(order);
    return err;
}

/* Writeback page replica data to original page
 * This is used in page_coherence.c too.
 */
int writeback_page_replica(struct page *page_replica)
{
    int order = 0;
    struct page *page_original = get_original_opt(page_replica);

    if (!page_original) {
        pr_err("[Err]%s: Original page is NULL for replica page %p\n", __func__, page_replica);
        return -EINVAL;
    }

    pr_info("[Info]%s: Writing back replica page %p to original page %p\n",
            __func__, page_replica, page_original);
    
    /* Step 1: Copy data from original to replica */
    copy_data_page(page_replica, page_original, order);

    /* Step 2: Flush cachelines */
    pr_info("[Info]%s: Flushing dcache for original page %p\n",
            __func__, page_original);
    flush_dcache_page(page_original);
    return 0;
}

int flush_page_replica(struct page *page_replica)
{
    int order = 0;
    int err;

    /* Step 1-2: Writeback page replica */
    err = writeback_page_replica(page_replica);
    if (err) {
        pr_err("[Err]%s: Failed to writeback replica page %p: %d\n",
                __func__, page_replica, err);
        return err;
    }

    struct page *page_original = get_original_opt(page_replica);

    /* Step 3: Set struct page information */
    page_original->private = page_replica->private & ~SWMC_TAG_MASK; // copy private data except tag bits
    page_original->private = page_original->private | SWMC_TAG_ACCESS;
    page_original->mapping = page_replica->mapping;
    page_original->index = page_replica->index;

    if (PageModified(page_replica) && PageShared(page_replica)) {
        pr_info("[Info]%s: Page replica 0x%lx is stale shared page, skip replication\n",
                __func__, page_to_pfn(page_replica));
        goto free_pages;
    }
    // if (PageModified(page_replica))
    //     SetPageModified(page_original);
    // if (PageShared(page_replica))
    //     SetPageShared(page_original);
    // if (PageCoherence(page_replica))
    //     SetPageCoherence(page_original);

    page_replica->private = 0; // clear private data
    page_replica->memcg_data = NULL;

    /* step 4: Remove replica page to LRU*/
    remove_replica_lru(page_replica);

    /* Step 5: Unmap page replica */
    struct address_space *mapping = page_original->mapping;
    pgoff_t index = page_original->index;
    unmap_mapping_pages(mapping, index, 1 << order, false);

    /* Step 6: Free page replica */
free_pages:
    __free_pages(page_replica, order);
    track_page_free(order);

    pr_info("[Info]%s: Successfully wrote back replica page %p to original pfn %lu\n",
            __func__, page_replica, page_to_pfn(page_original));

    return 0;
}

/* ========================================================================
 * Page Replication Daemon
 * ======================================================================== */

/*
 * 전제:
 * - struct page의 private 필드의 MSB부터 32bit를 access count로 사용.
 * - struct page의 private 필드의 LSB 2bit는 태그로 사용.
 * - struct page의 private 필드의 16~31 bit는 last_accessed_age로 사용.
 * - replica의 경우, original page의 private 필드는 replica page의 포인터로 사용.
 *
 * 대충 workflow
 * 1. PEBS 이벤트로 sampling마다 (original, replica에 관계없이) 
 *    1-1. access count +1
 *    1-2. last_accessed_age - monitoring_age 만큼 access count를 shift. (/2^n)
 *    1-3. access count가 hotness_threshold 넘는 page들은 replication_candidate에 추가
 * 2. replication_interval마다
 *    2-1. active_list_lru와 inactive_list_lru를 전부 돌면서, hotness_threshold를 못넘은 page들은 list에서 제거하고 eviction_list에 추가
 *    2-2. replication_candidate를 돌면서, element가 replication 되어있지 않다면 (original page라면) replication_list에 추가
 *    2-3. eviction_list는 flush, replication_list는 replication 수행.
 *    2-4. monitoring_age를 1 증가시킴.
 * 3. histogram 기반으로 hotness threshold 정함.
 * 4. histogram을 전부 1/2로 줄임.
 */

unsigned long hist [32];
unsigned long hotness_threshold = 10; // MSB index 기준.
unsigned long monitoring_age = 0;
unsigned long replication_interval = 60; // seconds // TODO: sysfs로 설정 가능하게 만들기
unsigned long hot_page_percentile = 20; // 상위 20%를 hot으로 간주. TODO: sysfs로 설정 가능하게 만들기

/* 각 페이지를 담을 리스트의 '노드' 구조체 */
struct page_list_node {
    struct page *page;
    struct list_head list; // 리스트 연결을 위한 멤버
};

/* 전역 변수 선언 방식 변경
 * - replication_candidate: 리스트의 시작점(head)
 * - eviction_list: 제거 후보 리스트의 시작점
 * - replication_list: 복제 리스트의 시작점
 */
LIST_HEAD(replication_candidate);
LIST_HEAD(eviction_list);
LIST_HEAD(replication_list);

/* 리스트 해제 함수 - 모든 노드를 메모리에서 해제 */
void free_page_list(struct list_head *head)
{
    struct page_list_node *node, *tmp;
    
    list_for_each_entry_safe(node, tmp, head, list) {
        list_del(&node->list);
        kfree(node);
    }
}

/* 리스트에 페이지 추가 */
void add_page_to_list(struct list_head *head, struct page *page)
{
    struct page_list_node *node;
    
    node = kmalloc(sizeof(struct page_list_node), GFP_KERNEL);
    if (!node) {
        pr_err("[Err]%s: Failed to allocate page list node\n", __func__);
        return;
    }
    
    node->page = page;
    INIT_LIST_HEAD(&node->list);
    list_add_tail(&node->list, head);
}

/* 리스트 클리어 - 모든 노드 삭제 */
void clear_page_list(struct list_head *head)
{
    struct page_list_node *node, *tmp;
    
    list_for_each_entry_safe(node, tmp, head, list) {
        list_del(&node->list);
        kfree(node);
    }
}


static void get_eviction_list(struct list_head *eviction_list, unsigned long threshold)
{
    unsigned long flags;
    struct page *page;
    
    spin_lock_irqsave(&replica_lru_lock, flags);
    
    list_for_each_entry(page, &replica_active_lru, lru) {
        unsigned long v = READ_ONCE(page->private);
        unsigned int access_count = swmc_access_count(v);
        int msb_index = fls64(access_count) - 1; // fls64 returns 1-based index
        if (msb_index < threshold) {
            add_page_to_list(eviction_list, page);
            remove_replica_lru(page);
        }
    }
    
    list_for_each_entry(page, &replica_inactive_lru, lru) {
        unsigned long v = READ_ONCE(page->private);
        unsigned int access_count = swmc_access_count(v);
        int msb_index = fls64(access_count) - 1; // fls64 returns 1-based index
        if (msb_index < threshold) {
            add_page_to_list(eviction_list, page);
            remove_replica_lru(page);
        }
    }
    
    spin_unlock_irqrestore(&replica_lru_lock, flags);
}

static int evict_pages(struct list_head *eviction_list)
{
    struct page_list_node *node, *tmp;
    int err;

    list_for_each_entry_safe(node, tmp, eviction_list, list) {
        struct page *page = node->page;
        err = flush_page_replica(page);
        if (err) {
            pr_err("[Err]%s: Failed to flush page replica %p: %d\n", __func__, page, err);
        }
    }

    clear_page_list(eviction_list);
    return 0;
}

static int replicate_pages(struct list_head *replication_list)
{
    struct page_list_node *node, *tmp;
    int err;

    list_for_each_entry_safe(node, tmp, replication_list, list) {
        struct page *page = node->page;
        err = create_page_replica(page, 0); // order 0
        if (err) {
            pr_err("[Err]%s: Failed to create page replica for %p: %d\n", __func__, page, err);
        }
    }

    clear_page_list(replication_list);
    return 0;
}

int handle_sampled_address(unsigned long virt_addr, unsigned int pid)
{
    struct page *page;
    unsigned long pfn;
    unsigned long v;
    unsigned int access_count;
    unsigned short last_accessed_age;
    unsigned long new_access_count;
    unsigned long flags;

    // get mm_struct from pid to tranlate virtual address to physical page
    struct task_struct *task;
    struct mm_struct *mm;
    int ret;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        pr_err("[Err]%s: Could not find task for pid %d\n", __func__, pid);
        return -EINVAL;
    }
    get_task_struct(task); // task_struct의 참조 카운트 증가
    rcu_read_unlock();

    // 2. task_struct에서 mm_struct 가져오기
    mm = get_task_mm(task);
    if (!mm) {
        pr_warn("[Err]%s: Could not get mm_struct for pid %d\n", __func__, pid);
        put_task_struct(task);
        return -EINVAL;
    }

    ret = get_user_pages_remote(
        mm,         // 대상 메모리 디스크립터
        virt_addr,      // 찾고자 하는 가상 주소
        1,          // 가져올 페이지 개수
        FOLL_WRITE,          // 읽기/쓰기 플래그
        &page,      // 결과를 저장할 struct page* 배열
        NULL
    );

    // get_user_pages_remote는 성공 시 가져온 페이지 수를 반환
    if (ret <= 0) {
        // 주소가 매핑되지 않았거나, 스왑 아웃되었거나, 잘못된 주소인 경우
        pr_err("[Err]%s: vaddr 0x%lx not mapped for pid %d\n", __func__, virt_addr, pid);
        mmput(mm);
        put_task_struct(task);
        return -EINVAL;
    }

    pr_info("[Info]%s: Sampled vaddr=0x%lx for pid=%d maps to page pfn=0x%lx\n",
            __func__, virt_addr, pid, page_to_pfn(page));

    // pfn = phys_addr >> PAGE_SHIFT;
    // page = pfn_to_page(pfn);
    if (!page) {
        // pr_err("[Err]%s: Invalid page for phys_addr=0x%lx\n", __func__, phys_addr);
        pr_err("[Err]%s: Could not get page for vaddr=0x%lx, pid=%d\n",
            __func__, virt_addr, pid);
        mmput(mm);
        put_task_struct(task);
        return -EINVAL;
    }
    if (!PageCoherence(page)) {
        pr_info("[Info]%s: Page 0x%lx is not coherence-enabled, skipping\n", __func__, pfn);
        mmput(mm);
        put_task_struct(task);
        return -EINVAL;
    }

    v = READ_ONCE(page->private);
    access_count = swmc_access_count(v);
    last_accessed_age = swmc_last_accessed_age(v);

    /* Age the access count based on last accessed age */
    if (monitoring_age > last_accessed_age) {
        unsigned short age_diff = monitoring_age - last_accessed_age;
        access_count >>= age_diff; // Divide by 2^age_diff
    }

    /* Increment access count */
    new_access_count = (unsigned long)access_count + 1;

    /* Update last accessed age */
    last_accessed_age = monitoring_age;

    /* Update the private field */
    page->private = (new_access_count << 32) | ((unsigned long)last_accessed_age << 16) | (v & SWMC_TAG_MASK);

    /* Update histogram */
    int new_msb_index = fls64(new_access_count) - 1; // fls64 returns 1-based index
    int old_msb_index = fls64(access_count) - 1;
    if (new_msb_index != old_msb_index) {
        hist[old_msb_index]--;
        hist[new_msb_index]++;
    }

    /* Check if access count exceeds hotness threshold */
    if (new_msb_index >= hotness_threshold) {
        add_page_to_list(&replication_candidate, page);
    }

    // 5. 작업 완료 후 페이지 참조 카운트 해제
    // 매우 중요! 그렇지 않으면 메모리 누수가 발생합니다.
    put_page(page);

    // 6. 사용이 끝난 구조체들의 참조 카운트 해제
    mmput(mm);
    put_task_struct(task);
    return 0;
}

static unsigned long calculate_hotness_threshold(unsigned long percentile)
{
    unsigned long total_samples = 0;
    unsigned long target_samples;
    unsigned long cumulative_samples = 0;
    int i;

    /* Calculate total samples */
    for (i = 0; i < 32; i++) {
        total_samples += hist[i];
    }

    if (total_samples == 0) {
        pr_info("[Info]%s: No samples collected yet, using default threshold\n", __func__);
        return hotness_threshold; // No samples yet
    }

    target_samples = (total_samples * percentile) / 100;

    /* Find the hotness threshold */
    for (i = 31; i >= 0; i--) {
        cumulative_samples += hist[i];
        if (cumulative_samples >= target_samples) {
            pr_info("[Info]%s: New hotness threshold calculated: %d (cumulative_samples=%lu)\n",
                    __func__, i, cumulative_samples);
            return i;
        }
    }

    pr_info("[Info]%s: Using lowest hotness threshold (0)\n", __func__);
    return 0; // Fallback to lowest threshold
}

#define CPUS_PER_SOCKET 16
#define BUFFER_SIZE	4096 /* 128: 1MB */

/* pebs events */
#define DRAM_LLC_LOAD_MISS  0x1d3
#define REMOTE_DRAM_LLC_LOAD_MISS   0x2d3
#define NVM_LLC_LOAD_MISS   0x80d1
#define ALL_STORES	    0x82d0
#define ALL_LOADS	    0x81d0
#define STLB_MISS_STORES    0x12d0
#define STLB_MISS_LOADS	    0x11d0
#define LLC_LOAD_MISS 0x20d1

enum events {
    ALL_LOAD = 0,
    ALL_STORE = 1,
    N_PEBSEVENTS
};

static __u64 get_pebs_event(enum events e)
{
    switch (e) {
        case ALL_LOAD:
            return ALL_LOADS;
        case ALL_STORE:
            return ALL_STORES;
        default:
            return N_PEBSEVENTS;
    }
}

static struct task_struct *replcation_daemon;
static struct perf_event ***mem_event;

struct pebs_sample {
    struct perf_event_header header;
    __u64 ip;
    __u32 pid, tid;
    __u64 addr;
    __u64 phys_addr;
};

static int __perf_event_open(__u64 config, __u64 cpu, __u64 type, int sampling_interval)
{
    struct perf_event_attr attr;
    struct file *file;
    int event_fd;

    memset(&attr, 0, sizeof(struct perf_event_attr));

    attr.type = PERF_TYPE_RAW;
    attr.size = sizeof(struct perf_event_attr);
    attr.config = config;
	attr.sample_period = sampling_interval;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR;
    attr.disabled = 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_callchain_kernel = 1;
    attr.exclude_callchain_user = 1;
    attr.precise_ip = 1;
    attr.enable_on_exec = 1;
    // attr.inherit = 1;

    event_fd = swmc__perf_event_open(&attr, -1, cpu, -1, 0);
    if (event_fd <= 0) {
        pr_err("[Err]%s: event_fd: %d\n", __func__, event_fd);
        return -1;
    }

    file = fget(event_fd);
    if (!file) {
        pr_err("[Err]%s: invalid file\n", __func__);
        return -1;
    }
    mem_event[cpu][type] = fget(event_fd)->private_data;
    return 0;
}

static int __pebs_init(int sampling_interval)
{
    int cpu, event;

    mem_event = kzalloc(sizeof(struct perf_event **) * CPUS_PER_SOCKET, GFP_KERNEL);
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
	mem_event[cpu] = kzalloc(sizeof(struct perf_event *) * N_PEBSEVENTS, GFP_KERNEL);
    }

    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {       
        // to disable PEBS of node 1 cpus
        if ((cpu >= 16 && cpu < 32) || (cpu >= 48 && cpu < 64)) {
            continue;
        }
        for (event = 0; event < N_PEBSEVENTS; event++) {
            if (get_pebs_event(event) == N_PEBSEVENTS) {
            mem_event[cpu][event] = NULL;
            continue;
            }
            if (__perf_event_open(get_pebs_event(event), cpu, event, sampling_interval)) return -1;
            if (swmc__perf_event_init(mem_event[cpu][event], BUFFER_SIZE)) return -1;
        }
    }
    return 0;
}

static void __pebs_cleanup(void)
{
    int cpu, event;
    pr_info("[Info]%s: Cleaning up PEBS events\n", __func__);
    for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
        for (event = 0; event < N_PEBSEVENTS; event++) {
            if (mem_event[cpu][event]) {
                pr_info("[Info]%s: Disabling PEBS event for CPU %d, event %d\n", __func__, cpu, event);
                perf_event_disable(mem_event[cpu][event]);
            }
        }
    }
}

static void add_rand_pages_to_replication_candidate(void)
{
    unsigned long nr_free_pages = 96UL * 1024 * 1024 * 1024 / PAGE_SIZE; // assume 96GB free memory

    unsigned long num_rand_pages = nr_free_pages * hot_page_percentile / 100; // use up to 80% of free pages

    unsigned long cxl_hdm_base = get_cxl_hdm_base();

    unsigned long cxl_hdm_base_pfn = cxl_hdm_base >> PAGE_SHIFT;

    unsigned long start_pfn = cxl_hdm_base_pfn + 1024 * 512; // skip first 2GB
    unsigned long end_pfn = cxl_hdm_base_pfn + num_rand_pages;

    pr_info("[Info]%s: Adding random pages, nr_free_pages=%lu, num_rand_pages=%lu, start_pfn=0x%lx, end_pfn=0x%lx\n",
            __func__, nr_free_pages, num_rand_pages, start_pfn, end_pfn);

    for (unsigned long i = start_pfn; i < end_pfn; i += 1) {
        struct page *page = pfn_to_page(i);
        add_page_to_list(&replication_candidate, page);
    }
}


static int kreplicationd(void *data)
{
    pr_info("[Info]%s: kreplicationd thread started\n", __func__);
    float useful_sample_ratio;
    int nr_sample, nr_incxl, nr_outcxl, nr_throttle, nr_unthrottle, nr_lost, nr_none; //add counter for invalid va sample events
    nr_sample = nr_incxl = nr_outcxl = nr_throttle = nr_unthrottle = nr_lost = nr_none = 0;

    //set clock for replication interval
    unsigned long last_replication_time = jiffies;
    unsigned long current_time;
    while (!kthread_should_stop()) {
        int cpu, event, cond = false;
        
        for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
            for (event = 0; event < N_PEBSEVENTS; event++) {
                do {
                    struct perf_buffer *rb;
                    struct perf_event_mmap_page *up;
                    struct perf_event_header *ph;
                    struct pebs_sample *te;
                    unsigned long pg_index, offset;
                    int page_shift;
                    __u64 head;

                    if (!mem_event[cpu][event]) {
                        pr_err("[Err]%s: mem_event[%d][%d] is NULL\n", __func__, cpu, event);
                        break;
                    }

                    __sync_synchronize();

                    rb = mem_event[cpu][event]->rb;
                    if (!rb) {
                        pr_err("[Err]%s: rb is NULL for cpu %d, event %d\n", __func__, cpu, event);
                        return -1;
                    }

                    up = READ_ONCE(rb->user_page);
                    head = READ_ONCE(up->data_head);
                    if (head == up->data_tail) {
                        // pr_info("[Info]%s: No new data in buffer for cpu %d, event %d\n", __func__, cpu, event);
                        break;
                    }

                    head -= up->data_tail;
                    if (head > (BUFFER_SIZE * 50 / 100)) {
                        cond = true;
                    } else if (head < (BUFFER_SIZE * 10 / 100)) {
                        cond = false;
                    }

                    smp_rmb();

                    page_shift = PAGE_SHIFT + page_order(rb);
                    offset = READ_ONCE(up->data_tail);
                    pg_index = (offset >> page_shift) & (rb->nr_pages - 1);
                    offset &= (1 << page_shift) - 1;

                    ph = (void*)(rb->data_pages[pg_index] + offset);
                    nr_sample++;
                    switch (ph->type) {
                    case PERF_RECORD_SAMPLE:
                        te = (struct pebs_sample *)ph;
                        pr_info("[Info]%s: PEBS sample: ip=0x%llx, pid=%d, tid=%d, addr=0x%llx, phys_addr=0x%llx\n",
                            __func__, te->ip, te->pid, te->tid, te->addr, te->phys_addr);
                        // if(!handle_sampled_address(te->phys_addr)) {
                        pr_info("[Info]%s: PEBS sample: ip=0x%llx, pid=%d, tid=%d, addr=0x%llx\n",
                            __func__, te->ip, te->pid, te->tid, te->addr);
                        if(!handle_sampled_address(te->addr, te->pid)) {
                            nr_incxl++;
                        } else {
                            nr_outcxl++;
                        }
                        break;
                    case PERF_RECORD_THROTTLE:
                        nr_throttle++;
                        break;
                    case PERF_RECORD_UNTHROTTLE:
                        nr_unthrottle++;
                        break;
                    case PERF_RECORD_LOST_SAMPLES:
                        nr_lost++;
                        break;
                    default:
                        nr_none++;
                        break;
                    }
                    smp_mb();
                    WRITE_ONCE(up->data_tail, up->data_tail + ph->size);
                } while (cond);
            }
        }
        msleep_interruptible(100);
        if (time_after(jiffies, last_replication_time + msecs_to_jiffies(replication_interval * 1000))) {
            pr_info("[Info]%s: Replication interval reached, processing replication candidates\n", __func__);

            add_rand_pages_to_replication_candidate();

            // Step 2-1: Process active and inactive replica lists for eviction
            get_eviction_list(&eviction_list, hotness_threshold);
            evict_pages(&eviction_list);

            // Step 2-2: Process replication candidates
            // 한 번의 순회로 처리: replica가 없으면 replication_list로 이동, 있으면 노드 삭제
            struct page_list_node *node, *tmp;
            list_for_each_entry_safe(node, tmp, &replication_candidate, list) {
                struct page *page = node->page;
                struct page *replica = get_replica_opt(page);
                
                if (!replica) {
                    // replica가 없으면 replication_list로 이동
                    list_move_tail(&node->list, &replication_list);
                } else {
                    // replica가 이미 있으면 노드 삭제
                    list_del(&node->list);
                    kfree(node);
                }
            }
            // replication_candidate는 이제 비어있음 (모두 이동되거나 삭제됨)

            // Step 2-3: Replicate pages
            replicate_pages(&replication_list);

            // Step 2-4: Increment monitoring age
            monitoring_age++;

            // Step 2-5: Set hotness threshold 
            hotness_threshold = calculate_hotness_threshold(hot_page_percentile);

            // Step 2-6: Cool down histogram
            for (int j = 1; j < 32; j++) {
                hist[j-1] += hist[j];
                hist[j] = 0;
            }

            // Step 2-7: Update last replication time
            last_replication_time = jiffies;
        }
    }
    pr_info("[Info]%s: PEBS sample stats: total=%d, incxl=%d, outcxl=%d, throttle=%d, unthrottle=%d, lost=%d, none=%d\n",
        __func__, nr_sample, nr_incxl, nr_outcxl, nr_throttle, nr_unthrottle, nr_lost, nr_none);
    pr_info("[Info]%s: kreplicationd thread stoped\n", __func__);
    
    /* Cleanup: free all remaining nodes in the lists */
    free_page_list(&replication_candidate);
    free_page_list(&eviction_list);
    free_page_list(&replication_list);
    
    return 0;
}

int swmc_replicationd_start(int sampling_interval)
{
    pr_info("[Info]%s: Initializing replication daemon\n", __func__);

    if (__pebs_init(sampling_interval)){
        pr_err("[Error]%s: Failed to initialize PEBS module\n", __func__);
        return -EINVAL;
    }

    if (replcation_daemon) {
        pr_err("[Error]%s: Access sampling task already running\n", __func__);
        return -EBUSY;
    }

    replcation_daemon = kthread_run(kreplicationd, NULL, "kreplicationd");
    if (IS_ERR(replcation_daemon)) {
        pr_err("[Error]%s: Failed to create access sampling task\n", __func__);
        __pebs_cleanup();
        return PTR_ERR(replcation_daemon);
    }

    pr_info("[Info]%s: Replication daemon started successfully\n", __func__);
    return 0;
}

void swmc_replicationd_stop(void)
{
    pr_info("[Info]%s: Stopping replication daemon\n", __func__);

    if (replcation_daemon) {
        kthread_stop(replcation_daemon);
        replcation_daemon = NULL;
        pr_info("[Info]%s: Replication daemon stopped\n", __func__);
    } else {
        pr_warn("[Warning]%s: Replication daemon not running\n", __func__);
    }

    __pebs_cleanup();
}

SYSCALL_DEFINE2(replication_start, int, sampling_interval, int, hot_page_percentage)
{ 
	int ret;

    hot_page_percentile = hot_page_percentage;
    
    ret = swmc_replicationd_start(sampling_interval); 

	return ret;
}

SYSCALL_DEFINE0(replication_stop)
{ 
    swmc_replicationd_stop(); 

    return 0;
}

static int __init page_replication_init(void)
{
    return page_replica_sysfs_init();
}

subsys_initcall(page_replication_init);

/* ========================================================================
 * Public API for page coherence integration
 * ======================================================================== */

int fetch_page_replica(struct page *original)
{
    int err;
    size_t size = PAGE_SIZE; // For order 0
    struct page *page_replica;
    page_replica = get_replica_opt(original);

    if (!page_replica) {
        pr_err("[Err]%s: Invalid page replica pointer\n", __func__);
        return -1;
    }

    /* Step 2: Copy data from source to replica using unified helper */
    err = copy_data_page(page_replica, original, 0);
    if (err) {
        pr_err("[Err]%s: Data copy failed: %d\n", __func__, err);
        return err;
    }

    return 0;
}

