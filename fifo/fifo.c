#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nerdbuero Staff");

#define NUM_MINORS 2

static dev_t dev;
static struct cdev char_dev;

#define FIFOSIZE 200
struct fifo
{
	int rcnt, wcnt;
	char buffer[FIFOSIZE];
};

static struct fifo fifo0 = {0, 0};
static struct fifo fifo1 = {0, 0};

static int fifo_io_open(struct inode* inodep, struct file* filep)
{
	printk("FIFO open called\n");
	return 0;
}

static int fifo_io_close(struct inode* inodep, struct file* filep) 
{
	printk("FIFO close called\n");
	return 0;
}

static ssize_t fifo_io_read(struct file* filep, char __user *data,
							size_t count, loff_t *pOffset)
{
	int bytes = 0;
	int to_read = 0;
	printk("FIFO read called\n");
	
	// +++____+++	wcnt=3	rcnt=7	diff=-4
	// __________	wcnt=0	rcnt=0	diff=0
	// +++++_____	wcnt=5	rcnt=0	diff=5
	to_read = fifo0.wcnt - fifo0.rcnt;
	if(to_read == 0) {
		return 0;
	} else if(to_read < 0) {
		to_read += FIFOSIZE;
	}
	
	// Check if we have enough bytes to fullfill the request
	to_read = to_read < count ? to_read : count;
	
	// Copy the bytes from kernelspace to userspace
	if(fifo0.wcnt > fifo0.rcnt) {
		bytes = copy_to_user(data, &(fifo0.buffer[fifo0.rcnt]), to_read);
		if(bytes == 0) {
			bytes = to_read;
		}
		fifo0.rcnt += bytes;
	} else {
		// Copy right chunk
		bytes = copy_to_user(data, &(fifo0.buffer[fifo0.rcnt]), FIFOSIZE - fifo0.rcnt);
		if(bytes > 0) {
			fifo0.rcnt += bytes;
			return bytes;
		} else {
			bytes = FIFOSIZE - fifo0.rcnt;
		}
		fifo0.rcnt = 0;
		bytes += fifo_io_read(filep, data + bytes, to_read - bytes, pOffset);
	}
	
	return bytes;
}

static ssize_t fifo_io_write(struct file* filep, const char __user *pDaten,
								size_t count, loff_t *pOffset)
{
	printk("FIFO write called\n");
	return 0;
}

struct file_operations fops =
{
   .owner	= THIS_MODULE,
   .open	= fifo_io_open,
   .release	= fifo_io_close,
   .read	= fifo_io_read,
   .write	= fifo_io_write
};

static int fifo_init(void)
{
	int ret;
	printk("FIFO module loaded\n");
	
	// Dynamically register a range of character device numbers
	ret = alloc_chrdev_region(&dev, 7, NUM_MINORS, "fifo");
	if(unlikely(ret != 0)) {
		printk("FIFO: error allocation chrdev range\n");
		return ret;
	}
	
	// Register character devices
	cdev_init(&char_dev, &fops);	// Init char_dev structure
	char_dev.owner = THIS_MODULE;	// Make us owner
	ret = cdev_add(&char_dev, dev, NUM_MINORS);
	if(unlikely(ret != 0)) {
		printk("FIFO: error adding character devices\n");
		return ret;
	}
	
	return 0;
}

static void fifo_exit(void)
{
	printk("FIFO module unloaded\n");
	
	// Unregister character devices
	cdev_del(&char_dev);
	
	// Unregister character device numbers
	unregister_chrdev_region(dev, NUM_MINORS);
}

module_init(fifo_init);
module_exit(fifo_exit);
