/*
 * nos-user-bde — Userspace BDE: /dev/nos-bde character device
 * ioctl: READ_REG, WRITE_REG, GET_DMA_INFO, SCHAN_OP
 * mmap: DMA pool (via remap_pfn_range)
 * Depends on nos_kernel_bde (kernel BDE exports).
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/device.h>

/* From nos_kernel_bde.ko */
extern void __iomem *nos_bde_get_bar0(void);
extern dma_addr_t nos_bde_get_dma_pbase(void);
extern size_t nos_bde_get_dma_size(void);
extern int nos_bde_schan_op(const __u32 *cmd, int cmd_words, __u32 *data, int data_words, int *status);

#define NOS_BDE_MAJOR	0
#define NOS_BDE_MINOR	0
#define DEVNAME		"nos-bde"

#define NOS_BDE_MAGIC	'B'
#define NOS_BDE_READ_REG		_IOWR(NOS_BDE_MAGIC, 1, struct nos_bde_reg)
#define NOS_BDE_WRITE_REG	_IOW(NOS_BDE_MAGIC, 2, struct nos_bde_reg)
#define NOS_BDE_GET_DMA_INFO	_IOR(NOS_BDE_MAGIC, 3, struct nos_bde_dma_info)
#define NOS_BDE_SCHAN_OP		_IOWR(NOS_BDE_MAGIC, 4, struct nos_bde_schan)

struct nos_bde_reg {
	__u32 offset;
	__u32 value;
};

struct nos_bde_dma_info {
	__u64 pbase;
	__u32 size;
};

struct nos_bde_schan {
	__u32 cmd[8];
	__u32 data[16];
	__s32 len;
	__s32 status;
};

static int nos_bde_mmap(struct file *filp, struct vm_area_struct *vma)
{
	size_t size = nos_bde_get_dma_size();
	dma_addr_t pbase = nos_bde_get_dma_pbase();
	void __iomem *bar0 = nos_bde_get_bar0();

	(void)filp;
	if (!bar0 || pbase == 0 || size == 0)
		return -ENODEV;
	if (vma->vm_end - vma->vm_start > size)
		return -EINVAL;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, pbase >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static long nos_bde_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __iomem *bar0 = nos_bde_get_bar0();
	struct nos_bde_reg reg;
	struct nos_bde_dma_info dma_info;
	struct nos_bde_schan schan;
	long err = 0;

	(void)filp;
	if (!bar0)
		return -ENODEV;

	switch (cmd) {
	case NOS_BDE_READ_REG:
		if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
			return -EFAULT;
		if (reg.offset >= 0x40000)
			return -EINVAL;
		reg.value = ioread32(bar0 + reg.offset);
		if (copy_to_user((void __user *)arg, &reg, sizeof(reg)))
			return -EFAULT;
		break;
	case NOS_BDE_WRITE_REG:
		if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
			return -EFAULT;
		if (reg.offset >= 0x40000)
			return -EINVAL;
		iowrite32(reg.value, bar0 + reg.offset);
		break;
	case NOS_BDE_GET_DMA_INFO:
		dma_info.pbase = nos_bde_get_dma_pbase();
		dma_info.size = (__u32)nos_bde_get_dma_size();
		if (copy_to_user((void __user *)arg, &dma_info, sizeof(dma_info)))
			return -EFAULT;
		break;
	case NOS_BDE_SCHAN_OP:
		if (copy_from_user(&schan, (void __user *)arg, sizeof(schan)))
			return -EFAULT;
		/* Call kernel BDE to run S-Channel DMA */
		if (nos_bde_schan_op(schan.cmd, schan.len, schan.data, 16, &schan.status) < 0)
			return -EIO;
		if (copy_to_user((void __user *)arg, &schan, sizeof(schan)))
			return -EFAULT;
		break;
	default:
		return -ENOTTY;
	}
	return err;
}

static const struct file_operations nos_bde_fops = {
	.owner	 = THIS_MODULE,
	.mmap	 = nos_bde_mmap,
	.unlocked_ioctl = nos_bde_ioctl,
};

static dev_t devt;
static struct cdev cdev;
static struct class *nos_bde_class;

static int __init nos_user_bde_init(void)
{
	int rc;
	nos_bde_class = class_create(THIS_MODULE, "nos_bde");
	if (IS_ERR(nos_bde_class))
		return PTR_ERR(nos_bde_class);
	rc = alloc_chrdev_region(&devt, 0, 1, DEVNAME);
	if (rc)
		goto err_class;
	cdev_init(&cdev, &nos_bde_fops);
	cdev.owner = THIS_MODULE;
	rc = cdev_add(&cdev, devt, 1);
	if (rc)
		goto err_region;
	if (!device_create(nos_bde_class, NULL, devt, NULL, DEVNAME)) {
		rc = -ENODEV;
		goto err_cdev;
	}
	pr_info("nos-user-bde: /dev/%s created\n", DEVNAME);
	return 0;
err_cdev:
	cdev_del(&cdev);
err_region:
	unregister_chrdev_region(devt, 1);
err_class:
	class_destroy(nos_bde_class);
	return rc;
}

static void __exit nos_user_bde_exit(void)
{
	device_destroy(nos_bde_class, devt);
	cdev_del(&cdev);
	unregister_chrdev_region(devt, 1);
	class_destroy(nos_bde_class);
}

module_init(nos_user_bde_init);
module_exit(nos_user_bde_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Userspace BDE /dev/nos-bde (open-nos-as5610)");
