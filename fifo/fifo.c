#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nerdbuero Staff");

#define NUM_MINORS 2
#define TRUE 0
#define FALSE 1

static dev_t dev;
static struct cdev char_dev;

#define FIFOSIZE 8
struct fifo
{
	int rcnt, wcnt;
	char lockdown;
	char buffer[FIFOSIZE];
};

static struct fifo fifo0 = {0, 0, FALSE};
static struct fifo fifo1 = {0, 0, FALSE};

void finalizeWrite()
{
	if (fifo0.wcnt >= FIFOSIZE)
	{
		fifo0.wcnt = 0;
	}
	if(fifo0.rcnt == fifo0.wcnt) 
	{
		fifo0.lockdown = TRUE;
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
	int bytes = 0;
	int to_read = 0;
	printk("FIFO read called\n");
	
	// +++____+++	wcnt=3	rcnt=7	diff=-4
	// __________	wcnt=0	rcnt=0	diff=0
	// +++++_____	wcnt=5	rcnt=0	diff=5
	to_read = fifo0.wcnt - fifo0.rcnt;
	if(to_read <= 0) 
	{
		to_read = FIFOSIZE - to_read;
	}
	
	// Check if we have enough bytes to fullfill the request
	to_read = to_read < count ? to_read : count;
	printk("%i",to_read);
	
	// Copy the bytes from kernelspace to userspace
	if(fifo0.wcnt > fifo0.rcnt) {
		bytes = copy_to_user(data, &(fifo0.buffer[fifo0.rcnt]), to_read);
		if(bytes == 0) {
			bytes = to_read;
		}
		fifo0.rcnt += bytes;
		
	} /**else {
		// Copy right chunk
		bytes = copy_to_user(data, &(fifo0.buffer[fifo0.rcnt]), FIFOSIZE - fifo0.rcnt);
		if(bytes > 0) {
			fifo0.rcnt += bytes;
		} else {
			bytes = FIFOSIZE - fifo0.rcnt;
			fifo0.rcnt = 0;
			bytes += fifo_io_read(filep, data + bytes, to_read - bytes, pOffset);
		}
	}*/
	
	// FIFO entsperren, sofern es gesperrt war und nun mind. 1 byte 
	// gelesen wurde
	if(fifo0.lockdown == TRUE && bytes > 0) {
		fifo0.lockdown = FALSE;
	}
	return bytes;
}

static ssize_t fifo_io_write(struct file* filep, const char __user *data,
								size_t count, loff_t *pOffset)
{
	printk("FIFO write called\n");
	
	
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
	
	
	if (fifo0.lockdown == TRUE)
	{
		return -EINVAL;
	}
	
	int totalBytesToWrite = fifo0.rcnt - fifo0.wcnt;
	
	int returnValue = 0;

	if (totalBytesToWrite <= 0)
	{
		totalBytesToWrite = FIFOSIZE + totalBytesToWrite; 
	}
	
	totalBytesToWrite = totalBytesToWrite < count ? totalBytesToWrite : count;
	

	if (fifo0.wcnt > fifo0.rcnt && fifo0.lockdown == FALSE)
	{
		
		if ((fifo0.wcnt + totalBytesToWrite) <= FIFOSIZE)
		{ 
			int n = copy_from_user(&(fifo0.buffer[fifo0.wcnt]), data, totalBytesToWrite);

			returnValue = count - n;
			fifo0.wcnt += returnValue;
			finalizeWrite();
		}
		else 
		{
			int rechterRand = FIFOSIZE - fifo0.wcnt;
			int n = copy_from_user(&(fifo0.buffer[fifo0.wcnt]), data, rechterRand);
			int linkerRand = totalBytesToWrite - rechterRand;
			n += copy_from_user(&(fifo0.buffer[0]), data+rechterRand,linkerRand);
			
			returnValue = count - n;
			fifo0.wcnt += returnValue;
			finalizeWrite();
			
		}
	}
	else if (fifo0.rcnt > fifo0.wcnt &&  fifo0.lockdown == false)
	{
		int n = copy_from_user(&(fifo0.buffer[fifo0.wcnt]), data, totalBytesToWrite);
		returnValue = count - n;
		fifo0.wcnt += returnValue;
		finalizeWrite();
		
	}
	else if (fifo0.rcnt == fifo0.wcnt && fifo0.lockdown == FALSE)
	{
		if ((fifo0.wcnt + totalBytesToWrite) <= FIFOSIZE)
		{
			printk("%i",count);
			int n = copy_from_user(&(fifo0.buffer[fifo0.wcnt]), data, totalBytesToWrite);
			returnValue = count -n;
			fifo0.wcnt += returnValue;
			finalizeWrite();
		}
		else 
		{
			int rechterRand = FIFOSIZE - fifo0.wcnt;
			int n = copy_from_user(&(fifo0.buffer[fifo0.wcnt]), data, rechterRand);
			int linkerRand = totalBytesToWrite - rechterRand;
			n+= copy_from_user(&(fifo0.buffer[0]), data+rechterRand,linkerRand);
			returnValue = n;
			
			fifo0.wcnt += returnValue;
			finalizeWrite();
			
		}
	}

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
