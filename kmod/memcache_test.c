#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DRV_NAME "memcache_test"
#define DEV_BASENAME "memcache"

enum memcache_type {
	MEMCACHE_WB = 0,
	MEMCACHE_UC = 1,
	MEMCACHE_WC = 2,
	MEMCACHE_MAX = 3,
};

struct memcache_region {
	enum memcache_type type;
	size_t size_bytes;
	unsigned long nr_pages;
	struct page **pages;
};

static unsigned int size_mb = 16;
module_param(size_mb, uint, 0644);

static int numa_node = -1;
module_param(numa_node, int, 0644);

static dev_t memcache_devt;
static struct class *memcache_class;
static struct cdev memcache_cdev;
static struct memcache_region regions[MEMCACHE_MAX];

static const char *type_name(enum memcache_type t)
{
	switch (t) {
	case MEMCACHE_WB:
		return "wb";
	case MEMCACHE_UC:
		return "uc";
	case MEMCACHE_WC:
		return "wc";
	default:
		return "unknown";
	}
}

static pgprot_t type_pgprot(enum memcache_type t, pgprot_t prot)
{
	switch (t) {
	case MEMCACHE_WB:
		return prot;
	case MEMCACHE_UC:
		return pgprot_noncached(prot);
	case MEMCACHE_WC:
		return pgprot_writecombine(prot);
	default:
		return prot;
	}
}

static int memcache_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);

	if (minor >= MEMCACHE_MAX)
		return -ENODEV;

	file->private_data = &regions[minor];
	return 0;
}

static long memcache_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct memcache_region *r = file->private_data;
	u64 v;

	switch (cmd) {
	case 0:
		v = r->size_bytes;
		if (copy_to_user((void __user *)arg, &v, sizeof(v)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}

static int memcache_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct memcache_region *r = file->private_data;
	unsigned long requested = vma->vm_end - vma->vm_start;
	unsigned long i;
	int ret;

	if (!r)
		return -EINVAL;

	if (requested > r->size_bytes)
		return -EINVAL;

	pr_info(DRV_NAME ": mmap %s requested=%lu bytes (%lu pages)\n",
		type_name(r->type), requested, requested >> PAGE_SHIFT);

	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_page_prot = type_pgprot(r->type, vma->vm_page_prot);

	for (i = 0; i < (requested >> PAGE_SHIFT); i++) {
		ret = vm_insert_page(vma, vma->vm_start + (i << PAGE_SHIFT), r->pages[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct file_operations memcache_fops = {
	.owner = THIS_MODULE,
	.open = memcache_open,
	.unlocked_ioctl = memcache_ioctl,
	.mmap = memcache_mmap,
	.llseek = no_llseek,
};

static int region_alloc(struct memcache_region *r, enum memcache_type type, size_t size_bytes)
{
	unsigned long i;
	int ret = 0;

	r->type = type;
	r->size_bytes = size_bytes;
	r->nr_pages = (size_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
	r->pages = kcalloc(r->nr_pages, sizeof(r->pages[0]), GFP_KERNEL);
	if (!r->pages)
		return -ENOMEM;

	for (i = 0; i < r->nr_pages; i++) {
		if (numa_node >= 0)
			r->pages[i] = alloc_pages_node(numa_node, GFP_KERNEL | __GFP_ZERO, 0);
		else
			r->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!r->pages[i]) {
			ret = -ENOMEM;
			goto err;
		}
	}

	return 0;

err:
	for (i = 0; i < r->nr_pages; i++) {
		if (r->pages[i])
			__free_page(r->pages[i]);
	}
	kfree(r->pages);
	r->pages = NULL;
	r->nr_pages = 0;
	r->size_bytes = 0;
	return ret;
}

static void region_free(struct memcache_region *r)
{
	unsigned long i;

	if (!r || !r->pages)
		return;

	for (i = 0; i < r->nr_pages; i++) {
		if (r->pages[i])
			__free_page(r->pages[i]);
	}

	kfree(r->pages);
	r->pages = NULL;
	r->nr_pages = 0;
	r->size_bytes = 0;
}

static int __init memcache_init(void)
{
	int ret;
	int i;
	size_t size_bytes;

	size_bytes = (size_t)size_mb * 1024 * 1024;
	if (!size_bytes)
		return -EINVAL;

	if (numa_node >= 0 && !node_online(numa_node)) {
		pr_err(DRV_NAME ": numa_node=%d is not online\n", numa_node);
		return -EINVAL;
	}

	pr_info(DRV_NAME ": init size_mb=%u size_bytes=%zu numa_node=%d\n", size_mb, size_bytes,
		numa_node);

	ret = alloc_chrdev_region(&memcache_devt, 0, MEMCACHE_MAX, DRV_NAME);
	if (ret)
		return ret;

	cdev_init(&memcache_cdev, &memcache_fops);
	memcache_cdev.owner = THIS_MODULE;
	ret = cdev_add(&memcache_cdev, memcache_devt, MEMCACHE_MAX);
	if (ret)
		goto err_unreg;

	memcache_class = class_create(THIS_MODULE, DRV_NAME);
	if (IS_ERR(memcache_class)) {
		ret = PTR_ERR(memcache_class);
		memcache_class = NULL;
		goto err_cdev;
	}

	for (i = 0; i < MEMCACHE_MAX; i++) {
		ret = region_alloc(&regions[i], (enum memcache_type)i, size_bytes);
		if (ret)
			goto err_regions;
	}

	for (i = 0; i < MEMCACHE_MAX; i++) {
		device_create(memcache_class, NULL, memcache_devt + i, NULL, "%s_%s", DEV_BASENAME,
			      type_name((enum memcache_type)i));
		pr_info(DRV_NAME ": /dev/%s_%s size_bytes=%zu pages=%lu first_page_nid=%d\n", DEV_BASENAME,
			type_name((enum memcache_type)i), regions[i].size_bytes, regions[i].nr_pages,
			(regions[i].pages && regions[i].pages[0]) ? page_to_nid(regions[i].pages[0]) : -1);
	}

	return 0;

err_regions:
	for (i = 0; i < MEMCACHE_MAX; i++)
		region_free(&regions[i]);
	if (memcache_class)
		class_destroy(memcache_class);
err_cdev:
	cdev_del(&memcache_cdev);
err_unreg:
	unregister_chrdev_region(memcache_devt, MEMCACHE_MAX);
	return ret;
}

static void __exit memcache_exit(void)
{
	int i;

	for (i = 0; i < MEMCACHE_MAX; i++)
		device_destroy(memcache_class, memcache_devt + i);

	for (i = 0; i < MEMCACHE_MAX; i++)
		region_free(&regions[i]);

	if (memcache_class)
		class_destroy(memcache_class);

	cdev_del(&memcache_cdev);
	unregister_chrdev_region(memcache_devt, MEMCACHE_MAX);
}

module_init(memcache_init);
module_exit(memcache_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("memcachetest");
MODULE_DESCRIPTION("Cache attribute test: wb/uc/wc mmap regions");
