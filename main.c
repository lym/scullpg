#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/fcntl.h>
#include <linux/aio.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/device.h>

#include "scullpg.h"

int scullpg_major	= SCULLPG_MAJOR;
int scullpg_devs	= SCULLPG_DEVS;
int scullpg_qset	= SCULLPG_QSET;
int scullpg_order	= SCULLPG_ORDER;

module_param(scullpg_major, int, 0);
module_param(scullpg_devs, int, 0);
module_param(scullpg_qset, int, 0);
module_param(scullpg_order, int, 0);

MODULE_AUTHOR("Salym Seyonga");
MODULE_LICENSE("GPL");

struct scullpg_dev *scullpg_devices;
int scullpg_trim(struct scullpg_dev *dev);
void scullpg_cleanup(void);
static struct class *sc;	/* for the device class for `/sys`*/

#ifdef SCULLPG_USE_PROC
/* put seq_file stuff here, for debugging */
#endif

int scullpg_open(struct inode *inode, struct file *filp)
{
	struct scullpg_dev *dev;

	dev = container_of(inode->i_cdev, struct scullpg_dev, cdev);

	/* trim to 0 if opened in write-only mode */
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		if (down_interruptible(&dev->sem))
				return -ERESTARTSYS;
		scullpg_trim(dev);
		up(&dev->sem);
	}

	filp->private_data = dev;

	return 0;
}

int scullpg_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/*
 * private/utility function for read()
 * TODO: possibly rename this in the future, current name seems a bit off*/
struct scullpg_dev *scullpg_follow(struct scullpg_dev *dev, int n)
{
	while (n--) {
		if (!dev->next) {
			dev->next = kmalloc(sizeof(struct scullpg_dev),
					    GFP_KERNEL);
			memset(dev->next, 0, sizeof(struct scullpg_dev));
		}
		dev = dev->next;
		continue;
	}
	return dev;
}

/* Data management: read and write */

ssize_t scullpg_read(struct file *filp, char __user *buf, size_t count,
		     loff_t *f_pos)
{
	struct scullpg_dev *dptr;
	int item, s_pos, q_pos, rest;
	struct scullpg_dev *dev	= filp->private_data;

	int quantum	= PAGE_SIZE << dev->order;
	int qset	= dev->qset;
	int itemsize	= quantum * qset; /* how many bytes in the listitem */
	ssize_t retval	= 0;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (*f_pos > dev->size)
		goto nothing;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	item	= ((long) *f_pos) / itemsize;
	rest	= ((long) *f_pos) % itemsize;
	s_pos	= rest / quantum;
	q_pos	= rest % quantum;

	/* follow the list up to the right position */
	dptr	= scullpg_follow(dev, item);

	if (!dptr->data)
		goto nothing;	/* don't fill holes */
	if (!dptr->data[s_pos])
		goto nothing;
	if (count > quantum - q_pos)
		count = quantum - q_pos; /* read only up to end of this quantum */

	if (copy_to_user(buf, (dptr->data[s_pos] + q_pos), count)) {
		retval	= -EFAULT;
		goto nothing;
	}
	up(&dev->sem);

	*f_pos += count;

	return count;

nothing:
	up(&dev->sem);
	return retval;
}

ssize_t scullpg_write(struct file *filp, const char __user *buf, size_t count,
		      loff_t *f_pos)
{
	struct scullpg_dev *dptr;
	int item, s_pos, q_pos, rest;
	struct scullpg_dev *dev	= filp->private_data;

	int quantum	= PAGE_SIZE << dev->order;
	int qset	= dev->qset;
	int itemsize	= quantum * qset;
	ssize_t retval	= -ENOMEM;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	/* find listitem, qset index and offset in the quantum */
	item	= ((long) *f_pos) / itemsize;
	rest	= ((long) *f_pos) % itemsize;
	s_pos	= rest / quantum;
	q_pos	= rest % quantum;

	/* traverse list to right position */
	dptr	= scullpg_follow(dev, item);
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(void *), GFP_KERNEL);
		if (!dptr->data)
			goto nomem;
		memset(dptr->data, 0, qset * sizeof(char *));
	}

	/* And the allocation of a single quantum....*/
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = (void *)__get_free_pages(GFP_KERNEL,
							     dptr->order);
		if (!dptr->data[s_pos])
			goto nomem;
		memset(dptr->data[s_pos], 0, PAGE_SIZE << dptr->order);
	}
	if (count > quantum - q_pos)
		count = quantum - q_pos; /* write only till end of quantum */
	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval	= -EFAULT;
		goto nomem;
	}
	*f_pos += count;

	/* update the size */
	if (dev->size < *f_pos)
		dev->size = *f_pos;
	up(&dev->sem);
	return count;
nomem:
	up(&dev->sem);
	return retval;
}

/* ioctl() */
long scullpg_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		  unsigned long arg)
{
	int tmp;
	int err = 0;
	int ret = 0;

	/* don't decode invalid cmds. return ENOTTY rather than EFAULT */
	if (_IOC_TYPE(cmd) != SCULLPG_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > SCULLPG_IOC_MAXNR)
		return -ENOTTY;

	/*
	 * the type is a bitmask, and VERIFY_WRITE catches R/W transfers.
	 * Note that the type is user-oriented, while verify_area is
	 * kernel-oriented, so the concept of read and write is reversed
	 */
	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE, (void __user *)arg,
				 _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ, (void __user *)arg,
				 _IOC_SIZE(cmd));
	if (err)
		return -EFAULT;

	switch(cmd) {
		case SCULLPG_IOCRESET:
			scullpg_qset	= SCULLPG_QSET;
			scullpg_order	= SCULLPG_ORDER;
			break;

		case SCULLPG_IOCSORDER:	/* Set: arg points to the value */
			ret = __get_user(scullpg_order, (int __user *)arg);
			break;

		case SCULLPG_IOCTORDER:	/* Tell: arg is the value */
			scullpg_order = arg;
			break;

		case SCULLPG_IOCGORDER:	/* Get: arg is pointer to result */
			ret = __put_user(scullpg_order, (int __user *)arg);
			break;

		case SCULLPG_IOCQORDER:	/* Query: return it (it's positive) */
			return scullpg_order;

		case SCULLPG_IOCXORDER: /* eXchange: use arg as pointer */
			tmp = scullpg_order;
			ret = __get_user(scullpg_order, (int __user *)arg);
			if (ret == 0)
				ret = __put_user(tmp, (int __user *)arg);
			break;

		case SCULLPG_IOCHORDER:	/* sHift: Tell + Query */
			tmp = scullpg_order;
			scullpg_order = arg;
			return tmp;

		case SCULLPG_IOCSQSET:
			ret = __get_user(scullpg_qset, (int __user *) arg);
			break;

		case SCULLPG_IOCTQSET:
			scullpg_qset = arg;
			break;

		case SCULLPG_IOCGQSET:
			ret = __put_user(scullpg_qset, (int __user *)arg);
			break;

		case SCULLPG_IOCQQSET:
			return scullpg_qset;

		case SCULLPG_IOCXQSET:
			tmp = scullpg_qset;
			ret = __get_user(scullpg_qset, (int __user *)arg);
			if (ret == 0)
				ret = __put_user(tmp,  (int __user *)arg);
			break;

		case SCULLPG_IOCHQSET:
			tmp = scullpg_qset;
			scullpg_qset = arg;
			return tmp;
		default:	/* redundant, as cmd was checked against MAXNR */
			return -ENOTTY;
	}
	return ret;
}

/*
 * The extended operations
 */
loff_t scullpg_llseek(struct file *filp, loff_t off, int whence)
{
	struct scullpg_dev *dev = filp->private_data;
	long newpos;

	switch(whence) {
		case 0:
			newpos = off;
			break;

		case 1:
			newpos = filp->f_pos + off;
			break;

		case 2:
			newpos = dev->size + off;
			break;

		default:
			return -EINVAL;
	}

	if (newpos < 0)
		return -EINVAL;
	filp->f_pos = newpos;
	return newpos;
}

/* A simple I/0 implementation */
struct async_work {
	struct kiocb *iocb;
	int result;
	struct work_struct work;
};

struct delayed_work *dwork;	/* for schedule delayed_work */

/* "Complete" an asynchronous operation */
static void scullpg_do_deferred_op(struct work_struct *work)
{
	struct async_work *p = container_of(work, struct async_work, work);
	aio_complete(p->iocb, p->result, 0);
	kfree(p);
}

static ssize_t scullpg_defer_op(int write, struct kiocb *iocb, char __user *buf,
				size_t count, loff_t pos)
{
	int result;
	struct async_work *stuff;

	/* Copy now while we can access the buffer */
	if (write)
		result = scullpg_write(iocb->ki_filp, buf, count, &pos);
	else
		result = scullpg_read(iocb->ki_filp, buf, count, &pos);

	/* If this is an asynchronous IOCB, we return our status now */
	if (is_sync_kiocb(iocb))
			return result;

	/* otherwise defer the completion for a few milliseconds */
	stuff = kmalloc(sizeof(*stuff), GFP_KERNEL);
	if (stuff == NULL)
		return result;	/* No memory, just complete now */
	stuff->iocb	= iocb;
	stuff->result	= result;
	dwork->work	= stuff->work;
	INIT_WORK(&stuff->work, scullpg_do_deferred_op);
	schedule_delayed_work(dwork, HZ/100);
	return -EIOCBQUEUED;
}

static ssize_t scullpg_aio_read(struct kiocb *iocb, char __user *buf,
				size_t count, loff_t pos)
{
	return scullpg_defer_op(0, iocb, buf, count, pos);
}

static ssize_t scullpg_aio_write(struct kiocb *iocb, const char __user *buf,
				 size_t count, loff_t pos)
{
	return scullpg_defer_op(1, iocb, (char __user *) buf, count, pos);
}

extern int scullpg_mmap(struct file *filp, struct vm_area_struct *vma);

/* fops */
struct file_operations scullpg_fops = {
	.owner		= THIS_MODULE,
	.llseek		= scullpg_llseek,
	.read		= scullpg_read,
	.write		= scullpg_write,
	.unlocked_ioctl	= scullpg_ioctl,
	.mmap		= scullpg_mmap,
	.open		= scullpg_open,
	.release	= scullpg_release,
	.aio_read	= scullpg_aio_read,
	.aio_write	= scullpg_aio_write,
};

/* Assumes `dev` is not NULL */
int scullpg_trim(struct scullpg_dev *dev)
{
	int i;
	struct scullpg_dev *next, *dptr;
	int qset	= dev->qset;	/* dev is not null */

	if (dev->vmas) /* don't trim: there are active mappings */
		return -EBUSY;

	for (dptr = dev; dptr; dptr = next) { /* for all list-items */
		if (dptr->data) {
			/* This code frees a whole quantum-set */
			for (i = 0; i < qset; i++)
				if (dptr->data[i])
					free_pages((unsigned long)(dptr->data[i]),
						   dptr->order);
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		if (dptr != dev)
			kfree(dptr);	/* all of them but the first */
	}
	dev->size	= 0;
	dev->qset	= scullpg_qset;
	dev->order	= scullpg_order;
	dev->next	= NULL;
	return 0;
}

static void scullpg_setup_cdev(struct scullpg_dev *dev, int index)
{
	int err;
	int devno = MKDEV(scullpg_major, index);

	if (dev == NULL) {
		pr_info("dev is null");
		return;
	}
	cdev_init(&dev->cdev, &scullpg_fops);
	dev->cdev.owner	= THIS_MODULE;
	dev->cdev.ops	= &scullpg_fops;

	err = cdev_add(&dev->cdev, devno, 1);
	if (err)
		pr_info("Error %d adding scull%d", err, index);
}

int scullpg_init(void)
{
	int result, i;
	dev_t dev = MKDEV(scullpg_major, 0);

	/* Register your major and accept a dynamic number */
	if (scullpg_major)
		result = register_chrdev_region(dev, scullpg_devs, "scullpg");
	else {
		result = alloc_chrdev_region(&dev, 0, scullpg_devs, "scullpg");
		scullpg_major = MAJOR(dev);
	}

	if (result < 0)
		return result;

	if ((sc = class_create(THIS_MODULE, "scullpg")) == NULL) {
		goto fail_init;
	}

	if (device_create(sc, NULL, dev, NULL, "scull_pg") == NULL) {
		class_destroy(sc);
		goto fail_init;
	}

	/* Allocate the devices -- we can't have them static, as the number
	 * can be specified at load time
	 */
	scullpg_devices = kmalloc(scullpg_devs * sizeof(struct scullpg_dev),
				  GFP_KERNEL);
	if (!scullpg_devices) {
		result = -ENOMEM;
		goto fail_init;
	}

	memset(scullpg_devices, 0, scullpg_devs * sizeof(struct scullpg_dev));
	for (i = 0; i < scullpg_devs; i++) {
		scullpg_devices[i].order	= scullpg_order;
		scullpg_devices[i].qset		= scullpg_qset;
		sema_init(&scullpg_devices[i].sem, 1);
		scullpg_setup_cdev(scullpg_devices + 1, i);
	}

	return 0;

fail_init:
	unregister_chrdev_region(dev, scullpg_devs);
	return result;
}

void scullpg_cleanup(void)
{
	int i;

	for (i = 0; i < scullpg_devs; i++) {
		cdev_del(&scullpg_devices[i].cdev);
		scullpg_trim(scullpg_devices + i);
	}
	kfree(scullpg_devices);
	unregister_chrdev_region(MKDEV(scullpg_major, 0), scullpg_devs);
}

module_init(scullpg_init);
module_exit(scullpg_cleanup);
