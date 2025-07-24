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
#include <linux/page_coherence.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/iomap.h>
#include <linux/gfp.h>
#include <linux/printk.h>
#include <linux/memcontrol.h>
#include <linux/hugetlb.h>    /* pfn_pmd, pmd_mkdirty, set_pmd_at */

#ifdef CONFIG_PAGE_COHERENCE

/**
 * allocate_replica_page - Allocate a new page or compound page for replica
 * @order: Page order (0 for single page, PMD_ORDER for 2MB)
 *
 * Returns: Allocated page pointer on success, NULL on failure
 */
static struct page *allocate_replica_page(unsigned int order)
{
    struct page *page;
    gfp_t gfp_flags = GFP_HIGHUSER_MOVABLE;

    /* compound allocation for huge pages */
    if (order > 0)
        gfp_flags |= __GFP_COMP;

    page = alloc_pages(gfp_flags, order);
    if (!page) {
        pr_err("[page_coherence] Failed to allocate replica page (order=%u)\n", order);
        return NULL;
    }

    pr_info("[page_coherence] Allocated replica page (order=%u, pfn=%lu)\n",
            order, page_to_pfn(page));
    return page;
}

/**
 * copy_page_data - Copy data from source to destination page
 * @dst_page: Destination page
 * @src_kaddr: Source kernel virtual address
 * @size: Size to copy (PAGE_SIZE or PMD_SIZE)
 *
 * Returns: 0 on success, negative error code on failure
 */
static int copy_page_data(struct page *dst_page, void *src_kaddr, size_t size)
{
    void *dst_kaddr;
    unsigned int nr_pages = size >> PAGE_SHIFT;
    unsigned int i;

    if (!dst_page || !src_kaddr) {
        pr_err("[page_coherence] Invalid parameters for page copy\n");
        return -EINVAL;
    }

    pr_info("[page_coherence] Copying %zu bytes (%u pages) from %p to pfn=%lu\n",
            size, nr_pages, src_kaddr, page_to_pfn(dst_page));

    if (nr_pages == 1) {
        dst_kaddr = kmap_local_page(dst_page);
        if (!dst_kaddr) {
            pr_err("[page_coherence] Failed to map destination page\n");
            return -ENOMEM;
        }
        memcpy(dst_kaddr, src_kaddr, PAGE_SIZE);
        kunmap_local(dst_kaddr);
    } else {
        for (i = 0; i < nr_pages; i++) {
            struct page *subpage = nth_page(dst_page, i);
            void *src_offset = src_kaddr + (i * PAGE_SIZE);

            dst_kaddr = kmap_local_page(subpage);
            if (!dst_kaddr) {
                pr_err("[page_coherence] Failed to map subpage %u\n", i);
                return -ENOMEM;
            }
            memcpy(dst_kaddr, src_offset, PAGE_SIZE);
            kunmap_local(dst_kaddr);
        }
    }

    pr_info("[page_coherence] Successfully copied page data\n");
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
                         size_t size, void *kaddr, pfn_t *pfn)
{
    struct page *replica_page;
    unsigned int order;
    pfn_t original_pfn = *pfn;
    pfn_t replica_pfn;
    int ret;
    bool write = iter->flags & IOMAP_WRITE;

    if (!vmf || !iter || !kaddr || !pfn) {
        pr_err("[page_coherence] Invalid parameters\n");
        return -EINVAL;
    }

    /* Determine page order */
    if (size == PMD_SIZE)
        order = PMD_ORDER;
    else if (size == PAGE_SIZE)
        order = 0;
    else {
        pr_err("[page_coherence] Unsupported size: %zu\n", size);
        return -EINVAL;
    }

    pr_info("[page_coherence] Fault at 0x%lx: size=%zu order=%u original pfn=%lu\n",
            vmf->address, size, order, pfn_t_to_pfn(original_pfn));

    // if pmd is already allocated, get the existing pmd.
    pmd_t *pmd = vmf->pmd;
    pr_info("[page_coherence] vmf->pmd: %p\n", pmd);
    pmd_t pmd_val = *pmd;
    if (!pmd_none(pmd_val)) {
        pr_info("[page_coherence] PMD already allocated.\n");
        // check if pmd is valid
        if (pmd_present(pmd_val)) {
            pr_info("[page_coherence] PMD is valid.\n");
        } else {
            pr_info("[page_coherence] PMD is invalid.\n");
        }
        // check if pmd is writable
        if (pmd_write(pmd_val)) {
            pr_info("[page_coherence] PMD is writable.\n");
        } else {
            pr_info("[page_coherence] PMD is not writable.\n");
        }
        // check if pmd is dirty
        if (pmd_dirty(pmd_val)) {
            pr_info("[page_coherence] PMD is dirty.\n");
        } else {
            pr_info("[page_coherence] PMD is not dirty.\n");
        }
        // check the pfn of the pmd
        unsigned long pmd_pfn_val = pmd_pfn(pmd_val);
        pr_info("[page_coherence] PMD PFN is %lu\n", pmd_pfn_val);
        
        // check the reason of the fault
        if (vmf->flags & FAULT_FLAG_WRITE) {
            pr_info("[page_coherence] Fault is a write fault.\n");
        } else {
            pr_info("[page_coherence] Fault is a read fault.\n");
        }

        // check if iomap fault is write
        if (write) {
            pr_info("[page_coherence] IOMAP fault is a write fault.\n");
        } else {
            pr_info("[page_coherence] IOMAP fault is a read fault.\n");
        }

        // if fault is write fault, and pmd is not writable, and pmd is not dirty, skip replication and copying
        if (pmd_present(pmd_val) && write && !pmd_write(*pmd) && !pmd_dirty(*pmd)) {
            pr_info("[page_coherence] Skipping replication and copying for write fault on non-writable PMD.\n");
            replica_pfn.val = pmd_pfn(pmd_val) | (original_pfn.val & PFN_FLAGS_MASK);
            *pfn = replica_pfn;
            pr_info("[page_coherence] Updated pfn to existing PMD PFN: %lu\n",
                    pfn_t_to_pfn(replica_pfn));
            return 0;
        }
    }   

    /* Allocate a replica page or compound page */
    replica_page = allocate_replica_page(order);
    if (!replica_page)
        return -ENOMEM;

    /* Copy data from original address to replica */
    ret = copy_page_data(replica_page, kaddr, size);
    if (ret) {
        pr_err("[page_coherence] Data copy failed: %d\n", ret);
        if (order > 0)
            __free_pages(replica_page, order);
        else
            __free_page(replica_page);
        return ret;
    }

    /* Build new PFN with preserved flags and update */
    replica_pfn.val = page_to_pfn(replica_page) |
                      (original_pfn.val & PFN_FLAGS_MASK);
    *pfn = replica_pfn;

    pr_info("[page_coherence] Replicated pfn=%lu at 0x%lx\n",
            pfn_t_to_pfn(replica_pfn), vmf->address);
    return 0;
}

#endif /* CONFIG_PAGE_COHERENCE */
