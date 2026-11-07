// SPDX-License-Identifier: GPL-2.0-only
/* Ashmem ABI on modern shmem/mmap_prepare, including mixed user granules.
 * Explicit unpin/pin/purge is supported. No background shrinker is installed;
 * unpin makes a region eligible for explicit purge, not a promise of eviction.
 * Huawei's separate CONFIG_PURGEABLE_ASHMEM extension is not advertised.
 */
#include <linux/bitmap.h>
#include <linux/capability.h>
#include <linux/falloc.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "ashmem-uapi.h"

#define ASHMEM_ABI_SHIFT 12
#define ASHMEM_ABI_PAGE (1UL << ASHMEM_ABI_SHIFT)
#define ASHMEM_PROT (PROT_READ | PROT_WRITE | PROT_EXEC)

struct hmos_area {
    struct list_head entry;
    struct mutex lock;
    char name[ASHMEM_NAME_LEN];
    unsigned long size;
    unsigned long prot;
    struct file *backing;
    unsigned long *unpinned;
    unsigned long *purged;
    unsigned int pages;
};

static LIST_HEAD(areas);
static DEFINE_MUTEX(areas_lock);

static int hmos_open(struct inode *inode, struct file *file)
{
    struct hmos_area *area = kzalloc(sizeof(*area), GFP_KERNEL_ACCOUNT);
    if (!area)
        return -ENOMEM;
    mutex_init(&area->lock);
    strscpy(area->name, ASHMEM_NAME_DEF, sizeof(area->name));
    area->prot = ASHMEM_PROT;
    file->private_data = area;
    mutex_lock(&areas_lock);
    list_add_tail(&area->entry, &areas);
    mutex_unlock(&areas_lock);
    return 0;
}

static int hmos_release(struct inode *inode, struct file *file)
{
    struct hmos_area *area = file->private_data;
    mutex_lock(&areas_lock);
    list_del(&area->entry);
    mutex_unlock(&areas_lock);
    if (area->backing)
        fput(area->backing);
    bitmap_free(area->unpinned);
    bitmap_free(area->purged);
    kfree(area);
    return 0;
}

static int hmos_mmap_prepare(struct vm_area_desc *desc)
{
    struct hmos_area *area = desc->file->private_data;
    struct vm_area_desc inner;
    unsigned long granule = mm_page_size(desc->mm);
    u64 offset = (u64)desc->pgoff << PAGE_SHIFT;
    int ret = 0;
#ifdef CONFIG_MM_SUBPAGE
    offset += desc->subpage_offset;
#endif
    mutex_lock(&area->lock);
    if (!area->size || offset > ALIGN(area->size, granule) ||
        desc->end - desc->start > ALIGN(area->size, granule) - offset) {
        ret = -EINVAL;
        goto out;
    }
    if ((vma_desc_test(desc, VMA_READ_BIT) && !(area->prot & PROT_READ)) ||
        (vma_desc_test(desc, VMA_WRITE_BIT) && !(area->prot & PROT_WRITE)) ||
        (vma_desc_test(desc, VMA_EXEC_BIT) && !(area->prot & PROT_EXEC))) {
        ret = -EPERM;
        goto out;
    }
    if (!(area->prot & PROT_READ))
        vma_desc_clear_flags(desc, VMA_MAYREAD_BIT);
    if (!(area->prot & PROT_WRITE))
        vma_desc_clear_flags(desc, VMA_MAYWRITE_BIT);
    if (!(area->prot & PROT_EXEC))
        vma_desc_clear_flags(desc, VMA_MAYEXEC_BIT);
    if (!area->backing) {
        area->backing = shmem_file_setup(area->name, area->size,
                                        mk_vma_flags(VMA_NORESERVE_BIT));
        if (IS_ERR(area->backing)) {
            ret = PTR_ERR(area->backing);
            area->backing = NULL;
            goto out;
        }
    }
    inner = *desc;
    inner.file = area->backing;
    inner.vm_file = get_file(area->backing);
    ret = vfs_mmap_prepare(area->backing, &inner);
    if (ret) {
        fput(inner.vm_file);
        goto out;
    }
    /* mmap core takes ownership of this new file reference. */
    desc->vm_file = inner.vm_file;
    desc->vm_ops = inner.vm_ops;
    desc->private_data = inner.private_data;
out:
    mutex_unlock(&area->lock);
    return ret;
}

static ssize_t hmos_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct hmos_area *area = iocb->ki_filp->private_data;
    struct file *backing;
    struct kiocb inner;
    ssize_t ret;
    mutex_lock(&area->lock);
    backing = area->backing ? get_file(area->backing) : NULL;
    mutex_unlock(&area->lock);
    if (!backing)
        return -EBADF;
    init_sync_kiocb(&inner, backing);
    inner.ki_pos = iocb->ki_pos;
    ret = backing->f_op->read_iter ? backing->f_op->read_iter(&inner, to) : -EOPNOTSUPP;
    iocb->ki_pos = inner.ki_pos;
    fput(backing);
    return ret;
}

static loff_t hmos_llseek(struct file *file, loff_t offset, int origin)
{
    struct hmos_area *area = file->private_data;
    return fixed_size_llseek(file, offset, origin, READ_ONCE(area->size));
}

static int prepare_bitmaps(struct hmos_area *area)
{
    unsigned long pages = DIV_ROUND_UP(area->size, ASHMEM_ABI_PAGE);
    if (!pages || pages > UINT_MAX)
        return -EINVAL;
    if (area->unpinned)
        return 0;
    area->unpinned = bitmap_zalloc(pages, GFP_KERNEL_ACCOUNT);
    area->purged = bitmap_zalloc(pages, GFP_KERNEL_ACCOUNT);
    if (!area->unpinned || !area->purged) {
        bitmap_free(area->unpinned);
        bitmap_free(area->purged);
        area->unpinned = area->purged = NULL;
        return -ENOMEM;
    }
    area->pages = pages;
    return 0;
}

static long pin_range(struct hmos_area *area, unsigned int cmd, unsigned long arg)
{
    struct ashmem_pin pin;
    unsigned long begin, end, length, bytes;
    int ret;
    if (copy_from_user(&pin, (void __user *)arg, sizeof(pin)))
        return -EFAULT;
    if (!area->backing)
        return -EINVAL;
    length = ALIGN(area->size, ASHMEM_ABI_PAGE);
    if (pin.offset > length)
        return -EINVAL;
    bytes = pin.len ? pin.len : length - pin.offset;
    if (!IS_ALIGNED(pin.offset, ASHMEM_ABI_PAGE) ||
        !IS_ALIGNED(bytes, ASHMEM_ABI_PAGE) ||
        bytes > length - pin.offset)
        return -EINVAL;
    begin = pin.offset >> ASHMEM_ABI_SHIFT;
    end = ((unsigned long)pin.offset + bytes) >> ASHMEM_ABI_SHIFT;
    ret = prepare_bitmaps(area);
    if (ret)
        return ret;
    if (cmd == ASHMEM_GET_PIN_STATUS)
        return find_next_bit(area->unpinned, end, begin) < end ?
            ASHMEM_IS_UNPINNED : ASHMEM_IS_PINNED;
    if (cmd == ASHMEM_UNPIN) {
        bitmap_set(area->unpinned, begin, end - begin);
        return 0;
    }
    ret = find_next_bit(area->purged, end, begin) < end ? ASHMEM_WAS_PURGED : ASHMEM_NOT_PURGED;
    bitmap_clear(area->unpinned, begin, end - begin);
    bitmap_clear(area->purged, begin, end - begin);
    return ret;
}

static long purge_all(void)
{
    struct hmos_area *area;
    unsigned long total = 0;
    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;
    mutex_lock(&areas_lock);
    list_for_each_entry(area, &areas, entry) {
        unsigned long first = 0;
        mutex_lock(&area->lock);
        if (!area->backing || !area->unpinned)
            goto next;
        while ((first = find_next_bit(area->unpinned, area->pages, first)) < area->pages) {
            unsigned long end = find_next_zero_bit(area->unpinned, area->pages, first);
            int ret = vfs_fallocate(area->backing, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                   (loff_t)first << ASHMEM_ABI_SHIFT,
                                   (loff_t)(end - first) << ASHMEM_ABI_SHIFT);
            if (!ret) {
                bitmap_set(area->purged, first, end - first);
                total += end - first;
            }
            first = end;
        }
next:
        mutex_unlock(&area->lock);
    }
    mutex_unlock(&areas_lock);
    return total;
}

static long hmos_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct hmos_area *area = file->private_data;
    long ret = 0;
    if (cmd == ASHMEM_PURGE_ALL_CACHES)
        return purge_all();
    mutex_lock(&area->lock);
    switch (cmd) {
    case ASHMEM_SET_NAME:
        if (area->backing) {
            ret = -EINVAL;
            break;
        }
        ret = strncpy_from_user(area->name, (void __user *)arg, sizeof(area->name) - 1);
        area->name[sizeof(area->name) - 1] = '\0';
        if (ret >= 0)
            ret = 0;
        break;
    case ASHMEM_GET_NAME:
        ret = copy_to_user((void __user *)arg, area->name, sizeof(area->name)) ? -EFAULT : 0;
        break;
    case ASHMEM_SET_SIZE:
        if (area->backing || area->unpinned || arg > MAX_LFS_FILESIZE - PAGE_SIZE)
            ret = -EINVAL;
        else
            area->size = arg;
        break;
    case ASHMEM_GET_SIZE:
        ret = area->size;
        break;
    case ASHMEM_SET_PROT_MASK:
        if ((arg & ~ASHMEM_PROT) || (arg & ~area->prot))
            ret = -EINVAL;
        else
            area->prot = arg;
        break;
    case ASHMEM_GET_PROT_MASK:
        ret = area->prot;
        break;
    case ASHMEM_PIN:
    case ASHMEM_UNPIN:
    case ASHMEM_GET_PIN_STATUS:
        ret = pin_range(area, cmd, arg);
        break;
    default:
        ret = -ENOTTY;
    }
    mutex_unlock(&area->lock);
    return ret;
}

static const struct file_operations hmos_fops = {
    .owner = THIS_MODULE,
    .open = hmos_open,
    .release = hmos_release,
    .unlocked_ioctl = hmos_ioctl,
    .mmap_prepare = hmos_mmap_prepare,
    .read_iter = hmos_read_iter,
    .llseek = hmos_llseek,
};

static struct miscdevice hmos_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "ashmem",
    .fops = &hmos_fops,
    .mode = 0666,
};

module_misc_device(hmos_device);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HarmonyOS ashmem ABI using modern shmem and mixed-granule mmap");
