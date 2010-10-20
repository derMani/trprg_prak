#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <asm/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nerdbuero Staff");

#define NUM_MINORS 2
#define TRUE 1
#define FALSE 0

static dev_t dev;
static struct cdev char_dev;

#define FIFOSIZE 8
struct fifo
{
	int rcnt, wcnt;
	char lockdown;
	char buffer[FIFOSIZE];
};

static struct fifo fifos[2] = {{0, 0, FALSE}, {0, 0, FALSE}};

static int level[NUM_MINORS] = {0, 0};
static int levelLen = NUM_MINORS;
module_param_array(level, int, &levelLen, S_IRUGO);
MODULE_PARM_DESC(level, "Fill level");

void finalizeWrite()
{
	if (fifo->wcnt >= FIFOSIZE)
	{
		fifo->wcnt = 0;
	}
	if(fifo->rcnt == fifo->wcnt) 
	{
		fifo->lockdown = TRUE;
	}
}

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
	printk("FIFO read called\n");
	
	struct fifo* fifo = &(fifos[iminor(filep->f_dentry->d_inode) - 7]);
	int bytes = 0;
	int to_read = 0;
	
	// +++____+++	wcnt=3	rcnt=7	diff=-4
	// __________	wcnt=0	rcnt=0	diff=0
	// +++++_____	wcnt=5	rcnt=0	diff=5
	to_read = fifo->wcnt - fifo->rcnt;
	if(to_read == 0) {
		return 0;
	} else if(to_read < 0) {
		to_read += FIFOSIZE;
/*
	to_read = fifo->wcnt - fifo->rcnt;
	if(to_read <= 0) 
	{
		to_read = FIFOSIZE - to_read;
>>>>>>> aa070c7b8e6aec11d58f9254b1d6c4cf6a7f6a92
	*/
	}
	
	// Check if we have enough bytes to fullfill the request
	to_read = to_read < count ? to_read : count;
	printk("%i",to_read);
	
	// Copy the bytes from kernelspace to userspace
	if(fifo->wcnt > fifo->rcnt) {
		bytes = copy_to_user(data, &(fifo->buffer[fifo->rcnt]), to_read);
		if(bytes == 0) {
			bytes = to_read;
		}
		fifo->rcnt += bytes;
	} else {
		// Copy right chunk
		bytes = copy_to_user(data, &(fifo->buffer[fifo->rcnt]), FIFOSIZE - fifo->rcnt);
		if(bytes > 0) {
			fifo->rcnt += bytes;
		} else {
			bytes = FIFOSIZE - fifo->rcnt;
			fifo->rcnt = 0;
			bytes += fifo_io_read(filep, data + bytes, to_read - bytes, pOffset);
		}
	}
	
	// FIFO entsperren, sofern es gesperrt war und nun mind. 1 byte 
	// gelesen wurde
	if(fifo->lockdown == TRUE && bytes > 0) {
		fifo->lockdown = FALSE;
	}
	return bytes;
}

static ssize_t fifo_io_write(struct file* filep, const char __user *data,
								size_t count, loff_t *pOffset)
{
	printk("FIFO write called\n");
	struct fifo* fifo = &(fifos[iminor(filep->f_dentry->d_inode) - 7]);
	
	// Fälle: wi steht für den Schreibindex an der Stelle i; ri für den Leseindex an Stelle i
	
	// I  
	// wi == ri  
	// In diesem Fall darf nur geschrieben werden, wenn der Buffer nicht voll ist. 
	// Wird durch Variable fifo[0-1].lockdown festgehalten
	
	// II  
	// ri < wi 
	// Schreibindex steht vor dem Leseindex.... In diesem Fall darf bis zum letzten < ri geschrieben werden.
	
	
	// III 
	// ri > wi 
	// Auch hier darf bis zum letzten < ri geschrieben werden
	
	
	if (fifo->lockdown == TRUE)
	{
		return -EINVAL;
	}
	
	int totalBytesToWrite = fifo->rcnt - fifo->wcnt;
	
	int returnValue = 0;

	if (totalBytesToWrite <= 0)
	{
		totalBytesToWrite = FIFOSIZE + totalBytesToWrite; 
	}
	
	totalBytesToWrite = totalBytesToWrite < count ? totalBytesToWrite : count;
	

	if (fifo->wcnt > fifo->rcnt && fifo->lockdown == FALSE)
	{
		
		if ((fifo->wcnt + totalBytesToWrite) <= FIFOSIZE)
		{ 
			int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, totalBytesToWrite);

			returnValue = count - n;
			fifo->wcnt += returnValue;
			finalizeWrite();
		}
		else 
		{
			int rechterRand = FIFOSIZE - fifo->wcnt;
			int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, rechterRand);
			int linkerRand = totalBytesToWrite - rechterRand;
			n += copy_from_user(&(fifo->buffer[0]), data+rechterRand,linkerRand);
			
			returnValue = count - n;
			fifo->wcnt += returnValue;
			finalizeWrite();
			
		}
	}
	else if (fifo->rcnt > fifo->wcnt &&  fifo->lockdown == false)
	{
		int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, totalBytesToWrite);
		returnValue = count - n;
		fifo->wcnt += returnValue;
		finalizeWrite();
		
	}
	else if (fifo->rcnt == fifo->wcnt && fifo->lockdown == FALSE)
	{
		if ((fifo->wcnt + totalBytesToWrite) <= FIFOSIZE)
		{
			printk("%i",count);
			int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, totalBytesToWrite);
			returnValue = count -n;
			fifo->wcnt += returnValue;
			finalizeWrite();
		}
		else 
		{
			int rechterRand = FIFOSIZE - fifo->wcnt;
			int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, rechterRand);
			int linkerRand = totalBytesToWrite - rechterRand;
			n+= copy_from_user(&(fifo->buffer[0]), data+rechterRand,linkerRand);
			returnValue = n;
			
			fifo->wcnt += returnValue;
			finalizeWrite();
			
		}
	}

	level[0] += returnvalue;
	return returnValue;
}

struct file_operations fops =
{
   .owner	= THIS_MODULE,
   .open	= fifo_io_open,
   .release	= fifo_io_close,
   .read	= fifo_io_read,
   .write	= fifo_io_write
};

static int __init fifo_init(void)
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

static void __exit fifo_exit(void)
{
	printk("FIFO module unloaded\n");
	
	// Unregister character devices
	cdev_del(&char_dev);
	
	// Unregister character device numbers
	unregister_chrdev_region(dev, NUM_MINORS);
}

module_init(fifo_init);
module_exit(fifo_exit);
