/*
 * Copyright (C) 2011 Piotr Jaroszyński
 */

#ifndef _CUDARAM_H_
#define _CUDARAM_H_

#include <linux/types.h>

struct cudaram_params {
	__u64 capacity; /* capacity in MB */
	__u64 buffer; /* userspace buffer */
	__u32 buffer_size; /* size of the userspace buffer in MB */
};

struct cudaram_work {
	__u64 id;
	__u32 dir;
	__u32 len;
	__u32 first_page;
};

/* 0xF1 is currently free - see Documentation/ioctl/ioctl-number.txt */
#define CUDARAM_ACTIVATE  _IOW(0xF1, 1, struct cudaram_params)
#define CUDARAM_WORK     _IOWR(0xF1, 2, struct cudaram_work)

#ifdef __KERNEL__

#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/genhd.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#define MB_SHIFT 20

#define SECTOR_SHIFT 9
#define SECTOR_SIZE (1 << SECTOR_SHIFT)
#define SECTORS_PER_PAGE_SHIFT (PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE (1 << SECTORS_PER_PAGE_SHIFT)

#define CUDARAM_STATE_FREE    0 /* free */
#define CUDARAM_STATE_TAKEN   1 /* control device taken */
#define CUDARAM_STATE_READY   2 /* ready to service requests */

struct cudaram_dev {
	unsigned int state; /* one of CUDARAM_STATE_* */

	spinlock_t lock; /* protect from make_request/ioctl races */
	struct mutex ctl_lock; /* protect from multiple ioctls */

	void __user *user_buffer; /* userspace buffer used to transfer data */

	wait_queue_head_t new_work; /* woken up on new work */
	struct bio *bio_first; /* list of pending bios */
	struct bio *bio_last; /* last bio for quick addition */
	struct bio *current_work; /* only a single bio can be the current work */

	int id; /* id corresponds to the minor of the block and control devices */
	struct request_queue *queue;
	struct gendisk *disk;
	struct cdev ctl; /* control device */

	struct list_head list; /* list of all devices */
};

#endif /* __KERNEL__ */

#endif /* _CUDARAM_H_ */
