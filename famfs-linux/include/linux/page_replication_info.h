/* include/linux/page_replication_info.h */
#ifndef _LINUX_PAGE_REPLICATION_INFO_H
#define _LINUX_PAGE_REPLICATION_INFO_H

#include <linux/mm_types.h>

/* 구조체 이름 변경: replica_page_info -> page_replication_info */
struct page_replication_info {
    struct page *linked_page;
    int custom_flag;
};

/* CONFIG 옵션 이름 변경: REPLICA_PAGE_EXTENSION -> CONFIG_PAGE_COHERENCE */
#ifdef CONFIG_PAGE_COHERENCE

extern struct page_replication_info *get_page_replication_info(struct page *page);

#else
static inline struct page_replication_info *get_page_replication_info(struct page *page)
{
    return NULL;
}
#endif /* CONFIG_PAGE_COHERENCE */

#endif /* _LINUX_PAGE_REPLICATION_INFO_H */