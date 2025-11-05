#ifndef _LINUX_PAGE_COHERENCE_H
#define _LINUX_PAGE_COHERENCE_H

#include <linux/mm.h>
#include <linux/pfn_t.h>
#include <linux/iomap.h>

struct vm_fault;
struct iomap_iter;

#ifdef CONFIG_PAGE_COHERENCE


/* Error codes for replica operations */
enum replica_error {
    REPLICA_SUCCESS = 0,
    REPLICA_SHARED_STATE = 1,
    REPLICA_ERROR_NOMEM = -ENOMEM,
    REPLICA_ERROR_INVAL = -EINVAL,
    REPLICA_ERROR_EXIST = -EEXIST,
    REPLICA_ERROR_NOENT = -ENOENT,
    REPLICA_ERROR_LOCK = -EAGAIN, // Error code for locking issues
    REPLICA_ERROR_ANY = -1, // Generic error code for any failure
};

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
			 size_t size, void *kaddr, pfn_t *pfn, pfn_t *pfnp);

/**
 * page_coherence_init - Initialize page coherence subsystem
 *
 * Returns: 0 on success, negative error code on failure
 */
int page_coherence_init(void);

/**
 * set_cxl_hdm_base - Set the CXL HDM base address
 * @base_addr: CXL HDM base physical address
 *
 * This function allows external modules to set the CXL HDM base address
 * during their initialization phase.
 */
void set_cxl_hdm_base(unsigned long base_addr);

/**
 * get_cxl_hdm_base - Get the current CXL HDM base address
 *
 * Returns: Current CXL HDM base address
 */
unsigned long get_cxl_hdm_base(void);

/* page_replication.c */
struct page *get_replica_opt(struct page *original_page);
void print_page_info(struct page *page, const char *context);
// bool check_page_replica_dirty(struct page *page_replica);
// struct page *create_page_replica(unsigned int order, pfn_t original_pfn, void *src_kaddr);
// int destroy_page_replica(struct page *page_replica);
int writeback_page_replica(struct page *page_replica);
int fetch_page_replica(struct page *original);
// struct page *get_page_replica_with_ref(pfn_t original_pfn, unsigned int order);
// void put_page_replica_ref(struct page *page_replica);
// int make_page_replica_invalid(struct page *page_replica);
// bool is_page_replica_invalid(struct page *page_replica);
// int __flush_page_replica(struct page *page_replica);
// int make_page_replica_dirty(struct page *page_replica);


// /* Replica folio management functions from page_replication.c */
// struct folio *replica_folio_create(unsigned int order, pfn_t original_pfn, 
//                                   void *src_kaddr, size_t size);
// struct folio *replica_folio_find(pfn_t original_pfn);
// void replica_folio_put(struct folio *replica_folio);
// int replica_folio_unmap_and_free(struct folio *replica_folio);
// int replica_folio_writeback(struct folio *replica_folio);
// int replica_folio_flush(struct folio *replica_folio);
// int replica_folio_write_protect(struct folio *replica_folio);

#else /* !CONFIG_PAGE_COHERENCE */

static inline int page_coherence_fault(struct vm_fault *vmf, 
				       const struct iomap_iter *iter,
				       size_t size, void *kaddr, pfn_t *pfn, pfn_t *pfnp)
{
	return 0;
}

static inline int page_coherence_init(void)
{
	return 0;
}

static inline void set_cxl_hdm_base(unsigned long base_addr)
{
	/* No-op when page coherence is disabled */
}

static inline unsigned long get_cxl_hdm_base(void)
{
	return 0;
}

static inline struct folio *replica_folio_create(unsigned int order, pfn_t original_pfn, 
                                               void *src_kaddr, size_t size)
{
	return ERR_PTR(-ENODEV);
}

static inline struct folio *replica_folio_find(pfn_t original_pfn)
{
	return NULL;
}

static inline int replica_folio_unmap_and_free(struct folio *replica_folio)
{
	return 0;
}

static inline int replica_folio_writeback(struct folio *replica_folio)
{
	return 0;
}

static inline int replica_folio_flush(struct folio *replica_folio)
{
	return 0;
}

static inline int replica_folio_write_protect(struct folio *replica_folio)
{
	return 0;
}

#endif /* CONFIG_PAGE_COHERENCE */

#endif /* _LINUX_PAGE_COHERENCE_H */
