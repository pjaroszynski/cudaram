/*
 * Copyright (C) 2011 Piotr Jaroszyński
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "cudaram.h"

static unsigned int num_devices;
#define MAX_DEVICES 32
#define DEFAULT_NUM_DEVICES 2

static int cudaram_major;
static dev_t cudaram_ctl_number;
static struct class *cudaram_ctl_class;

static LIST_HEAD(cudaram_devices);
static DEFINE_MUTEX(cudaram_devices_mutex); /* protect cudaram_devices */

static const struct file_operations cudaram_ctl_fops;
static const struct block_device_operations cudaram_bops;

/* Add bio to the queue */
static void cudaram_push_bio(struct cudaram_dev *cudaram, struct bio *bio)
{
	bio->bi_next = NULL;

	if (cudaram->bio_last != NULL) {
		cudaram->bio_last->bi_next = bio;
		cudaram->bio_last = bio;
	} else {
		cudaram->bio_first = bio;
		cudaram->bio_last = bio;
	}
}

/* Get the first bio in the queue */
static struct bio *cudaram_pop_bio(struct cudaram_dev *cudaram) {
	struct bio *bio = cudaram->bio_first;

	if (bio != NULL) {
		cudaram->bio_first = bio->bi_next;
		if (cudaram->bio_last == bio)
			cudaram->bio_last = NULL;
	}
	return bio;
}

/* Flush all the pending bios */
static void cudaram_flush_bio(struct bio *bio)
{
	struct bio *next;

	while (bio) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		bio_io_error(bio);
		bio = next;
	}
}

static int cudaram_make_request(struct request_queue *queue, struct bio *bio)
{
	int i;
	int ready;
	struct bio_vec *bvec;
	struct cudaram_dev *cudaram = queue->queuedata;

	pr_debug("%s sec %zd size %u\n", bio_data_dir(bio) == READ ? "read" : "write", bio->bi_sector, bio->bi_size);

	bio_for_each_segment(bvec, bio, i) {
		pr_debug(" bvec len %u off %u\n", bvec->bv_len, bvec->bv_offset);
	}

	/* TODO: should spin_lock_irq be used here? */
	spin_lock(&cudaram->lock);
	ready = cudaram->state == CUDARAM_STATE_READY;
	if (ready)
		cudaram_push_bio(cudaram, bio);
	spin_unlock(&cudaram->lock);

	if (ready)
		wake_up(&cudaram->new_work);
	else
		bio_io_error(bio);

	return 0;
}

static struct cudaram_dev *cudaram_alloc(int id)
{
	int err;
	struct cudaram_dev *cudaram;

	cudaram = kzalloc(sizeof(*cudaram), GFP_KERNEL);
	if (!cudaram)
		return NULL;

	cudaram->id = id;
	cudaram->state = CUDARAM_STATE_FREE;
	spin_lock_init(&cudaram->lock);
	mutex_init(&cudaram->ctl_lock);
	init_waitqueue_head(&cudaram->new_work);

	cudaram->queue = blk_alloc_queue(GFP_KERNEL);
	if (!cudaram->queue) {
		pr_err("Error allocating disk queue for device %d\n", id);
		goto err_free_cudaram;
	}

	blk_queue_make_request(cudaram->queue, &cudaram_make_request);
	cudaram->queue->queuedata = cudaram;

	/* We want requests of k*PAGE_SIZE size */
	blk_queue_logical_block_size(cudaram->queue, PAGE_SIZE);
	blk_queue_physical_block_size(cudaram->queue, PAGE_SIZE);
	blk_queue_io_min(cudaram->queue, PAGE_SIZE);
	blk_queue_io_opt(cudaram->queue, PAGE_SIZE);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, cudaram->queue);

	cdev_init(&cudaram->ctl, &cudaram_ctl_fops);

	cudaram->disk = alloc_disk(1);
	if (!cudaram->disk) {
		pr_err("Error allocating disk structure for device %d\n", id);
		goto err_cleanup_queue;
	}

	cudaram->disk->major = cudaram_major;
	cudaram->disk->first_minor = id;
	cudaram->disk->fops = &cudaram_bops;
	cudaram->disk->queue = cudaram->queue;
	cudaram->disk->private_data = cudaram;

	snprintf(cudaram->disk->disk_name, DISK_NAME_LEN, "cudaram%d", id);

	err = cdev_add(&cudaram->ctl, cudaram_ctl_number + id, 1);
	if (err) {
		pr_err("Error adding the cudaramctl for device %d\n", id);
		goto err_put_disk;
	}
	device_create(cudaram_ctl_class, NULL, MKDEV(MAJOR(cudaram_ctl_number), id), NULL, "cudaramctl%d", id);

	pr_info("cudaram_alloc() end\n");
	return cudaram;

err_put_disk:
	put_disk(cudaram->disk);
err_cleanup_queue:
	blk_cleanup_queue(cudaram->queue);
err_free_cudaram:
	kfree(cudaram);

	return NULL;
}

static void cudaram_free(struct cudaram_dev *cudaram)
{
	device_destroy(cudaram_ctl_class, MKDEV(MAJOR(cudaram_ctl_number), cudaram->id));
	cdev_del(&cudaram->ctl);
	put_disk(cudaram->disk);
	blk_cleanup_queue(cudaram->queue);
	kfree(cudaram);
}

/* Get/allocate the cudaram device with a specific id */
static struct cudaram_dev *cudaram_get(int id) {
	struct cudaram_dev *cudaram;

	mutex_lock(&cudaram_devices_mutex);

	list_for_each_entry(cudaram, &cudaram_devices, list) {
		if (cudaram->id == id) {
			mutex_unlock(&cudaram_devices_mutex);
			return cudaram;
		}
	}

	cudaram = cudaram_alloc(id);
	list_add_tail(&cudaram->list, &cudaram_devices);

	mutex_unlock(&cudaram_devices_mutex);

	return cudaram;
}

/**
 * Take the device.
 *
 * Marks the device as taken if it's free.
 *
 * Must be called with the ctl_lock held.
 */
static int cudaram_take(struct cudaram_dev *cudaram)
{
	unsigned int state;
	spin_lock(&cudaram->lock);
	state = cudaram->state;
	if (state == CUDARAM_STATE_FREE)
		cudaram->state = CUDARAM_STATE_TAKEN;
	spin_unlock(&cudaram->lock);

	return state == CUDARAM_STATE_FREE;
}

/**
 * Activate the device.
 *
 * Activates the device if it's taken.
 *
 * Must be called with the ctl_lock held.
 */
static int cudaram_activate(struct cudaram_dev *cudaram, struct cudaram_params __user *uparams)
{
	unsigned int state;
	struct cudaram_params params;
	struct block_device *bdev;

	bdev = bdget_disk(cudaram->disk, 0);

	spin_lock(&cudaram->lock);
	state = cudaram->state;
	spin_unlock(&cudaram->lock);

	if (state != CUDARAM_STATE_TAKEN)
		return -EBUSY;

	if (copy_from_user(&params, uparams, sizeof(params)))
		return -EFAULT;

	cudaram->user_buffer = (void *)params.buffer;

	blk_queue_max_hw_sectors(cudaram->queue, params.buffer_size << (MB_SHIFT - SECTOR_SHIFT));
	set_capacity(cudaram->disk, params.capacity << (MB_SHIFT - SECTOR_SHIFT));

	spin_lock(&cudaram->lock);
	cudaram->state = CUDARAM_STATE_READY;
	spin_unlock(&cudaram->lock);

	return 0;
}

static void cudaram_deactivate(struct cudaram_dev *cudaram)
{
	struct bio *bio;
	struct block_device *bdev;
	unsigned int state;

	spin_lock(&cudaram->lock);
	state = cudaram->state;
	cudaram->state = CUDARAM_STATE_FREE;
	bio = cudaram->bio_first;
	cudaram->bio_first = NULL;
	cudaram->bio_last = NULL;
	spin_unlock(&cudaram->lock);

	cudaram_flush_bio(bio);

	/* TODO: Could be nice to remove the disk here */
	bdev = bdget_disk(cudaram->disk, 0);
	if (IS_ERR(bdev))
		return;
	invalidate_bdev(bdev);
	set_capacity(cudaram->disk, 0);
}

static int cudaram_ctl_open(struct inode *inode, struct file *filp)
{
	struct cudaram_dev *cudaram;

	cudaram = cudaram_get(iminor(inode));
	if (!cudaram)
		return -ENODEV;

	if (!cudaram_take(cudaram))
		return -EBUSY;

	filp->private_data = cudaram;

	return 0;
}

static int cudaram_ctl_release(struct inode *inode, struct file *filp)
{
	struct cudaram_dev *cudaram = filp->private_data;

	if (!cudaram)
		return -ENODEV;

	cudaram_deactivate(cudaram);

	return 0;
}

/* Process work done - acknowledge the writes, get data for reads */
static int cudaram_process_work(struct cudaram_dev *cudaram, struct cudaram_work *work)
{
	int err, i;
	struct bio *bio = cudaram->current_work;

	if (work->id == 0)
		return 0;
	
	if (!bio)
		return 0;

	if ((__u64)bio != work->id) {
		pr_err("Bad work id %llu != %llu", (__u64)cudaram->current_work, work->id);
		return -1;
	}

	pr_debug("process work %s len %u first_page %u\n",
			work->dir == READ ? "read" : "write", work->len, work->first_page);
	
	/* We only need to copy the data if a read request was completed */
	if (bio_data_dir(bio) == READ) {
		struct bio_vec *bvec;
		bio_for_each_segment(bvec, bio, i) {
			void *kdata = kmap(bvec->bv_page);
			void __user *udata = cudaram->user_buffer + PAGE_SIZE * i;
			err = copy_from_user(kdata + bvec->bv_offset, udata + bvec->bv_offset, bvec->bv_len);
			kunmap(kdata);
			if (err) {
				pr_err("Bad copy_from_user for %d", i);
				return err;
			}
		}
	}

	/* Reset the id so that it won't be resubmitted */
	work->id = 0;

	/* current_work completed */
	cudaram->current_work = NULL;
	bio_endio(bio, 0);

	return 0;
}

static int cudaram_get_work(struct cudaram_dev *cudaram, struct cudaram_work *work)
{
	int i, err;
	struct bio *bio;

	if (cudaram->current_work) {
		/* Already have work to do - shouldn't really happen */
		bio = cudaram->current_work;
	} else {
		/* Get first bio from the pending list */
		spin_lock(&cudaram->lock);
		bio = cudaram_pop_bio(cudaram);
		cudaram->current_work = bio;
		spin_unlock(&cudaram->lock);
	}

	/* Shouldn't really happen either, but check to be safe */
	if (unlikely(!bio)) {
		work->id = 0;
		work->len = 0;
		return 0;
	}

	work->id = (__u64)bio;

	work->dir = bio_data_dir(bio);
	work->len = bio_segments(bio);
	work->first_page = bio->bi_sector >> SECTORS_PER_PAGE_SHIFT;

	if (work->dir == WRITE) {
		struct bio_vec *bvec;
		bio_for_each_segment(bvec, bio, i) {
			void *kdata = kmap(bvec->bv_page);
			void __user *udata = cudaram->user_buffer + PAGE_SIZE * i;
			err = copy_to_user(udata, kdata, PAGE_SIZE);
			kunmap(kdata);
			if (err) {
				pr_err("copy_to_user failed");
				return -EFAULT;
			}
		}
	}

	return 0;
}

/* Process completed work and return new work */
static int cudaram_work(struct cudaram_dev *cudaram, struct cudaram_work __user *user_work)
{
	struct cudaram_work work;
	int err = 0;

	if (copy_from_user(&work, user_work, sizeof(work)))
		return -EFAULT;

	err = cudaram_process_work(cudaram, &work);
	if (err)
		return err;

	/* Reset the id so that it won't be returned to the daemon again if we get interrupted */
	work.id = 0;

	/* TODO: Is the != NULL check safe w/o locking? */
	if (wait_event_interruptible(cudaram->new_work, cudaram->bio_first != NULL)) {
		err = -ERESTARTSYS;
		goto out;
	}

	cudaram_get_work(cudaram, &work);

out:
	if (copy_to_user(user_work, &work, sizeof(work)))
		return -EFAULT;

	return err;
}

static long cudaram_ctl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int err = 0;
	struct cudaram_dev *cudaram = filp->private_data;

	if (!cudaram || !cudaram->disk)
		return -ENODEV;

	mutex_lock(&cudaram->ctl_lock);

	switch (cmd) {
		case CUDARAM_WORK:
			err = cudaram_work(cudaram, (struct cudaram_work __user *)arg);
			break;
		case CUDARAM_ACTIVATE:
			err = cudaram_activate(cudaram, (struct cudaram_params __user *)arg);
			break;
		default:
			err = -EINVAL;
	}

	mutex_unlock(&cudaram->ctl_lock);

	return err;
}

static const struct file_operations cudaram_ctl_fops = {
	.owner = THIS_MODULE,
	.open = &cudaram_ctl_open,
	.release = &cudaram_ctl_release,
	.unlocked_ioctl = &cudaram_ctl_ioctl,
};

static const struct block_device_operations cudaram_bops = {
	.owner = THIS_MODULE
};

static int __init cudaram_init(void)
{
	int err, id;
	struct cudaram_dev *cudaram, *tmp;

	cudaram_ctl_class = class_create(THIS_MODULE, "cudaramctl");
	if (IS_ERR(cudaram_ctl_class)) {
		pr_err("Failed to create cudaram class\n");
		return PTR_ERR(cudaram_ctl_class);
	}

	err = alloc_chrdev_region(&cudaram_ctl_number, 0, 0, "cudaramctl");
	if (err) {
		pr_err("Failed to register the cudaramctl device\n");
		goto err_class_destroy;
	}

	cudaram_major = register_blkdev(0, "cudaram");
	if (cudaram_major <= 0) {
		pr_err("Failed to register the cudaram device\n");
		err = cudaram_major;
		goto err_unregister_chrdev;
	}

	if (num_devices == 0)
		num_devices = DEFAULT_NUM_DEVICES;
	if (num_devices > MAX_DEVICES)
		num_devices = MAX_DEVICES;

	for (id = 0; id < num_devices; ++id) {
		cudaram = cudaram_alloc(id);
		if (!cudaram) {
			err = -ENOMEM;
			goto err_free_devices;
		}
		list_add_tail(&cudaram->list, &cudaram_devices);
	}

	/* TODO
	 * It could be nicer to add the disks only if the userspace daemon is
	 * active, but then removing them when it goes away might be tricky
	 */
	list_for_each_entry(cudaram, &cudaram_devices, list)
		add_disk(cudaram->disk);

	return 0;

err_free_devices:
	list_for_each_entry_safe(cudaram, tmp, &cudaram_devices, list) {
		list_del(&cudaram->list);
		cudaram_free(cudaram);
	}
err_unregister_chrdev:
	unregister_chrdev_region(cudaram_ctl_number, 1);
err_class_destroy:
	class_destroy(cudaram_ctl_class);

	return err;
}

static void __exit cudaram_exit(void)
{
	struct cudaram_dev *cudaram, *tmp;

	list_for_each_entry_safe(cudaram, tmp, &cudaram_devices, list) {
		list_del(&cudaram->list);
		del_gendisk(cudaram->disk);
		cudaram_free(cudaram);
	}

	unregister_blkdev(cudaram_major, "cudaram");
	unregister_chrdev_region(cudaram_ctl_number, 1);
	class_destroy(cudaram_ctl_class);
}

module_init(cudaram_init);
module_exit(cudaram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of cudaram devices");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Piotr Jaroszyński <p.jaroszynski@gmail.com>");
MODULE_DESCRIPTION("CUDA RAM Block Device");

