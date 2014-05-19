/*
 * mmap.c -- memory mapping for the scullp character module
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <asm/pgtable.h>
#include <linux/fs.h>

#include "scullpg.h"

/*
 * open and close: just keep track of how many times the device is mapped, to
 * avoid prematurely releasing it.
 */
void scullpg_vma_open(struct vm_area_struct *vma)
{
	struct scullpg_dev *dev	= vma->vm_private_data;

	dev->vmas++;
}

void scullpg_vma_close(struct vm_area_struct *vma)
{
	struct scullpg_dev *dev	= vma->vm_private_data;
	dev->vmas--;
}

static int scullpg_vma_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	unsigned long offset;
	struct page *page;

	struct scullpg_dev *ptr;
	struct scullpg_dev *dev = vma->vm_private_data;
	void *pageptr	= NULL;

	down(&dev->sem);
        offset = (unsigned long)vmf->virtual_address - vma->vm_start;
	if (offset >= dev->size)
		goto out;

	offset >>= PAGE_SHIFT;
	for (ptr = dev; ptr && offset >= dev->qset;) {
		ptr = ptr->next;
		offset -= dev->qset;
	}

	if (ptr && ptr->data)
		pageptr = ptr->data[offset];
	if (!pageptr)
		goto out;
	page = virt_to_page(pageptr);

	get_page(page);
	vmf->page = page;
	//if (type)
	//	*type = VM_FAULT_MINOR;
	up(&dev->sem);
	return 0;
out:
	up(&dev->sem);
	return (int)page;
}

struct vm_operations_struct scullpg_vm_ops = {
	.open	= scullpg_vma_open,
	.close	= scullpg_vma_close,
	.fault	= scullpg_vma_fault,
};

int scullpg_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct inode *inode	= filp->f_dentry->d_inode;
	//struct inode *inode	= filp->f_path.dentry->d_inode;

	/* refuse to map if order is not zero */
	if (scullpg_devices[iminor(inode)].order)
		return -ENODEV;

	vma->vm_ops	= &scullpg_vm_ops;
	//vma->vm_flags  |= VM_RESERVED;
	vma->vm_flags  |= (VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_private_data	= filp->private_data;
	scullpg_vma_open(vma);
	return 0;
}
