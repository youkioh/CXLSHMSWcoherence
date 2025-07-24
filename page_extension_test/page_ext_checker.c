#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>           // For alloc_page, __free_page
#include <linux/gfp.h>          // For GFP_KERNEL
#include <linux/page_ext.h>     // For page extension definitions
#include <linux/page-flags.h>   // For general page flags

// 이 헤더 파일이 필요합니다!
#include <linux/page_idle.h>    // For PageIdle(), set_page_idle(), etc.

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gemini");
MODULE_DESCRIPTION("A simple module to demonstrate usage of an existing page extension (PageIdle)");

static struct page *test_page;

/* 모듈이 로드될 때 호출되는 초기화 함수 */
static int __init pg_ext_init(void)
{
    pr_info("Page Extension Checker Module loaded.\n");

    // CONFIG_PAGE_IDLE_FLAG가 활성화되어 있는지 확인합니다.
    // 이 설정이 켜져 있어야 PageIdle 관련 pg_ext 기능이 존재합니다.
#if defined(CONFIG_PAGE_EXTENSION) && defined(CONFIG_PAGE_IDLE_FLAG)
    pr_info("This kernel has CONFIG_PAGE_IDLE_FLAG enabled, demonstrating PageIdle extension.\n");

    // 1. 테스트를 위한 페이지를 하나 할당받습니다.
    test_page = alloc_page(GFP_KERNEL);
    if (!test_page) {
        pr_warn("Failed to allocate a page for the test.\n");
        return -ENOMEM;
    }

    // 2. 페이지 할당 직후 PageIdle 상태를 확인합니다.
    // 페이지가 방금 사용(할당)되었으므로 'idle' 상태가 아닙니다 (결과: No).
    bool is_idle = PageIdle(test_page);
    pr_info("1. After allocation, is the page idle? -> %s\n", is_idle ? "Yes" : "No");

    // 3. 페이지를 강제로 'idle' 상태로 만듭니다.
    // set_page_idle() 함수는 내부적으로 pg_ext 영역의 PageIdle 플래그를 설정합니다.
    set_page_idle(test_page);
    pr_info("2. Marking the page as idle using set_page_idle().\n");

    // 4. 상태 변경 후 다시 확인합니다.
    // 이제 'idle' 상태여야 합니다 (결과: Yes).
    is_idle = PageIdle(test_page);
    pr_info("3. After marking, is the page idle? -> %s\n", is_idle ? "Yes" : "No");

    // 5. 페이지의 'idle' 상태를 해제합니다.
    // 페이지에 접근(쓰기)하면 커널이 자동으로 idle 상태를 해제하지만,
    // 여기서는 명시적인 함수 clear_page_idle()을 사용합니다.
    clear_page_idle(test_page);
    pr_info("4. Clearing the idle flag using clear_page_idle().\n");

    // 6. 최종 상태를 확인합니다.
    // 다시 'idle' 상태가 아니어야 합니다 (결과: No).
    is_idle = PageIdle(test_page);
    pr_info("5. After clearing, is the page idle? -> %s\n", is_idle ? "Yes" : "No");

#else
    pr_warn("This kernel does not have CONFIG_PAGE_IDLE_FLAG enabled.\n");
    pr_warn("Cannot demonstrate PageIdle extension. Please check your kernel configuration.\n");
#endif

    return 0;
}

/* 모듈이 제거될 때 호출되는 종료 함수 */
static void __exit pg_ext_exit(void)
{
    // 모듈이 종료될 때 할당했던 페이지를 반드시 해제합니다.
#if defined(CONFIG_PAGE_EXTENSION) && defined(CONFIG_PAGE_IDLE_FLAG)
    if (test_page) {
        __free_page(test_page);
    }
#endif
    pr_info("Exiting Page Extension Checker Module.\n");
}

module_init(pg_ext_init);
module_exit(pg_ext_exit);