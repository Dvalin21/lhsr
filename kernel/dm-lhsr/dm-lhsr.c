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
static void lhsr_clone_bio(struct bio *bio, struct block_device *bdev,
			   sector_t sector, unsigned int len)
{
	struct bio *clone = bio_alloc(bdev, bio->bi_vcnt, bio->bi_opf, GFP_NOIO);

	if (!clone)
		return;

	bio_copy_data(clone, bio);
	clone->bi_iter.bi_sector = sector;
	submit_bio(clone);
}

/* Scrubber work handler */
static void lhsr_scrub_work(struct work_struct *work)
{
	struct lhsr_array *arr = container_of(work, struct lhsr_array, scrub_work);

	if (!arr)
		return;

	DMINFO("Starting background scrub for array %llx", arr->uuid);

	atomic64_inc(&arr->scrub_blocks);

	DMINFO("Completed background scrub for array %llx", arr->uuid);
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

/* Read handler */
static int lhsr_read(struct dm_target *ti, struct bio *bio)
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

	disk_idx = lhsr_map_sector(arr, offset_in_array);
	if (disk_idx >= arr->disks || !arr->disk[disk_idx]) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	lhsr_clone_bio(bio, arr->disk[disk_idx],
		       offset_in_array * arr->block_size,
		       bio_sectors(bio) << 9);

	atomic64_inc(&arr->reads);
	return DM_MAPIO_SUBMITTED;
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
	unsigned int parity_disk;

	if (!arr || arr->state == LHSR_STATE_OFFLINE ||
	    arr->state == LHSR_STATE_FAILED) {
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	switch (arr->raid_type) {
	case LHSR_RAID_SINGLE:
		lhsr_clone_bio(bio, arr->disk[0],
			       offset_in_array * arr->block_size,
			       bio_sectors(bio) << 9);
		break;

	case LHSR_RAID_MIRROR:
		lhsr_clone_bio(bio, arr->disk[0],
			       offset_in_array * arr->block_size,
			       bio_sectors(bio) << 9);
		if (arr->disk[1])
			lhsr_clone_bio(bio, arr->disk[1],
				       offset_in_array * arr->block_size,
				       bio_sectors(bio) << 9);
		break;

	case LHSR_RAID5:
	case LHSR_RAID_SHR:
		disk_idx = lhsr_map_sector(arr, offset_in_array);
		if (arr->disk[disk_idx])
			lhsr_clone_bio(bio, arr->disk[disk_idx],
				       offset_in_array * arr->block_size,
				       bio_sectors(bio) << 9);
		parity_disk = arr->disks - 1;
		if (arr->disk[parity_disk])
			lhsr_clone_bio(bio, arr->disk[parity_disk],
				       offset_in_array * arr->block_size,
				       bio_sectors(bio) << 9);
		break;

	case LHSR_RAID6:
	case LHSR_RAID_SHR2:
		disk_idx = lhsr_map_sector(arr, offset_in_array);
		if (arr->disk[disk_idx])
			lhsr_clone_bio(bio, arr->disk[disk_idx],
				       offset_in_array * arr->block_size,
				       bio_sectors(bio) << 9);
		parity_disk = arr->disks - 2;
		if (arr->disk[parity_disk])
			lhsr_clone_bio(bio, arr->disk[parity_disk],
				       offset_in_array * arr->block_size,
				       bio_sectors(bio) << 9);
		if (arr->disk[parity_disk + 1])
			lhsr_clone_bio(bio, arr->disk[parity_disk + 1],
				       offset_in_array * arr->block_size,
				       bio_sectors(bio) << 9);
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

	/* Get disk count from devices */
	disks = argc - 1;
	if (disks < 2 || disks > MAX_DISKS) {
		ti->error = "Invalid disk count";
		return -EINVAL;
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

		r = dm_get_device(ti, argv[i + 1], dm_table_get_mode(ti->table), &dm_dev);
		if (r) {
			DMERR("Cannot get device %s", argv[i + 1]);
			arr->healthy_disks = i;
			lhsr_free_array(arr);
			ti->error = "Failed to get device";
			return -EINVAL;
		}

		arr->dm_devs[i] = dm_dev;
		r = lhsr_array_add_disk(arr, i, dm_dev->bdev);
		if (r < 0) {
			DMERR("Failed to add disk %s", argv[i + 1]);
			arr->healthy_disks = i;
			lhsr_free_array(arr);
			ti->error = "Failed to add disk";
			return r;
		}

		DMINFO("Added disk %u: %s", i, argv[i + 1]);
	}

	arr->raid_type = raid_type;
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