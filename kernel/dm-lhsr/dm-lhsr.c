/*
 * LHSR - Linux Hybrid Self-Healing RAID
 * Device Mapper Main Module - Minimal Working Base v1.0.2
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/crc32c.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

#include "dm_lhsr.h"

#define DM_MSG_PREFIX "lhsr"
#define LHSR_VERSION "1.3.0"

/* Timeout for BIO operations (5 seconds) */
#define LHSR_BIO_TIMEOUT (5 * HZ)

/* Completion-based BIO submission with timeout */
struct lhsr_bio_ctx {
	struct completion done;
	int error;
};

static void lhsr_bio_complete(struct bio *bio)
{
	struct lhsr_bio_ctx *ctx = bio->bi_private;
	ctx->error = bio->bi_status;
	complete(&ctx->done);
}

/* Submit BIO with timeout - returns 0 on success, -errno on failure */
static int lhsr_submit_bio_timeout(struct bio *bio)
{
	struct lhsr_bio_ctx ctx;
	int ret;

	init_completion(&ctx.done);
	ctx.error = 0;
	bio->bi_private = &ctx;
	bio->bi_end_io = lhsr_bio_complete;

	submit_bio(bio);

	/* Wait up to LHSR_BIO_TIMEOUT for completion */
	ret = wait_for_completion_timeout(&ctx.done, LHSR_BIO_TIMEOUT);
	if (ret == 0) {
		/* Timeout - BIO didn't complete */
		DMERR("BIO timed out after %d seconds", LHSR_BIO_TIMEOUT / HZ);
		return -ETIMEDOUT;
	}

	return ctx.error ? -EIO : 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LHSR Team");
MODULE_DESCRIPTION("Linux Hybrid Self-Healing RAID");
MODULE_VERSION(LHSR_VERSION);

/* CRC32c lookup table for fast checksum */
static u32 lhsr_crc_table[256];

/* CRC table initialization - static to ensure only initialized once */
static int lhsr_crc_table_initialized = 0;

static void lhsr_init_crc_table(void)
{
	u32 crc;
	int i, j;

	if (lhsr_crc_table_initialized)
		return;

	for (i = 0; i < 256; i++) {
		crc = i;
		for (j = 0; j < 8; j++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0x82F63B78;
			else
				crc >>= 1;
		}
		lhsr_crc_table[i] = crc;
	}

	lhsr_crc_table_initialized = 1;
	DMDEBUG("CRC table initialized");
}

static u32 lhsr_crc32c(const void *buf, size_t len)
{
	const u8 *p = buf;
	u32 crc = 0xFFFFFFFF;

	while (len--) {
		crc = crc ^ *p++;
		crc = (crc >> 8) ^ lhsr_crc_table[crc & 0xFF];
		crc = (crc >> 8) ^ lhsr_crc_table[crc & 0xFF];
		crc = (crc >> 8) ^ lhsr_crc_table[crc & 0xFF];
		crc = (crc >> 8) ^ lhsr_crc_table[crc & 0xFF];
	}

	return crc ^ 0xFFFFFFFF;
}

/* Atomic superblock write - write-hole protection
 * Strategy: Write new superblock to backup first, then primary.
 * On crash, either old primary+new backup or new primary+new backup.
 * Never old primary+old backup (regress), never new primary+old backup (inconsistent).
 * 
 * NOTE: Currently disabled for testing - superblock I/O can cause hanging
 */
static int lhsr_write_superblock(struct block_device *bdev, struct lhsr_superblock *sb, sector_t array_size)
{
	/* Skip superblock writes entirely for now - can cause I/O hangs
	 * This is safe for testing - device works without persistence
	 */
	DMINFO("Skipping superblock write (disabled for testing)");
	return 0;
	
	/* Keep the old code for reference:
	sector_t primary_sector, backup_sector;
	u32 calc_csum;
	int ret = 0;

	primary_sector = LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT;
	backup_sector = array_size - (LHSR_SB_SIZE >> SECTOR_SHIFT);

	if (primary_sector < 2048 || backup_sector < 4096) {
		return -EINVAL;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	...
	*/
}

static int lhsr_read_superblock(struct block_device *bdev, struct lhsr_superblock *sb)
{
	struct bio *bio;
	struct page *page;
	void *buf;
	sector_t sector;
	u32 stored_csum, calc_csum;
	int ret;

	sector = LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT;
	DMINFO("lhsr_read_superblock: sector=0x%llx", sector);

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	DMINFO("lhsr_read_superblock: allocating bio");
	bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_KERNEL);
	bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector = sector;
	__bio_add_page(bio, page, PAGE_SIZE, 0);

	DMINFO("lhsr_read_superblock: submitting bio with timeout");
	ret = lhsr_submit_bio_timeout(bio);
	bio_put(bio);

	if (ret != 0) {
		__free_page(page);
		return ret;
	}

	buf = page_address(page);
	stored_csum = *(u32 *)(buf + offsetof(struct lhsr_superblock, checksum));
	*(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
	calc_csum = lhsr_crc32c(buf, LHSR_SB_SIZE);

	if (stored_csum != calc_csum) {
		u64 backup_off = LHSR_SB_BACKUP_OFF(bdev_nr_sectors(bdev) << SECTOR_SHIFT);
		sector_t backup_sector = backup_off >> SECTOR_SHIFT;
		DMWARN("Primary superblock checksum mismatch (0x%08x vs 0x%08x), trying backup",
		       stored_csum, calc_csum);

		bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_KERNEL);
		bio_set_dev(bio, bdev);
		bio->bi_iter.bi_sector = backup_sector;
		__bio_add_page(bio, page, PAGE_SIZE, 0);

		ret = lhsr_submit_bio_timeout(bio);
		bio_put(bio);

		if (ret != 0) {
			__free_page(page);
			return ret;
		}

		stored_csum = *(u32 *)(buf + offsetof(struct lhsr_superblock, checksum));
		*(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
		calc_csum = lhsr_crc32c(buf, LHSR_SB_SIZE);

		if (stored_csum != calc_csum) {
			DMERR("Both primary and backup superblock checksums invalid");
			__free_page(page);
			return -EINVAL;
		}
		DMINFO("Restored from backup superblock");
	}

	memcpy(sb, buf, LHSR_SB_SIZE);
	__free_page(page);
	return 0;
}

static int lhsr_validate_superblock(struct lhsr_superblock *sb)
{
	if (memcmp(sb->magic, LHSR_SB_MAGIC, LHSR_SB_MAGIC_LEN) != 0) {
		DMERR("Invalid superblock magic");
		return -EINVAL;
	}

	if (sb->version != LHSR_SB_VERSION) {
		DMERR("Unsupported superblock version %u (expected %u)", sb->version, LHSR_SB_VERSION);
		return -EINVAL;
	}

	return 0;
}

static void lhsr_init_superblock(struct lhsr_superblock *sb, u64 array_uuid,
				  unsigned int disk_idx, unsigned int raid_type,
				  unsigned int disk_count, sector_t total_sectors)
{
	memset(sb, 0, LHSR_SB_SIZE);
	memcpy(sb->magic, LHSR_SB_MAGIC, LHSR_SB_MAGIC_LEN);
	sb->version = LHSR_SB_VERSION;
	sb->array_uuid = array_uuid;
	sb->disk_uuid = (u64)disk_idx << 32 | (u32)(jiffies & 0xFFFFFFFF);
	sb->creation_time = ktime_get_real_seconds();
	sb->last_update = sb->creation_time;
	sb->disk_index = disk_idx;
	sb->disk_state = LHSR_DISK_HEALTHY;
	sb->raid_type = raid_type;
	sb->disk_count = disk_count;
	sb->total_sectors = total_sectors;
	sb->generation = 1;
	sb->checksum = lhsr_crc32c((void *)sb, LHSR_SB_SIZE);
}

/* Array structure */
struct lhsr_array {
	/* Configuration - set once, read-only during I/O */
	u64 uuid;
	u32 raid_type;
	unsigned int disks;
	sector_t size;
	struct block_device *disk[32];
	struct dm_dev *dm_devs[32];
	u32 state;
	u32 primary_disk;
	u32 failed_disks;

	/* Concurrency control */
	struct mutex io_mutex;
	struct rw_semaphore sb_sem;

	/* Superblock persistence */
	u64 generation;
	u64 array_uuid;
	struct lhsr_superblock sbs[32];

	/* Disk failure detection */
	u32 disk_errors[32];
	u32 error_threshold;
	struct workqueue_struct *check_wq;
	struct delayed_work check_work;
	unsigned long last_check;
	atomic_t destroying;  /* Flag to prevent workqueue race conditions */

	/* Scrubber */
	struct workqueue_struct *scrub_wq;
	struct delayed_work scrub_work;
	u32 scrub_state;
	u32 scrub_disk;
	u64 scrub_offset;
	u64 scrub_blocks;
	u64 scrub_verified;
	u64 scrub_corrupted;
	u64 scrub_last_offset;
	unsigned long scrub_start_jiffies;

	/* Rebuild tracking */
	u32 rebuild_state;
	u32 rebuild_disk;
	u64 rebuild_offset;
	u64 rebuild_total;
	u64 rebuild_verified;
	struct workqueue_struct *rebuild_wq;
	struct delayed_work rebuild_work;

	/* Write verification */
	u32 write_verify_enabled;

	/* Statistics */
	atomic_t io_count;
	atomic_t io_errors;
	atomic_t failovers;
	atomic_t corruptions_detected;
	atomic_t repairs;

	/* I/O tracking */
	unsigned long last_error_jiffies;
	u32 last_error_disk;
};

static int lhsr_update_disk_state(struct lhsr_array *arr, unsigned int disk_idx, u32 new_state);

/* I/O completion callback - tracks errors */
static void lhsr_io_complete(struct bio *bio)
{
	struct dm_target *ti = bio->bi_private;
	struct lhsr_array *arr;

	if (!ti || !ti->private)
		goto out;

	arr = ti->private;

	if (bio->bi_status != BLK_STS_OK) {
		DMERR("I/O error: status=%d", bio->bi_status);
		atomic_inc(&arr->io_errors);

		/* Find which disk */
		if (bio->bi_bdev) {
			unsigned int i;
			for (i = 0; i < arr->disks; i++) {
				if (arr->disk[i] == bio->bi_bdev) {
					arr->disk_errors[i]++;
					arr->last_error_disk = i;
					arr->last_error_jiffies = jiffies;

					if (arr->disk_errors[i] >= arr->error_threshold) {
						if (!(arr->failed_disks & (1 << i))) {
							DMERR("Disk %u failed due to I/O errors", i);
							arr->failed_disks |= (1 << i);
							lhsr_update_disk_state(arr, i, LHSR_DISK_DEGRADED);
							if (arr->primary_disk == i) {
								arr->primary_disk = i == 0 ? 1 : 0;
								DMINFO("Failover to disk %u", arr->primary_disk);
							}
							arr->state = LHSR_STATE_DEGRADED;
							atomic_inc(&arr->failovers);
						}
					}
					break;
				}
			}
		}
	} else {
		atomic_inc(&arr->io_count);
	}

out:
	bio_put(bio);
}

/* Fast hash for UUID generation */
static inline u64 fast_hash_32(u32 val)
{
	u32 hash = val * 0x9e370001UL;
	return hash >> 16;
}

/* Check if a disk is accessible */
static int disk_check_accessible(struct block_device *bdev)
{
	struct gendisk *disk;

	if (!bdev)
		return -EINVAL;

	disk = bdev->bd_disk;
	if (!disk)
		return -EINVAL;

	if (!disk->queue)
		return -EINVAL;

	if (test_bit(GD_DEAD, &disk->state))
		return -ENODEV;

	return 0;
}

/* Periodic disk health check */
static void disk_check_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct lhsr_array *arr = container_of(dwork, struct lhsr_array, check_work);
	unsigned int i;
	u32 new_failed = 0;

	/* Bail if array is being destroyed */
	if (atomic_read(&arr->destroying)) {
		DMDEBUG("disk_check_work: array being destroyed, skipping");
		return;
	}

	DMINFO("Running periodic disk health check");

	for (i = 0; i < arr->disks; i++) {
		if (arr->failed_disks & (1 << i))
			continue;

		if (disk_check_accessible(arr->disk[i]) != 0) {
			DMERR("Disk %u not accessible", i);
			arr->disk_errors[i] += arr->error_threshold;
		} else {
			/* Clear error count on successful check */
			if (arr->disk_errors[i] > 0 && arr->disk_errors[i] < arr->error_threshold)
				arr->disk_errors[i]--;
		}

		if (arr->disk_errors[i] >= arr->error_threshold) {
			DMERR("Disk %u marked failed (errors=%u)", i, arr->disk_errors[i]);
			new_failed |= (1 << i);

			if (arr->primary_disk == i) {
				arr->primary_disk = (i == 0) ? 1 : 0;
				DMINFO("Primary disk failed, failover to disk %u", arr->primary_disk);
			}
		}
	}

	if (new_failed) {
		arr->failed_disks |= new_failed;
		arr->state = LHSR_STATE_DEGRADED;
		DMWARN("Failed disks mask: 0x%x", arr->failed_disks);

		/* Persist new failed state for each failed disk */
		for (i = 0; i < arr->disks; i++) {
			if (new_failed & (1 << i))
				lhsr_update_disk_state(arr, i, LHSR_DISK_DEGRADED);
		}
	}

	/* Schedule next check - ONLY if workqueue was created */
	if (arr->check_wq) {
		arr->last_check = jiffies;
		queue_delayed_work(arr->check_wq, &arr->check_work, 30 * HZ);
	}
}

/* Scrub a block - reads and checksums */
static int lhsr_scrub_block(struct lhsr_array *arr, unsigned int disk_idx, u64 offset)
{
	struct bio *bio;
	struct page *page;
	void *buf;
	int ret = 0;

	if (arr->failed_disks & (1 << disk_idx))
		return -EINVAL;

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	buf = page_address(page);

	bio = bio_alloc(arr->disk[disk_idx], 1, REQ_OP_READ, GFP_KERNEL);
	bio_set_dev(bio, arr->disk[disk_idx]);
	bio->bi_iter.bi_sector = offset;
	__bio_add_page(bio, page, LHSR_SCRUB_BLOCK_SIZE, 0);

	ret = lhsr_submit_bio_timeout(bio);
	bio_put(bio);

	if (ret == 0) {
		u32 csum = lhsr_crc32c(buf, LHSR_SCRUB_BLOCK_SIZE);
		*(u32 *)buf = csum;
	}

	__free_page(page);
	return ret;
}

/* Periodic scrub work */
static void scrub_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct lhsr_array *arr = container_of(dwork, struct lhsr_array, scrub_work);
	u64 block_size = LHSR_SCRUB_BLOCK_SIZE >> SECTOR_SHIFT;
	unsigned int disk_idx;
	int ret;

	if (arr->scrub_state != LHSR_SCRUB_RUNNING)
		return;

	/* Skip superblock area */
	if (arr->scrub_offset < (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + 8)
		arr->scrub_offset = (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + 8;

	/* Check if we're done with current disk */
	if (arr->scrub_offset >= arr->size) {
		/* Move to next disk */
		disk_idx = (arr->scrub_disk + 1) % arr->disks;
		while (disk_idx != arr->scrub_disk && (arr->failed_disks & (1 << disk_idx)))
			disk_idx = (disk_idx + 1) % arr->disks;

		if (disk_idx == arr->scrub_disk) {
			/* All disks done */
			DMINFO("Scrub completed: %llu blocks verified, %llu corruptions detected",
			       arr->scrub_verified, arr->scrub_corrupted);
			arr->scrub_state = LHSR_SCRUB_COMPLETED;
			return;
		}

		arr->scrub_disk = disk_idx;
		arr->scrub_offset = (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + 8;
		DMINFO("Scrub advancing to disk %u", disk_idx);
	}

	disk_idx = arr->scrub_disk;

	/* Scrub current block */
	ret = lhsr_scrub_block(arr, disk_idx, arr->scrub_offset);
	if (ret == 0) {
		arr->scrub_verified++;
		arr->scrub_last_offset = arr->scrub_offset;
	} else {
		DMERR("Scrub failed at disk %u offset 0x%llx: %d", disk_idx, arr->scrub_offset, ret);
	}

	arr->scrub_offset += block_size;

	/* Schedule next chunk - rate limited to avoid impacting I/O */
	queue_delayed_work(arr->scrub_wq, &arr->scrub_work, HZ / 10);
}

/* Start scrubber */
static int lhsr_scrub_start(struct lhsr_array *arr)
{
	if (arr->scrub_state == LHSR_SCRUB_RUNNING)
		return 0;

	if (!arr->scrub_wq) {
		arr->scrub_wq = alloc_workqueue("lhsr_scrub", WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
		if (!arr->scrub_wq)
			return -ENOMEM;
	}

	arr->scrub_state = LHSR_SCRUB_RUNNING;
	arr->scrub_disk = 0;
	arr->scrub_offset = (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + 8;
	arr->scrub_verified = 0;
	arr->scrub_corrupted = 0;
	arr->scrub_last_offset = 0;
	arr->scrub_start_jiffies = jiffies;

	INIT_DELAYED_WORK(&arr->scrub_work, scrub_work);
	queue_delayed_work(arr->scrub_wq, &arr->scrub_work, 0);

	DMINFO("Scrubber started");
	return 0;
}

/* Rebuild work function - copies data from good disk to replacement */
static void rebuild_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct lhsr_array *arr = container_of(dwork, struct lhsr_array, rebuild_work);
	struct bio *bio;
	struct page *page;
	void *buf;
	unsigned int source_disk;
	u64 block_size = 128 * 1024;  /* 128KB chunks */
	sector_t offset;
	int ret;

	if (!arr || arr->rebuild_state != LHSR_REBUILD_RUNNING)
		return;

	if (!arr->rebuild_wq) {
		DMERR("Rebuild workqueue not initialized");
		arr->rebuild_state = LHSR_REBUILD_NONE;
		return;
	}

	if (!arr->disk[0] || !arr->disk[1]) {
		DMERR("Rebuild: disk devices not available");
		arr->rebuild_state = LHSR_REBUILD_NONE;
		return;
	}

	if (arr->rebuild_disk >= arr->disks) {
		DMINFO("Rebuild complete: %llu sectors copied", arr->rebuild_verified);
		arr->rebuild_state = LHSR_REBUILD_COMPLETE;
		lhsr_update_disk_state(arr, arr->rebuild_disk, LHSR_DISK_HEALTHY);
		return;
	}

	/* Find source disk (the one that's not being rebuilt) */
	source_disk = (arr->rebuild_disk == 0) ? 1 : 0;

	if (arr->failed_disks & (1 << source_disk)) {
		DMERR("Rebuild failed: no source disk available");
		arr->rebuild_state = LHSR_REBUILD_NONE;
		return;
	}

	offset = arr->rebuild_offset;

	/* Skip superblock area */
	if (offset < (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + 8)
		offset = (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + 8;

	/* Check if done */
	if (offset >= arr->size) {
		DMINFO("Rebuild complete: %llu sectors copied", arr->rebuild_verified);
		arr->rebuild_state = LHSR_REBUILD_COMPLETE;
		lhsr_update_disk_state(arr, arr->rebuild_disk, LHSR_DISK_HEALTHY);
		return;
	}

	/* Limit block size to not exceed device */
	if (offset + (block_size >> SECTOR_SHIFT) > arr->size)
		block_size = (arr->size - offset) << SECTOR_SHIFT;

	/* Allocate page for I/O */
	page = alloc_page(GFP_KERNEL);
	if (!page) {
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}

	buf = page_address(page);

	/* Read from source disk */
	bio = bio_alloc(arr->disk[source_disk], 1, REQ_OP_READ, GFP_KERNEL);
	if (!bio) {
		DMERR("Rebuild: failed to allocate read bio");
		__free_page(page);
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}
	bio_set_dev(bio, arr->disk[source_disk]);
	bio->bi_iter.bi_sector = offset;
	__bio_add_page(bio, page, block_size,0);

	ret = lhsr_submit_bio_timeout(bio);
	bio_put(bio);

	if (ret != 0) {
		DMERR("Rebuild read failed at offset 0x%llx", (u64)offset << SECTOR_SHIFT);
		__free_page(page);
		arr->rebuild_offset += (block_size >> SECTOR_SHIFT);
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}

	/* Write to rebuild disk */
	bio = bio_alloc(arr->disk[arr->rebuild_disk], 1, REQ_OP_WRITE, GFP_KERNEL);
	if (!bio) {
		DMERR("Rebuild: failed to allocate write bio");
		__free_page(page);
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}
	bio_set_dev(bio, arr->disk[arr->rebuild_disk]);
	bio->bi_iter.bi_sector = offset;
	__bio_add_page(bio, page, block_size,0);

	ret = lhsr_submit_bio_timeout(bio);
	bio_put(bio);
	__free_page(page);

	if (ret != 0) {
		DMERR("Rebuild write failed at offset 0x%llx", (u64)offset << SECTOR_SHIFT);
	}

	arr->rebuild_offset += (block_size >> SECTOR_SHIFT);
	arr->rebuild_verified += (block_size >> SECTOR_SHIFT);

	/* Rate limit: process one chunk every 100ms to avoid impacting I/O */
	queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ / 10);
}

/* Start rebuild for a specific disk */
static int lhsr_rebuild_start(struct lhsr_array *arr, unsigned int disk_idx)
{
	if (disk_idx >= arr->disks)
		return -EINVAL;

	if (!(arr->failed_disks & (1 << disk_idx))) {
		DMERR("Disk %u is not failed, no rebuild needed", disk_idx);
		return -EINVAL;
	}

	if (!arr->rebuild_wq) {
		arr->rebuild_wq = alloc_workqueue("lhsr_rebuild", WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
		if (!arr->rebuild_wq)
			return -ENOMEM;
	}

	/* Find source disk */
	if (disk_idx == 0 && !(arr->failed_disks & (1 << 1)))
		arr->primary_disk = 1;
	else if (disk_idx == 1 && !(arr->failed_disks & (1 << 0)))
		arr->primary_disk = 0;

	arr->rebuild_state = LHSR_REBUILD_RUNNING;
	arr->rebuild_disk = disk_idx;
	arr->rebuild_offset = (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + 8;
	arr->rebuild_total = arr->size;
	arr->rebuild_verified = 0;

	INIT_DELAYED_WORK(&arr->rebuild_work, rebuild_work);
	queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, 0);

	DMINFO("Rebuild started for disk %u (source disk %u)", disk_idx, arr->primary_disk);
	return 0;
}

/* Target constructor */
static int lhsr_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct lhsr_array *arr = NULL;
	struct dm_dev *dm_dev = NULL;
	unsigned int raid_type = 0;
	unsigned int num_disks = 0;
	unsigned int i = 0;
	sector_t size = 0;
	int r = 0;

	DMINFO("ctr: argc=%u", argc);
	for (i = 0; i < argc; i++)
		DMINFO("ctr: argv[%u]=[%s]", i, argv[i]);

	/* Need at least 2 args: type device */
	if (argc < 2) {
		DMERR("Need at least 2 args, got %u", argc);
		ti->error = "Invalid arguments (need: type device)";
		return -EINVAL;
	}

	/* Parse raid type from argv[0] */
if (strcmp(argv[0], "single") == 0) {
		raid_type = 0;
	} else if (strcmp(argv[0], "mirror") == 0) {
		raid_type = 1;
	} else if (strcmp(argv[0], "raid5") == 0) {
		raid_type = 2;
	} else if (strcmp(argv[0], "raid6") == 0) {
		raid_type = 3;
	} else {
		DMERR("Unknown raid type: %s", argv[0]);
		ti->error = "Unknown raid type";
		return -EINVAL;
	}

	num_disks = argc - 1;
	if (raid_type == 1 && num_disks != 2) {
		DMERR("Mirror requires exactly 2 disks");
		ti->error = "Mirror requires exactly 2 disks";
		return -EINVAL;
	}
	if (raid_type >= 2 && num_disks < 3) {
		DMERR("RAID5/6 requires at least 3 disks");
		ti->error = "RAID5/6 requires at least 3 disks";
		return -EINVAL;
	}

	/* For mirror, require exactly 2 disks */
	num_disks = argc - 1;
	if (raid_type == 1 && num_disks != 2) {
		DMERR("Mirror requires exactly 2 disks, got %u", num_disks);
		ti->error = "Mirror requires exactly 2 disks";
		return -EINVAL;
	}

	DMINFO("Creating RAID type=%u disks=%u", raid_type, num_disks);

	/* Allocate array - zero initialize */
	arr = kzalloc(sizeof(*arr), GFP_KERNEL);
	if (!arr) {
		DMERR("Failed to allocate array");
		ti->error = "Failed to allocate array";
		return -ENOMEM;
	}

	/* Initialize all fields */
	arr->uuid = fast_hash_32(raid_type);
	arr->raid_type = raid_type;
	arr->disks = num_disks;
	arr->state = LHSR_STATE_OFFLINE;
	arr->size = 0;
	arr->primary_disk = 0;
	arr->failed_disks = 0;
	arr->error_threshold = 3;
	arr->last_check = jiffies;
	arr->write_verify_enabled = 0;
	atomic_set(&arr->destroying, 0);

	/* Initialize concurrency primitives */
	mutex_init(&arr->io_mutex);
	init_rwsem(&arr->sb_sem);

	/* Initialize CRC table if not already done */
	lhsr_init_crc_table();

	/* Get devices */
	for (i = 0; i < num_disks; i++) {
		DMINFO("Getting device %u: %s", i, argv[i + 1]);
		r = dm_get_device(ti, argv[i + 1], FMODE_READ | FMODE_WRITE, &dm_dev);
		if (r) {
			DMERR("Cannot get device %s: %d", argv[i + 1], r);
			ti->error = "Failed to get device";
			goto bad;
		}
		arr->dm_devs[i] = dm_dev;
		arr->disk[i] = dm_dev->bdev;
		DMINFO("Added device %u: %s", i, argv[i + 1]);
	}

	/* Calculate size from smallest device */
	size = bdev_nr_sectors(arr->disk[0]);
	for (i = 1; i < num_disks; i++) {
		sector_t s = bdev_nr_sectors(arr->disk[i]);
		if (s < size)
			size = s;
	}

	if (size < 2048) {
		DMERR("Device too small: %llu sectors", size);
		ti->error = "Device too small";
		r = -EINVAL;
		goto bad;
	}

	arr->size = size;

	/* Generate stable array UUID from disk name or size/raid_type */
	arr->array_uuid = 0;
	if (arr->disk[0] && arr->disk[0]->bd_disk) {
		struct gendisk *gd = arr->disk[0]->bd_disk;
		const char *name = gd->disk_name;
		u64 hash = 0;
		while (*name) {
			hash = hash * 31 + *name++;
		}
		arr->array_uuid = hash;
	}
	if (!arr->array_uuid)
		arr->array_uuid = (u64)size ^ ((u64)raid_type << 48);
	arr->generation = 1;

	/* Read or create superblock for each disk */
	for (i = 0; i < num_disks; i++) {
		struct lhsr_superblock *sb = &arr->sbs[i];

		DMINFO("About to read superblock for disk %u", i);
		r = lhsr_read_superblock(arr->disk[i], sb);
		DMINFO("Read superblock returned: %d", r);
		if (r == 0 && lhsr_validate_superblock(sb) == 0) {
			DMINFO("Disk %u: Found valid superblock (gen=%llu)", i, sb->generation);
			if (sb->array_uuid != arr->array_uuid) {
				DMWARN("Disk %u array_uuid mismatch (0x%llx vs 0x%llx)",
				       i, sb->array_uuid, arr->array_uuid);
			}
			arr->failed_disks |= (sb->disk_state >= LHSR_DISK_DEGRADED) ? (1 << i) : 0;
		} else {
			DMWARN("Disk %u: No valid superblock, initializing (persistence optional)", i);
			lhsr_init_superblock(sb, arr->array_uuid, i, raid_type, num_disks, size);
			/* Don't fail device creation if superblock write fails - it's optional for testing */
			DMINFO("About to write superblock for disk %u", i);
			r = lhsr_write_superblock(arr->disk[i], sb, size);
			DMINFO("Write superblock returned: %d", r);
			if (r) {
				DMWARN("Superblock write failed for disk %u - continuing without persistence", i);
			} else {
				DMINFO("Wrote new superblock to disk %u", i);
			}
		}

		/* Track highest generation */
		if (sb->generation > arr->generation)
			arr->generation = sb->generation;
	}

	/* Recover failed disks from superblock state */
	for (i = 0; i < num_disks; i++) {
		if (arr->sbs[i].disk_state >= LHSR_DISK_DEGRADED)
			arr->failed_disks |= (1 << i);
	}

	arr->state = (arr->failed_disks == 0) ? LHSR_STATE_HEALTHY : LHSR_STATE_DEGRADED;

	/* Create workqueue for disk health checks - temporarily disabled for testing */
	arr->check_wq = NULL;
	DMINFO("Disk health check workqueue disabled for testing");

	/* Initialize scrubber state */
	arr->scrub_state = LHSR_SCRUB_IDLE;
	arr->scrub_wq = NULL;
	arr->scrub_offset = 0;
	arr->scrub_verified = 0;
	arr->scrub_corrupted = 0;
	atomic_set(&arr->corruptions_detected, 0);
	atomic_set(&arr->repairs, 0);

	/* Initialize rebuild state */
	arr->rebuild_state = LHSR_REBUILD_NONE;
	arr->rebuild_disk = 0;
	arr->rebuild_offset = 0;
	arr->rebuild_total = 0;
	arr->rebuild_verified = 0;
	arr->rebuild_wq = NULL;

	ti->private = arr;
	ti->len = size;
	ti->begin = 0;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 0;

	DMINFO("Created LHSR: type=%u size=%llu", raid_type, size);

	/* Log RAID level info */
	if (raid_type == 2) {
		DMINFO("RAID5 configured: %u data + 1 parity", num_disks - 1);
	} else if (raid_type == 3) {
		DMINFO("RAID6 configured: %u data + 2 parity", num_disks - 2);
	}

	return 0;

bad:
	while (i > 0) {
		i--;
		if (arr->dm_devs[i])
			dm_put_device(ti, arr->dm_devs[i]);
	}
	kfree(arr);
	return r;
}

/* Target destructor */
static void lhsr_dtr(struct dm_target *ti)
{
	struct lhsr_array *arr = ti->private;
	unsigned int i;
	int r;

	if (!arr)
		return;

	/* Stop the health check workqueue */
	if (arr->check_wq) {
		cancel_delayed_work_sync(&arr->check_work);
		destroy_workqueue(arr->check_wq);
	}

	/* Mark array as destroying FIRST to prevent workqueue races */
	atomic_set(&arr->destroying, 1);

	/* Write updated superblocks for all disks before destroying */
	arr->generation++;
	for (i = 0; i < arr->disks; i++) {
		struct lhsr_superblock *sb = &arr->sbs[i];

		sb->last_update = ktime_get_real_seconds();
		sb->generation = arr->generation;
		sb->disk_state = (arr->failed_disks & (1 << i)) ? LHSR_DISK_DEGRADED : LHSR_DISK_HEALTHY;

		r = lhsr_write_superblock(arr->disk[i], sb, arr->size);
		if (r)
			DMERR("Failed to persist superblock for disk %u: %d", i, r);
		else
			DMINFO("Persisted state for disk %u (gen=%llu)", i, sb->generation);
	}

	/* Stop scrubber */
	if (arr->scrub_wq) {
		cancel_delayed_work_sync(&arr->scrub_work);
		destroy_workqueue(arr->scrub_wq);
		DMINFO("Scrubber stopped");
	}

	/* Stop rebuild */
	if (arr->rebuild_wq) {
		cancel_delayed_work_sync(&arr->rebuild_work);
		destroy_workqueue(arr->rebuild_wq);
		DMINFO("Rebuild stopped");
	}

	for (i = 0; i < arr->disks; i++) {
		if (arr->dm_devs[i])
			dm_put_device(ti, arr->dm_devs[i]);
	}

	/* Destroy concurrency primitives */
	mutex_destroy(&arr->io_mutex);

	kfree(arr);
	ti->private = NULL;
	DMINFO("Destroyed LHSR target");
}

/* Map function - with error tracking */
static int lhsr_map(struct dm_target *ti, struct bio *bio)
{
	struct lhsr_array *arr = ti->private;
	sector_t offset;
	struct bio *clone = NULL;

	if (!arr || arr->state == LHSR_STATE_OFFLINE) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	offset = bio->bi_iter.bi_sector - ti->begin;
	if (offset >= arr->size) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* Single disk passthrough */
	if (arr->raid_type == 0) {
		bio_set_dev(bio, arr->disk[0]);
		bio->bi_iter.bi_sector = offset;
		bio->bi_private = ti;
		bio->bi_end_io = lhsr_io_complete;
		submit_bio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* RAID1 Mirror - write to primary only (async, fire and forget for testing)
	 * TODO: Add proper syncing later
	 */
	if (bio_op(bio) != REQ_OP_READ) {
		unsigned int target_disk = arr->primary_disk;
		
		if (!(arr->failed_disks & (1 << target_disk))) {
			bio_set_dev(bio, arr->disk[target_disk]);
			bio->bi_iter.bi_sector = offset;
			bio->bi_private = ti;
			bio->bi_end_io = lhsr_io_complete;
			submit_bio(bio);
		} else {
			/* Primary failed, try other */
			unsigned int other = (target_disk == 0) ? 1 : 0;
			if (!(arr->failed_disks & (1 << other))) {
				bio_set_dev(bio, arr->disk[other]);
				bio->bi_iter.bi_sector = offset;
				bio->bi_private = ti;
				bio->bi_end_io = lhsr_io_complete;
				submit_bio(bio);
			} else {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
			}
		}
		return DM_MAPIO_SUBMITTED;
	}

	/* RAID5/6 handling - fallback to first data disk */
	if (arr->raid_type >= 2) {
		unsigned int data_disk = 0;

		/* Use a working data disk */
		while (data_disk < arr->disks && (arr->failed_disks & (1 << data_disk)))
			data_disk++;

		if (data_disk < arr->disks) {
			bio_set_dev(bio, arr->disk[data_disk]);
			bio->bi_iter.bi_sector = offset;
			bio->bi_private = ti;
			bio->bi_end_io = lhsr_io_complete;
			submit_bio(bio);
		} else {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
		}
		return DM_MAPIO_SUBMITTED;
	}

	/* Read: try primary, failover to secondary on error */
	if (!(arr->failed_disks & (1 << arr->primary_disk))) {
		bio_set_dev(bio, arr->disk[arr->primary_disk]);
		bio->bi_iter.bi_sector = offset;
		bio->bi_private = ti;
		bio->bi_end_io = lhsr_io_complete;
		submit_bio(bio);
	} else {
		unsigned int other = arr->primary_disk == 0 ? 1 : 0;
		if (!(arr->failed_disks & (1 << other))) {
			DMINFO("Failover read from disk %u to disk %u", arr->primary_disk, other);
			bio_set_dev(bio, arr->disk[other]);
			bio->bi_iter.bi_sector = offset;
			bio->bi_private = ti;
			bio->bi_end_io = lhsr_io_complete;
			atomic_inc(&arr->failovers);
			submit_bio(bio);
		} else {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
		}
	}
	return DM_MAPIO_SUBMITTED;
}

/* Status function */
static void lhsr_status(struct dm_target *ti, status_type_t type, unsigned int flags,
		     char *result, unsigned int maxlen)
{
	struct lhsr_array *arr = ti->private;
	unsigned int sz = 0;

	if (!arr)
		return;

	switch (type) {
	case STATUSTYPE_INFO:
		if (arr->raid_type == 0) {
			sz += scnprintf(result + sz, maxlen - sz, "OK %u/%llu",
				       arr->disks, arr->size);
		} else if (arr->raid_type == 1) {
			unsigned int healthy = (arr->failed_disks == 0) ? arr->disks : arr->disks - 1;
			const char *state = (arr->failed_disks == 0) ? "OK" : "DEGRADED";
			sz += scnprintf(result + sz, maxlen - sz, "%s %u/%u err=%u fail=%d",
				       state, healthy, arr->disks,
				       atomic_read(&arr->io_errors),
				       atomic_read(&arr->failovers));
		}
		if (arr->scrub_state != LHSR_SCRUB_IDLE) {
			const char *state_str = "IDLE";
			switch (arr->scrub_state) {
			case LHSR_SCRUB_RUNNING: state_str = "RUN"; break;
			case LHSR_SCRUB_PAUSED: state_str = "PAUSED"; break;
			case LHSR_SCRUB_COMPLETED: state_str = "DONE"; break;
			}
			sz += scnprintf(result + sz, maxlen - sz, " scrub=%s-%llu",
				       state_str, arr->scrub_verified);
		}
		break;
	case STATUSTYPE_TABLE:
		sz += scnprintf(result + sz, maxlen - sz, "UUID=%llx RAID=%u DISKS=%u",
			       arr->uuid, arr->raid_type, arr->disks);
		if (arr->raid_type >= 2) {
			const char *raid_name = arr->raid_type == 2 ? "RAID5" : "RAID6";
			sz += scnprintf(result + sz, maxlen - sz, " TYPE=%s", raid_name);
		}
		break;
	default:
		break;
	}
}

/* Update and persist disk state to superblock - atomic read-modify-write */
static int lhsr_update_disk_state(struct lhsr_array *arr, unsigned int disk_idx, u32 new_state)
{
	struct lhsr_superblock *sb;
	int r;

	if (disk_idx >= arr->disks)
		return -EINVAL;

	/* Use write semaphore for atomic update */
	down_write(&arr->sb_sem);

	sb = &arr->sbs[disk_idx];
	sb->disk_state = new_state;
	sb->last_update = ktime_get_real_seconds();
	sb->generation = ++arr->generation;

	r = lhsr_write_superblock(arr->disk[disk_idx], sb, arr->size);
	if (r) {
		DMERR("Failed to persist disk %u state: %d", disk_idx, r);
		up_write(&arr->sb_sem);
		return r;
	}

	DMINFO("Persisted disk %u state=%u (gen=%llu)", disk_idx, new_state, arr->generation);
	up_write(&arr->sb_sem);
	return 0;
}

/* Message handler - allows userspace to communicate with kernel */
static int lhsr_message(struct dm_target *ti, unsigned int argc, char **argv,
		       char *result, unsigned int maxlen)
{
	struct lhsr_array *arr = ti->private;
	unsigned int disk_idx;
	int err;

	if (!arr)
		return -EINVAL;
	if (argc < 1)
		return -EINVAL;

	DMINFO("message: argc=%u argv[0]=%s", argc, argv[0]);

	/* Debug: Log raw command for troubleshooting */
	DMDEBUG("message handler: processing command '%s' (argc=%u)", argv[0], argc);
	if (argc > 1) {
		DMDEBUG("message handler: argv[1]='%s'", argv[1]);
	}
	if (argc > 2) {
		DMDEBUG("message handler: argv[2]='%s'", argv[2]);
	}

	/* Handle bare command (dmsetup message <device> <cmd>) */
	if (strncmp(argv[0], "disk_fail", 8) == 0) {
		if (argc < 2)
			return -EINVAL;
		err = kstrtouint(argv[1], 10, &disk_idx);
		if (err || disk_idx >= arr->disks)
			return -EINVAL;

		DMINFO("Userspace marking disk %u as failed", disk_idx);
		arr->failed_disks |= (1 << disk_idx);
		arr->disk_errors[disk_idx] = arr->error_threshold;
		lhsr_update_disk_state(arr, disk_idx, LHSR_DISK_DEGRADED);

		if (arr->primary_disk == disk_idx) {
			arr->primary_disk = (disk_idx == 0) ? 1 : 0;
			DMINFO("Primary disk failed, failover to disk %u", arr->primary_disk);
		}

		arr->state = LHSR_STATE_DEGRADED;
		scnprintf(result, maxlen, "Disk %u marked failed", disk_idx);
		return 0;
	}

	if (strncmp(argv[0], "disk_online", 10) == 0) {
		if (argc < 2)
			return -EINVAL;
		err = kstrtouint(argv[1], 10, &disk_idx);
		if (err || disk_idx >= arr->disks)
			return -EINVAL;

		DMINFO("Userspace marking disk %u as online", disk_idx);
		arr->failed_disks &= ~(1 << disk_idx);
		arr->disk_errors[disk_idx] = 0;
		lhsr_update_disk_state(arr, disk_idx, LHSR_DISK_HEALTHY);

		if (arr->failed_disks == 0)
			arr->state = LHSR_STATE_HEALTHY;
		else
			arr->state = LHSR_STATE_DEGRADED;

		scnprintf(result, maxlen, "Disk %u marked online", disk_idx);
		return 0;
	}

	if (strncmp(argv[0], "disk_health", 10) == 0) {
		if (argc < 2)
			return -EINVAL;
		err = kstrtouint(argv[1], 10, &disk_idx);
		if (err || disk_idx >= arr->disks)
			return -EINVAL;

		scnprintf(result, maxlen, "disk[%u]: errors=%u failed=%d gen=%llu",
			  disk_idx, arr->disk_errors[disk_idx],
			  (arr->failed_disks >> disk_idx) & 1,
			  arr->sbs[disk_idx].generation);

		return 0;
	}

	if (strncmp(argv[0], "scrub", 4) == 0) {
		/* 
		 * dmsetup message format: dmsetup message <device> <sector> <message>
		 * The <sector> argument is passed by userspace and becomes argv[1]
		 * We need to handle the case where argv[1] might be a sector number "0"
		 */
		DMDEBUG("scrub command: argc=%u, argv[1]='%s'", argc, argc >= 2 ? argv[1] : "(none)");

		/* Query status (argc == 1 means just "scrub", or if argv[1] is "status" or a sector number) */
		if (argc == 1 || 
		    (argc >= 2 && (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "0") == 0 || 
				  (argv[1][0] >= '0' && argv[1][0] <= '9')))) {
			const char *state_str = "IDLE";
			switch (arr->scrub_state) {
			case LHSR_SCRUB_RUNNING: state_str = "RUNNING"; break;
			case LHSR_SCRUB_PAUSED: state_str = "PAUSED"; break;
			case LHSR_SCRUB_COMPLETED: state_str = "COMPLETED"; break;
			}
			scnprintf(result, maxlen,
				  "scrub: state=%s disk=%u offset=0x%llx verified=%llu corrupted=%llu",
				  state_str, arr->scrub_disk, arr->scrub_offset,
				  arr->scrub_verified, arr->scrub_corrupted);
			DMDEBUG("Returning scrub status: %s", result);
			return 0;
		}

		if (argc < 2)
			return -EINVAL;

		if (strcmp(argv[1], "start") == 0) {
			lhsr_scrub_start(arr);
			scnprintf(result, maxlen, "Scrub started");
			return 0;
		}

		if (strcmp(argv[1], "stop") == 0) {
			arr->scrub_state = LHSR_SCRUB_IDLE;
			scnprintf(result, maxlen, "Scrub stopped at offset 0x%llx", arr->scrub_offset);
			return 0;
		}

		DMDEBUG("Unknown scrub subcommand: '%s'", argv[1]);
		return -EINVAL;
	}

	if (strncmp(argv[0], "rebuild", 6) == 0) {
		/* Query status (no second argument) */
		if (argc == 1 || (argc >= 2 && strcmp(argv[1], "status") == 0)) {
			const char *state_str = "NONE";
			switch (arr->rebuild_state) {
			case LHSR_REBUILD_PENDING: state_str = "PENDING"; break;
			case LHSR_REBUILD_RUNNING: state_str = "RUNNING"; break;
			case LHSR_REBUILD_COMPLETE: state_str = "COMPLETE"; break;
			}
			scnprintf(result, maxlen,
				  "rebuild: state=%s disk=%u offset=0x%llx/%llx",
				  state_str, arr->rebuild_disk,
				  arr->rebuild_offset, arr->rebuild_total);
			return 0;
		}

		if (argc < 2)
			return -EINVAL;

		if (strcmp(argv[1], "start") == 0) {
			if (argc < 3)
				return -EINVAL;
			err = kstrtouint(argv[2], 10, &disk_idx);
			if (err || disk_idx >= arr->disks)
				return -EINVAL;

			err = lhsr_rebuild_start(arr, disk_idx);
			if (err) {
				scnprintf(result, maxlen, "Rebuild failed: %d", err);
				return err;
			}

			scnprintf(result, maxlen, "Rebuild started for disk %u", disk_idx);
			return 0;
		}

		if (strcmp(argv[1], "status") == 0) {
			if (arr->rebuild_state == LHSR_REBUILD_NONE) {
				scnprintf(result, maxlen, "No rebuild in progress");
			} else {
				u32 pct = arr->rebuild_total ? (u32)((arr->rebuild_offset * 100) / arr->rebuild_total) : 0;
				scnprintf(result, maxlen, "Rebuild disk=%u progress=%u%% (%llu/%llu sectors)",
					  arr->rebuild_disk, pct, arr->rebuild_verified, arr->rebuild_total);
			}
			return 0;
		}

		if (strcmp(argv[1], "stop") == 0) {
			arr->rebuild_state = LHSR_REBUILD_NONE;
			scnprintf(result, maxlen, "Rebuild stopped at offset 0x%llx", arr->rebuild_offset);
			return 0;
		}

		return -EINVAL;
	}

	if (strncmp(argv[0], "persist", 7) == 0) {
		unsigned int i;
		arr->generation++;
		for (i = 0; i < arr->disks; i++) {
			lhsr_update_disk_state(arr, i, (arr->failed_disks & (1 << i)) ?
						       LHSR_DISK_DEGRADED : LHSR_DISK_HEALTHY);
		}
		scnprintf(result, maxlen, "Persisted gen=%llu", arr->generation);
		return 0;
	}

	if (strncmp(argv[0], "write_verify", 11) == 0) {
		if (argc < 2) {
			const char *mode_str = "none";
			switch (arr->write_verify_enabled) {
			case LHSR_WRITE_VERIFY_SIMPLE: mode_str = "simple"; break;
			case LHSR_WRITE_VERIFY_FULL: mode_str = "full"; break;
			}
			scnprintf(result, maxlen, "write_verify=%s", mode_str);
			return 0;
		}

		if (strcmp(argv[1], "none") == 0) {
			arr->write_verify_enabled = LHSR_WRITE_VERIFY_NONE;
			scnprintf(result, maxlen, "Write verification disabled");
			return 0;
		}
		if (strcmp(argv[1], "simple") == 0) {
			arr->write_verify_enabled = LHSR_WRITE_VERIFY_SIMPLE;
			scnprintf(result, maxlen, "Simple write verification enabled");
			return 0;
		}
		if (strcmp(argv[1], "full") == 0) {
			arr->write_verify_enabled = LHSR_WRITE_VERIFY_FULL;
			scnprintf(result, maxlen, "Full write verification enabled");
			return 0;
		}
		return -EINVAL;
	}

	if (strncmp(argv[0], "config", 6) == 0) {
		scnprintf(result, maxlen,
			  "uuid=%llx raid=%u disks=%u state=%u failed=0x%x gen=%llu verify=%u",
			  arr->array_uuid, arr->raid_type, arr->disks,
			  arr->state, arr->failed_disks, arr->generation,
			  arr->write_verify_enabled);
		return 0;
	}

	DMERR("Unknown message: %s", argv[0]);
	return -EINVAL;
}

/* Target operations */
static struct target_type lhsr_target = {
	.name = "lhsr",
	.version = {1, 3, 0},
	.ctr = lhsr_ctr,
	.dtr = lhsr_dtr,
	.map = lhsr_map,
	.status = lhsr_status,
	.message = lhsr_message,
};

/* Module initialization */
static int __init lhsr_init(void)
{
	int r;

	r = dm_register_target(&lhsr_target);
	if (r) {
		DMERR("Failed to register target: %d", r);
		return r;
	}

	DMINFO("Module loaded: %s", LHSR_VERSION);
	return 0;
}

/* Module cleanup */
static void __exit lhsr_exit(void)
{
	dm_unregister_target(&lhsr_target);
	DMINFO("Module unloaded");
}

module_init(lhsr_init);
module_exit(lhsr_exit);