#ifndef _LINUX_PAGE_COHERENCE_H
#define _LINUX_PAGE_COHERENCE_H

#include <linux/mm.h>
#include <linux/pfn_t.h>
#include <linux/iomap.h>

struct vm_fault;
struct iomap_iter;

#ifdef CONFIG_PAGE_COHERENCE

/**
 * page_coherence_fault - Handle page coherence faults
 * @vmf: Fault information structure
 * @iter: IOMAP iterator
 * @size: Size of the fault (PAGE_SIZE or PMD_SIZE)
 * @kaddr: Kernel virtual address of the original page
 * @pfn: Pointer to the page frame number, will be updated to replica PFN
 *
 * This function handles page coherence faults by creating a replica page
 * and updating the PFN to point to the replica instead of the original.
 * The replica pages are added to a page cache-like structure to make them
 * reclaimable by kswapd.
 *
 * Returns: 0 on success, negative error code on failure
 */
int page_coherence_fault(struct vm_fault *vmf, const struct iomap_iter *iter,
			 size_t size, void *kaddr, pfn_t *pfn);

/**
 * page_coherence_init - Initialize page coherence subsystem
 *
 * Returns: 0 on success, negative error code on failure
 */
int page_coherence_init(void);

#else /* !CONFIG_PAGE_COHERENCE */

static inline int page_coherence_fault(struct vm_fault *vmf, 
				       const struct iomap_iter *iter,
				       size_t size, void *kaddr, pfn_t *pfn)
{
	return 0;
}

static inline int page_coherence_init(void)
{
	return 0;
}

#endif /* CONFIG_PAGE_COHERENCE */

#endif /* _LINUX_PAGE_COHERENCE_H */
