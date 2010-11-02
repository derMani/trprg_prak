#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <linux/semaphore.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nerdbuero Staff");

#define NUM_MINORS 2

static dev_t dev;
static struct cdev char_dev;

#define FIFOSIZE 8
struct fifo
{
	atomic_t rcnt;
	atomic_t wcnt;
	atomic_t level;
	struct semaphore lock;
	wait_queue_head_t read_queue;
	wait_queue_head_t write_queue;
	char buffer[FIFOSIZE];
};

static struct fifo fifos[NUM_MINORS];

static struct semaphore level_lock;
static int level[NUM_MINORS] = {0, 0};
static int level_len = NUM_MINORS;
module_param_array(level, int, &level_len, S_IRUGO);
MODULE_PARM_DESC(level, "Fill level");

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

static int lockFifo(struct semaphore* sema, struct file* filep)
{
	if((filep->f_flags & O_NONBLOCK) != 0) // NON BLOCKING
	{
		if(down_trylock(sema) != 0) //down_trylock : Returns 0 if the mutex has been acquired successfully or 1 if it it cannot be acquired.
		{
			// Someone else has it
			return -EAGAIN;
		} // else we have the semaphore
	}
	else // BLOCKING
	{
		if(down_interruptible(sema) != 0)
		{
			return -EINTR;
		} // else we have the semaphore
	}
	return 0;
}

static ssize_t fifo_io_read(struct file* filep, char __user *data,
							size_t count, loff_t *pOffset)
{
	printk("FIFO read called\n");

	int fifoIdx = iminor(filep->f_dentry->d_inode) - 7;
	struct fifo* fifo = &(fifos[fifoIdx]);
	int ret;

	// Try to get lock on semaphore
	int s;
rlock:
	if((s = lockFifo(&(fifo->lock), filep)) != 0)
	{
		// Something went wrong... we have no semaphore
		return s;
	}

	// Check if there is something to read
	if(atomic_read(&(fifo->level)) == 0)
	{
		// Release the lock to wait for somebody to write something into the fifo 
		up(&(fifo->lock));
		
		// And go to sleep
		if((s = wait_event_interruptible(fifo->read_queue, atomic_read(&(fifo->level)) > 0)) != 0) // interruptible weil killbar sein wollen
		{
			return -ERESTARTSYS; // Signal
		}
		else
		{
			goto rlock; // Lock again
		}
	}

	int to_read = atomic_read(&(fifo->level)); //fifo->wcnt - fifo->rcnt;
	/*if(to_read == 0) // && level[fifoIdx] < FIFOSIZE - 1) //fifo->lockdown == FALSE)
	{
		ret = 0;
	}
	else*/
	{
		int rcnt = atomic_read(&(fifo->rcnt));
		int wcnt = atomic_read(&(fifo->wcnt));
		int new_rcnt;
		/*if(to_read <= 0)
		{
			to_read += FIFOSIZE;
		}*/

		// Check if we have enough bytes to fullfill the request
		to_read = to_read < count ? to_read : count;
		printk("Zeichen zu lesen %i\n", to_read);
		//printk("Writecount %i\n", fifo->wcnt);
		//printk("Readcount %i\n", fifo->rcnt);
		printk("Fuellstand %i\n",  level[fifoIdx]);
		
		int unreadBytes = 0;
		// ++++____
		if(wcnt > rcnt)
		{
			unreadBytes = copy_to_user(data, &(fifo->buffer[rcnt]), to_read);
			new_rcnt = (rcnt + to_read - unreadBytes) % FIFOSIZE;
		// ++__++++
		}
		else if(wcnt < rcnt)
		{
			if(rcnt + to_read < FIFOSIZE)
			{
				unreadBytes = copy_to_user(data, &(fifo->buffer[rcnt]), to_read);
				new_rcnt = (rcnt + to_read - unreadBytes) % FIFOSIZE;
			}
			else
			{
				int rechterRand = FIFOSIZE - rcnt;
				// Rechter Rand
				unreadBytes = copy_to_user(data, &(fifo->buffer[rcnt]), rechterRand);
				new_rcnt = (rcnt + rechterRand - unreadBytes) % FIFOSIZE;
				if(unreadBytes == 0)
				{
					// Linken Rand
					unreadBytes = copy_to_user(data + rechterRand, &(fifo->buffer[0]), to_read - rechterRand);
					new_rcnt = (new_rcnt + to_read - rechterRand) % FIFOSIZE;
				}
			}
		}
		else if(rcnt == wcnt) //fifo->lockdown == TRUE)
		{
			int rechterRand = FIFOSIZE - rcnt;
			// Rechter Rand
			unreadBytes = copy_to_user(data, &(fifo->buffer[rcnt]), rechterRand);
			new_rcnt = (rcnt + rechterRand - unreadBytes) % FIFOSIZE;
			if(unreadBytes == 0)
			{
				// Linken Rand
				unreadBytes = copy_to_user(data + to_read - rechterRand, &(fifo->buffer[0]), to_read - rechterRand);
				new_rcnt = (new_rcnt + to_read - rechterRand) % FIFOSIZE;
			}
		}

		printk("read: cnt %i\n", new_rcnt); 
		// FIFO entsperren, sofern es gesperrt war und nun mind. 1 byte 
		// gelesen wurde
		ret = to_read - unreadBytes;
		/*if(ret > 0)
		{new_rcnt
			fifo->lockdown = FALSE;
		}*/
		level[fifoIdx] -= ret;
		atomic_set(&(fifo->rcnt), new_rcnt);
		atomic_sub(ret, &(fifo->level));
		printk("Fuellstand %i\n",  level[fifoIdx]);
	}
	
	// Release semaphore
	up(&(fifo->lock));
	
	// Wake up waiting write processes
	wake_up_interruptible(&(fifo->write_queue));
	
	return ret;
}

static ssize_t fifo_io_write(struct file* filep, const char __user *data,
								size_t count, loff_t *pOffset)
{
	printk("FIFO write called\n");

	int fifoIdx = iminor(filep->f_dentry->d_inode) - 7;
	struct fifo* fifo = &(fifos[fifoIdx]);
	printk("Fuellstand %i\n",  level[fifoIdx]);
	
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

	// Try to get lock on semaphore
	int s;
wlock:
	if((s = lockFifo(&(fifo->lock), filep)) != 0)
	{
		return s;
	}

	// Check if there is space to write something
	if(atomic_read(&(fifo->level)) == FIFOSIZE)
	{
		// Release the lock
		up(&(fifo->lock));
		
		// And go to sleep
		if((s = wait_event_interruptible(fifo->write_queue, atomic_read(&(fifo->level)) < FIFOSIZE)) != 0)
		{
			return -ERESTARTSYS; // Signal
		}
		else
		{
			goto wlock; // Lock again
		}
	}

	int returnValue = 0;
	//int level = 
	
/*	if (level[fifoIdx] >= FIFOSIZE - 1) //(fifo->lockdown == TRUE)
	{
		returnValue = -EINVAL;
	}
	else*/
	{
		int totalBytesToWrite = atomic_read(&(fifo->level)); //fifo->rcnt - fifo->wcnt;
		int rcnt = atomic_read(&(fifo->rcnt));
		int wcnt = atomic_read(&(fifo->wcnt));
		int new_wcnt;
		/*if (totalBytesToWrite <= 0)
		{
			totalBytesToWrite = FIFOSIZE + totalBytesToWrite; 
		}*/

		totalBytesToWrite = totalBytesToWrite < count ? totalBytesToWrite : count;
		printk("%i\n", totalBytesToWrite);

		if (wcnt > rcnt) // && level[fifoIdx] < FIFOSIZE - 1) //fifo->lockdown == FALSE)
		{
			if ((wcnt + totalBytesToWrite) <= FIFOSIZE)
			{ 
				int n = copy_from_user(&(fifo->buffer[wcnt]), data, totalBytesToWrite);

				returnValue = totalBytesToWrite - n;
				new_wcnt += returnValue;
			}
			else 
			{
				int rechterRand = FIFOSIZE - wcnt;
				int n = copy_from_user(&(fifo->buffer[wcnt]), data, rechterRand);
				int linkerRand = totalBytesToWrite - rechterRand;
				n += copy_from_user(&(fifo->buffer[0]), data+rechterRand,linkerRand);

				returnValue = totalBytesToWrite - n;
				new_wcnt += returnValue;
			}
		}
		else if (rcnt > wcnt) // && level[fifoIdx] < FIFOSIZE - 1) // fifo->lockdown == FALSE)
		{
			int n = copy_from_user(&(fifo->buffer[wcnt]), data, totalBytesToWrite);
			returnValue = totalBytesToWrite - n;
			new_wcnt += returnValue;
		}
		else if (rcnt == wcnt) // && level[fifoIdx] < FIFOSIZE - 1) //fifo->lockdown == FALSE)
		{
			if ((wcnt + totalBytesToWrite) <= FIFOSIZE)
			{
				printk("%i\n", count);
				int n = copy_from_user(&(fifo->buffer[wcnt]), data, totalBytesToWrite);
				returnValue = totalBytesToWrite -n;
				new_wcnt += returnValue;
			}
			else 
			{
				int rechterRand = FIFOSIZE - wcnt;
				int n = copy_from_user(&(fifo->buffer[wcnt]), data, rechterRand);
				int linkerRand = totalBytesToWrite - rechterRand;
				n+= copy_from_user(&(fifo->buffer[0]), data+rechterRand,linkerRand);
				returnValue = totalBytesToWrite - n;

				new_wcnt += returnValue;
			}
		}

		new_wcnt = new_wcnt % FIFOSIZE;
		printk("write: wcnt %i\n", new_wcnt);
		level[fifoIdx] += returnValue;
		printk("Fuellstand %i\n",  level[fifoIdx]);
		atomic_set(&(fifo->wcnt), new_wcnt);
		atomic_add(returnValue, &(fifo->level));
	}
	// Release semaphore
	up(&(fifos[fifoIdx].lock));
	
	// Wake up waiting reading processes
	wake_up_interruptible(&(fifo->read_queue));
	
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
	int ret, n;
	printk("FIFO module loaded\n");

	// Dynamically register a range of character device numbers
	ret = alloc_chrdev_region(&dev, 7, NUM_MINORS, "fifo");
	if(unlikely(ret != 0))
	{
		printk("FIFO: error allocation chrdev range\n");
		return ret;
	}

	// Register character devices
	cdev_init(&char_dev, &fops);	// Init char_dev structure
	char_dev.owner = THIS_MODULE;	// Make us owner
	ret = cdev_add(&char_dev, dev, NUM_MINORS);
	if(unlikely(ret != 0))
	{
		printk("FIFO: error adding character devices\n");
		//unregister chr region
		unregister_chrdev_region(dev, NUM_MINORS);
		return ret;
	}
	
	// Initialize fifo structs (queues, semaphores, ...)
	for(n = 0; n < NUM_MINORS; n++)
	{
		sema_init(&(fifos[n].lock), 1);
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
