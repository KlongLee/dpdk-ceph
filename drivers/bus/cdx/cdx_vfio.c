/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2022-2023, Advanced Micro Devices, Inc.
 */

/**
 * @file
 * CDX probing using Linux VFIO.
 *
 * This code tries to determine if the CDX device is bound to VFIO driver,
 * and initialize it (map MMIO regions, set up interrupts) if that's the case.
 *
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <rte_eal_paging.h>
#include <rte_malloc.h>
#include <rte_vfio.h>

#include "bus_cdx_driver.h"
#include "cdx_logs.h"
#include "private.h"

/**
 * A structure describing a CDX mapping.
 */
struct cdx_map {
	void *addr;
	char *path;
	uint64_t offset;
	uint64_t size;
};

/**
 * A structure describing a mapped CDX resource.
 * For multi-process we need to reproduce all CDX mappings in secondary
 * processes, so save them in a tailq.
 */
struct mapped_cdx_resource {
	TAILQ_ENTRY(mapped_cdx_resource) next;
	char name[RTE_DEV_NAME_MAX_LEN];      /**< CDX device name */
	char path[PATH_MAX];
	int nb_maps;
	struct cdx_map maps[RTE_CDX_MAX_RESOURCE];
};

/** mapped cdx device list */
TAILQ_HEAD(mapped_cdx_res_list, mapped_cdx_resource);

static struct rte_tailq_elem cdx_vfio_tailq = {
	.name = "VFIO_CDX_RESOURCE_LIST",
};
EAL_REGISTER_TAILQ(cdx_vfio_tailq)

static struct mapped_cdx_resource *
cdx_vfio_find_and_unmap_resource(struct mapped_cdx_res_list *vfio_res_list,
		struct rte_cdx_device *dev)
{
	struct mapped_cdx_resource *vfio_res = NULL;
	const char *dev_name = dev->device.name;
	struct cdx_map *maps;
	int i;

	/* Get vfio_res */
	TAILQ_FOREACH(vfio_res, vfio_res_list, next) {
		if (strcmp(vfio_res->name, dev_name))
			continue;
		break;
	}

	if  (vfio_res == NULL)
		return vfio_res;

	CDX_BUS_INFO("Releasing CDX mapped resource for %s", dev_name);

	maps = vfio_res->maps;
	for (i = 0; i < vfio_res->nb_maps; i++) {
		if (maps[i].addr) {
			CDX_BUS_DEBUG("Calling cdx_unmap_resource for %s at %p",
				dev_name, maps[i].addr);
			cdx_unmap_resource(maps[i].addr, maps[i].size);
		}
	}

	return vfio_res;
}

static int
cdx_vfio_unmap_resource_primary(struct rte_cdx_device *dev)
{
	char cdx_addr[PATH_MAX] = {0};
	struct mapped_cdx_resource *vfio_res = NULL;
	struct mapped_cdx_res_list *vfio_res_list;

	vfio_res_list =
		RTE_TAILQ_CAST(cdx_vfio_tailq.head, mapped_cdx_res_list);
	vfio_res = cdx_vfio_find_and_unmap_resource(vfio_res_list, dev);

	/* if we haven't found our tailq entry, something's wrong */
	if (vfio_res == NULL) {
		CDX_BUS_ERR("%s cannot find TAILQ entry for cdx device!",
			cdx_addr);
		return -1;
	}

	TAILQ_REMOVE(vfio_res_list, vfio_res, next);
	rte_free(vfio_res);
	return 0;
}

static int
cdx_vfio_unmap_resource_secondary(struct rte_cdx_device *dev)
{
	struct mapped_cdx_resource *vfio_res = NULL;
	struct mapped_cdx_res_list *vfio_res_list;

	vfio_res_list =
		RTE_TAILQ_CAST(cdx_vfio_tailq.head, mapped_cdx_res_list);
	vfio_res = cdx_vfio_find_and_unmap_resource(vfio_res_list, dev);

	/* if we haven't found our tailq entry, something's wrong */
	if (vfio_res == NULL) {
		CDX_BUS_ERR("%s cannot find TAILQ entry for CDX device!",
			dev->device.name);
		return -1;
	}

	return 0;
}

int
cdx_vfio_unmap_resource(struct rte_cdx_device *dev)
{
	if (rte_eal_process_type() == RTE_PROC_PRIMARY)
		return cdx_vfio_unmap_resource_primary(dev);
	else
		return cdx_vfio_unmap_resource_secondary(dev);
}

static int
cdx_vfio_setup_device(int vfio_dev_fd)
{
	/*
	 * Reset the device. If the device is not capable of resetting,
	 * then it updates errno as EINVAL.
	 */
	if (ioctl(vfio_dev_fd, VFIO_DEVICE_RESET) && errno != EINVAL) {
		CDX_BUS_ERR("Unable to reset device! Error: %d (%s)", errno,
			strerror(errno));
		return -1;
	}

	return 0;
}

static int
cdx_vfio_mmap_resource(int vfio_dev_fd, struct mapped_cdx_resource *vfio_res,
		int index, int additional_flags)
{
	struct cdx_map *map = &vfio_res->maps[index];
	void *vaddr;

	if (map->size == 0) {
		CDX_BUS_DEBUG("map size is 0, skip region %d", index);
		return 0;
	}

	/* reserve the address using an inaccessible mapping */
	vaddr = mmap(map->addr, map->size, 0, MAP_PRIVATE |
		     MAP_ANONYMOUS | additional_flags, -1, 0);
	if (vaddr != MAP_FAILED) {
		void *map_addr = NULL;

		if (map->size) {
			/* actual map of first part */
			map_addr = cdx_map_resource(vaddr, vfio_dev_fd,
						    map->offset, map->size,
						    RTE_MAP_FORCE_ADDRESS);
		}

		if (map_addr == NULL) {
			munmap(vaddr, map->size);
			vaddr = MAP_FAILED;
			CDX_BUS_ERR("Failed to map cdx MMIO region %d", index);
			return -1;
		}
	} else {
		CDX_BUS_ERR("Failed to create inaccessible mapping for MMIO region %d",
			index);
		return -1;
	}

	map->addr = vaddr;
	return 0;
}

/*
 * region info may contain capability headers, so we need to keep reallocating
 * the memory until we match allocated memory size with argsz.
 */
static int
cdx_vfio_get_region_info(int vfio_dev_fd, struct vfio_region_info **info,
		int region)
{
	struct vfio_region_info *ri;
	size_t argsz = sizeof(*ri);
	int ret;

	ri = malloc(sizeof(*ri));
	if (ri == NULL) {
		CDX_BUS_ERR("Cannot allocate memory for VFIO region info");
		return -1;
	}
again:
	memset(ri, 0, argsz);
	ri->argsz = argsz;
	ri->index = region;

	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_GET_REGION_INFO, ri);
	if (ret < 0) {
		free(ri);
		return ret;
	}
	if (ri->argsz != argsz) {
		struct vfio_region_info *tmp;

		argsz = ri->argsz;
		tmp = realloc(ri, argsz);

		if (tmp == NULL) {
			/* realloc failed but the ri is still there */
			free(ri);
			CDX_BUS_ERR("Cannot reallocate memory for VFIO region info");
			return -1;
		}
		ri = tmp;
		goto again;
	}
	*info = ri;

	return 0;
}

static int
find_max_end_va(const struct rte_memseg_list *msl, void *arg)
{
	size_t sz = msl->len;
	void *end_va = RTE_PTR_ADD(msl->base_va, sz);
	void **max_va = arg;

	if (*max_va < end_va)
		*max_va = end_va;
	return 0;
}

static void *
cdx_find_max_end_va(void)
{
	void *va = NULL;

	rte_memseg_list_walk(find_max_end_va, &va);
	return va;
}

static int
cdx_vfio_map_resource_primary(struct rte_cdx_device *dev)
{
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	char cdx_addr[PATH_MAX] = {0};
	static void *cdx_map_addr;
	struct mapped_cdx_resource *vfio_res = NULL;
	struct mapped_cdx_res_list *vfio_res_list =
		RTE_TAILQ_CAST(cdx_vfio_tailq.head, mapped_cdx_res_list);
	const char *dev_name = dev->device.name;
	struct cdx_map *maps;
	int vfio_dev_fd, i, ret;

	ret = rte_vfio_setup_device(RTE_CDX_BUS_DEVICES_PATH, dev_name,
				    &vfio_dev_fd, &device_info);
	if (ret)
		return ret;

	/* allocate vfio_res and get region info */
	vfio_res = rte_zmalloc("VFIO_RES", sizeof(*vfio_res), 0);
	if (vfio_res == NULL) {
		CDX_BUS_ERR("Cannot store VFIO mmap details");
		goto err_vfio_dev_fd;
	}
	memcpy(vfio_res->name, dev_name, RTE_DEV_NAME_MAX_LEN);

	/* get number of registers */
	vfio_res->nb_maps = device_info.num_regions;

	/* map memory regions */
	maps = vfio_res->maps;

	for (i = 0; i < vfio_res->nb_maps; i++) {
		struct vfio_region_info *reg = NULL;
		void *vaddr;

		ret = cdx_vfio_get_region_info(vfio_dev_fd, &reg, i);
		if (ret < 0) {
			CDX_BUS_ERR("%s cannot get device region info error %i (%s)",
				dev_name, errno, strerror(errno));
			goto err_vfio_res;
		}

		/* skip non-mmappable regions */
		if ((reg->flags & VFIO_REGION_INFO_FLAG_MMAP) == 0) {
			free(reg);
			continue;
		}

		/* try mapping somewhere close to the end of hugepages */
		if (cdx_map_addr == NULL)
			cdx_map_addr = cdx_find_max_end_va();

		vaddr = cdx_map_addr;
		cdx_map_addr = RTE_PTR_ADD(vaddr, (size_t)reg->size);

		cdx_map_addr = RTE_PTR_ALIGN(cdx_map_addr,
					     sysconf(_SC_PAGE_SIZE));

		maps[i].addr = vaddr;
		maps[i].offset = reg->offset;
		maps[i].size = reg->size;
		maps[i].path = NULL; /* vfio doesn't have per-resource paths */

		ret = cdx_vfio_mmap_resource(vfio_dev_fd, vfio_res, i, 0);
		if (ret < 0) {
			CDX_BUS_ERR("%s mapping region %i failed: %s",
				cdx_addr, i, strerror(errno));
			free(reg);
			goto err_vfio_res;
		}

		dev->mem_resource[i].addr = maps[i].addr;
		dev->mem_resource[i].len = maps[i].size;

		free(reg);
	}

	if (cdx_vfio_setup_device(vfio_dev_fd) < 0) {
		CDX_BUS_ERR("%s setup device failed", dev_name);
		goto err_vfio_res;
	}

	TAILQ_INSERT_TAIL(vfio_res_list, vfio_res, next);

	return 0;
err_vfio_res:
	cdx_vfio_find_and_unmap_resource(vfio_res_list, dev);
	rte_free(vfio_res);
err_vfio_dev_fd:
	rte_vfio_release_device(RTE_CDX_BUS_DEVICES_PATH, dev_name, vfio_dev_fd);
	return -1;
}

static int
cdx_vfio_map_resource_secondary(struct rte_cdx_device *dev)
{
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	char cdx_addr[PATH_MAX] = {0};
	int vfio_dev_fd;
	int i, ret;
	struct mapped_cdx_resource *vfio_res = NULL;
	struct mapped_cdx_res_list *vfio_res_list =
		RTE_TAILQ_CAST(cdx_vfio_tailq.head, mapped_cdx_res_list);
	const char *dev_name = dev->device.name;
	struct cdx_map *maps;

	/* if we're in a secondary process, just find our tailq entry */
	TAILQ_FOREACH(vfio_res, vfio_res_list, next) {
		if (strcmp(vfio_res->name, dev_name))
			continue;
		break;
	}
	/* if we haven't found our tailq entry, something's wrong */
	if (vfio_res == NULL) {
		CDX_BUS_ERR("%s cannot find TAILQ entry for cdx device!",
			dev_name);
		return -1;
	}

	ret = rte_vfio_setup_device(RTE_CDX_BUS_DEVICES_PATH, dev_name,
					&vfio_dev_fd, &device_info);
	if (ret)
		return ret;

	/* map MMIO regions */
	maps = vfio_res->maps;

	for (i = 0; i < vfio_res->nb_maps; i++) {
		ret = cdx_vfio_mmap_resource(vfio_dev_fd, vfio_res, i, MAP_FIXED);
		if (ret < 0) {
			CDX_BUS_ERR("%s mapping MMIO region %i failed: %s",
				dev_name, i, strerror(errno));
			goto err_vfio_dev_fd;
		}

		dev->mem_resource[i].addr = maps[i].addr;
		dev->mem_resource[i].len = maps[i].size;
	}

	return 0;
err_vfio_dev_fd:
	rte_vfio_release_device(RTE_CDX_BUS_DEVICES_PATH, cdx_addr, vfio_dev_fd);
	return -1;
}

/*
 * map the CDX resources of a CDX device in virtual memory (VFIO version).
 * primary and secondary processes follow almost exactly the same path
 */
int
cdx_vfio_map_resource(struct rte_cdx_device *dev)
{
	if (rte_eal_process_type() == RTE_PROC_PRIMARY)
		return cdx_vfio_map_resource_primary(dev);
	else
		return cdx_vfio_map_resource_secondary(dev);
}
