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

#include "dm_lhsr.h"

#define DM_MSG_PREFIX "lhsr"
#define LHSR_VERSION "1.0.3"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LHSR Team");
MODULE_DESCRIPTION("Linux Hybrid Self-Healing RAID");
MODULE_VERSION(LHSR_VERSION);

/* Array structure */
struct lhsr_array {
	u64 uuid;
	u32 raid_type;
	unsigned int disks;
	sector_t size;
	struct block_device *disk[32];
	struct dm_dev *dm_devs[32];
	u32 state;
	u32 primary_disk;
	u32 failed_disks;

	/* Disk failure detection */
	u32 disk_errors[32];
	u32 error_threshold;
	struct workqueue_struct *check_wq;
	struct delayed_work check_work;
	unsigned long last_check;

	/* Statistics */
	atomic_t io_count;
	atomic_t io_errors;
	atomic_t failovers;

	/* I/O tracking */
	unsigned long last_error_jiffies;
	u32 last_error_disk;
};

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
							if (arr->primary_disk == i) {
								arr->primary_disk = i == 0 ? 1 : 0;
								DMINFO("Failover to disk %u", arr->primary_disk);
							}
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
		DMWARN("Failed disks mask: 0x%x", arr->failed_disks);
	}

	/* Schedule next check */
	arr->last_check = jiffies;
	queue_delayed_work(arr->check_wq, &arr->check_work, 30 * HZ);
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
		raid_type = 0;  /* JBOD - single disk */
	} else if (strcmp(argv[0], "mirror") == 0) {
		raid_type = 1;  /* RAID1 - mirroring */
	} else {
		DMERR("Unknown raid type: %s", argv[0]);
		ti->error = "Unknown raid type";
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
	arr->error_threshold = 3; /* Mark failed after 3 errors */
	arr->last_check = jiffies;

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
	arr->state = LHSR_STATE_ONLINE;

	/* Create workqueue for disk health checks */
	arr->check_wq = alloc_workqueue("lhsr_check", WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
	if (!arr->check_wq) {
		DMERR("Failed to create workqueue");
		r = -ENOMEM;
		goto bad;
	}
	INIT_DELAYED_WORK(&arr->check_work, disk_check_work);
	queue_delayed_work(arr->check_wq, &arr->check_work, 10 * HZ);

	ti->private = arr;
	ti->len = size;
	ti->begin = 0;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 0;

	DMINFO("Created LHSR: type=%u size=%llu", raid_type, size);
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

	if (!arr)
		return;

	/* Stop the health check workqueue */
	if (arr->check_wq) {
		cancel_delayed_work_sync(&arr->check_work);
		destroy_workqueue(arr->check_wq);
	}

	for (i = 0; i < arr->disks; i++) {
		if (arr->dm_devs[i])
			dm_put_device(ti, arr->dm_devs[i]);
	}

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

	if (!arr || arr->state != LHSR_STATE_ONLINE) {
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

	/* RAID1 Mirror - write to both disks */
	if (bio_op(bio) != REQ_OP_READ) {
		if (!(arr->failed_disks & (1 << 0))) {
			clone = bio_alloc_clone(arr->disk[0], bio, GFP_NOIO, &fs_bio_set);
			if (clone) {
				clone->bi_iter.bi_sector = offset;
				clone->bi_private = ti;
				clone->bi_end_io = lhsr_io_complete;
				submit_bio(clone);
			} else {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
		} else if (!(arr->failed_disks & (1 << 1))) {
			clone = bio_alloc_clone(arr->disk[1], bio, GFP_NOIO, &fs_bio_set);
			if (clone) {
				clone->bi_iter.bi_sector = offset;
				clone->bi_private = ti;
				clone->bi_end_io = lhsr_io_complete;
				submit_bio(clone);
			} else {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
		} else {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		if (!(arr->failed_disks & (1 << 1))) {
			clone = bio_alloc_clone(arr->disk[1], bio, GFP_NOIO, &fs_bio_set);
			if (clone) {
				clone->bi_iter.bi_sector = offset;
				clone->bi_private = ti;
				clone->bi_end_io = lhsr_io_complete;
				submit_bio(clone);
			}
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
		break;
	case STATUSTYPE_TABLE:
		sz += scnprintf(result + sz, maxlen - sz, "UUID=%llx RAID=%u DISKS=%u",
			       arr->uuid, arr->raid_type, arr->disks);
		break;
	default:
		break;
	}
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

		if (arr->primary_disk == disk_idx) {
			arr->primary_disk = (disk_idx == 0) ? 1 : 0;
			DMINFO("Primary disk failed, failover to disk %u", arr->primary_disk);
		}

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

		return 0;
	}

	if (strncmp(argv[0], "disk_health", 10) == 0) {
		if (argc < 2)
			return -EINVAL;
		err = kstrtouint(argv[1], 10, &disk_idx);
		if (err || disk_idx >= arr->disks)
			return -EINVAL;

		scnprintf(result, maxlen, "disk[%u]: errors=%u failed=%d",
			  disk_idx, arr->disk_errors[disk_idx],
			  (arr->failed_disks >> disk_idx) & 1);

		return 0;
	}

	DMERR("Unknown message: %s", argv[0]);
	return -EINVAL;
}

/* Target operations */
static struct target_type lhsr_target = {
	.name = "lhsr",
	.version = {1, 0, 2},
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