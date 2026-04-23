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

#include "dm_lhsr.h"

#define DM_MSG_PREFIX "lhsr"
#define LHSR_VERSION "1.0.2"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LHSR Team");
MODULE_DESCRIPTION("Linux Hybrid Self-Healing RAID");
MODULE_VERSION(LHSR_VERSION);

/* Simple array structure */
struct lhsr_array {
	u64 uuid;
	u32 raid_type;
	unsigned int disks;
	sector_t size;
	struct block_device *disk[32];
	struct dm_dev *dm_devs[32];
	u32 state;
	u32 primary_disk;      /* Which disk is primary for reads */
	u32 failed_disks;      /* Bitmask of failed disks */
};

/* Fast hash for UUID generation */
static inline u64 fast_hash_32(u32 val)
{
	u32 hash = val * 0x9e370001UL;
	return hash >> 16;
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

	for (i = 0; i < arr->disks; i++) {
		if (arr->dm_devs[i])
			dm_put_device(ti, arr->dm_devs[i]);
	}

	kfree(arr);
	ti->private = NULL;
	DMINFO("Destroyed LHSR target");
}

/* Map function */
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
		submit_bio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* RAID1 Mirror - write to all disks */
	if (bio_op(bio) != REQ_OP_READ) {
		/* Write: send to both disks */
		bio_set_dev(bio, arr->disk[0]);
		bio->bi_iter.bi_sector = offset;
		submit_bio(bio);

		/* Clone for second disk */
		clone = bio_alloc_clone(arr->disk[1], bio, GFP_NOIO, &fs_bio_set);
		if (!clone) {
			DMWARN("Failed to clone bio for mirror write");
			return DM_MAPIO_SUBMITTED;
		}
		submit_bio(clone);
		return DM_MAPIO_SUBMITTED;
	}

	/* Read: try primary, failover to secondary if failed */
	if (!(arr->failed_disks & (1 << arr->primary_disk))) {
		bio_set_dev(bio, arr->disk[arr->primary_disk]);
		bio->bi_iter.bi_sector = offset;
		submit_bio(bio);
	} else {
		/* Primary failed, use other disk */
		unsigned int other = arr->primary_disk == 0 ? 1 : 0;
		if (!(arr->failed_disks & (1 << other))) {
			bio_set_dev(bio, arr->disk[other]);
			bio->bi_iter.bi_sector = offset;
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
			sz += scnprintf(result + sz, maxlen - sz, "%s %u/%u",
				       state, healthy, arr->disks);
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

/* Target operations */
static struct target_type lhsr_target = {
	.name = "lhsr",
	.version = {1, 0, 2},
	.ctr = lhsr_ctr,
	.dtr = lhsr_dtr,
	.map = lhsr_map,
	.status = lhsr_status,
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