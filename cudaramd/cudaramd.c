/*
 * Copyright (C) 2011 Piotr Jaroszyński
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>

#include <linux/fs.h>
#include <linux/types.h>

#include <cuda.h>

#include "../kmod/cudaram.h" /* for ioctl */
#include "print.h"

#define MB_SHIFT 20
#define DEFAULT_BUFFER_SIZE 1

static long PAGE_SIZE;

struct cudaram_dev {
	int fd;
	int id;
	CUdeviceptr data;
	void *buf;
};

int init_cuda(struct cudaram_dev *cudaram)
{
	CUdevice cuDevice;
	int deviceCount;

	cuInit(0);
	cuDeviceGetCount(&deviceCount);
	if (deviceCount == 0) {
		pr_err("There is no device supporting CUDA.\n");
		return -1;
	}

	cuDeviceGet(&cuDevice, 0);
	CUcontext cuContext;

	if (cuCtxCreate(&cuContext, CU_CTX_MAP_HOST, cuDevice) != CUDA_SUCCESS) {
		pr_err("Failed to created the cuda context\n");
		return -1;
	}

	return 0;
}

int init_device(struct cudaram_dev *cudaram, int id, int capacity, int buffer_size)
{
	int err;
	char path[32];
	struct cudaram_params params;

	err = snprintf(path, sizeof(path), "/dev/cudaramctl%d", id);
	if (err < 0) {
		pr_err("vsnprintf failed (%s)\n", strerror(errno));
		return -1;
	}

	cudaram->fd = open(path, O_RDWR);
	if (cudaram->fd < 0) {
		pr_err("Opening the control device '%s' failed (%s)\n", path, strerror(errno));
		return -1;
	}

	if (cuMemAlloc(&cudaram->data, capacity << MB_SHIFT) != CUDA_SUCCESS) {
		pr_err("Allocating cuda data failed\n");
		goto err_close;
	}
	cuMemsetD32(cudaram->data, 0, capacity << (MB_SHIFT - 2));

	if (cuMemAllocHost(&cudaram->buf, buffer_size << MB_SHIFT) != CUDA_SUCCESS) {
		pr_err("Allocating cuda buffer failed\n");
		goto err_free_data;
	}

	params.capacity = capacity;
	params.buffer = (__u64)cudaram->buf;
	params.buffer_size = buffer_size;

	err = mlockall(MCL_FUTURE);
	if (err) {
		pr_err("Locking the memory failed (%s)\n", strerror(errno));
		goto err_free_buf;
	}

	err = ioctl(cudaram->fd, CUDARAM_ACTIVATE, &params);
	if (err) {
		pr_err("Activating the device failed (%s)\n", strerror(errno));
		goto err_free_buf;
	}

	return 0;

err_free_buf:
	cuMemFreeHost(cudaram->buf);
err_free_data:
	cuMemFree(cudaram->data);
err_close:
	close(cudaram->fd);

	return -1;
}

int work(struct cudaram_dev *cudaram)
{
	int err;
	struct cudaram_work work;
	work.id = 0;

	while (1) {
		err = ioctl(cudaram->fd, CUDARAM_WORK, &work);
		if (err) {
			pr_err("ioctl(%d, CUDARAM_WORK, ...) failed (%s)\n", cudaram->fd, strerror(errno));
			return 1;
		}

		if (!work.id)
			continue;

		pr_debug("work %s len %u first_page %u\n", work.dir == READ ? "read" : "write", work.len, work.first_page);

		CUdeviceptr first = cudaram->data + work.first_page * PAGE_SIZE;
		if (work.dir == READ)
			cuMemcpyDtoH(cudaram->buf, first, work.len * PAGE_SIZE);
		else
			cuMemcpyHtoD(first, cudaram->buf, work.len * PAGE_SIZE);
	}
}

int main(int argc, char **argv)
{
	int id, capacity, buffer_size;
	struct cudaram_dev cudaram;

	if (argc != 3 && argc != 4) {
		pr_err("Usage: %s cudaram_id capacityMB [buffer_sizeMB]\n", argv[0]);
		return EXIT_FAILURE;
	}

	PAGE_SIZE = sysconf(_SC_PAGESIZE);
	if (PAGE_SIZE < 0) {
		pr_err("Unable to get PAGE_SIZE\n");
		return EXIT_FAILURE;
	}

	id = atoi(argv[1]);
	if (id < 0) {
		pr_err("Invalid cudaram device id\n");
		return EXIT_FAILURE;
	}

	capacity = atoi(argv[2]);
	if (capacity < 0) {
		pr_err("Invalid capacity\n");
		return EXIT_FAILURE;
	}

	buffer_size = DEFAULT_BUFFER_SIZE;
	if (argc == 4) {
		buffer_size = atoi(argv[3]);
		if (buffer_size < 0) {
			pr_err("Invalid buffer_size\n");
			return EXIT_FAILURE;
		}
	}
	if (init_cuda(&cudaram))
		return EXIT_FAILURE;

	if (init_device(&cudaram, id, capacity, buffer_size))
		return EXIT_FAILURE;

	if (work(&cudaram))
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}
