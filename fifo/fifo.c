#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nerdbuero Staff");

static int fifo_init(void)
{
	printk("FIFO module loaded\n");
	return 0;
}

static void fifo_exit(void)
{
	printk("FIFO module unloaded\n");
}

module_init(fifo_init);
module_exit(fifo_exit);
