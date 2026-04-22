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
#define LHSR_STATE_REBUILDING 4
#define LHSR_STATE_SUSPENDED 5
#define LHSR_STATE_FAILED    6

/* RAID types */
#define LHSR_RAID_SINGLE   0
#define LHSR_RAID_MIRROR   1
#define LHSR_RAID5         2
#define LHSR_RAID6         3
#define LHSR_RAID_SHR      4
#define LHSR_RAID_SHR2     5

/* IOCTL commands */
#define LHSR_IOCTL_STATUS    0x100
#define LHSR_IOCTL_REBUILD  0x101
#define LHSR_IOCTL_SCRUB    0x102
#define LHSR_IOCTL_REPAIR   0x103
#define LHSR_IOCTL_PREDICT  0x104

/* Checksum algorithms */
#define LHSR_CHECKSUM_NONE    0
#define LHSR_CHECKSUM_CRC32C  1
#define LHSR_CHECKSUM_BLAKE3  2

/* Self-healing modes */
#define LHSR_HEAL_DISABLED   0
#define LHSR_HEAL_PASSIVE    1
#define LHSR_HEAL_SCHEDULED  2
#define LHSR_HEAL_AGGRESSIVE 3

/* Maximum configuration */
#define LHSR_MAX_DISKS      32
#define LHSR_MAX_SEGMENTS   16
#define LHSR_MAX_NAME_LEN   32

/* Forward declarations */
struct lhsr_array;
struct lhsr_disk;
struct lhsr_segment;

/*
 * LHSR Disk Structure
 */
struct lhsr_disk {
	struct block_device *bdev;
	u64 uuid;
	u64 size;           /* in sectors */
	u32 role;           /* data, parity, spare, etc */
	u32 state;          /* healthy, degraded, failed */
	u32 health_score;   /* 0-100 */
	u32 error_count;
	u64 last_error;

	/* SMART data */
	u64 smart_reallocated;
	u64 smart_pending;
	u64 smart_uncorrectable;
	u32 temperature;

	/* Link to array */
	struct lhsr_array *array;
};

/*
 * LHSR Segment Structure
 * Represents a contiguous region with specific RAID config
 */
struct lhsr_segment {
	u64 start_sector;
	u64 length;
	u32 raid_type;
	u32 disk_count;
	u32 data_disks;
	u32 parity_disks;
	u64 disk_bitmap;    /* which disks used */
};

/*
 * LHSR Array Structure
 */
struct lhsr_array {
	/* Identification */
	u64 uuid;
	char name[LHSR_MAX_NAME_LEN];

	/* Configuration */
	u32 raid_type;
	unsigned int disks;
	unsigned int healthy_disks;
	unsigned int total_capacity;  /* in sectors */

	/* State */
	u32 state;
	atomic_t refcount;

	/* Disks */
	struct block_device **disk;
	struct dm_dev **dm_devs;
	struct list_head disks_list;

	/* Segments for SHR layout */
	unsigned int num_segments;
	struct lhsr_segment *segments;

	/* Block size */
	u32 block_size;

	/* Checksumming */
	u32 checksum_algo;
	u32 heal_mode;

	/* Bio pools for each CPU */
	struct bio_set bio_pool[NR_CPUS];

	/* Background tasks */
	struct work_struct scrub_work;
	struct work_struct rebuild_work;
	struct work_struct monitor_work;

	/* Statistics */
	atomic64_t reads;
	atomic64_t writes;
	atomic64_t read_errors;
	atomic64_t write_errors;
	atomic64_t recovered_blocks;
	atomic64_t scrub_blocks;
	atomic64_t bitrot_detected;

	/* Synchronization */
	struct rw_semaphore lock;
	struct list_head list;
	dev_t dev;
};

/* Fast hash for UUID generation */
static inline u64 fast_hash_32(u32 val)
{
	u32 hash = val * 0x9e370001UL;
	return hash >> 16;
}

/* LHSR Array Functions */
struct lhsr_array *lhsr_find_array(dev_t dev);
struct lhsr_array *lhsr_alloc_array(u64 uuid, unsigned int disks);
void lhsr_free_array(struct lhsr_array *arr);
int lhsr_array_add_disk(struct lhsr_array *arr, unsigned int index, struct block_device *bdev);
unsigned int lhsr_get_disk_count(struct lhsr_array *arr);

/* RAID Operations */
int lhsr_calculate_parity(struct lhsr_array *arr, void *data, void **disks, unsigned int count);
int lhsr_reconstruct_block(struct lhsr_array *arr, unsigned int block_idx, void *result);
int lhsr_verify_checksum(struct lhsr_array *arr, void *data, u32 expected, unsigned int algo);

/* Self-Healing */
int lhsr_schedule_scrub(struct lhsr_array *arr);
int lhsr_repair_block(struct lhsr_array *arr, u64 block_id);
int lhsr_detect_bitrot(struct lhsr_array *arr, u64 block_id);

/* Monitoring */
int lhsr_get_disk_health(struct lhsr_array *arr, unsigned int disk_idx);
int lhsr_predict_failure(struct lhsr_array *arr, unsigned int *risk_disks);

#endif /* DM_LHSR_H */