/*
 * Header file for UBI support for U-Boot
 *
 * Adaptation from kernel to U-Boot
 *
 *  Copyright (C) 2005-2007 Samsung Electronics
 *  Kyungmin Park <kyungmin.park@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __UBOOT_UBI_H
#define __UBOOT_UBI_H

#include <compiler.h>
#include <linux/compat.h>
#include <malloc.h>
#include <div64.h>
#include <linux/math64.h>
#include <linux/crc32.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/string.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/ubi.h>

#ifdef CONFIG_CMD_ONENAND
#include <onenand_uboot.h>
#endif

#include <linux/errno.h>

/* build.c */
#define get_device(...)
#define put_device(...)
#define ubi_sysfs_init(...)		0
#define ubi_sysfs_close(...)		do { } while (0)

#ifndef __UBIFS_H__
#include "../drivers/mtd/ubi/ubi.h"
#endif

/* functions */
int ubi_mtd_param_parse(const char *val, struct kernel_param *kp);
int ubi_init(void);
void ubi_exit(void);

/**
 * ubi_detach() - detach UBI from MTD partition
 *
 * This function performs the cleanup of the UBI subsystem to make sure the
 * MTD partition can be safely used for another purpose, or be attached again
 * with ubi_part().
 *
 * Any mounted UBIFS will be unmounted automatically.
 *
 * Return: 0
 */
int ubi_detach(void);

/**
 * ubi_part() - attach UBI to MTD partition
 * @part_name: name of the MTD partition to attach
 * @vid_header_offset: VID header offset (string)
 *
 * This function detaches any existing UBI device, then probes for the
 * specified MTD partition, and then scans it to initialize UBI.
 *
 * @vid_header_offset is optional and is usually set to NULL.
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_part(const char *part_name, const char *vid_header_offset);

/**
 * ubi_volume_write() - write data to UBI volume
 * @volume: name of the volume to write to
 * @buf: data buffer to be written
 * @offset: start offset for writing
 * @size: number of bytes to write
 *
 * This function writes data to the specific UBI volume. If the offset is zero,
 * it initiates a full volume update. Otherwise, it performs an offset-based
 * write using LEB changes.
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_volume_write(const char *volume, const void *buf, loff_t offset,
		     size_t size);

/**
 * ubi_volume_read() - read data from UBI volume
 * @volume: name of the volume to read from
 * @buf: buffer to hold the read data
 * @offset: start offset for reading
 * @size: number of bytes to read
 *
 * This function reads data from the specified UBI volume. If @size is zero,
 * the function reads the entire volume content starting from @offset.
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_volume_read(const char *volume, void *buf, loff_t offset, size_t size);

/**
 * ubi_create_vol() - create UBI volume
 * @volume: name of the volume to create
 * @size: size of the volume in bytes
 * @dynamic: create dynamic volume if set to true
 * @vol_id: volume ID
 * @skipcheck: skip CRC check on this volume if set to true
 *
 * This function creates a new UBI volume with the specified parameters.
 * If @size is negative, all available space will be used.
 * For volume ID auto assignment, pass %UBI_VOL_NUM_AUTO to @vol_id.
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_create_vol(const char *volume, int64_t size, bool dynamic, int vol_id,
		   bool skipcheck);

/**
 * ubi_find_volume() - find UBI volume by name
 * @volume: name of the volume to find
 *
 * This function searches for a UBI volume with the specified name in the
 * current UBI device.
 *
 * Return: pointer to the UBI volume structure, or %NULL if not found.
 */
struct ubi_volume *ubi_find_volume(const char *volume);

/**
 * ubi_remove_vol() - remove UBI volume
 * @volume: name of the volume to remove
 *
 * This function removes an existing UBI volume from the current UBI device.
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_remove_vol(const char *volume);

/* --- board-port additions (Airoha AN758x web recovery / scripting) --- */

/**
 * ubi_volume_exists() - check whether a volume is present
 * @volume: name of the volume to look up
 *
 * Return: true if the volume exists on the attached UBI device.
 */
bool ubi_volume_exists(const char *volume);

/**
 * ubi_volume_create() - create a volume with automatic ID assignment
 * @volume: name of the volume to create
 * @size: size in bytes, or 0 to use all remaining space
 * @dynamic: create a dynamic volume if set to true
 *
 * Convenience wrapper around ubi_create_vol() using %UBI_VOL_NUM_AUTO and
 * without the CRC skip flag.
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_volume_create(const char *volume, int64_t size, bool dynamic);

/**
 * ubi_volume_remove() - remove a volume by name
 * @volume: name of the volume to remove
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_volume_remove(const char *volume);

/**
 * ubi_volume_rename() - rename a volume
 * @oldname: current volume name
 * @newname: new volume name
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_volume_rename(const char *oldname, const char *newname);

/**
 * ubi_volume_get_size() - report a volume's used and reserved size
 * @volume: name of the volume to query
 * @used_bytes: filled with the used size, or %NULL
 * @reserved_bytes: filled with the reserved size, or %NULL
 *
 * Return: 0 on success, or -ENODEV if UBI is not attached or the volume is
 * not found.
 */
int ubi_volume_get_size(const char *volume, size_t *used_bytes,
			size_t *reserved_bytes);

/**
 * ubi_volume_read_quiet() - read data from UBI volume without chatter
 * @volume: name of the volume to read from
 * @buf: buffer to hold the read data
 * @offset: start offset for reading
 * @size: number of bytes to read
 *
 * Same as ubi_volume_read(), kept as a separate entry point for the console
 * streaming path of the web recovery page, which must not interleave status
 * messages with the transferred payload.
 *
 * Return: 0 on success, or -ve on error.
 */
int ubi_volume_read_quiet(const char *volume, void *buf, loff_t offset,
			  size_t size);

extern struct ubi_device *ubi_devices[];
int cmd_ubifs_mount(const char *vol_name);
int cmd_ubifs_umount(void);

#if IS_ENABLED(CONFIG_UBI_BLOCK)
int ubi_bind(struct udevice *dev);
#else
static inline int ubi_bind(struct udevice *dev)
{
	return -EOPNOTSUPP;
}
#endif

#endif
