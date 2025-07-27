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

#ifdef CONFIG_PAGE_COHERENCE

/*
 * Page Coherence Management for CXL Shared Memory
 *
 * This file implements page coherence functionality for managing
 * replica pages in CXL shared memory environments.
 */

// initialize xarray for original pfn -> replica page mapping
static DEFINE_XARRAY(original_to_replica_xa);

// dummy base PA for CXL HDM 
unsigned long cxl_hdm_base = 0x1e80000000; // TODO: change this to set by messaging layer module with insmod


/* =============================================================================
 * MESSAGE HANDLING FUNCTIONS
 * ============================================================================= */

// Fetch message handling
// M-> S, S -> S, I -> I
static int swmc_kmsg_handle_fetch(struct swmc_kmsg_message *msg)
{
    int ret = 0;
    struct payload_data *payload = &msg->payload;
    // Validate message
    if (!msg || msg->header.type != SWMC_KMSG_TYPE_FETCH) {
        pr_err("[page_coherence] Invalid fetch message\n");
        return -EINVAL;
    }

    pr_info("[page_coherence] Handling fetch message for offset 0x%lx\n", payload->cxl_hdm_offset);

    // calculate original pfn from payload->cxl_hdm_offset
    unsigned long original_phys_addr = cxl_hdm_base + payload->cxl_hdm_offset;
    pfn_t original_pfn;

    if (payload->page_order == 0) {
        original_pfn = pfn_to_pfn_t(original_phys_addr >> PAGE_SHIFT);
    }
    else if (payload->page_order == PMD_ORDER) {
        original_pfn = pfn_to_pfn_t(original_phys_addr >> PMD_SHIFT);
    } else {
        pr_err("[page_coherence] Invalid page order: %d\n", payload->page_order);
        return -EINVAL;
    }

    // find replica page from original pfn
    struct page *replica_page = xa_load(&original_to_replica_xa, original_pfn.val);

    if (!replica_page) { // I -> I
        pr_info("[page_coherence] No replica page found (I->I transition)\n");
    } else { // M -> S, S -> S
        pr_info("[page_coherence] Replica page found (M->S or S->S transition)\n");
        
        // TODO: find all PTE mapped to replica page
        // TODO: if there is any PTE with R/W=1, change R/W=0
        // TODO: if there is no PTE with R/W=0
    }
    // send back fetch_ack_message using payload of original kmsg
    ret = swmc_kmsg_unicast(SWMC_KMSG_TYPE_FETCH_ACK, msg->header.ws_id, msg->header.from_nid, payload);

    return 0;
}

// Fetch_ack message handling
static int swmc_kmsg_handle_fetch_ack(struct swmc_kmsg_message *msg)
{
    // check if the message is valid
    if (!msg || msg->header.type != SWMC_KMSG_TYPE_FETCH_ACK) {
        pr_err("[page_coherence] Invalid fetch_ack message\n");
        return -EINVAL;
    }

    pr_info("[page_coherence] Handling fetch_ack message for offset 0x%lx\n", msg->payload.cxl_hdm_offset);

    // find the wait station by ID
    struct wait_station *ws = wait_station(msg->header.ws_id);
    if (!ws) {
        pr_err("[page_coherence] Invalid wait station ID: %d\n", msg->header.ws_id);
        return -EINVAL;
    }

    // Decrease pending count atomically
    if (atomic_dec_and_test(&ws->pendings_count)) {
        // All fetch ACKs received, wake up the wait station
        pr_info("[page_coherence] All fetch ACKs received for wait station %d\n", msg->header.ws_id);
        complete(&ws->pendings);
    } else {
        pr_info("[page_coherence] Fetch ACK received, pending count: %d\n",
                atomic_read(&ws->pendings_count));
    }

    return 0;
}

// Invalidate message handling
// M -> I, S -> I, I -> I (M -> I is violated)
static int swmc_kmsg_handle_invalidate(struct swmc_kmsg_message *msg)
{
    // Validate message
    if (!msg || msg->header.type != SWMC_KMSG_TYPE_INVALIDATE) {
        pr_err("[page_coherence] Invalid invalidate message\n");
        return -EINVAL;
    }

    struct payload_data *payload = &msg->payload;
    int ret = 0;

    pr_info("[page_coherence] Handling invalidate message for offset 0x%lx\n", payload->cxl_hdm_offset);
    
    // calculate original pfn from payload->cxl_hdm_offset
    unsigned long original_phys_addr = cxl_hdm_base + payload->cxl_hdm_offset;
    pfn_t original_pfn;

    if (payload->page_order == 0) {
        original_pfn = pfn_to_pfn_t(original_phys_addr >> PAGE_SHIFT);
    }
    else if (payload->page_order == PMD_ORDER) {
        original_pfn = pfn_to_pfn_t(original_phys_addr >> PMD_SHIFT);
    } else {
        pr_err("[page_coherence] Invalid page order: %d\n", payload->page_order);
        return -EINVAL;
    }

    // find replica page from original pfn
    struct page *replica_page = xa_load(&original_to_replica_xa, original_pfn.val);

    if (!replica_page) { // I -> I
        pr_info("[page_coherence] No replica page found (I->I transition)\n");
    }
    else { // M -> I, S -> I
        pr_info("[page_coherence] Replica page found (M->I or S->I transition)\n");
        
        // TODO: find all PTE mapped to replica page
        // TODO: if there is any PTE with R/W=1, violated (no direct I -> M) 
        // TODO: -> send error and kernel panic
        // TODO: if there is no PTE with R/W=1 (S -> I)
        // TODO: -> free replica page, change linked_page to NULL

        // For now, assume S->I transition and clean up
        xa_erase(&original_to_replica_xa, original_pfn.val);
        if (payload->page_order > 0)
            __free_pages(replica_page, payload->page_order);
        else
            __free_page(replica_page);
            
        pr_info("[page_coherence] Freed replica page and removed from xa\n");
    }

    // send back invalidate_ack_message using payload of original kmsg
    ret = swmc_kmsg_unicast(SWMC_KMSG_TYPE_INVALIDATE_ACK, msg->header.ws_id, msg->header.from_nid, payload);

    return 0;
}

// Invalidate_ack message handling
static int swmc_kmsg_handle_invalidate_ack(struct swmc_kmsg_message *msg)
{
    // check if the message is valid
    if (!msg || msg->header.type != SWMC_KMSG_TYPE_INVALIDATE_ACK) {
        pr_err("[page_coherence] Invalid invalidate_ack message\n");
        return -EINVAL;
    }

    pr_info("[page_coherence] Handling invalidate_ack message for offset 0x%lx\n", msg->payload.cxl_hdm_offset);

    // find the wait station by ID
    struct wait_station *ws = wait_station(msg->header.ws_id);
    if (!ws) {
        pr_err("[page_coherence] Invalid wait station ID: %d\n", msg->header.ws_id);
        return -EINVAL;
    }

    // Decrease pending count atomically
    if (atomic_dec_and_test(&ws->pendings_count)) {
        // All invalidate ACKs received, wake up the wait station
        pr_info("[page_coherence] All invalidate ACKs received for wait station %d\n", msg->header.ws_id);
        complete(&ws->pendings);
    } else {
        pr_info("[page_coherence] Invalidate ACK received, pending count: %d\n",
                atomic_read(&ws->pendings_count));
    }

    return 0;
}

// Error message handling
static int swmc_kmsg_handle_error(struct swmc_kmsg_message *msg)
{
    // Validate message
    if (!msg || msg->header.type != SWMC_KMSG_TYPE_ERROR) {
        pr_err("[page_coherence] Invalid error message\n");
        return -EINVAL;
    }

    pr_err("[page_coherence] Received error message from node %d for offset 0x%lx\n",
           msg->header.from_nid, msg->payload.cxl_hdm_offset);

    // TODO: Handle error appropriately
    return 0;
}


/* =============================================================================
 * PAGE COHERENCE FAULT HANDLING
 * ============================================================================= */

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
        pr_err("[page_coherence] Failed to get wait station\n");
        return -ENOMEM;
    }
    
    // broadcast message
    ret = swmc_kmsg_broadcast(msg_type, ws->id, payload);
    if (ret) {
        pr_err("[page_coherence] Failed to send %s message: %d\n", 
               msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate", ret);
        // Continue anyway for now - could implement fallback
    }

    void *wait_result = wait_at_station(ws);
    if (IS_ERR(wait_result)) {
        ret = PTR_ERR(wait_result);
        pr_err("[page_coherence] Failed to wait at station: %d\n", ret);
        return ret;
    }

    pr_info("[page_coherence] Waited at station, received response for %s message\n",
            msg_type == SWMC_KMSG_TYPE_FETCH ? "fetch" : "invalidate");
    return 0;
}

/* Helper function to handle PMD reuse case */
static int handle_pmd_reuse(pmd_t pmd_val, pfn_t original_pfn, pfn_t *pfn)
{
    pfn_t replica_pfn;
    
    pr_info("[page_coherence] Skipping replication and copying for write fault on non-writable PMD.\n");
    replica_pfn.val = pmd_pfn(pmd_val) | (original_pfn.val & PFN_FLAGS_MASK);
    *pfn = replica_pfn;
    pr_info("[page_coherence] Updated pfn to existing PMD PFN: %lu\n",
            pfn_t_to_pfn(replica_pfn));
    return 0;
}

/* Helper function to free replica page */
static void free_replica_page(struct page *replica_page, unsigned int order)
{
    if (order > 0)
        __free_pages(replica_page, order);
    else
        __free_page(replica_page);
}

/* Helper function to setup replica page mapping */
static int setup_replica_mapping(struct page *replica_page, pfn_t original_pfn, 
                                unsigned int order)
{
    struct page_replication_info *replica_info;

    // original pfn -> replica pfn with custom xarrays
    // key: original_pfn.val, value: replica_page
    xa_store(&original_to_replica_xa, original_pfn.val, replica_page, GFP_KERNEL);

    // replica page -> original pfn with page extension for local reclamation
    replica_info = get_page_replication_info(replica_page);
    if (!replica_info) {
        pr_err("[page_coherence] Failed to get replica page replication info\n");
        free_replica_page(replica_page, order);
        return -EINVAL;
    }

    // Link the replica page to the original page
    replica_info->original_pfn = original_pfn; // Store original pfn
    
    // Update page replication info
    set_page_replication_info(replica_page, replica_info);

    pr_info("[page_coherence] Linked replica page %p to original pfn %lu\n",
            replica_page, pfn_t_to_pfn(original_pfn));
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
    // bool leader; // is this fault is the leader fault of this cxl_hdm_offset?
    struct payload_data payload;
    pmd_t *pmd;
    pmd_t pmd_val;
    int node_count;
    unsigned long cxl_hdm_offset;
    bool need_invalidate = false;

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

    // Get CXL HDM offset for this fault
    cxl_hdm_offset = pfn_t_to_pfn(original_pfn) * PAGE_SIZE - cxl_hdm_base;
    
    // TODO: check if this is the leader fault for this cxl_hdm_offset. for now, assume every fault is a leader
    // leader = true;

    payload.cxl_hdm_offset = cxl_hdm_offset;
    payload.page_order = order;

    node_count = swmc_kmsg_node_count();
    if (node_count <= 0) {
        pr_err("[page_coherence] Invalid node count: %d\n", node_count);
        return -EINVAL;
    }

    /* I -> S, I -> M, S -> M (I -> M is handled to I -> S -> M) */

    // if pmd is already allocated, get the existing pmd.
    pmd = vmf->pmd;
    // pr_info("[page_coherence] vmf->pmd: %p\n", pmd);
    pmd_val = *pmd;

    /* Determine state transition and required operations */
    if (pmd_none(pmd_val)) { 
        // I -> S, I -> M
        pr_info("[page_coherence] PMD is not allocated, proceeding with allocation.\n");
        need_invalidate = write; // Only invalidate if this is a write fault
    } else if (pmd_present(pmd_val) && write && !pmd_write(*pmd) && !pmd_dirty(*pmd)) { 
        // S -> M
        pr_info("[page_coherence] PMD already allocated, checking conditions...\n");
        need_invalidate = true;
        
        // Early return for PMD reuse case
        ret = broadcast_message_and_wait(SWMC_KMSG_TYPE_INVALIDATE, node_count, 
                                       &payload, cxl_hdm_offset);
        if (ret)
            return ret;
            
        return handle_pmd_reuse(pmd_val, original_pfn, pfn);
    }

    /* Handle fetch message (always needed for new allocations) */
    if (pmd_none(pmd_val)) {
        ret = broadcast_message_and_wait(SWMC_KMSG_TYPE_FETCH, node_count, 
                                       &payload, cxl_hdm_offset);
        if (ret)
            return ret;
    }

    /* Handle invalidate message if needed */
    if (need_invalidate) {
        pr_info("[page_coherence] Broadcasting invalidate message for offset 0x%lx\n", cxl_hdm_offset);
        ret = broadcast_message_and_wait(SWMC_KMSG_TYPE_INVALIDATE, node_count, 
                                       &payload, cxl_hdm_offset);
        if (ret)
            return ret;
    }

    /* Allocate a replica page or compound page */
    replica_page = allocate_replica_page(order);
    if (!replica_page)
        return -ENOMEM;

    /* Copy data from original address to replica */
    ret = copy_page_data(replica_page, kaddr, size);
    if (ret) {
        pr_err("[page_coherence] Data copy failed: %d\n", ret);
        free_replica_page(replica_page, order);
        return ret;
    }
    
    // just to check if devdax has struct page. not to do with this function
    struct page *original_page_ptr = pfn_t_to_page(original_pfn);
    if (!original_page_ptr) {
        pr_err("[page_coherence] Invalid original page pointer\n");
        free_replica_page(replica_page, order);
        return -EINVAL;
    }
    pr_info("[page_coherence] Original page: %p, replica page: %p\n", original_page_ptr, replica_page);

    /* Register original pfn <-> replica pfn mapping with page replication info */
    ret = setup_replica_mapping(replica_page, original_pfn, order);
    if (ret)
        return ret;

    /* Build new PFN with preserved flags and update */
    replica_pfn.val = page_to_pfn(replica_page) |
                      (original_pfn.val & PFN_FLAGS_MASK);
    *pfn = replica_pfn;

    pr_info("[page_coherence] Replicated pfn=%lu at 0x%lx\n",
            pfn_t_to_pfn(replica_pfn), vmf->address);
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
    
    pr_info("[page_coherence] Initializing page coherence subsystem\n");

    // Initialize the xarray for original to replica mapping
    xa_init(&original_to_replica_xa);

    // Register message handlers
    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_FETCH, swmc_kmsg_handle_fetch);
    if (ret) {
        pr_err("[page_coherence] Failed to register fetch handler: %d\n", ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_INVALIDATE, swmc_kmsg_handle_invalidate);
    if (ret) {
        pr_err("[page_coherence] Failed to register invalidate handler: %d\n", ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_FETCH_ACK, swmc_kmsg_handle_fetch_ack);
    if (ret) {
        pr_err("[page_coherence] Failed to register fetch_ack handler: %d\n", ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_INVALIDATE_ACK, swmc_kmsg_handle_invalidate_ack);
    if (ret) {
        pr_err("[page_coherence] Failed to register invalidate_ack handler: %d\n", ret);
        return ret;
    }

    ret = swmc_kmsg_register_callback(SWMC_KMSG_TYPE_ERROR, swmc_kmsg_handle_error);
    if (ret) {
        pr_err("[page_coherence] Failed to register error handler: %d\n", ret);
        return ret;
    }

    pr_info("[page_coherence] Page coherence subsystem initialized\n");
    return 0;

}

subsys_initcall(page_coherence_init);

#endif /* CONFIG_PAGE_COHERENCE */
