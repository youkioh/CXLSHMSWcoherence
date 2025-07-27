/* mm/page_replication_info.c */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gfp.h>

/* 필요한 헤더들을 올바른 순서로 포함합니다 */
#include <linux/mm_types.h>
#include <linux/page_ext.h>

/* 이 모듈의 공개 인터페이스 헤더를 포함합니다 */
#include <swmc/page_replication_info.h>

// --- 부팅 파라미터 처리 및 need 함수 ---
/* 'replica' -> 'page_replication'으로 변경 */
static bool page_replication_ext_enabled;

/* 'replica' -> 'page_replication'으로 변경 */
static int __init parse_page_replication_ext_opt(char *str)
{
	if (!str)
		return -EINVAL;
	if (strcmp(str, "on") == 0) {
		page_replication_ext_enabled = true;
		pr_info("[page_replication] Page replication extension enabled via boot parameter\n");
	}
	return 0;
}
/* 부팅 파라미터 이름도 통일: 'replica_page_ext' -> 'page_replication_ext' */
early_param("page_replication_ext", parse_page_replication_ext_opt);

/* 'replica' -> 'page_replication'으로 변경 */
static bool __init page_replication_ext_need(void)
{
	pr_info("[page_replication] page_replication_ext_need() called, enabled=%d\n", 
		page_replication_ext_enabled);
	return page_replication_ext_enabled;
}

/* --- page extension 오퍼레이션 정의 ---
 * mm/page_ext.c 에서 참조해야 하므로 'static'을 제거합니다.
 * 이름도 'replica' -> 'page_replication'으로 변경
 */
struct page_ext_operations page_replication_ext_ops = {
    /* 구조체 이름도 통일 (헤더 파일 수정 필요) */
	.size = sizeof(struct page_replication_info),
	.need = page_replication_ext_need,
	.need_shared_flags = false
};

/* --- 접근자 함수 구현 --- */
struct page_replication_info *get_page_replication_info(struct page *page)
{
	struct page_ext *page_ext;

	page_ext = page_ext_get(page);
	if (!page_ext) {
		pr_err("[get_page_replication_info] Failed to get page extension for page %p\n", page);
		return NULL;
	}

	return (struct page_replication_info *)((unsigned long)page_ext +
					     page_replication_ext_ops.offset);
}

/* -- page_replication_info 변경자 구현 --- */
void set_page_replication_info(struct page *page, struct page_replication_info *info)
{
	struct page_ext *page_ext;

	page_ext = page_ext_get(page);
	if (!page_ext)
		return;

	/* 페이지 확장 정보에 새로운 정보를 설정합니다 */
	memcpy((void *)((unsigned long)page_ext + page_replication_ext_ops.offset),
	       info, sizeof(struct page_replication_info));
}

/* 심볼 공개 */
EXPORT_SYMBOL_GPL(get_page_replication_info);
EXPORT_SYMBOL_GPL(set_page_replication_info);