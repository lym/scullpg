#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>

/* For debugging */
#undef PDEBUG
#ifdef SCULLPG_DEBUG
#	ifdef __KERNEL__
#		define PDEBUG(fmt, args...) printk(KERN_DEBUG "scullp: " fmt, ## args)
#	else
		/* for user space */
#		define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#	endif
#else
#	define PDEBUG(fmt, args...)
#endif

#undef PDEBUGG
#define PDEBUGG(fmt, args...)

#define SCULLPG_MAJOR	0	/* dynamic major by default */
#define SCULLPG_DEVS	4	/* scullpg0 through scullpg3 */

/*
 * The "bare" device is just a variable-length region of memory. Use a linked
 * list of indirect blocks.
 *
 * "scullpg_dev->data" points to an array of pointers, each pointer refers to a
 * memory page.
 *
 * The array (quantum-set) is SCULLPG_QSET long.
 */

#define SCULLPG_ORDER	0	/* one page at a time */
#define SCULLPG_QSET	500

struct scullpg_dev {
	void **data;
	struct scullpg_dev *next;	/* next list-item */
	int vmas;			/* active mappings */
	int order;			/* the current allocation order */
	int qset;			/* the current array size */
	size_t size;			/* 32-bit will suffice */
	struct semaphore sem;
	struct cdev cdev;
};

extern struct scullpg_dev *scullpg_devices;
extern struct file_operations scullpg_fops;

/* The configurable parameters */
extern int scullpg_major;
extern int scullpg_devs;
extern int scullpg_order;
extern int scullpg_qset;

/* shared functions */
int scullpg_trim(struct scullpg_dev *dev);
struct scullpg_dev *scullpg_follow(struct scullpg_dev *dev, int n);

#ifdef SCULLPG_DEBUG
#	define SCULLPG_USE_PROC
#endif

/* ioctl */

/* Use 0x81 as magic number */
#define SCULLPG_IOC_MAGIC	0x81		/* 00-0C */

#define SCULLPG_IOCRESET	_IO(SCULLPG_IOC_MAGIC, 0)

/*
 * S means "Set" through a pointer
 * T means "Tell" directly
 * G means "Get" (to a pointed var)
 * Q means "Query", response is on the return value
 * X means "eXchange": G and S atomically
 * H means "sHift": T and Q atomically
 */
#define SCULLPG_IOCSORDER	_IOW(SCULLPG_IOC_MAGIC,	  1, int)
#define SCULLPG_IOCTORDER	_IO(SCULLPG_IOC_MAGIC,	  2)
#define SCULLPG_IOCGORDER	_IOR(SCULLPG_IOC_MAGIC,	  3, int)
#define SCULLPG_IOCQORDER	_IO(SCULLPG_IOC_MAGIC,	  4)
#define SCULLPG_IOCXORDER	_IOWR(SCULLPG_IOC_MAGIC,  5, int)
#define SCULLPG_IOCHORDER	_IO(SCULLPG_IOC_MAGIC,	  6)
#define SCULLPG_IOCSQSET	_IOW(SCULLPG_IOC_MAGIC,	  7, int)
#define SCULLPG_IOCTQSET	_IO(SCULLPG_IOC_MAGIC,	  8)
#define SCULLPG_IOCGQSET	_IOR(SCULLPG_IOC_MAGIC,	  9, int)
#define SCULLPG_IOCQQSET	_IO(SCULLPG_IOC_MAGIC,	 10)
#define SCULLPG_IOCXQSET	_IOWR(SCULLPG_IOC_MAGIC, 11, int)
#define SCULLPG_IOCHQSET	_IO(SCULLPG_IOC_MAGIC,	 12)

#define SCULLPG_IOC_MAXNR	12
