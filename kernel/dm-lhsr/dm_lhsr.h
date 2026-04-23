/*
 * LHSR Kernel Module Header
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#ifndef DM_LHSR_H
#define DM_LHSR_H

#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/list.h>

/* RAID states */
#define LHSR_STATE_OFFLINE    0
#define LHSR_STATE_ONLINE     1
#define LHSR_STATE_HEALTHY   2
#define LHSR_STATE_DEGRADED  3

/* Superblock magic and offsets */
#define LHSR_SB_MAGIC         "LHSRDISK"
#define LHSR_SB_MAGIC_LEN     8
#define LHSR_SB_VERSION       1
#define LHSR_SB_PRIMARY_OFF   (4 * 1024 * 1024)   /* 4MB from start */
#define LHSR_SB_BACKUP_OFF(disk_size) ((disk_size) - (8 * 1024 * 1024))  /* 8MB from end */

/* Superblock on-disk structure */
struct lhsr_superblock {
	__u8     magic[8];
	__u32    version;
	__u64    array_uuid;
	__u64    disk_uuid;
	__u64    creation_time;
	__u64    last_update;
	__u32    disk_index;
	__u32    disk_state;
	__u32    raid_type;
	__u32    disk_count;
	__u64    total_sectors;
	__u64    generation;
	__u32    checksum;
	__u32    flags;
	__u8     reserved[44];
} __attribute__((packed));

#define LHSR_SB_SIZE          sizeof(struct lhsr_superblock)

/* Disk states */
#define LHSR_DISK_HEALTHY     0
#define LHSR_DISK_DEGRADED    1
#define LHSR_DISK_FAILED      2
#define LHSR_DISK_REBUILDING  3

#endif /* DM_LHSR_H */