#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/types.h>

static char *dax_name = "dax0.0";  // 이제 이름만 받음
module_param(dax_name, charp, 0444);
MODULE_PARM_DESC(dax_name, "devdax device name (e.g., dax0.0)");

static phys_addr_t get_dax_physical_range(const char *name)
{
    struct file *filp;
    char sysfs_path[256];
    char buffer[256];
    loff_t pos = 0;
    ssize_t bytes_read;
    phys_addr_t start = 0;

    /* resource: get start physical address */
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/bus/dax/devices/%s/resource", name);
    filp = filp_open(sysfs_path, O_RDONLY, 0);
    if (!IS_ERR(filp)) {
        bytes_read = kernel_read(filp, buffer, sizeof(buffer) - 1, &pos);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            sscanf(buffer, "0x%llx", &start);
        }
        filp_close(filp, NULL);
    }

    return start;  // 0이면 실패 의미
}

static int __init dax_lookup_init(void)
{
    phys_addr_t start;

    pr_info("Looking up devdax name: %s\n", dax_name);
    start = get_dax_physical_range(dax_name);

    if (start == 0) {
        pr_err("Failed to find devdax physical address.\n");
        return -ENODEV;
    }

    pr_info("devdax %s: start physical address = 0x%llx\n",
            dax_name, (unsigned long long)start);
    return 0;
}

static void __exit dax_lookup_exit(void)
{
    pr_info("devdax lookup module unloaded\n");
}

module_init(dax_lookup_init);
module_exit(dax_lookup_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Get devdax physical address by name using sysfs");
