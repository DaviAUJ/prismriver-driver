#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_AUTHOR("davi");
MODULE_DESCRIPTION("Simple hello world from kernel");
MODULE_LICENSE("GPL");

static __init int helloInit(void) {
    pr_info("hello world\n");

    return 0;
}

static __exit void helloExit(void) {
    pr_info("Goodbye World\n");
}

module_init(helloInit);
module_exit(helloExit);