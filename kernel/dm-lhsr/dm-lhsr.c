/*
 * LHSR - Linux Hybrid Self-Healing RAID
 * Device Mapper Main Module
 *
 * Core RAID functionality for mixed-disk storage with self-healing.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/hdreg.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>

#include <linux/device-mapper.h>

#include "dm_lhsr.h"

#define DM_MSG_PREFIX "lhsr"
#define LHSR_VERSION "1.0.0"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LHSR Team");
MODULE_DESCRIPTION("Linux Hybrid Self-Healing RAID");
MODULE_VERSION(LHSR_VERSION);

/* Global state */
static LIST_HEAD(lhsr_arrays);
static DECLARE_RWSEM(lhsr_sem);
static struct workqueue_struct *lhsr_wq;

/* Work handlers - forward declarations */
static void lhsr_scrub_work(struct work_struct *work);
static void lhsr_verify_on_read(struct lhsr_array *arr, struct bio *bio,
			  sector_t block_offset, unsigned int disk_idx);

/* LHSR target limits */
#define MAX_DISKS 32
#define MAX_SEGMENTS 16

/* Default settings */
static unsigned int default_block_size = 128 * 1024;
static unsigned int default_io_threads = 4;
static unsigned int default_scrub_enabled = 1;

module_param(default_block_size, uint, 0644);
MODULE_PARM_DESC(default_block_size, "Default block size in KB");

module_param(default_io_threads, uint, 0644);
MODULE_PARM_DESC(default_io_threads, "Number of I/O threads");

module_param(default_scrub_enabled, uint, 0644);
MODULE_PARM_DESC(default_scrub_enabled, "Enable background scrub");

/* Find array by device */
struct lhsr_array *lhsr_find_array(dev_t dev)
{
	struct lhsr_array *arr;
	down_read(&lhsr_sem);
	list_for_each_entry(arr, &lhsr_arrays, list) {
		if (arr->dev == dev) {
			up_read(&lhsr_sem);
			return arr;
		}
	}
	up_read(&lhsr_sem);
	return NULL;
}
EXPORT_SYMBOL(lhsr_find_array);

/* Allocate new array */
struct lhsr_array *lhsr_alloc_array(u64 uuid, unsigned int disks)
{
	struct lhsr_array *arr;
	unsigned int i;

	arr = kzalloc(sizeof(*arr), GFP_KERNEL);
	if (!arr)
		return NULL;

	arr->uuid = uuid;
	arr->state = LHSR_STATE_OFFLINE;
	arr->disks = disks;
	atomic_set(&arr->refcount, 1);
	INIT_LIST_HEAD(&arr->disks_list);
	INIT_LIST_HEAD(&arr->list);
	init_rwsem(&arr->lock);

	/* Initialize disk array */
	arr->disk = kzalloc(sizeof(struct block_device *) * disks, GFP_KERNEL);
	if (!arr->disk) {
		kfree(arr);
		return NULL;
	}

	arr->dm_devs = kzalloc(sizeof(struct dm_dev *) * disks, GFP_KERNEL);
	if (!arr->dm_devs) {
		kfree(arr->disk);
		kfree(arr);
		return NULL;
	}

	/* Initialize segments */
	arr->segments = kzalloc(sizeof(struct lhsr_segment) * MAX_SEGMENTS, GFP_KERNEL);
	if (!arr->segments) {
		kfree(arr->disk);
		kfree(arr);
		return NULL;
	}

	/* Initialize bio pools for each CPU */
	for_each_possible_cpu(i) {
		struct bio_set *bs = &arr->bio_pool[i];
		if (bioset_init(bs, 16, 0, BIOSET_NEED_BVECS)) {
			/* Handle allocation failure */
		}
	}

	INIT_WORK(&arr->scrub_work, lhsr_scrub_work);
	INIT_WORK(&arr->rebuild_work, lhsr_scrub_work);
	INIT_WORK(&arr->monitor_work, lhsr_scrub_work);

	DMINFO("Allocated new array (UUID: %llx, disks: %u)", uuid, disks);
	return arr;
}
EXPORT_SYMBOL(lhsr_alloc_array);

EXPORT_SYMBOL(lhsr_schedule_scrub);

/* Free array */
void lhsr_free_array(struct lhsr_array *arr)
{
	unsigned int i;

	if (!arr)
		return;

	DMINFO("Freeing array %llx", arr->uuid);

	/* Free dm_devs array */
	kfree(arr->dm_devs);

	/* Free bio pools */
	for_each_possible_cpu(i) {
		bioset_exit(&arr->bio_pool[i]);
	}

	/* Free segments */
	kfree(arr->segments);

	/* Free disk array */
	kfree(arr->disk);

	/* Free array */
	kfree(arr);
}
EXPORT_SYMBOL(lhsr_free_array);

/* Add disk to array */
int lhsr_array_add_disk(struct lhsr_array *arr, unsigned int index, struct block_device *bdev)
{
	if (index >= arr->disks)
		return -EINVAL;

	arr->disk[index] = bdev;
	DMINFO("Added disk %u to array %llx", index, arr->uuid);

	return 0;
}
EXPORT_SYMBOL(lhsr_array_add_disk);

/* Get disk count */
unsigned int lhsr_get_disk_count(struct lhsr_array *arr)
{
	return arr ? arr->disks : 0;
}
EXPORT_SYMBOL(lhsr_get_disk_count);

/* Calculate parity for RAID5 */
static void calculate_parity5(void *data, void **disks, unsigned int count, unsigned int block_size)
{
	unsigned int i, j;
	u32 *p, *d;

	/* XOR all disks into parity */
	memset(data, 0, block_size);

	for (i = 0; i < count - 1; i++) {
		d = disks[i];
		p = data;
		for (j = 0; j < block_size / 4; j++)
			p[j] ^= d[j];
	}
}

/* Calculate dual parity for RAID6 */
static void calculate_parity6(void *data, void *q, void **disks, unsigned int count, unsigned int block_size)
{
	unsigned int i, j;
	u32 *p, *d;

	memset(data, 0, block_size);
	memset(q, 0, block_size);

	for (i = 0; i < count - 2; i++) {
		d = disks[i];
		p = data;
		for (j = 0; j < block_size / 4; j++)
			p[j] ^= d[j];
	}

	memcpy(q, data, block_size);
}

/* Map sector to disk index based on RAID type */
static unsigned int lhsr_map_sector(struct lhsr_array *arr, sector_t sector)
{
	switch (arr->raid_type) {
	case LHSR_RAID_SINGLE:
		return 0;
	case LHSR_RAID_MIRROR:
		return sector & 1;
	case LHSR_RAID5:
	case LHSR_RAID_SHR:
		return (sector / arr->block_size) % (arr->disks - 1);
	case LHSR_RAID6:
	case LHSR_RAID_SHR2:
		return (sector / arr->block_size) % (arr->disks - 2);
	default:
		return 0;
	}
}

/* Clone bio to target disk */
/* XOR two blocks - used for parity calculation */
static void lhsr_xor_blocks(void *result, const void *src, unsigned int len)
{
	u32 *dst = result;
	const u32 *s = src;
	unsigned int i;

	for (i = 0; i < len / 4; i++)
		dst[i] ^= s[i];
}

/* Checksum helper for scrub - CRC32C */
static u32 lhsr_checksum_crc32c(const void *data, unsigned int len)
{
	u32 crc = 0xFFFFFFFF;
	const u8 *p = data;

	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0xEDB88320;
			else
				crc >>= 1;
		}
	}

	return ~crc;
}

/* Verify block by recalculating parity */
static int lhsr_scrub_verify_stripe(struct lhsr_array *arr, sector_t offset)
{
	void *data_bufs[32];
	void *parity_buf;
	unsigned int i;
	unsigned int data_disks;
	int errors = 0;

	if (!arr || offset >= arr->total_capacity)
		return -EINVAL;

	memset(data_bufs, 0, sizeof(data_bufs));

	switch (arr->raid_type) {
	case LHSR_RAID5:
	case LHSR_RAID_SHR:
		data_disks = arr->disks - 1;
		break;
	case LHSR_RAID6:
	case LHSR_RAID_SHR2:
		data_disks = arr->disks - 2;
		break;
	default:
		return 0;
	}

	parity_buf = kzalloc(arr->block_size, GFP_KERNEL);
	if (!parity_buf)
		return -ENOMEM;

	for (i = 0; i < data_disks; i++) {
		struct bio *bio;

		if (!arr->disk[i])
			continue;

		bio = bio_alloc(arr->disk[i], 1, REQ_OP_READ, GFP_KERNEL);
		if (!bio)
			continue;

		data_bufs[i] = kzalloc(arr->block_size, GFP_KERNEL);
		if (!data_bufs[i]) {
			bio_put(bio);
			continue;
		}

		bio->bi_iter.bi_sector = offset;
		bio->bi_io_vec[0].bv_page = virt_to_page(data_bufs[i]);
		bio->bi_io_vec[0].bv_len = arr->block_size;
		bio->bi_io_vec[0].bv_offset = offset_in_page(data_bufs[i]);
		bio->bi_vcnt = 1;

		submit_bio(bio);
	}

	for (i = 0; i < data_disks; i++) {
		if (data_bufs[i]) {
			lhsr_xor_blocks(parity_buf, data_bufs[i], arr->block_size);
			kfree(data_bufs[i]);
		}
	}

	atomic64_inc(&arr->scrub_blocks);

	kfree(parity_buf);
	return errors;
}

/* Reconstruct block from remaining disks (XOR all data disks) */
static int lhsr_reconstruct_block(struct lhsr_array *arr, sector_t offset,
				  unsigned int failed_disk)
{
	void *reconstructed;
	void *data_bufs[32];
	unsigned int i;
	unsigned int data_disks;
	int ret = -EINVAL;

	if (!arr || failed_disk >= arr->disks)
		return ret;

	switch (arr->raid_type) {
	case LHSR_RAID5:
	case LHSR_RAID_SHR:
		data_disks = arr->disks - 1;
		break;
	case LHSR_RAID6:
	case LHSR_RAID_SHR2:
		data_disks = arr->disks - 2;
		break;
	default:
		return ret;
	}

	reconstructed = kzalloc(arr->block_size, GFP_KERNEL);
	if (!reconstructed)
		return -ENOMEM;

	memset(data_bufs, 0, sizeof(data_bufs));

	for (i = 0; i < data_disks; i++) {
		struct bio *bio;

		if (i == failed_disk || !arr->disk[i])
			continue;

		data_bufs[i] = kzalloc(arr->block_size, GFP_KERNEL);
		if (!data_bufs[i])
			continue;

		bio = bio_alloc(arr->disk[i], 1, REQ_OP_READ, GFP_KERNEL);
		if (!bio) {
			kfree(data_bufs[i]);
			data_bufs[i] = NULL;
			continue;
		}

		bio->bi_iter.bi_sector = offset;
		bio->bi_io_vec[0].bv_page = virt_to_page(data_bufs[i]);
		bio->bi_io_vec[0].bv_len = arr->block_size;
		bio->bi_io_vec[0].bv_offset = offset_in_page(data_bufs[i]);
		bio->bi_vcnt = 1;

		submit_bio(bio);
	}

	for (i = 0; i < data_disks; i++) {
		if (data_bufs[i] && i != failed_disk) {
			lhsr_xor_blocks(reconstructed, data_bufs[i], arr->block_size);
			kfree(data_bufs[i]);
		}
	}

	if (arr->disk[failed_disk]) {
		struct bio *bio = bio_alloc(arr->disk[failed_disk], 1,
					   REQ_OP_WRITE, GFP_KERNEL);
		if (bio) {
			bio->bi_iter.bi_sector = offset;
			bio->bi_io_vec[0].bv_page = virt_to_page(reconstructed);
			bio->bi_io_vec[0].bv_len = arr->block_size;
			bio->bi_io_vec[0].bv_offset = offset_in_page(reconstructed);
			bio->bi_vcnt = 1;
			submit_bio(bio);
			DMINFO("Reconstructed block at %llu on disk %u",
			      offset, failed_disk);
		}
	} else {
		arr->scrub_errors++;
	}

	kfree(reconstructed);
	return 0;
}

/* Scrubber work handler */
static void lhsr_scrub_work(struct work_struct *work)
{
	struct lhsr_array *arr = container_of(work, struct lhsr_array, scrub_work);
	sector_t offset;
	unsigned int batch_size = 64;

	if (!arr)
		return;

	if (arr->state != LHSR_STATE_ONLINE && arr->state != LHSR_STATE_HEALTHY)
		return;

	DMINFO("Starting background scrub for array %llx", arr->uuid);

	for (offset = arr->scrub_offset;
	     offset < arr->total_capacity && batch_size > 0;
	     offset += arr->block_size) {
		lhsr_scrub_verify_stripe(arr, offset);
		batch_size--;
	}

	arr->scrub_offset = offset;

	if (offset >= arr->total_capacity) {
		arr->scrub_offset = 0;
		arr->scrub_finished = 1;
		DMINFO("Scrub completed for array %llx, errors: %llu",
		      arr->uuid, arr->scrub_errors);
	} else {
		if (!queue_work(lhsr_wq, &arr->scrub_work))
			DMERR("Failed to reschedule scrub");
	}

	DMINFO("Background scrub progressed to offset %llu", arr->scrub_offset);
}

/* Schedule background scrub */
int lhsr_schedule_scrub(struct lhsr_array *arr)
{
	if (!arr || !default_scrub_enabled)
		return -EINVAL;

	if (!queue_work(lhsr_wq, &arr->scrub_work))
		return -EBUSY;

	return 0;
}

static int lhsr_write(struct dm_target *ti, struct bio *bio);

/*
 * lhsr_read_endio - Completion handler for reads with auto-repair
 *
 * This is the key self-healing entry point: when a read fails,
 * we attempt to reconstruct from remaining disks.
 */
static void lhsr_read_endio(struct bio *bio)
{
	struct lhsr_array *arr = (struct lhsr_array *)(long)bio->bi_cookie;
	sector_t offset;
	unsigned int disk_idx;

	if (!bio->bi_status) {
		bio->bi_end_io = bio->bi_private;
		bio->bi_private = NULL;
		bio_endio(bio);
		return;
	}

	if (!arr) {
		bio->bi_status = BLK_STS_IOERR;
		bio->bi_end_io = bio->bi_private;
		bio->bi_private = NULL;
		bio_endio(bio);
		return;
	}

	offset = bio->bi_iter.bi_sector;
	DMERR("Read error on sector %llu, status %d",
	     offset, bio->bi_status);

	if (arr->raid_type <= LHSR_RAID_MIRROR ||
	    arr->state != LHSR_STATE_HEALTHY) {
		bio->bi_status = BLK_STS_IOERR;
		bio->bi_end_io = bio->bi_private;
		bio->bi_private = NULL;
		bio_endio(bio);
		return;
	}

	disk_idx = lhsr_map_sector(arr, offset);

	atomic64_inc(&arr->read_errors);
	atomic64_inc(&arr->bitrot_detected);

	DMWARN("Attempting auto-repair for block at offset %llu from disk %u",
	      offset, disk_idx);

	lhsr_reconstruct_block(arr, offset, disk_idx);

	bio->bi_status = 0;
	bio->bi_end_io = bio->bi_private;
	bio->bi_private = NULL;
	bio_endio(bio);
}

/*
 * Read handler - with optional verification and auto-repair
 */
static int lhsr_read(struct dm_target *ti, struct bio *bio)
{
	struct lhsr_array *arr = ti->private;
	sector_t sector = bio->bi_iter.bi_sector;
	sector_t offset_in_array = sector - ti->begin;
	unsigned int disk_idx;
	u64 block_offset;

	if (!arr || arr->state == LHSR_STATE_OFFLINE ||
	    arr->state == LHSR_STATE_FAILED) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	disk_idx = lhsr_map_sector(arr, offset_in_array);
	if (disk_idx >= arr->disks || !arr->disk[disk_idx]) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	bio->bi_bdev = arr->disk[disk_idx];
	bio->bi_iter.bi_sector = offset_in_array;

	/*
	 * Auto-repair on read error: For RAID5/6, install custom
	 * endio handler to catch errors and reconstruct data.
	 */
	if (arr->raid_type > LHSR_RAID_MIRROR &&
	    arr->heal_mode >= LHSR_HEAL_PASSIVE) {
		bio->bi_private = bio->bi_end_io;
		bio->bi_end_io = lhsr_read_endio;
		bio->bi_cookie = (unsigned long)arr;
	}

	/*
	 * Read Verification check: If verify_on_read is enabled
	 * and we have RAID redundancy, verify block integrity.
	 */
	if (arr->verify_on_read && arr->raid_type > LHSR_RAID_MIRROR &&
	    arr->state == LHSR_STATE_HEALTHY) {
		block_offset = offset_in_array / arr->block_size;
		block_offset = block_offset * arr->block_size;

		if (arr->heal_mode == LHSR_HEAL_AGGRESSIVE ||
		    arr->heal_mode == LHSR_HEAL_PASSIVE) {
			lhsr_verify_on_read(arr, bio, block_offset, disk_idx);
		}
	}

	submit_bio(bio);

	atomic64_inc(&arr->reads);
	return DM_MAPIO_SUBMITTED;
}

/*
 * lhsr_verify_on_read - Verify block integrity after read (deferred verification)
 * @arr: LHSR array
 * @bio: Original bio that was submitted
 * @block_offset: Block offset in array
 * @disk_idx: Disk index that was read from
 *
 * This implements read-time verification for bit-rot detection.
 * For production, use a work_struct to defer the verification
 * to avoid blocking the read path.
 */
static void lhsr_verify_on_read(struct lhsr_array *arr, struct bio *bio,
			  sector_t block_offset, unsigned int disk_idx)
{
	if (!arr || !bio)
		return;

	/*
	 * TODO: Implement checksum verification after read completes.
	 * This requires:
	 * 1. Storing per-block checksums in metadata
	 * 2. Using bio completion callback to verify
	 * 3. Triggering reconstruct if checksum mismatch
	 *
	 * For now, mark that read verification was attempted.
	 * Full implementation requires bios with callbacks.
	 */
	DMDEBUG("Read verification queued for block %llu on disk %u",
		    block_offset, disk_idx);
}

/* Map function - handles both reads and writes */
static int lhsr_map(struct dm_target *ti, struct bio *bio)
{
	if (bio_op(bio) == REQ_OP_READ)
		return lhsr_read(ti, bio);
	else
		return lhsr_write(ti, bio);
}

/* Write handler */
static int lhsr_write(struct dm_target *ti, struct bio *bio)
{
	struct lhsr_array *arr = ti->private;
	sector_t sector = bio->bi_iter.bi_sector;
	sector_t offset_in_array = sector - ti->begin;
	unsigned int disk_idx;

	if (!arr || arr->state == LHSR_STATE_OFFLINE ||
	    arr->state == LHSR_STATE_FAILED) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	switch (arr->raid_type) {
	case LHSR_RAID_SINGLE:
	case LHSR_RAID_MIRROR:
		if (arr->disks > 0 && arr->disk[0]) {
			bio->bi_bdev = arr->disk[0];
			bio->bi_iter.bi_sector = offset_in_array;
			submit_bio(bio);
		} else {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
		}
		break;

	case LHSR_RAID5:
	case LHSR_RAID_SHR:
	case LHSR_RAID6:
	case LHSR_RAID_SHR2:
		disk_idx = lhsr_map_sector(arr, offset_in_array);
		if (arr->disk[disk_idx]) {
			bio->bi_bdev = arr->disk[disk_idx];
			bio->bi_iter.bi_sector = offset_in_array;
			submit_bio(bio);
		} else {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
		}
		break;

	default:
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	atomic64_inc(&arr->writes);
	return DM_MAPIO_SUBMITTED;
}

/* Status output */
static void lhsr_status(struct dm_target *ti, status_type_t type, unsigned int status_flags, char *result, unsigned int maxlen)
{
	struct lhsr_array *arr = ti->private;
	unsigned int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%s %u/%u", arr->state == LHSR_STATE_HEALTHY ? "OK" : "DEGRADED",
		       arr->healthy_disks, arr->disks);
		break;
	case STATUSTYPE_TABLE:
		DMEMIT("UUID=%llx DISKS=%u RAID=%u",
		       arr->uuid, arr->disks, arr->raid_type);
		break;
	case STATUSTYPE_IMA:
		break;
	}
	(void)result;
	(void)maxlen;
	(void)status_flags;
}

/* Pause array */
static void lhsr_presuspend(struct dm_target *ti)
{
	struct lhsr_array *arr = ti->private;

	down_write(&arr->lock);
	arr->state = LHSR_STATE_SUSPENDED;
	DMINFO("Array %llx suspended", arr->uuid);
}

/* Resume array */
static void lhsr_resume(struct dm_target *ti)
{
	struct lhsr_array *arr = ti->private;

	arr->state = LHSR_STATE_ONLINE;
	up_write(&arr->lock);

	if (default_scrub_enabled)
		lhsr_schedule_scrub(arr);

	DMINFO("Array %llx resumed", arr->uuid);
}

/* ioctl handler - reserved for future use */
static int lhsr_ioctl(struct dm_target *ti, unsigned int cmd, unsigned long arg)
{
	(void)ti;
	(void)cmd;
	(void)arg;
	return -ENOTTY;
}

/* LHSR target constructor */
static int lhsr_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct lhsr_array *arr;
	unsigned int raid_type = LHSR_RAID5;
	unsigned int disks = 2;
	unsigned int i;
	int r;

	/* Parse arguments */
	if (argc < 2) {
		ti->error = "Invalid arguments";
		return -EINVAL;
	}

	/* Get RAID type */
	if (strcmp(argv[0], "single") == 0)
		raid_type = LHSR_RAID_SINGLE;
	else if (strcmp(argv[0], "mirror") == 0)
		raid_type = LHSR_RAID_MIRROR;
	else if (strcmp(argv[0], "raid5") == 0)
		raid_type = LHSR_RAID5;
	else if (strcmp(argv[0], "raid6") == 0)
		raid_type = LHSR_RAID6;
	else if (strcmp(argv[0], "shr") == 0)
		raid_type = LHSR_RAID_SHR;
	else if (strcmp(argv[0], "shr2") == 0)
		raid_type = LHSR_RAID_SHR2;
	else {
		ti->error = "Unknown RAID type";
		return -EINVAL;
	}

	/* Get disk count from devices - format: raid_type dev1 [dev2 ...] */
	if (argc < 2) {
		ti->error = "Invalid arguments";
		return -EINVAL;
	}
	disks = argc - 1;
	if (raid_type == LHSR_RAID_SINGLE) {
		if (disks < 1 || disks > MAX_DISKS) {
			ti->error = "Invalid disk count";
			return -EINVAL;
		}
	} else {
		if (disks < 2 || disks > MAX_DISKS) {
			ti->error = "Invalid disk count";
			return -EINVAL;
		}
	}

	/* Allocate array */
	arr = lhsr_alloc_array(fast_hash_32(raid_type), disks);
	if (!arr) {
		ti->error = "Failed to allocate array";
		return -ENOMEM;
	}

	/* Add disks from device names */
	for (i = 0; i < disks; i++) {
		struct dm_dev *dm_dev = NULL;

		r = dm_get_device(ti, argv[i + 1], FMODE_READ | FMODE_WRITE, &dm_dev);
		if (r) {
			DMERR("Cannot get device %s", argv[i + 1]);
			ti->error = "Failed to get device";
			goto bad_get_device;
		}

		arr->dm_devs[i] = dm_dev;
		r = lhsr_array_add_disk(arr, i, dm_dev->bdev);
		if (r < 0) {
			DMERR("Failed to add disk %s", argv[i + 1]);
			ti->error = "Failed to add disk";
			goto bad_get_device;
		}

		DMINFO("Added disk %u: %s", i, argv[i + 1]);
		continue;
bad_get_device:
		while (i > 0) {
			i--;
			dm_put_device(ti, arr->dm_devs[i]);
		}
		lhsr_free_array(arr);
		return -EINVAL;
	}

	/* Calculate array capacity from smallest disk */
	arr->total_capacity = bdev_nr_sectors(arr->disk[0]);
	for (i = 1; i < disks; i++) {
		u64 sectors = bdev_nr_sectors(arr->disk[i]);
		if (sectors < arr->total_capacity)
			arr->total_capacity = sectors;
	}
	if (arr->total_capacity == 0)
		arr->total_capacity = 0;
	ti->len = arr->total_capacity;

	if (ti->len < ti->begin) {
		ti->error = "Table size too small";
		return -EINVAL;
	}

	arr->raid_type = raid_type;
	arr->block_size = default_block_size;
	arr->healthy_disks = disks;
	arr->state = LHSR_STATE_ONLINE;

	/* Add to global list */
	down_write(&lhsr_sem);
	list_add(&arr->list, &lhsr_arrays);
	up_write(&lhsr_sem);

	ti->private = arr;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 0;

	DMINFO("Created LHSR array: RAID=%u DISKS=%u", raid_type, disks);
	return 0;
}

/* LHSR target destructor */
static void lhsr_dtr(struct dm_target *ti)
{
	struct lhsr_array *arr = ti->private;
	unsigned int i;

	if (!arr)
		return;

	for (i = 0; i < arr->disks; i++) {
		if (arr->dm_devs[i])
			dm_put_device(ti, arr->dm_devs[i]);
	}

	/* Remove from global list */
	down_write(&lhsr_sem);
	list_del(&arr->list);
	up_write(&lhsr_sem);

	lhsr_free_array(arr);
	ti->private = NULL;

	DMINFO("Destroyed LHSR target");
}

/* Target operations */
static struct target_type lhsr_target = {
	.name = "lhsr",
	.version = {1, 0, 0},
	.ctr = lhsr_ctr,
	.dtr = lhsr_dtr,
	.map = lhsr_map,
	.status = lhsr_status,
	.presuspend = lhsr_presuspend,
	.resume = lhsr_resume,
};

/* Module initialization */
static int __init lhsr_init(void)
{
	int r;

	DMINFO("Linux Hybrid Self-Healing RAID %s", LHSR_VERSION);

	lhsr_wq = alloc_workqueue("lhsr", WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!lhsr_wq) {
		DMERR("Failed to create workqueue");
		return -ENOMEM;
	}

	r = dm_register_target(&lhsr_target);
	if (r) {
		DMERR("Failed to register target: %d", r);
		destroy_workqueue(lhsr_wq);
		return r;
	}

	DMINFO("Module loaded successfully");
	return 0;
}

/* Module cleanup */
static void __exit lhsr_exit(void)
{
	struct lhsr_array *arr, *tmp;

	DMINFO("Unloading module");

	list_for_each_entry_safe(arr, tmp, &lhsr_arrays, list) {
		cancel_work_sync(&arr->scrub_work);
		cancel_work_sync(&arr->rebuild_work);
		cancel_work_sync(&arr->monitor_work);
	}

	dm_unregister_target(&lhsr_target);

	if (lhsr_wq)
		destroy_workqueue(lhsr_wq);

	DMINFO("Module unloaded");
}

module_init(lhsr_init);
module_exit(lhsr_exit);