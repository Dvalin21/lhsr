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

/* Workqueue timeout in jiffies (5 seconds) */
#define LHSR_WORKQUEUE_TIMEOUT (5 * HZ)

/* RAID states */
#define LHSR_STATE_OFFLINE    0
#define LHSR_STATE_ONLINE     1
#define LHSR_STATE_HEALTHY   2
#define LHSR_STATE_DEGRADED  3

/* Superblock magic and offsets */
#define LHSR_SB_MAGIC         "LHSRDISK"
#define LHSR_SB_MAGIC_LEN     8
#define LHSR_SB_VERSION       1
/* Superblock lives in reserved space at END of device, outside user data area */
#define LHSR_SB_SECTORS      16   /* 16 sectors (8KB) reserved at end for superblock metadata */

/* RAID level identifiers */
enum lhsr_raid_type {
	LHSR_RAID0 = 0,
	LHSR_RAID1,
	LHSR_RAID5,
	LHSR_RAID6,
};

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
	__u32    raid_type;	/* enum lhsr_raid_type — on-disk, stays __u32 */
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

/* Scrubber states */
#define LHSR_SCRUB_IDLE       0
#define LHSR_SCRUB_RUNNING    1
#define LHSR_SCRUB_PAUSED     2
#define LHSR_SCRUB_COMPLETED  3

/* Default scrub block size (128KB) */
#define LHSR_SCRUB_BLOCK_SIZE (128 * 1024)

/* Rebuild states */
#define LHSR_REBUILD_NONE     0
#define LHSR_REBUILD_PENDING  1
#define LHSR_REBUILD_RUNNING  2
#define LHSR_REBUILD_COMPLETE 3

/* Write verification */
#define LHSR_WRITE_VERIFY_NONE  0
#define LHSR_WRITE_VERIFY_SIMPLE 1
#define LHSR_WRITE_VERIFY_FULL   2

/* Block verification flags (stored in checksum cache upper 32 bits) */
#define LHSR_BLOCK_VERIFIED   0x01
#define LHSR_BLOCK_CORRUPT    0x02

#endif /* DM_LHSR_H */