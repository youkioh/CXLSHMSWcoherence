#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>

/* 모듈 라이선스 선언 */
MODULE_LICENSE("GPL");
/* 모듈 작성자 선언 */
MODULE_AUTHOR("Gemini");
/* 모듈 설명 */
MODULE_DESCRIPTION("A simple kernel module to print the size of struct page");

/* 모듈이 적재될 때 호출되는 함수 */
static int __init page_size_init(void)
{
    /* pr_info는 printk(KERN_INFO ...)와 동일하며, 커널 로그에 정보를 출력합니다. */
    /* sizeof(struct page)를 통해 컴파일 시점의 struct page 크기를 계산합니다. */
    pr_info("Size of struct page: %zu bytes\n", sizeof(struct page));

    /* 모듈 초기화가 성공적으로 완료되었음을 알립니다. */
    return 0;
}

/* 모듈이 제거될 때 호출되는 함수 */
static void __exit page_size_exit(void)
{
    pr_info("Exiting Page Size Checker Module.\n");
}

/* 모듈 진입점과 종료점을 커널에 등록합니다. */
module_init(page_size_init);
module_exit(page_size_exit);