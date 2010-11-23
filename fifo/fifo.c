#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <linux/semaphore.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <linux/interrupt.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nerdbuero Staff");

#define NUM_MINORS 3
#define FIFOSIZE 11
#define FIRST_MINOR 0

static dev_t dev;
static struct cdev char_dev;
static struct workqueue_struct* workqueue;
static struct work_struct work;
static atomic_t working = ATOMIC_INIT(0);
static struct timer_list copytimer;
static struct tasklet_struct fifo_tasklet;

struct fifo
{
	int rcnt;
	int wcnt;
	atomic_t level;
	struct semaphore lock;
	wait_queue_head_t read_queue;
	wait_queue_head_t write_queue;
	char buffer[FIFOSIZE];
};

// First half are write fifos, second half are read fifos
static struct fifo fifos[NUM_MINORS * 2];

static struct semaphore level_lock;
static int level[NUM_MINORS];
static int level_len = NUM_MINORS;
module_param_array(level, int, &level_len, S_IRUGO);
MODULE_PARM_DESC(level, "Fill level");

static unsigned long copy(void* dst, const void* src, unsigned long cnt)
{
	unsigned long n;
	for(n = cnt - 1; n >= 0; n--)
	{
		((char*)dst)[n] = ((const char*)src)[n];
	}
	return 0;
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

static int lockFifo(struct semaphore* sema, unsigned int flags)
{
	if((flags & O_NONBLOCK) != 0) // NON BLOCKING
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

static ssize_t fifo_read(int fifoIdx, void* data, size_t count, int flags, 
	unsigned long (*copy_func)(void* to, const void* from, unsigned long n))
{
	struct fifo* fifo = &(fifos[fifoIdx]);
	int ret, to_read;

	// Try to get lock on semaphore
	int s;
rlock:
	if((s = lockFifo(&(fifo->lock), flags)) != 0)
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
		if(s = wait_event_interruptible(fifo->read_queue, atomic_read(&(fifo->level)) > 0) != 0) // interruptible weil killbar sein wollen
		{
			return -ERESTARTSYS; // Signal
		}
		else
		{
			goto rlock; // Lock again
		}
	}

	to_read = atomic_read(&(fifo->level));

	// Check if we have enough bytes to fullfill the request
	to_read = to_read < count ? to_read : count;
	printk("Zeichen zu lesen %i\n", to_read);
	//printk("Writecount %i\n", fifo->wcnt);
	//printk("Readcount %i\n", fifo->rcnt);
	printk("Fuellstand %i\n",  level[fifoIdx]);
	
	int unreadBytes = 0;
	// ++++____
	if(fifo->wcnt > fifo->rcnt)
	{
		unreadBytes = copy_func(data, &(fifo->buffer[fifo->rcnt]), to_read);
		fifo->rcnt = (fifo->rcnt + to_read - unreadBytes) % FIFOSIZE;
	// ++__++++
	}
	else if(fifo->wcnt < fifo->rcnt)
	{
		if(fifo->rcnt + to_read < FIFOSIZE)
		{
			unreadBytes = copy_func(data, &(fifo->buffer[fifo->rcnt]), to_read);
			fifo->rcnt = (fifo->rcnt + to_read - unreadBytes) % FIFOSIZE;
		}
		else
		{
			int rechterRand = FIFOSIZE - fifo->rcnt;
			// Rechter Rand
			unreadBytes = copy_func(data, &(fifo->buffer[fifo->rcnt]), rechterRand);
			fifo->rcnt = (fifo->rcnt + rechterRand - unreadBytes) % FIFOSIZE;
			if(unreadBytes == 0)
			{
				// Linken Rand
				unreadBytes = copy_func(data + rechterRand, &(fifo->buffer[0]), to_read - rechterRand);
				fifo->rcnt = (fifo->rcnt + to_read - rechterRand) % FIFOSIZE;
			}
		}
	}
	else if(fifo->rcnt == fifo->wcnt) //fifo->lockdown == TRUE)
	{
		int rechterRand = FIFOSIZE - fifo->rcnt;
		// Rechter Rand
		unreadBytes = copy_func(data, &(fifo->buffer[fifo->rcnt]), rechterRand);
		fifo->rcnt = (fifo->rcnt + rechterRand - unreadBytes) % FIFOSIZE;
		if(unreadBytes == 0)
		{
			// Linken Rand
			unreadBytes = copy_func(data + to_read - rechterRand, &(fifo->buffer[0]), to_read - rechterRand);
			fifo->rcnt = (fifo->rcnt + to_read - rechterRand) % FIFOSIZE;
		}
	}
	printk("read: cnt %i\n", fifo->rcnt); 
	// FIFO entsperren, sofern es gesperrt war und nun mind. 1 byte 
	// gelesen wurde
	ret = to_read - unreadBytes;
	level[fifoIdx] -= ret;
	//atomic_set(&(fifo->rcnt), new_rcnt);
	atomic_sub(ret, &(fifo->level));
	printk("Fuellstand %i\n",  level[fifoIdx]);

	// Wake up waiting write processes
	wake_up_interruptible(&(fifo->write_queue));
	
	// Release semaphore
	up(&(fifo->lock));
	
	return ret;
}

static ssize_t fifo_io_read(struct file* filep, char __user *data,
							size_t count, loff_t *pOffset)
{
	printk("FIFO read called\n");

	int fifoIdx = (iminor(filep->f_dentry->d_inode) - FIRST_MINOR) + NUM_MINORS;
	return fifo_read(fifoIdx, data, count, filep->f_flags, copy_to_user);
}

static ssize_t fifo_write(int fifoIdx, void* data, size_t count, int flags, 
	unsigned long (*copy_func)(void* to, const void* from, unsigned long n))
{
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
	if((s = lockFifo(&(fifo->lock), flags)) != 0)
	{
		return s;
	}

	// Check if there is space to write something
	if(atomic_read(&(fifo->level)) == FIFOSIZE)
	{
		// Release the lock
		up(&(fifo->lock));
		
		
		printk("Write-Methode: Schlaf Data, Schlaf!");
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
		int totalBytesToWrite = FIFOSIZE - atomic_read(&(fifo->level)); //fifo->rcnt - fifo->wcnt;
		/*int rcnt = atomic_read(&(fifo->rcnt));
		int wcnt = atomic_read(&(fifo->wcnt));
		int new_wcnt = wcnt;*/
		/*if (totalBytesToWrite <= 0)
		{
			totalBytesToWrite = FIFOSIZE + totalBytesToWrite; 
	    }*/

		totalBytesToWrite = totalBytesToWrite < count ? totalBytesToWrite : count;
		printk("%i\n", totalBytesToWrite);

		if (fifo->wcnt > fifo->rcnt) // && level[fifoIdx] < FIFOSIZE - 1) //fifo->lockdown == FALSE)
		{
			if ((fifo->wcnt + totalBytesToWrite) <= FIFOSIZE)
			{ 
				int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, totalBytesToWrite);

				returnValue = totalBytesToWrite - n;
				fifo->wcnt += returnValue;
			}
			else 
			{
				int rechterRand = FIFOSIZE - fifo->wcnt;
				int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, rechterRand);
				int linkerRand = totalBytesToWrite - rechterRand;
				n += copy_from_user(&(fifo->buffer[0]), data+rechterRand,linkerRand);

				returnValue = totalBytesToWrite - n;
				fifo->wcnt += returnValue;
			}
		}
		else if (fifo->rcnt > fifo->wcnt) // && level[fifoIdx] < FIFOSIZE - 1) // fifo->lockdown == FALSE)
		{
			int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, totalBytesToWrite);
			returnValue = totalBytesToWrite - n;
			fifo->wcnt += returnValue;
		}
		else if (fifo->rcnt == fifo->wcnt) // && level[fifoIdx] < FIFOSIZE - 1) //fifo->lockdown == FALSE)
		{
			if ((fifo->wcnt + totalBytesToWrite) <= FIFOSIZE)
			{
				printk("%i\n", count);
				int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, totalBytesToWrite);
				returnValue = totalBytesToWrite -n;
				fifo->wcnt += returnValue;
			}
			else 
			{
				int rechterRand = FIFOSIZE - fifo->wcnt;
				int n = copy_from_user(&(fifo->buffer[fifo->wcnt]), data, rechterRand);
				int linkerRand = totalBytesToWrite - rechterRand;
				n+= copy_from_user(&(fifo->buffer[0]), data+rechterRand,linkerRand);
				returnValue = totalBytesToWrite - n;

				fifo->wcnt += returnValue;
			}
		}

		fifo->wcnt = fifo->wcnt % FIFOSIZE;
		printk("write: wcnt %i\n", fifo->wcnt);
		level[fifoIdx] += returnValue;
		printk("Fuellstand %i\n",  level[fifoIdx]);
		//atomic_set(&(fifo->wcnt), new_wcnt);
		atomic_add(returnValue, &(fifo->level));
	}
	
	// Wake up waiting reading processes
	wake_up_interruptible(&(fifo->read_queue));
	
	// Release semaphore
	up(&(fifos[fifoIdx].lock));
	
	if (fifoIdx == 2)
	{
		tasklet_schedule(&fifo_tasklet);
	}
	
	return returnValue;
}

static ssize_t fifo_io_write(struct file* filep, const char __user *data,
								size_t count, loff_t *pOffset)
{
	printk("FIFO write called\n");

	int fifoIdx = iminor(filep->f_dentry->d_inode) - FIRST_MINOR; // aktuelles fifo 
	
	
	return fifo_write(fifoIdx, data, count, filep->f_flags, copy_from_user);
}

struct file_operations fops =
{
   .owner	= THIS_MODULE,
   .open	= fifo_io_open,
   .release	= fifo_io_close,
   .read	= fifo_io_read,
   .write	= fifo_io_write
};

// Work queue handler that copies bytes of write fifo 0 to read fifo 0
static void wq_copy(void* data)
{
	int read, written;
	char buf[FIFOSIZE];	// Works only for small fifos

	while(atomic_read(&working) > 0)
	{
		// Copy from fifo 0 into buffer
		if((read = fifo_read(0, buf, FIFOSIZE, 0, copy)) < 0) // INS ERSTE FIFO KOPIEREN 
		{
			printk("Interrupted");
			// Error or exit
			continue;
		}
	
		written = 0;
		while(written < read)
		{
			// Copy from buffer into fifo 3
			written += fifo_write(3, buf + written, read - written, 0, copy);
		}
	}
}

static void tasklet_copy(void* data)
{
	int read, written;
	char buf[FIFOSIZE];	// Works only for small fifos
	
	// Copy from fifo 0 into buffer
	read = fifo_read(2, buf, FIFOSIZE, O_NONBLOCK, copy); 
	
	written = 0;
	while(written < read)
	{
			// Copy from buffer into fifo 3
			written += fifo_write(3, buf + written, read - written, 0, copy);
	}
}
	


static void timer_copy(unsigned long data)
{
	
}

static int __init fifo_init(void)
{
	int ret, n;
	printk("FIFO module loading...\n");

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
	for(n = 0; n < NUM_MINORS * 2; n++)
	{
		sema_init(&(fifos[n].lock), 1);
        init_waitqueue_head(&(fifos[n].read_queue));     
        init_waitqueue_head(&(fifos[n].write_queue));
        fifos->rcnt = 0;
        fifos->wcnt = 0;
        atomic_set(&(fifos->level), 0);
		level[n] = 0;
	}
	
	// Create work queue
	workqueue = create_singlethread_workqueue("fifo");
	INIT_WORK(&work, wq_copy);
	atomic_set(&working, 1);
	queue_work(workqueue, &work);
	
	// Initialize timer
	init_timer(&copytimer);



	// Initialize tasklet
	DECLARE_TASKLET(fifo_tasklet,tasklet_copy,0);
	
	
	printk("FIFO module loaded.\n");
	return 0;
}

static void __exit fifo_exit(void)
{
	printk("FIFO module unloading...\n");

	// Unregister character devices
	cdev_del(&char_dev);

	// Unregister character device numbers
	unregister_chrdev_region(dev, NUM_MINORS);
	
	// Destroy workqueue
	atomic_set(&working, 0);
	printk("atomic_set(&working, 0);\n");
	wake_up_interruptible(&(fifos[0].read_queue));  // Wake up the thread if sleeping
	wake_up_interruptible(&(fifos[3].write_queue)); // in a wait queue
	printk("wake up read/write queues\n");
	//destroy_workqueue(workqueue);
	
	// Destroy timer
	
	printk("FIFO module unloaded.\n");
}

module_init(fifo_init);
module_exit(fifo_exit);
