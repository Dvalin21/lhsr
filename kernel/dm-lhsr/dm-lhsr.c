/*
 * LHSR - Linux Hybrid Self-Healing RAID
 * Device Mapper Main Module - Minimal Working Base v1.0.2
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 *
 * DATA STRUCTURE DESIGN:
 *   The core data structure is struct lhsr_array, which models a RAID array
 *   as a set of block devices, a raid type (single/mirror/raid5/raid6), and
 *   associated metadata (superblocks, checksum cache, scrub/rebuild state).
 *
 *   I/O model: All synchronous I/O uses submit_bio_wait() — the kernel's
 *   standard synchronous bio submission API.  There is NO ad-hoc timeout
 *   mechanism.  If a device is dead the block layer's own timeouts will fire.
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
#include <linux/delay.h>

#include "dm_lhsr.h"

#define DM_MSG_PREFIX "lhsr"
#define LHSR_VERSION "1.3.0"

/* Forward declarations for RAID5/6 write */
struct lhsr_raid_5_write_ctx;
struct lhsr_raid_5_read_ctx;
struct lhsr_array;
static void lhsr_raid_5_data_endio(struct bio *bio);
static void lhsr_raid_5_parity_endio(struct bio *bio);
static void lhsr_raid_5_read_endio(struct bio *bio);
static void lhsr_xor_parity(void *parity, void **data, unsigned int data_disks, size_t len);
static void lhsr_rs_parity(void *parity_p, void *parity_q, void **data,
                           unsigned int data_disks, size_t len);

static struct bio_set lhsr_bioset;

/* Precomputed 2^i in GF(2^8) for RAID6 Reed-Solomon */
static u8 rs_power_table[256];

/* Active device tracking to prevent use-after-unload */
static atomic_t lhsr_active_devices = ATOMIC_INIT(0);
static int lhsr_module_exiting = 0;

/*
 * RAID5/6 write context - tracks in-flight data writes and parity write
 *
 * DATA STRUCTURE DESIGN:
 *   data_bufs[] and data_pages[] are indexed by the RAW data-disk index
 *   (0..num_disks-1).  Entries for failed disks or bios whose clone
 *   allocation failed remain NULL and are SKIPPED during parity computation.
 *   This is CRITICAL because:
 *     (a) bio_alloc_clone() can fail for individual disks,
 *     (b) loop/ram devices complete synchronously inside submit_bio(),
 *         meaning data_endio runs BEFORE the submit loop finishes.
 *   A compacted scheme (disk_map + variable-length data_bufs) would Oops
 *   on NULL entries because we cannot update "working" atomically before
 *   synchronous completion fires.
 *
 *   Parity computation builds a temporary compacted array of non-NULL
 *   buffers on the stack (max 32 entries × 8 bytes = 256 bytes).
 */
struct lhsr_raid_5_write_ctx {
	struct bio *orig_bio;		/* Original bio to complete */
	struct lhsr_array *arr;	/* Array context */
	atomic_t pending;		/* Count of pending data writes + parity */
	int status;			/* Final status */
	void *parity_buf;		/* P parity buffer (RAID5/6) */
	void *parity_q_buf;		/* Q parity buffer (RAID6 only) */
	struct page *parity_page;	/* Page backing P parity buffer */
	struct page *parity_q_page;	/* Page backing Q parity buffer (RAID6) */
	struct bio *parity_bio;	/* P parity write bio */
	struct bio *parity_q_bio;	/* Q parity write bio (RAID6) */
	unsigned int num_disks;	/* Total data disks in array */
	unsigned int working;		/* Number of SUBMITTED data writes */
	sector_t offset;		/* Sector offset for writes */
	size_t bio_size;		/* Byte count stored at write start (safe from completion) */
	void **data_bufs;		/* Array of data buffers (num_disks entries, idx=disk_idx) */
	struct page **data_pages;	/* Array of pages for kunmap (num_disks entries, idx=disk_idx) */
};

/* RAID5/6 read reconstruction context */
struct lhsr_raid_5_read_ctx {
	struct bio *orig_bio;		/* Original bio to complete */
	struct lhsr_array *arr;		/* Array context */
	atomic_t pending;			/* Count of pending reads */
	int status;			/* Final status */
	void *recon_buf;			/* Reconstructed data buffer */
	unsigned int target_disk;	/* Which disk we're reconstructing for */
	unsigned int num_disks;	/* Total data disks */
	unsigned int working;		/* Number of working data disks */
	unsigned int num_slots;		/* Total buffer slots = working + parity_reads */
	sector_t offset;			/* Sector offset */
	size_t bio_size;			/* Byte count stored at read start (safe from completion) */
	void **data_bufs;			/* Array of data+parity buffers */
	unsigned int disk_map[0];	/* Flex array: slot_idx -> disk_idx */
};


MODULE_LICENSE("GPL");
MODULE_AUTHOR("LHSR Team");
MODULE_DESCRIPTION("Linux Hybrid Self-Healing RAID");
MODULE_VERSION(LHSR_VERSION);

/*
 * CRC32c checksum using kernel API
 * Kernel's crc32c() computes CRC32c (Castagnoli) with:
 *   - Initial value: 0xFFFFFFFF
 *   - Final XOR: 0xFFFFFFFF
 *   - Polynomial: 0x1EDC6F41 (same as 0x82F63B78 reflected)
 *
 * Synchronous I/O: All sync BIO operations use submit_bio_wait(), the kernel's
 * standard single-page synchronous I/O API.  No ad-hoc timeouts — the block
 * layer handles timeout and error reporting internally.  This eliminates an
 * entire class of races (calling bio_endio on in-flight BIOs, freeing pages
 * while the device is still DMA'ing, etc).
 */

/* Atomic superblock write - write-hole protection
 * Strategy: Write new superblock to backup first, then primary.
 * On crash, either old primary+new backup or new primary+new backup.
 * Never old primary+old backup (regress), never new primary+old backup (inconsistent).
 *
 * NVMe SAFETY: Uses REQ_FUA to ensure data is persisted to media
 * before returning. This prevents data loss on hard power-off.
 */
static int lhsr_write_superblock(struct block_device *bdev, struct lhsr_superblock *sb,
				 sector_t array_size, sector_t disk_offset)
{
	struct bio *bio;
	struct page *page;
	void *buf;
	sector_t primary_sector, backup_sector;
	u64 backup_off;
	int ret;

	/* Calculate sectors */
	primary_sector = (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + disk_offset;
	backup_off = LHSR_SB_BACKUP_OFF(array_size << SECTOR_SHIFT);
	backup_sector = backup_off >> SECTOR_SHIFT;

	/* Allocate page for I/O */
	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	buf = page_address(page);

	/* Calculate checksum using kernel CRC32c API */
	memcpy(buf, sb, LHSR_SB_SIZE);
	/* Zero checksum field for CRC calculation */
	*(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
	sb->checksum = crc32c(0xFFFFFFFF, buf, LHSR_SB_SIZE) ^ 0xFFFFFFFF;
	/* Copy final superblock with valid checksum */
	memcpy(buf, sb, LHSR_SB_SIZE);

	/*
	 * Step 1: Write to BACKUP location first.
	 * Uses submit_bio_wait() — the kernel's standard synchronous BIO API.
	 */
	bio = bio_alloc(bdev, 1, REQ_OP_WRITE | REQ_SYNC | REQ_FUA, GFP_KERNEL);
	if (!bio) {
		__free_page(page);
		return -ENOMEM;
	}
	bio->bi_iter.bi_sector = backup_sector;
	if (!bio_add_page(bio, page, PAGE_SIZE, 0)) {
		DMERR("Failed to add page to backup write bio");
		bio_put(bio);
		__free_page(page);
		return -ENOMEM;
	}

	DMINFO("lhsr_write_superblock: writing backup to sector %llu", (u64)backup_sector);
	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret != 0) {
		DMERR("Backup superblock write failed at sector %llu: %d",
		       (u64)backup_sector, ret);
		__free_page(page);
		return ret;
	}

	/*
	 * Step 2: Write to PRIMARY location.
	 * Now we have new backup + old primary (crash-safe).
	 * After this: new backup + new primary.
	 */
	bio = bio_alloc(bdev, 1, REQ_OP_WRITE | REQ_SYNC | REQ_FUA, GFP_KERNEL);
	if (!bio) {
		__free_page(page);
		return -ENOMEM;
	}
	bio->bi_iter.bi_sector = primary_sector;
	if (!bio_add_page(bio, page, PAGE_SIZE, 0)) {
		DMERR("Failed to add page to primary write bio");
		bio_put(bio);
		__free_page(page);
		return -ENOMEM;
	}

	DMINFO("lhsr_write_superblock: writing primary to sector %llu", (u64)primary_sector);
	ret = submit_bio_wait(bio);
	bio_put(bio);
	__free_page(page);

	if (ret != 0) {
		DMERR("Primary superblock write failed at sector %llu: %d",
		       (u64)primary_sector, ret);
		return ret;
	}

	DMINFO("Superblock write successful (backup + primary)");
	return 0;
}

static int lhsr_read_superblock(struct block_device *bdev, struct lhsr_superblock *sb,
				sector_t disk_offset)
{
	struct bio *bio;
	struct page *page;
	void *buf;
	sector_t sector;
	u32 stored_csum, calc_csum;
	int ret;

	sector = (LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT) + disk_offset;
	DMINFO("lhsr_read_superblock: sector=0x%llx", sector);

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	DMINFO("lhsr_read_superblock: allocating bio");
	bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_KERNEL);
	if (!bio) {
		__free_page(page);
		return -ENOMEM;
	}
	bio_set_dev(bio, bdev);
	bio->bi_iter.bi_sector = sector;
	if (!bio_add_page(bio, page, PAGE_SIZE, 0)) {
		DMERR("Failed to add page to primary read bio");
		bio_put(bio);
		__free_page(page);
		return -ENOMEM;
	}

	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret != 0) {
		DMERR("Superblock read failed at sector %llu: %d", (u64)sector, ret);
		__free_page(page);
		return -EIO;
	}

	buf = page_address(page);
	stored_csum = *(u32 *)(buf + offsetof(struct lhsr_superblock, checksum));
	*(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
	calc_csum = crc32c(0xFFFFFFFF, buf, LHSR_SB_SIZE) ^ 0xFFFFFFFF;

	if (stored_csum != calc_csum) {
		u64 backup_off = LHSR_SB_BACKUP_OFF(bdev_nr_sectors(bdev) << SECTOR_SHIFT);
		sector_t backup_sector = backup_off >> SECTOR_SHIFT;
		DMWARN("Primary superblock checksum mismatch (0x%08x vs 0x%08x), trying backup",
		       stored_csum, calc_csum);

		bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_KERNEL);
		if (!bio) {
			__free_page(page);
			return -ENOMEM;
		}
		bio_set_dev(bio, bdev);
		bio->bi_iter.bi_sector = backup_sector;
		__bio_add_page(bio, page, PAGE_SIZE, 0);

		ret = submit_bio_wait(bio);
		bio_put(bio);

		if (ret != 0) {
			DMERR("Superblock read failed at backup sector %llu: %d",
			       (u64)backup_sector, ret);
			__free_page(page);
			return -EIO;
		}

		stored_csum = *(u32 *)(buf + offsetof(struct lhsr_superblock, checksum));
		*(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
		calc_csum = crc32c(0xFFFFFFFF, buf, LHSR_SB_SIZE) ^ 0xFFFFFFFF;

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
	sb->checksum = crc32c(0xFFFFFFFF, (void *)sb, LHSR_SB_SIZE) ^ 0xFFFFFFFF;
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
	sector_t disk_offset[32];	/* Per-disk offset in sectors */
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

	/* Simple in-memory checksum cache for scrubber (sparse array) */
	/* We store checksums for every 128KB block, indexed by offset/sector */
#define LHSR_CKSUM_BUCKETS 1024
	struct lhsr_cksum_entry {
		u64 offset;     /* Sector offset of block start */
		u32 cksum;     /* CRC32c value */
		u32 flags;      /* Status flags */
	} cksum_cache[LHSR_CKSUM_BUCKETS];
	u32 cksum_count;  /* Number of valid entries */

	/* RAID5 write completion tracking */
	atomic_t inflight_writes;
	struct bio *orig_bio;        /* Original bio for completion */
	unsigned int data_disks_written; /* Count of data disks written */
};

/* RAID5/6 READ reconstruction completion */
static void lhsr_raid_5_read_endio(struct bio *bio)
{
	struct lhsr_raid_5_read_ctx *ctx = bio->bi_private;
	struct lhsr_array *arr = ctx->arr;
	int is_raid6 = (arr->raid_type == 3);
	size_t bio_size = ctx->bio_size; /* Use stored size, NOT orig_bio->bi_iter after completion */
	unsigned int slot;
	unsigned int i;

	if (bio->bi_status)
		ctx->status = bio->bi_status;

	/* Find which slot this bio corresponds to in disk_map */
	slot = ctx->num_slots; /* Invalid sentinel */
	for (i = 0; i < ctx->num_slots; i++) {
		unsigned int disk_idx = ctx->disk_map[i];
		if (bio->bi_bdev == arr->disk[disk_idx]) {
			slot = i;
			break;
		}
	}

	/* Populate data_bufs entry from this completed bio's data */
	if (slot < ctx->num_slots && ctx->data_bufs && ctx->data_bufs[slot]) {
		struct bio_vec bv;
		struct bvec_iter iter;
		void *src;

		bio_for_each_segment(bv, bio, iter) {
			src = kmap(bv.bv_page) + bv.bv_offset;
			memcpy(ctx->data_bufs[slot], src, bv.bv_len);
			kunmap(bv.bv_page);
			break; /* Single segment for now */
		}
	}

	if (atomic_dec_and_test(&ctx->pending)) {
		/* All reads done - reconstruct missing data */
		if (ctx->status == 0 && ctx->recon_buf) {
			unsigned int failed_count = 0;
			unsigned int failed[2];
			unsigned int j;
			int all_bufs_valid = 1;

			/* Find which disks failed */
			for (j = 0; j < ctx->num_disks; j++) {
				if (arr->failed_disks & (1 << j)) {
					if (failed_count < 2)
						failed[failed_count] = j;
					failed_count++;
				}
			}

			/* Validate ALL data buffers before XOR — any NULL
			 * means an allocation failed in the map function;
			 * dereferencing it would crash the kernel.
			 */
			for (j = 0; j < ctx->num_slots; j++) {
				if (!ctx->data_bufs[j]) {
					all_bufs_valid = 0;
					break;
				}
			}

			if (all_bufs_valid) {
				if (failed_count == 1) {
					/*
					 * Single failure: XOR all survivors.
					 * For RAID5: data disks + P parity.
					 * For RAID6: data disks + P+Q, but only
					 * one is missing, so XOR of survivors works.
					 */
					lhsr_xor_parity(ctx->recon_buf,
							 (void **)ctx->data_bufs,
							 ctx->num_slots, bio_size);
				} else if (failed_count >= 2) {
					/*
					 * RAID6 double-failure (or RAID5 >1 failure).
					 * Proper Reed-Solomon reconstruction is NOT
					 * yet implemented.  Return error instead of
					 * producing garbage data or crashing.
					 * TODO: Implement RS decode for double failure.
					 */
					DMERR("RAID%c: double-failure reconstruction not "
					      "implemented (%u failed disks)",
					      is_raid6 ? '6' : '5', failed_count);
					ctx->status = BLK_STS_IOERR;
				}
			} else {
				DMERR("RAID5/6 read: data buffer missing (allocation failure)");
				ctx->status = BLK_STS_RESOURCE;
			}

			/* Copy reconstructed data to original bio */
			if (ctx->status == 0) {
				struct bio_vec bv;
				struct bvec_iter iter;
				bio_for_each_segment(bv, ctx->orig_bio, iter) {
					void *dst = kmap(bv.bv_page) + bv.bv_offset;
					memcpy(dst, ctx->recon_buf, bv.bv_len);
					kunmap(bv.bv_page);
					break; /* Single segment for now */
				}
			}
		}

		/* Complete original bio */
		ctx->orig_bio->bi_status = ctx->status;
		bio_endio(ctx->orig_bio);

		/* Cleanup - use num_slots to cover all data+parity buffers */
		kfree(ctx->recon_buf);
		if (ctx->data_bufs) {
			unsigned int j;
			for (j = 0; j < ctx->num_slots; j++) {
				if (ctx->data_bufs[j])
					kfree(ctx->data_bufs[j]);
			}
			kfree(ctx->data_bufs);
		}
		kfree(ctx);
	}

	bio_put(bio);
}

static int lhsr_update_disk_state(struct lhsr_array *arr, unsigned int disk_idx, u32 new_state);
static void lhsr_xor_parity(void *parity, void **data, unsigned int data_disks, size_t len);

/* I/O context to save original completion */
struct lhsr_io_ctx {
	bio_end_io_t *orig_endio;
	void *orig_private;
	struct dm_target *ti;  /* Save target for completion */
};

/* Forward declaration */
static void lhsr_io_complete(struct bio *bio);

/* Helper to set up I/O tracking - saves original completion */
static int lhsr_setup_io_tracking(struct bio *bio, struct dm_target *ti)
{
	struct lhsr_io_ctx *ctx;
	
	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	
	/* Save original completion (set by DM core) */
	ctx->orig_endio = bio->bi_end_io;
	ctx->orig_private = bio->bi_private;
	ctx->ti = ti;  /* Save target for completion */
	
	/* Set up our tracking */
	bio->bi_private = ctx;
	bio->bi_end_io = lhsr_io_complete;
	
	return 0;
}

/* I/O completion callback - tracks errors */
static void lhsr_io_complete(struct bio *bio)
{
	struct lhsr_io_ctx *ctx = bio->bi_private;
	struct dm_target *ti;
	struct lhsr_array *arr;
	bio_end_io_t *orig_endio;
	void *orig_private;

	if (!ctx)
		return;

	/* Restore original bio state */
	orig_endio = ctx->orig_endio;
	orig_private = ctx->orig_private;
	bio->bi_private = orig_private;

	/* Get our data from saved ti */
	ti = ctx->ti;  /* CORRECT: use saved ti, not orig_private */
	if (!ti || !ti->private) {
		kfree(ctx);
		goto out;
	}

	arr = ti->private;

	if (bio->bi_status != BLK_STS_OK) {
		DMERR("I/O error: status=%d", bio->bi_status);
		atomic_inc(&arr->io_errors);

		/* Find which disk */
		if (bio->bi_bdev) {
			unsigned int i;
			int should_fail = 0;
			for (i = 0; i < arr->disks; i++) {
				if (arr->disk[i] == bio->bi_bdev) {
					arr->disk_errors[i]++;
					arr->last_error_disk = i;
					arr->last_error_jiffies = jiffies;

					if (arr->disk_errors[i] >= arr->error_threshold) {
						should_fail = 1;
					}
					break;
				}
			}

			/* Update failed_disks with lock protection */
			if (should_fail) {
				down_write(&arr->sb_sem);
				if (!(arr->failed_disks & (1 << i))) {
					DMERR("Disk %u failed due to I/O errors", i);
					arr->failed_disks |= (1 << i);
					up_write(&arr->sb_sem);
					lhsr_update_disk_state(arr, i, LHSR_DISK_DEGRADED);
					if (arr->primary_disk == i) {
						unsigned int k;
						arr->primary_disk = arr->disks; /* invalid */
						for (k = 0; k < arr->disks; k++) {
							if (k == i) continue;
							if (!(arr->failed_disks & (1 << k))) {
								arr->primary_disk = k;
								break;
							}
						}
						if (arr->primary_disk < arr->disks)
							DMINFO("Failover to disk %u", arr->primary_disk);
						else
							DMERR("No working disk for failover");
					}
					arr->state = LHSR_STATE_DEGRADED;
					atomic_inc(&arr->failovers);
				} else {
					up_write(&arr->sb_sem);
				}
			}
		}
	} else {
		atomic_inc(&arr->io_count);
	}

	kfree(ctx);

out:
	/* Call original completion function (DM's clone_endio) */
	if (orig_endio)
		orig_endio(bio);
}

/* Fast hash for UUID generation */
/* Fast hash using 64-bit arithmetic to avoid overflow */
static inline u64 fast_hash_32(u32 val)
{
	u64 hash = (u64)val * 0x9e370001UL;
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
static void __used disk_check_work(struct work_struct *work)
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
				unsigned int k;
				arr->primary_disk = arr->disks; /* invalid */
				for (k = 0; k < arr->disks; k++) {
					if (k == i) continue;
					if (!(arr->failed_disks & (1 << k)) &&
					    !(new_failed & (1 << k))) {
						arr->primary_disk = k;
						break;
					}
				}
				if (arr->primary_disk < arr->disks)
					DMINFO("Primary disk failed, failover to disk %u", arr->primary_disk);
				else
					DMERR("No working disk for failover");
			}
		}
	}

	if (new_failed) {
		/* Update failed_disks with lock protection */
		down_write(&arr->sb_sem);
		arr->failed_disks |= new_failed;
		up_write(&arr->sb_sem);
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

/* Scrub a block - reads and verifies checksums
 *
 * Uses proper page cache pages for BIO I/O instead of vmalloc'd memory.
 * vmalloc_to_page() is unreliable for I/O because kvmalloc may return
 * kmalloc memory, and block layer BIOs require struct page backing.
 */
static int lhsr_scrub_block(struct lhsr_array *arr, unsigned int disk_idx, u64 offset)
{
	struct lhsr_cksum_entry *entry = NULL;
	struct bio *bio;
	struct page **pages;
	void *buf;
	u32 stored_csum = 0;
	u32 calc_csum;
	unsigned int nr_pages;
	int ret = 0;
	int i;

	if (arr->failed_disks & (1 << disk_idx))
		return -EINVAL;

	/* Check if we have a stored checksum for this offset */
	for (i = 0; i < arr->cksum_count; i++) {
		if (arr->cksum_cache[i].offset == offset) {
			entry = &arr->cksum_cache[i];
			stored_csum = entry->cksum;
			break;
		}
	}

	nr_pages = (LHSR_SCRUB_BLOCK_SIZE + PAGE_SIZE - 1) >> PAGE_SHIFT;

	/* Allocate pages for I/O — proper struct page backing */
	pages = kcalloc(nr_pages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (i = 0; i < nr_pages; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i]) {
			while (i--)
				__free_page(pages[i]);
			kfree(pages);
			return -ENOMEM;
		}
	}

	/* Read the block using bio — use the bioset */
	bio = bio_alloc_bioset(arr->disk[disk_idx], nr_pages,
			       REQ_OP_READ, GFP_KERNEL, &lhsr_bioset);
	if (!bio) {
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		return -ENOMEM;
	}
	bio->bi_iter.bi_sector = offset + arr->disk_offset[disk_idx];
	for (i = 0; i < nr_pages; i++) {
		unsigned int page_off = i << PAGE_SHIFT;
		unsigned int page_bytes = min_t(unsigned int,
			LHSR_SCRUB_BLOCK_SIZE - page_off, PAGE_SIZE);
		__bio_add_page(bio, pages[i], page_bytes, 0);
	}

	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret != 0) {
		DMERR("Scrub read failed at offset 0x%llx: %d", offset, ret);
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		return ret;
	}

	/* Copy to linear buffer for checksum calculation,
	 * then free the I/O pages immediately */
	buf = kmalloc(LHSR_SCRUB_BLOCK_SIZE, GFP_KERNEL);
	if (!buf) {
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		return -ENOMEM;
	}

	{
		void *buf_ptr = buf;
		for (i = 0; i < nr_pages; i++) {
			unsigned int page_off = i << PAGE_SHIFT;
			unsigned int page_bytes = min_t(unsigned int,
				LHSR_SCRUB_BLOCK_SIZE - page_off, PAGE_SIZE);
			void *kaddr = kmap(pages[i]);
			memcpy(buf_ptr, kaddr, page_bytes);
			kunmap(pages[i]);
			buf_ptr += page_bytes;
		}
	}

	for (i = 0; i < nr_pages; i++)
		__free_page(pages[i]);
	kfree(pages);

	/* Calculate checksum of read data */
	calc_csum = crc32c(0xFFFFFFFF, buf, LHSR_SCRUB_BLOCK_SIZE) ^ 0xFFFFFFFF;

	if (stored_csum == 0) {
		/* No stored checksum - this is a new block, store the checksum */
		if (arr->cksum_count < LHSR_CKSUM_BUCKETS) {
			entry = &arr->cksum_cache[arr->cksum_count];
			entry->offset = offset;
			entry->cksum = calc_csum;
			entry->flags = LHSR_BLOCK_VERIFIED;
			arr->cksum_count++;
			DMINFO("Scrub: Stored new checksum for offset 0x%llx", offset);
		} else {
			DMWARN("Scrub: Checksum cache full, cannot store checksum for offset 0x%llx", offset);
		}
		kfree(buf);
		return 0;
	}

	/* Verify checksum */
	if (calc_csum != stored_csum) {
		DMERR("Scrub: Checksum mismatch at offset 0x%llx (stored=0x%08x, calc=0x%08x)",
		       offset, stored_csum, calc_csum);
		if (entry) {
			entry->flags |= LHSR_BLOCK_CORRUPT;
			atomic_inc(&arr->corruptions_detected);
		}
		kfree(buf);
		return -EIO;
	}

	/* Checksum verified successfully */
	if (entry) {
		entry->flags |= LHSR_BLOCK_VERIFIED;
	}
	kfree(buf);
	return 0;
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

	/* Bail if array is being destroyed - prevent use-after-free */
	if (atomic_read(&arr->destroying)) {
		DMDEBUG("scrub_work: array being destroyed, skipping");
		return;
	}

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
	struct page **pages;
	unsigned int nr_pages;
	unsigned int i;  /* Iterator for page loops */
	unsigned int source_disk;
	u64 block_size = 128 * 1024;  /* 128KB chunks */
	sector_t offset;
	int ret;

	if (!arr || arr->rebuild_state != LHSR_REBUILD_RUNNING)
		return;

	/* Bail if array is being destroyed - prevent use-after-free */
	if (atomic_read(&arr->destroying)) {
		DMDEBUG("rebuild_work: array being destroyed, skipping");
		return;
	}

	if (!arr->rebuild_wq) {
		DMERR("Rebuild workqueue not initialized");
		arr->rebuild_state = LHSR_REBUILD_NONE;
		return;
	}

	if (arr->rebuild_disk >= arr->disks) {
		DMINFO("Rebuild complete: %llu sectors copied", arr->rebuild_verified);
		arr->rebuild_state = LHSR_REBUILD_COMPLETE;
		lhsr_update_disk_state(arr, arr->rebuild_disk, LHSR_DISK_HEALTHY);
		return;
	}

	if (!arr->disk[arr->rebuild_disk]) {
		DMERR("Rebuild: target disk %u not available", arr->rebuild_disk);
		arr->rebuild_state = LHSR_REBUILD_NONE;
		return;
	}

	/* Find source disk: iterate all disks to find a working one */
	source_disk = arr->disks; /* invalid sentinel */
	for (i = 0; i < arr->disks; i++) {
		if (i == arr->rebuild_disk)
			continue;
		if (!(arr->failed_disks & (1 << i)) && arr->disk[i]) {
			source_disk = i;
			break;
		}
	}

	if (source_disk >= arr->disks) {
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

	/* Allocate pages for I/O - block_size may be > PAGE_SIZE */
	nr_pages = (block_size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	pages = kcalloc(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}

	for (i = 0; i < nr_pages; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i]) {
			unsigned int j;
			for (j = 0; j < i; j++)
				__free_page(pages[j]);
			kfree(pages);
			queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
			return;
		}
	}

	/* Read from source disk */
	DMDEBUG("rebuild: reading from disk %u at sector %llu (offset=0x%llx)",
		source_disk, (u64)offset, (u64)offset << SECTOR_SHIFT);
	bio = bio_alloc_bioset(arr->disk[source_disk], nr_pages,
			     REQ_OP_READ, GFP_KERNEL, &lhsr_bioset);
	if (!bio) {
		DMERR("Rebuild: failed to allocate read bio");
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}
	bio_set_dev(bio, arr->disk[source_disk]);
	bio->bi_iter.bi_sector = offset + arr->disk_offset[source_disk];
	DMDEBUG("rebuild: bio sector=%llu", (u64)bio->bi_iter.bi_sector);
	for (i = 0; i < nr_pages; i++) {
		unsigned int page_bytes = (i == nr_pages - 1) ?
			block_size - (i << PAGE_SHIFT) : PAGE_SIZE;
		__bio_add_page(bio, pages[i], page_bytes, 0);
	}

	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret != 0) {
		DMERR("Rebuild read failed at offset 0x%llx, ret=%d", (u64)offset << SECTOR_SHIFT, ret);
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		arr->rebuild_offset += (block_size >> SECTOR_SHIFT);
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}

	/* Write to rebuild disk */
	bio = bio_alloc(arr->disk[arr->rebuild_disk], nr_pages, REQ_OP_WRITE, GFP_KERNEL);
	if (!bio) {
		DMERR("Rebuild: failed to allocate write bio");
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}
	bio_set_dev(bio, arr->disk[arr->rebuild_disk]);
	bio->bi_iter.bi_sector = offset + arr->disk_offset[arr->rebuild_disk];
	for (i = 0; i < nr_pages; i++) {
		unsigned int page_bytes = (i == nr_pages - 1) ?
			block_size - (i << PAGE_SHIFT) : PAGE_SIZE;
		__bio_add_page(bio, pages[i], page_bytes, 0);
	}

	ret = submit_bio_wait(bio);
	bio_put(bio);

	/* Free pages */
	for (i = 0; i < nr_pages; i++)
		__free_page(pages[i]);
	kfree(pages);

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

	/* Find source disk: iterate all disks to find a working one */
	{
		unsigned int k;
		arr->primary_disk = arr->disks; /* invalid sentinel */
		for (k = 0; k < arr->disks; k++) {
			if (k == disk_idx)
				continue;
			if (!(arr->failed_disks & (1 << k))) {
				arr->primary_disk = k;
				break;
			}
		}
		if (arr->primary_disk >= arr->disks) {
			DMERR("Rebuild start: no source disk available");
			return -EINVAL;
		}
	}

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

	/* Prevent new devices during module exit */
	if (lhsr_module_exiting) {
		DMERR("ctr: Module is exiting, refusing new device");
		ti->error = "Module is unloading";
		return -EBUSY;
	}

	DMINFO("ctr: argc=%u", argc);
	for (i = 0; i < argc; i++)
		DMINFO("ctr: argv[%u]=[%s]", i, argv[i]);

	/* Validate argument count based on raid type */
	/* Format: type [device offset] [device offset] ... */
	/* Minimum: type + 1 device + 1 offset = 3 args */
	if (argc < 3) {
		DMERR("Need at least 3 args (type device offset), got %u", argc);
		ti->error = "Invalid arguments (need: type device offset [device offset] ...)";
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

	/* Arguments: type [device offset] [device offset] ... */
	/* So for N disks: argc = 1 + N*2, num_disks = (argc - 1) / 2 */
	/* Special case: "single" has 1 disk but argc=2 (type + device), so num_disks=1 */
	if (strcmp(argv[0], "single") == 0) {
		num_disks = 1;
	} else {
		num_disks = (argc - 1) / 2;
	}

	if (raid_type == 1 && num_disks != 2) {
		DMERR("Mirror requires exactly 2 disks, got %u", num_disks);
		ti->error = "Mirror requires exactly 2 disks";
		/* arr not yet allocated - kfree(NULL) is safe but don't set bad precedent */
		return -EINVAL;
	}
	if (raid_type >= 2 && num_disks < 3) {
		DMERR("RAID5/6 requires at least 3 disks, got %u", num_disks);
		ti->error = "RAID5/6 requires at least 3 disks";
		/* arr not yet allocated */
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

	/* Track active device */
	atomic_inc(&lhsr_active_devices);
	DMINFO("ctr: Active devices now: %d", atomic_read(&lhsr_active_devices));

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

	/* Initialize RAID5 write completion tracking */
	atomic_set(&arr->inflight_writes, 0);
	arr->orig_bio = NULL;
	arr->data_disks_written = 0;

	/* Initialize concurrency primitives */
	mutex_init(&arr->io_mutex);
	init_rwsem(&arr->sb_sem);

	/* Initialize CRC table if not already done */
	/* CRC32c uses kernel crypto API - no initialization needed */

	/* Get devices - arguments are "device offset" pairs */
	/* argv[0] = type, then argv[1], argv[2] = dev1, offset1, etc. */
	DMINFO("ctr: num_disks=%u, argc=%u", num_disks, argc);
	for (i = 0; i < num_disks; i++) {
		const char *dev_name = argv[1 + i * 2];
		sector_t offset = simple_strtoull(argv[2 + i * 2], NULL, 0);
		DMINFO("ctr: Getting device %u: argv[%d]='%s', offset=%llu",
		       i, 1 + i * 2, dev_name, (u64)offset);
		r = dm_get_device(ti, dev_name, FMODE_READ | FMODE_WRITE, &dm_dev);
		if (r) {
			DMERR("ctr: Cannot get device '%s': %d", dev_name, r);
			ti->error = "Failed to get device";
			goto bad;
		}
		arr->dm_devs[i] = dm_dev;
		arr->disk[i] = dm_dev->bdev;
		arr->disk_offset[i] = offset;
		DMINFO("Added device %u: %s (offset %llu)", i, dev_name, (u64)offset);
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
		r = lhsr_read_superblock(arr->disk[i], sb, arr->disk_offset[i]);
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
			r = lhsr_write_superblock(arr->disk[i], sb, size, arr->disk_offset[i]);
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
	arr->cksum_count = 0;  /* Initialize checksum cache */
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

	/* Limit bios to PAGE_SIZE for RAID5/6 so single-page XOR works */
	if (arr->raid_type >= 2)
		ti->max_io_len = PAGE_SIZE >> SECTOR_SHIFT;

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
	/* Decrement active device count on error */
	atomic_dec(&lhsr_active_devices);
	DMINFO("ctr: Active devices now (error): %d", atomic_read(&lhsr_active_devices));
	kfree(arr);
	return r;
}

/* Target destructor */
static void lhsr_dtr(struct dm_target *ti)
{
	struct lhsr_array *arr = ti->private;
	unsigned int i;
	int r;

	DMINFO("dtr: START - arr=%p", arr);

	if (!arr) {
		DMINFO("dtr: arr is NULL, returning");
		return;
	}

	/* Decrement active device count */
	atomic_dec(&lhsr_active_devices);
	DMINFO("dtr: Active devices now: %d", atomic_read(&lhsr_active_devices));

	/* Mark array as destroying FIRST to prevent workqueue races */
	DMINFO("dtr: Setting destroying flag");
	atomic_set(&arr->destroying, 1);

	DMINFO("dtr: Stopping health check workqueue (check_wq=%p)", arr->check_wq);

	/* Stop the health check workqueue with timeout */
	if (arr->check_wq) {
		DMINFO("dtr: Canceling check workqueue...");
		if (!cancel_delayed_work_sync(&arr->check_work)) {
			DMWARN("dtr: check_work did not complete in time, forcing");
			flush_workqueue(arr->check_wq);
		}
		destroy_workqueue(arr->check_wq);
		arr->check_wq = NULL;
		DMINFO("dtr: check workqueue destroyed");
	}

	DMINFO("dtr: Writing superblocks for %u disks", arr->disks);

	/* Write updated superblocks for all disks before destroying */
	arr->generation++;
	for (i = 0; i < arr->disks; i++) {
		struct lhsr_superblock *sb = &arr->sbs[i];

		DMINFO("dtr: Processing disk %u", i);
		sb->last_update = ktime_get_real_seconds();
		sb->generation = arr->generation;
		sb->disk_state = (arr->failed_disks & (1 << i)) ? LHSR_DISK_DEGRADED : LHSR_DISK_HEALTHY;

		DMINFO("dtr: Writing superblock for disk %u", i);
		r = lhsr_write_superblock(arr->disk[i], sb, arr->size, arr->disk_offset[i]);
		if (r)
			DMERR("dtr: Failed to persist superblock for disk %u: %d", i, r);
		else
			DMINFO("dtr: Persisted state for disk %u (gen=%llu)", i, sb->generation);
	}

	DMINFO("dtr: Stopping scrubber (scrub_wq=%p)", arr->scrub_wq);

	/* Stop scrubber */
	if (arr->scrub_wq) {
		DMINFO("dtr: Canceling scrub workqueue...");
		arr->scrub_state = LHSR_SCRUB_IDLE;
		if (!cancel_delayed_work_sync(&arr->scrub_work)) {
			DMWARN("dtr: scrub_work did not complete in time, forcing");
			flush_workqueue(arr->scrub_wq);
		}
		destroy_workqueue(arr->scrub_wq);
		arr->scrub_wq = NULL;
		DMINFO("dtr: Scrubber stopped");
	}

	DMINFO("dtr: Stopping rebuild (rebuild_wq=%p)", arr->rebuild_wq);

	/* Stop rebuild */
	if (arr->rebuild_wq) {
		DMINFO("dtr: Canceling rebuild workqueue...");
		arr->rebuild_state = LHSR_REBUILD_NONE;
		if (!cancel_delayed_work_sync(&arr->rebuild_work)) {
			DMWARN("dtr: rebuild_work did not complete in time, forcing");
			flush_workqueue(arr->rebuild_wq);
		}
		destroy_workqueue(arr->rebuild_wq);
		arr->rebuild_wq = NULL;
		DMINFO("dtr: Rebuild stopped");
	}

	DMINFO("dtr: Putting devices...");

	for (i = 0; i < arr->disks; i++) {
		DMINFO("dtr: Putting device %u (dm_devs[%u]=%p)", i, i, arr->dm_devs[i]);
		if (arr->dm_devs[i]) {
			dm_put_device(ti, arr->dm_devs[i]);
			DMINFO("dtr: Device %u released", i);
		}
	}

	DMINFO("dtr: Destroying mutex");

	/* Destroy concurrency primitives */
	mutex_destroy(&arr->io_mutex);

	DMINFO("dtr: Freeing arr %p", arr);
	kfree(arr);
	ti->private = NULL;
	DMINFO("dtr: END - Destroyed LHSR target");
}

/* XOR parity calculation for RAID5 */
static void lhsr_xor_parity(void *parity, void **data, unsigned int data_disks, size_t len)
{
	u8 *p = parity;
	u8 **src = (u8 **)data;
	unsigned int i, j;
	size_t long_words = len / sizeof(long);
	size_t rem_bytes = len % sizeof(long);

	/* Initialize parity with first data block */
	memcpy(p, src[0], len);

	/* XOR remaining data blocks using long words */
	for (i = 1; i < data_disks; i++) {
		long *p_long = (long *)p;
		long *s_long = (long *)src[i];
		for (j = 0; j < long_words; j++)
			p_long[j] ^= s_long[j];
	}

	/* Handle remaining bytes */
	if (rem_bytes) {
		size_t offset = long_words * sizeof(long);
		for (i = 1; i < data_disks; i++) {
			for (j = 0; j < rem_bytes; j++)
				p[offset + j] ^= src[i][offset + j];
		}
	}
}

/* RAID5/6 write completion callbacks */
static void lhsr_raid_5_data_endio(struct bio *bio)
{
	struct lhsr_raid_5_write_ctx *ctx = bio->bi_private;
	struct lhsr_array *arr = ctx->arr;
	struct bio_vec bv;
	struct bvec_iter iter;
	void *data_buf;
	unsigned int data_idx, i;
	int is_raid6 = (arr->raid_type == 3);
	int parity_disks = is_raid6 ? 2 : 1;
	unsigned int p_parity_idx = arr->disks - parity_disks;
	unsigned int q_parity_idx = arr->disks - 1;

	if (bio->bi_status)
		ctx->status = bio->bi_status;

	/*
	 * Find which data disk this bio belongs to by matching bdev.
	 * Use the RAW disk index directly into data_bufs[] — NO compacted
	 * disk_map.  Entries for failed/bio-alloc-failed disks remain NULL
	 * and are skipped during parity computation below.
	 */
	data_idx = ctx->num_disks;	/* Invalid sentinel */
	for (i = 0; i < ctx->num_disks; i++) {
		if (bio->bi_bdev == arr->disk[i]) {
			data_idx = i;
			break;
		}
	}

	/* Populate data_bufs from this completed bio's data */
	if (data_idx < ctx->num_disks) {
		bio_for_each_segment(bv, bio, iter) {
			data_buf = kmap(bv.bv_page) + bv.bv_offset;
			ctx->data_bufs[data_idx] = data_buf;
			ctx->data_pages[data_idx] = bv.bv_page;
			break; /* Single segment for now */
		}
	}

	if (atomic_dec_and_test(&ctx->pending)) {
		/* All data writes done — compute parity */
		size_t bio_size = ctx->bio_size;
		void *xor_inputs[32];
		unsigned int xor_count = 0;
		int parity_slots = 0;
		bool p_can_write, q_can_write;

		/*
		 * Build compacted array of non-NULL data buffers for XOR.
		 * data_bufs[] is indexed by raw disk index; entries for
		 * disks whose clone allocation failed remain NULL.
		 * We skip those to avoid Oops in lhsr_xor_parity().
		 */
		for (i = 0; i < ctx->num_disks; i++) {
			if (ctx->data_bufs[i])
				xor_inputs[xor_count++] = ctx->data_bufs[i];
		}

		/* Compute P parity (XOR for both RAID5 and RAID6) */
		if (xor_count > 0)
			lhsr_xor_parity(ctx->parity_buf, xor_inputs,
					xor_count, bio_size);

		/* For RAID6, also compute Q parity */
		if (is_raid6 && ctx->parity_q_buf && xor_count > 0)
			lhsr_rs_parity(ctx->parity_buf, ctx->parity_q_buf,
				       xor_inputs, xor_count, bio_size);

		/* Unmap all data buffers */
		for (i = 0; i < ctx->num_disks; i++) {
			if (ctx->data_bufs[i] && ctx->data_pages[i]) {
				kunmap(ctx->data_pages[i]);
				ctx->data_bufs[i] = NULL;
			}
		}

		/*
		 * Determine which parity disks are available (single read of
		 * failed_disks — snapshot for the entire submit section).
		 */
		p_can_write = !(arr->failed_disks & (1 << p_parity_idx));
		q_can_write = is_raid6 && ctx->parity_q_buf &&
			      !(arr->failed_disks & (1 << q_parity_idx));

		/*
		 * Pre-increment pending counter for ALL parity writes BEFORE
		 * any submit_bio().  Prevents synchronous completion from
		 * reaching counter zero and freeing ctx while we still need
		 * it for subsequent submits.
		 */
		if (p_can_write) {
			atomic_inc(&ctx->pending);
			parity_slots++;
		}
		if (q_can_write) {
			atomic_inc(&ctx->pending);
			parity_slots++;
		}

		/* Write P parity */
		if (p_can_write) {
			struct bio *parity_bio;

			parity_bio = bio_alloc_bioset(arr->disk[p_parity_idx], 1,
						      REQ_OP_WRITE, GFP_NOIO, &lhsr_bioset);
			if (!parity_bio) {
				ctx->status = BLK_STS_RESOURCE;
				atomic_dec(&ctx->pending);
				parity_slots--;
			} else if (bio_add_page(parity_bio, ctx->parity_page,
						bio_size, 0) < bio_size) {
				bio_put(parity_bio);
				ctx->status = BLK_STS_RESOURCE;
				atomic_dec(&ctx->pending);
				parity_slots--;
			} else {
				parity_bio->bi_iter.bi_sector = ctx->offset + arr->disk_offset[p_parity_idx];
				parity_bio->bi_end_io = lhsr_raid_5_parity_endio;
				parity_bio->bi_private = ctx;
				ctx->parity_bio = parity_bio;
				submit_bio(parity_bio);
				/*
				 * submit_bio() may complete synchronously on
				 * loop/ram devices.  If so, parity_endio already
				 * decremented pending.  ctx is still alive because
				 * parity_slots > 0 means at least one reservation
				 * hasn't been consumed yet (or it was 1 and the
				 * one submit consumed it — but parity_endio then
				 * frees ctx so we MUST NOT touch ctx after this).
				 */
			}
		}

		/* For RAID6, write Q parity */
		if (q_can_write) {
			struct bio *q_parity_bio;

			q_parity_bio = bio_alloc_bioset(arr->disk[q_parity_idx], 1,
							REQ_OP_WRITE, GFP_NOIO, &lhsr_bioset);
			if (!q_parity_bio) {
				atomic_dec(&ctx->pending);
				parity_slots--;
				ctx->status = BLK_STS_RESOURCE;
			} else if (bio_add_page(q_parity_bio, ctx->parity_q_page,
						bio_size, 0) < bio_size) {
				bio_put(q_parity_bio);
				atomic_dec(&ctx->pending);
				parity_slots--;
				ctx->status = BLK_STS_RESOURCE;
			} else {
				q_parity_bio->bi_iter.bi_sector = ctx->offset + arr->disk_offset[q_parity_idx];
				q_parity_bio->bi_end_io = lhsr_raid_5_parity_endio;
				q_parity_bio->bi_private = ctx;
				ctx->parity_q_bio = q_parity_bio;
				submit_bio(q_parity_bio);
			}
		}

		/*
		 * If NO parity bio was submitted, complete the original bio HERE.
		 * parity_endio was never called (all reservations released), so
		 * it is safe to touch ctx.
		 */
		if (parity_slots == 0) {
			ctx->orig_bio->bi_status = ctx->status;
			bio_endio(ctx->orig_bio);
			if (ctx->parity_q_page)
				__free_page(ctx->parity_q_page);
			__free_page(ctx->parity_page);
			kfree(ctx->data_bufs);
			kfree(ctx->data_pages);
			kfree(ctx);
		}
	}

	bio_put(bio);
}

static void lhsr_raid_5_parity_endio(struct bio *bio)
{
	struct lhsr_raid_5_write_ctx *ctx = bio->bi_private;

	if (bio->bi_status)
		ctx->status = bio->bi_status;

	/* Use atomic counter: when it reaches 0, all parity writes done */
	if (atomic_dec_and_test(&ctx->pending)) {
		/* All done - complete original bio */
		ctx->orig_bio->bi_status = ctx->status;
		bio_endio(ctx->orig_bio);
		if (ctx->parity_q_page)
			__free_page(ctx->parity_q_page);
		__free_page(ctx->parity_page);
		if (ctx->data_bufs)
			kfree(ctx->data_bufs);
		if (ctx->data_pages)
			kfree(ctx->data_pages);
		kfree(ctx);
	}

	bio_put(bio);
}

/* Map function - with error tracking */
static int lhsr_map(struct dm_target *ti, struct bio *bio)
{
	struct lhsr_array *arr = ti->private;
	sector_t offset;

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
		bio->bi_iter.bi_sector = offset + arr->disk_offset[0];
		if (lhsr_setup_io_tracking(bio, ti) < 0) {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		submit_bio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* RAID1 Mirror - write to primary only */
	if (arr->raid_type == 1) {
		if (bio_op(bio) != REQ_OP_READ) {
			unsigned int target_disk = arr->primary_disk;
		
			if (!(arr->failed_disks & (1 << target_disk))) {
				bio_set_dev(bio, arr->disk[target_disk]);
				bio->bi_iter.bi_sector = offset + arr->disk_offset[target_disk];
				if (lhsr_setup_io_tracking(bio, ti) < 0) {
					bio->bi_status = BLK_STS_IOERR;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
				submit_bio(bio);
			} else {
				/* Primary failed, try other */
				unsigned int other = (target_disk == 0) ? 1 : 0;
				if (!(arr->failed_disks & (1 << other))) {
					bio_set_dev(bio, arr->disk[other]);
					bio->bi_iter.bi_sector = offset + arr->disk_offset[other];
					if (lhsr_setup_io_tracking(bio, ti) < 0) {
						bio->bi_status = BLK_STS_IOERR;
						bio_endio(bio);
						return DM_MAPIO_SUBMITTED;
					}
					submit_bio(bio);
				} else {
					bio->bi_status = BLK_STS_IOERR;
					bio_endio(bio);
				}
			}
			return DM_MAPIO_SUBMITTED;
		}
	}

	/* RAID5/6 handling */
	if (arr->raid_type >= 2) {
		unsigned int data_disks;
		unsigned int parity_disks = (arr->raid_type == 2) ? 1 : 2;
		unsigned int i;
		int is_write = (bio_op(bio) != REQ_OP_READ);

		data_disks = arr->disks - parity_disks;

		if (is_write) {
			unsigned int working_disks = 0;
			struct lhsr_raid_5_write_ctx *ctx;
			struct bio *clone;
			size_t bio_size = bio->bi_iter.bi_size;

			DMDEBUG("RAID5/6 write: %u data disks, %u parity disks",
			       data_disks, parity_disks);

			/* Count working data disks */
			for (i = 0; i < data_disks; i++) {
				if (!(arr->failed_disks & (1 << i)))
					working_disks++;
			}

			if (working_disks == 0) {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

		/*
		 * Allocate write context.
		 * data_bufs/data_pages are indexed by RAW data-disk index
		 * (0..data_disks-1).  No disk_map needed.
		 */
		ctx = kzalloc(sizeof(*ctx), GFP_NOIO);
		if (!ctx) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		ctx->orig_bio = bio;
		ctx->arr = arr;
		ctx->num_disks = data_disks;
		ctx->working = 0;	/* Will be set to submitted count after loop */
		ctx->offset = offset;
		ctx->bio_size = bio_size;
		ctx->status = 0;

		/*
		 * Set pending to working_disks BEFORE the submit loop.
		 * Each successful submit gets a data_endio that decrements.
		 * Each failed allocation decrements directly.
		 */
		atomic_set(&ctx->pending, working_disks);

		/* Allocate parity buffers */
		ctx->parity_page = alloc_page(GFP_NOIO);
		if (!ctx->parity_page) {
			kfree(ctx);
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		ctx->parity_buf = page_address(ctx->parity_page);
		memset(ctx->parity_buf, 0, bio_size);

		/* For RAID6, also allocate Q parity buffer */
		if (arr->raid_type == 3) {
			ctx->parity_q_page = alloc_page(GFP_NOIO);
			if (!ctx->parity_q_page) {
				__free_page(ctx->parity_page);
				kfree(ctx);
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
			ctx->parity_q_buf = page_address(ctx->parity_q_page);
			memset(ctx->parity_q_buf, 0, bio_size);
		}

		/*
		 * Allocate data buffers indexed by RAW data-disk index.
		 * Entries for failed disks or failed clones stay NULL.
		 */
		ctx->data_bufs = kzalloc(sizeof(void *) * data_disks, GFP_NOIO);
		if (!ctx->data_bufs) {
			if (ctx->parity_q_page)
				__free_page(ctx->parity_q_page);
			__free_page(ctx->parity_page);
			kfree(ctx);
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		ctx->data_pages = kzalloc(sizeof(struct page *) * data_disks, GFP_NOIO);
		if (!ctx->data_pages) {
			kfree(ctx->data_bufs);
			if (ctx->parity_q_page)
				__free_page(ctx->parity_q_page);
			__free_page(ctx->parity_page);
			kfree(ctx);
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		/*
		 * Clone bio for each working data disk and submit.
		 * We use the RAW disk index directly — no compaction.
		 * data_bufs[i] is filled by data_endio when that disk's
		 * clone completes (or stays NULL if clone_alloc failed).
		 */
			{
				unsigned int submitted = 0;
				for (i = 0; i < data_disks; i++) {
					if (arr->failed_disks & (1 << i))
						continue;

					clone = bio_alloc_clone(arr->disk[i], bio,
								GFP_NOIO, &lhsr_bioset);
					if (!clone) {
						ctx->status = BLK_STS_RESOURCE;
						atomic_dec(&ctx->pending);
						continue;
					}

					clone->bi_iter.bi_sector = offset + arr->disk_offset[i];
					clone->bi_end_io = lhsr_raid_5_data_endio;
					clone->bi_private = ctx;

					if (lhsr_setup_io_tracking(clone, ti) < 0) {
						bio_put(clone);
						ctx->status = BLK_STS_IOERR;
						atomic_dec(&ctx->pending);
						continue;
					}

					submit_bio(clone);
					/*
					 * submit_bio() may complete SYNCHRONOUSLY on
					 * loop/ram devices.  data_endio fires here,
					 * decrementing pending and possibly filling
					 * ctx->data_bufs[i].  That's safe because
					 * data_bufs[i] is indexed by raw disk index i,
					 * NOT by a compacted working index.
					 */
					submitted++;
				}
				ctx->working = submitted;

				/* If no writes were submitted, fail */
				if (submitted == 0) {
					bio->bi_status = ctx->status ? ctx->status : BLK_STS_RESOURCE;
					bio_endio(bio);
					if (ctx->parity_q_page)
						__free_page(ctx->parity_q_page);
					__free_page(ctx->parity_page);
					kfree(ctx->data_bufs);
					kfree(ctx->data_pages);
					kfree(ctx);
					return DM_MAPIO_SUBMITTED;
				}

				/*
				 * Orphaned completion: if pending reached 0 during the
				 * submit loop (all data_endios and error decrements
				 * exhausted synchronously), then data_endio's "all
				 * done" path did NOT trigger because the parity_prep
				 * code within it computed parity using the old
				 * working_disks count.
				 *
				 * Now that ctx->working == submitted, we compute
				 * parity and write it synchronously.  This prevents
				 * silent data corruption (stripe with data but no
				 * parity).
				 */
				if (atomic_read(&ctx->pending) == 0) {
					void *xor_inputs[32];
					unsigned int xor_count = 0;
					unsigned int k;
					unsigned int p_parity = arr->disks - parity_disks;
					unsigned int q_parity = arr->disks - 1;

					/* Build compacted array of non-NULL buffers */
					for (k = 0; k < data_disks; k++) {
						if (ctx->data_bufs[k])
							xor_inputs[xor_count++] =
								ctx->data_bufs[k];
					}

					/* Compute P parity */
					if (xor_count > 0)
						lhsr_xor_parity(ctx->parity_buf,
								xor_inputs,
								xor_count, bio_size);

					/* Compute Q parity for RAID6 */
					if (arr->raid_type == 3 && ctx->parity_q_buf
					    && xor_count > 0)
						lhsr_rs_parity(ctx->parity_buf,
							       ctx->parity_q_buf,
							       xor_inputs,
							       xor_count, bio_size);

					/* Write P parity synchronously */
					if (!(arr->failed_disks & (1 << p_parity))
					    && ctx->parity_page) {
						struct bio *pbio = bio_alloc_bioset(
							arr->disk[p_parity], 1,
							REQ_OP_WRITE, GFP_NOIO,
							&lhsr_bioset);
						if (pbio) {
							if (bio_add_page(pbio,
							    ctx->parity_page,
							    bio_size, 0) >= bio_size) {
								pbio->bi_iter.bi_sector =
									offset + arr->disk_offset[p_parity];
								submit_bio_wait(pbio);
							}
							bio_put(pbio);
						}
					}

					/* Write Q parity synchronously (RAID6) */
					if (arr->raid_type == 3
					    && !(arr->failed_disks & (1 << q_parity))
					    && ctx->parity_q_page) {
						struct bio *qbio = bio_alloc_bioset(
							arr->disk[q_parity], 1,
							REQ_OP_WRITE, GFP_NOIO,
							&lhsr_bioset);
						if (qbio) {
							if (bio_add_page(qbio,
							    ctx->parity_q_page,
							    bio_size, 0) >= bio_size) {
								qbio->bi_iter.bi_sector =
									offset + arr->disk_offset[q_parity];
								submit_bio_wait(qbio);
							}
							bio_put(qbio);
						}
					}

					/* Complete original bio */
					ctx->orig_bio->bi_status = ctx->status;
					bio_endio(ctx->orig_bio);
					for (k = 0; k < data_disks; k++) {
						if (ctx->data_bufs[k] && ctx->data_pages[k])
							kunmap(ctx->data_pages[k]);
					}
					if (ctx->parity_q_page)
						__free_page(ctx->parity_q_page);
					__free_page(ctx->parity_page);
					kfree(ctx->data_bufs);
					kfree(ctx->data_pages);
					kfree(ctx);
					return DM_MAPIO_SUBMITTED;
				}
				/*
				 * Normal async path: pending > 0, meaning data_endio
				 * callbacks (and possibly parity_endio) will handle
				 * everything including cleanup.  DO NOT touch ctx.
				 */
			}

			return DM_MAPIO_SUBMITTED;
		}

	/* RAID5/6 READ: check for failed disk, set up reconstruction if needed */
	{
		unsigned int target_disk = 0;
		int target_failed = 0;
		
		/* Find first working disk */
		for (i = 0; i < data_disks; i++) {
			if (!(arr->failed_disks & (1 << i))) {
				target_disk = i;
				break;
			}
		}
		
		/* Check if target disk failed */
		target_failed = (arr->failed_disks & (1 << target_disk));
		
		if (target_failed) {
			/* Degraded mode - set up reconstruction */
			struct lhsr_raid_5_read_ctx *ctx;
			unsigned int working_disks = 0;
			unsigned int total_slots = 0;
			unsigned int j;
			
			/* Count working data disks */
			for (i = 0; i < data_disks; i++) {
				if (!(arr->failed_disks & (1 << i)))
					working_disks++;
			}
			
			if (working_disks == 0) {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
			
			/* Count total survivors: working data + surviving parity */
			total_slots = working_disks;
			for (j = 0; j < parity_disks; j++) {
				if (!(arr->failed_disks & (1 << (data_disks + j))))
					total_slots++;
			}
			
			/* Allocate read context with flex array sized for total_slots */
			ctx = kzalloc(sizeof(*ctx) + (sizeof(unsigned int) * total_slots), GFP_NOIO);
			if (!ctx) {
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
			
			ctx->orig_bio = bio;
			ctx->arr = arr;
			ctx->target_disk = target_disk;
			ctx->num_disks = data_disks;
			ctx->working = working_disks;
			ctx->num_slots = total_slots;
			ctx->offset = offset;
			ctx->bio_size = bio->bi_iter.bi_size;
			atomic_set(&ctx->pending, total_slots);
			ctx->status = 0;
			
			/* Allocate reconstruction buffer */
			ctx->recon_buf = kzalloc(bio->bi_iter.bi_size, GFP_NOIO);
			if (!ctx->recon_buf) {
				kfree(ctx);
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
			
			/* Allocate data+parity buffers array — one per surviving disk */
			ctx->data_bufs = kzalloc(sizeof(void *) * total_slots, GFP_NOIO);
			if (!ctx->data_bufs) {
				kfree(ctx->recon_buf);
				kfree(ctx);
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
			
			/* Build disk_map: slot_idx -> disk_idx (data first, then parity) */
			j = 0;
			for (i = 0; i < data_disks; i++) {
				if (!(arr->failed_disks & (1 << i))) {
					ctx->disk_map[j] = i;
					j++;
				}
			}
			for (i = 0; i < parity_disks; i++) {
				unsigned int parity_disk = data_disks + i;
				if (!(arr->failed_disks & (1 << parity_disk))) {
					ctx->disk_map[j] = parity_disk;
					j++;
				}
			}
			
			/* Submit reads to ALL surviving disks (data + parity) in one loop */
			for (i = 0; i < ctx->num_slots; i++) {
				struct bio *clone;
				unsigned int disk_idx = ctx->disk_map[i];
				
				clone = bio_alloc_clone(arr->disk[disk_idx], 
							bio, GFP_NOIO, &lhsr_bioset);
				if (!clone) {
					ctx->status = BLK_STS_RESOURCE;
					atomic_dec(&ctx->pending);
					continue;
				}
				ctx->data_bufs[i] = kzalloc(bio->bi_iter.bi_size, GFP_NOIO);
				if (!ctx->data_bufs[i]) {
					bio_put(clone);
					ctx->status = BLK_STS_RESOURCE;
					atomic_dec(&ctx->pending);
					continue;
				}
				clone->bi_iter.bi_sector = offset + arr->disk_offset[disk_idx];
				clone->bi_end_io = lhsr_raid_5_read_endio;
				clone->bi_private = ctx;
				submit_bio(clone);
			}
			
			/* If no reads submitted (all allocs failed), fail immediately */
			if (atomic_read(&ctx->pending) == 0) {
				bio->bi_status = ctx->status ? ctx->status : BLK_STS_RESOURCE;
				bio_endio(bio);
				kfree(ctx->recon_buf);
				if (ctx->data_bufs) {
					for (i = 0; i < total_slots; i++) {
						if (ctx->data_bufs[i])
							kfree(ctx->data_bufs[i]);
					}
					kfree(ctx->data_bufs);
				}
				kfree(ctx);
				return DM_MAPIO_SUBMITTED;
			}

			return DM_MAPIO_SUBMITTED;
		} else {
			/* Normal read from working disk */
			bio_set_dev(bio, arr->disk[target_disk]);
			bio->bi_iter.bi_sector = offset + arr->disk_offset[target_disk];
			if (lhsr_setup_io_tracking(bio, ti) < 0) {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
			submit_bio(bio);
			return DM_MAPIO_SUBMITTED;
		}
	}
	}

	/* Read: try primary, failover to any working disk */
	if (!(arr->failed_disks & (1 << arr->primary_disk))) {
		bio_set_dev(bio, arr->disk[arr->primary_disk]);
		bio->bi_iter.bi_sector = offset + arr->disk_offset[arr->primary_disk];
		if (lhsr_setup_io_tracking(bio, ti) < 0) {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		submit_bio(bio);
	} else {
		/* Primary failed - scan all disks for working one */
		unsigned int i;
		int found = 0;

		for (i = 0; i < arr->disks; i++) {
			if (i == arr->primary_disk)
				continue;
			if (!(arr->failed_disks & (1 << i))) {
				DMINFO("Failover read from disk %u to disk %u",
				       arr->primary_disk, i);
				bio_set_dev(bio, arr->disk[i]);
				bio->bi_iter.bi_sector = offset + arr->disk_offset[i];
				if (lhsr_setup_io_tracking(bio, ti) < 0) {
					bio->bi_status = BLK_STS_IOERR;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
				atomic_inc(&arr->failovers);
				submit_bio(bio);
				found = 1;
				break;
			}
		}

		if (!found) {
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

	r = lhsr_write_superblock(arr->disk[disk_idx], sb, arr->size, arr->disk_offset[disk_idx]);
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
		/* Use write semaphore for atomic update of failed_disks */
		down_write(&arr->sb_sem);
		arr->failed_disks |= (1 << disk_idx);
		arr->disk_errors[disk_idx] = arr->error_threshold;
		up_write(&arr->sb_sem);
		lhsr_update_disk_state(arr, disk_idx, LHSR_DISK_DEGRADED);

		if (arr->primary_disk == disk_idx) {
			unsigned int k;
			arr->primary_disk = arr->disks; /* invalid */
			for (k = 0; k < arr->disks; k++) {
				if (k == disk_idx) continue;
				if (!(arr->failed_disks & (1 << k))) {
					arr->primary_disk = k;
					break;
				}
			}
			if (arr->primary_disk < arr->disks)
				DMINFO("Primary disk failed, failover to disk %u", arr->primary_disk);
			else
				DMERR("No working disk for failover");
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
		/* Use write semaphore for atomic update of failed_disks */
		down_write(&arr->sb_sem);
		arr->failed_disks &= ~(1 << disk_idx);
		arr->disk_errors[disk_idx] = 0;
		up_write(&arr->sb_sem);
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

/* Forward declaration */
static void __init lhsr_rs_table_init(void);
static int __init lhsr_init(void)
{
	int r;

	r = bioset_init(&lhsr_bioset, 4, 0, BIOSET_NEED_BVECS);
	if (r) {
		DMERR("Failed to initialize bioset: %d", r);
		return r;
	}

	/* Initialize Reed-Solomon table for RAID6 */
	lhsr_rs_table_init();

	r = dm_register_target(&lhsr_target);
	if (r) {
		DMERR("Failed to register LHSR target: %d", r);
		bioset_exit(&lhsr_bioset);
		return r;
	}

	DMINFO("Module loaded: %s (target registered)", LHSR_VERSION);
	return 0;
}

/* Module cleanup */
static void __exit lhsr_exit(void)
{
	int waited = 0;
	const int max_wait = 30; /* Maximum 30 seconds */

	DMINFO("Module unload: Setting exiting flag");
	lhsr_module_exiting = 1;

	/* Wait for active devices to drain */
	DMINFO("Module unload: Waiting for active devices (current: %d)",
	       atomic_read(&lhsr_active_devices));
	while (atomic_read(&lhsr_active_devices) > 0 && waited < max_wait) {
		DMWARN("Module unload: Waiting for %d active device(s)...",
		       atomic_read(&lhsr_active_devices));
		msleep(1000);
		waited++;
	}

	if (atomic_read(&lhsr_active_devices) > 0) {
		DMERR("Module unload: Timed out waiting for %d active device(s)! Forcing unload.",
		      atomic_read(&lhsr_active_devices));
	}

	/* Now safe to unregister and cleanup */
	/* rcu_barrier() required before dm_unregister_target() in 6.12+ kernels
	 * to ensure any pending RCU callbacks (from DM/block layer) complete */
	rcu_barrier();
	/* In kernel 6.12+, dm_unregister_target() returns void and uses BUG() on failure */
	dm_unregister_target(&lhsr_target);
	bioset_exit(&lhsr_bioset);
	DMINFO("Module unloaded");
}

/*
 * Reed-Solomon P+Q parity for RAID6
 * P = XOR of all data blocks (same as RAID5)
 * Q = sum of (coeff[i] * data[i]) where coeff[i] = 2^i precomputed in GF(2^8)
 *
 * If parity_q is NULL, only P is computed (safe for callers that only
 * need P parity via this function).
 */
static void lhsr_rs_parity(void *parity_p, void *parity_q, void **data,
                           unsigned int data_disks, size_t len)
{
	u8 *p = parity_p;
	u8 *q = parity_q;
	u8 **src = (u8 **)data;
	unsigned int i, j;

	memset(p, 0, len);
	if (q)
		memset(q, 0, len);

	/* For each data block, compute P and Q */
	for (i = 0; i < data_disks; i++) {
		u8 coeff = rs_power_table[i];	/* 2^i precomputed */
		u8 *s = src[i];

		/* P = XOR (same as RAID5) */
		for (j = 0; j < len; j++)
			p[j] ^= s[j];

		/* Q = sum of (coeff * data[i]) in GF(2^8) */
		if (q) {
			for (j = 0; j < len; j++) {
				u8 val = s[j];
				u8 result = 0;
				u8 high_bit;
				u8 b = coeff;

				/* GF(2^8) multiplication using Russian peasant */
				while (b) {
					if (b & 1)
						result ^= val;
					high_bit = val & 0x80;
					val = (val << 1) ^ (high_bit ? 0x1d : 0);
					b >>= 1;
				}
				q[j] ^= result;
			}
		}
	}
}

/* Initialize Reed-Solomon power table: rs_power_table[i] = 2^i in GF(2^8) */
static void __init lhsr_rs_table_init(void)
{
	unsigned int i;
	rs_power_table[0] = 1;
	for (i = 1; i < 256; i++)
		rs_power_table[i] = (rs_power_table[i-1] << 1) ^
			(rs_power_table[i-1] & 0x80 ? 0x1d : 0);
}

module_init(lhsr_init);
module_exit(lhsr_exit);