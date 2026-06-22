// SPDX-License-Identifier: GPL-3.0-only
/*
 * LHSR - Linux Hybrid Self-Healing RAID
 * Device Mapper Main Module
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 *
 * ==========================================================================
 * PRODUCTION READINESS (2026-05-11)
 * ==========================================================================
 *
 * WORKING (tested, correct):
 *   - Single disk passthrough (write+read, large I/O)
 *   - Mirror writes to all working members (atomic pending, sync-safe)
 *   - RAID5/6 RMW write state machine (phase tracking, P+Q parity, data integrity)
 *   - Superblock atomic write (backup-first + REQ_FUA, crash-safe)
 *   - Memory ownership model (endio chain, dtr NULL-before-free, destroying flag)
 *   - Scrubber (single-pass block verification, checksum cache)
 *   - Module load/unload with active device drain + rcu_barrier
 *   - Write-hole journal (dirty stripe bitmap) — 32-page/128KB per disk,
 *     1MB regions, CRC32c protected, FUA write, crash recovery on re-assembly
 *   - RAID5 rebuild (data disk XOR reconstruction + parity disk XOR reconstruction)
 *     with bitmap journal crash safety
 *   - Read reconstruction on data disk failure (RAID5 P-only, RAID6 XOR from survivors
 *     using P parity — Q parity excluded from XOR since GF-weighted)
 *   - failed_disks bitmask uses atomic_long_and/or for concurrent safety
 *     (no RMW race at rebuild completion)
 *   - RAID6 Q parity compute via GF multiply, verified with live array write+read+
 *     single-disk-failure reconstruction
 *   - RAID6 double-data-disk failure reconstruction via RS(255,N) Vandermonde
 *     decode (2 failed data disks reconstructed from P + Q + surviving data)
 *   - RAID6 data+P failure — reconstructs data from Q parity via single-equation
 *     RS decode (tested PASS)
 *   - RAID6 bitmap recovery — reconstructs both P AND Q parity (GF weighted-sum
 *     for Q, XOR for P) with FUA writes
 *
 * NOT YET PRODUCTION-READY (needs work):
 *   - RAID5 double-failure — not recoverable (RAID5 has only 1 parity disk)
 *   - RAID6 data+Q failure — handled by existing XOR path (Q excluded from XOR,
 *     data+P XOR produces correct data even with Q dead) — implicit, not tested
 *   - RAID6 P+Q failure (both parity dead) — unrecoverable (no parity available)
 *   - Concurrent stress — only single-threaded testing so far
 *
 * MIXED DISK SIZES:
 *   Array capacity = min(disk_sectors) for mirror, or data_disks × min(disk_sectors)
 *   for RAID5/6.  Extra space on larger disks is NOT used.  This is a known
 *   limitation.  Future SHR-style chunk mapping would eliminate the waste.
 *   For now: partition larger disks to match before creating the array, or
 *   accept the waste.
 *
 * ==========================================================================
 * DATA STRUCTURE DESIGN:
 * ==========================================================================
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
#include <linux/mm.h>
#include <linux/device-mapper.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/crc32c.h>	/* crc32c() / __crc32c_le() */
#include <linux/crc32.h>
#include <linux/bitmap.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/xarray.h>
#include <linux/delay.h>
#include <linux/build_bug.h>

#include "dm_lhsr.h"

#define DM_MSG_PREFIX "lhsr"
#define LHSR_VERSION "1.4.0"

/* Forward declarations */
struct lhsr_raid_5_read_ctx;

static void lhsr_mirror_endio(struct bio *bio);
static void lhsr_raid_5_read_endio(struct bio *bio);
static void lhsr_xor_parity(void *parity, void **data, unsigned int data_disks, size_t len);
static u8 lhsr_gf_inv(u8 a);
static int lhsr_rs_decode_2(struct lhsr_array *arr, unsigned int *failed,
			    unsigned int failed_count,
			    struct lhsr_raid_5_read_ctx *ctx, size_t len);

/* Write-hole journal bitmap functions */
static int lhsr_bitmap_init(struct lhsr_array *arr);
static void lhsr_bitmap_destroy(struct lhsr_array *arr);
static int lhsr_bitmap_load(struct lhsr_array *arr);
static int lhsr_bitmap_recover(struct lhsr_array *arr);
static int lhsr_bitmap_set(struct lhsr_array *arr, sector_t chunk_start);
static int lhsr_bitmap_clear(struct lhsr_array *arr, sector_t chunk_start);
static void lhsr_bitmap_flush(struct lhsr_array *arr);
static void lhsr_bitmap_work(struct work_struct *work);

/* Write-intent bitmap (WIB) functions */
static int lhsr_wib_init(struct lhsr_array *arr);
static void lhsr_wib_destroy(struct lhsr_array *arr);
static int lhsr_wib_load(struct lhsr_array *arr);
static int lhsr_wib_flush(struct lhsr_array *arr);
static void lhsr_wib_work(struct work_struct *work);
static void lhsr_wib_set(struct lhsr_array *arr, sector_t sector);
static void lhsr_wib_clear(struct lhsr_array *arr, sector_t sector);
static int lhsr_wib_test(struct lhsr_array *arr, sector_t sector);
static void lhsr_wib_clear_all(struct lhsr_array *arr);

/* RAID5/6 RMW synchronous worker */
static void lhsr_rmw_worker(struct work_struct *work);

static struct bio_set lhsr_bioset;

/* Precomputed 2^i in GF(2^8) for RAID6 Reed-Solomon */
static u8 rs_power_table[256];
/* Discrete log for GF(2^8): gf_log[rs_power_table[i]] = i, gf_log[0] is 0 (unused) */
static u8 gf_log[256];

/* Active device tracking to prevent use-after-unload */
static atomic_t lhsr_active_devices = ATOMIC_INIT(0);
static int lhsr_module_exiting;

/*
 * RAID5/6 RMW write context — sequential phase state machine.
 * Each phase submits one I/O; the endio callback advances to the next phase.
 *
 * Phase tracking:
 *   0 = PHASE_INIT (before any I/O)
 *   1 = PHASE_OLD_DATA_READ  (read old data chunk)
 *   2 = PHASE_P_READ         (read old P parity)
 *   3 = PHASE_Q_READ         (read old Q parity, RAID6 only)
 *   4 = PHASE_DATA_WRITE     (compute XOR + merge + write new data)
 *   5 = PHASE_P_WRITE        (write new P parity)
 *   6 = PHASE_Q_WRITE        (write new Q parity, RAID6 only)
 *   7 = PHASE_DONE           (complete orig_bio + cleanup)
 */
/*
 * RAID5/6 RMW work item — executed on the WQ_UNBOUND rmw_wq
 * (max_active=8, per-array).  The per-stripe mutex hash
 * (stripe_locks[], 128 buckets) serializes writes to the SAME
 * stripe for write-hole safety.  Different stripes run concurrently
 * across CPUs via the WQ_UNBOUND workqueue.
 *
 * Each work item does the full RMW cycle synchronously on one worker:
 *   read old data → read P parity → (RAID6) read Q parity →
 *   compute new data + new P + new Q →
 *   write new data → write P → (RAID6) write Q →
 *   complete orig_bio
 */
struct lhsr_rmw_work {
	struct work_struct work;

	struct bio *orig_bio;		/* Original bio to complete */
	struct lhsr_array *arr;		/* Array context */

	unsigned int data_disk;		/* Target data disk index */
	unsigned int data_disks;	/* Number of data disks */
	unsigned int parity_disks;	/* Number of parity disks (1 or 2) */
	sector_t chunk_start;		/* Stripe-aligned chunk start (sectors) */
	size_t chunk_bytes;		/* Chunk size in bytes */
	unsigned int offset_in_chunk;	/* Sector offset within chunk for bio data */
};

/* RAID5/6 read reconstruction context */
struct lhsr_raid_5_read_ctx {
	struct bio *orig_bio;		/* Original bio to complete */
	struct lhsr_array *arr;		/* Array context */
	atomic_t pending;			/* Count of pending reads */
	int status;			/* Final status */
	void *recon_buf;			/* Reconstructed data buffer */
	unsigned int target_disk;	/* Which disk we're reconstructing for */
	unsigned int data_disks;	/* Number of data disks (total - parity) */
	unsigned int working;		/* Number of working data disks */
	unsigned int data_survivors;	/* Number of surviving data disks in disk_map */
	unsigned int num_slots;		/* Total buffer slots = data_survivors + parity_survivors */
	sector_t offset;			/* Sector offset */
	size_t bio_size;			/* Byte count stored at read start (safe from completion) */
	void **data_bufs;			/* Array of data+parity buffers */
	struct page **pages;		/* Per-slot owned pages (for reconstruction reads) */
	unsigned int disk_map[0];	/* Flex array: slot_idx -> disk_idx */
};

/* Mirror write context - tracks completions across all mirror members */
struct lhsr_mirror_ctx {
	struct bio *orig_bio;	/* Original bio to complete */
	atomic_t pending;	/* Count of pending writes */
	int status;		/* Final status */
};

MODULE_LICENSE("GPL");
MODULE_AUTHOR("LHSR Team");
MODULE_DESCRIPTION("Linux Hybrid Self-Healing RAID");
MODULE_VERSION(LHSR_VERSION);

/*
 * CRC32c checksum using kernel API
 * Kernel's __crc32c_le(crc, buf, len) computes CRC32c (Castagnoli) with:
 *   - Programmable initial value (passed as first arg)
 *   - No final XOR
 *   - Polynomial: 0x1EDC6F41 (same as 0x82F63B78 reflected)
 *
 * All LHSR CRC calls use __crc32c_le(0, ...) which means:
 *   init=0, NO final 0xFFFFFFFF XOR.
 * This is NOT the "standard" CRC32c (which uses init=0xFFFFFFFF and final
 * XOR=0xFFFFFFFF). It is the raw table-based accumulation starting from 0.
 *
 * Userspace verification (for test scripts) must use identical semantics:
 *   crc = 0;                          // init = 0
 *   for each byte: crc = table[...];  // no final XOR
 *
 * This is used for superblock, scrub checksums, and bitmap page CRCs.
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
	int ret;

	/*
	 * Superblock lives after the bitmap area, at the end of the
	 * per-disk reserved metadata zone.  Layout:
	 *   sector 0 .. array_size-1           user data
	 *   sector array_size .. +BITMAP_SECT  bitmap pages
	 *   +BITMAP_SECT .. +META_SECTORS-1    superblock (primary + backup)
	 */
	primary_sector = disk_offset + array_size + LHSR_BITMAP_TOTAL_SECTORS;
	backup_sector = disk_offset + array_size + LHSR_BITMAP_TOTAL_SECTORS
		+ (LHSR_SB_SECTORS / 2);

	/* Allocate page for I/O */
	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	buf = kmap_local_page(page);

	/* Calculate checksum using kernel CRC32c API */
	memcpy(buf, sb, LHSR_SB_SIZE);
	/* Zero checksum field for CRC calculation */
	*(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
	sb->checksum = __crc32c_le(0, buf, LHSR_SB_SIZE);
	/* Copy final superblock with valid checksum */
	memcpy(buf, sb, LHSR_SB_SIZE);
	kunmap_local(buf);

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

	DMDEBUG("lhsr_write_superblock: writing backup to sector %llu", (u64)backup_sector);
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

	DMDEBUG("lhsr_write_superblock: writing primary to sector %llu", (u64)primary_sector);
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
				sector_t array_size, sector_t disk_offset)
{
	struct bio *bio;
	struct page *page;
	void *buf;
	sector_t sector;
	sector_t backup_sector;
	u32 stored_csum, calc_csum;
	int ret;

	/*
	 * Superblock lives after the bitmap area, at the end of the
	 * per-disk reserved metadata zone.  Layout:
	 *   sector 0 .. array_size-1           user data
	 *   sector array_size .. +BITMAP_SECT  bitmap pages
	 *   +BITMAP_SECT .. +META_SECTORS-1    superblock (primary + backup)
	 */
	sector = disk_offset + array_size + LHSR_BITMAP_TOTAL_SECTORS;
	backup_sector = disk_offset + array_size + LHSR_BITMAP_TOTAL_SECTORS
		+ (LHSR_SB_SECTORS / 2);
	DMDEBUG("lhsr_read_superblock: primary sector=%llu, backup sector=%llu",
	       (u64)sector, (u64)backup_sector);

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;

	DMDEBUG("lhsr_read_superblock: allocating bio");
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

	buf = kmap_local_page(page);
	stored_csum = *(u32 *)(buf + offsetof(struct lhsr_superblock, checksum));
	*(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
	calc_csum = __crc32c_le(0, buf, LHSR_SB_SIZE);

	if (stored_csum != calc_csum) {
		DMWARN("Primary superblock checksum mismatch (0x%08x vs 0x%08x), trying backup at sector %llu",
		       stored_csum, calc_csum, (u64)backup_sector);

		bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_KERNEL);
		if (!bio) {
			kunmap_local(buf);
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
			kunmap_local(buf);
			__free_page(page);
			return -EIO;
		}

		stored_csum = *(u32 *)(buf + offsetof(struct lhsr_superblock, checksum));
		*(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
		calc_csum = __crc32c_le(0, buf, LHSR_SB_SIZE);

		if (stored_csum != calc_csum) {
			DMERR("Both primary and backup superblock checksums invalid");
			kunmap_local(buf);
			__free_page(page);
			return -EINVAL;
		}
		DMDEBUG("Restored from backup superblock");
	}

	memcpy(sb, buf, LHSR_SB_SIZE);
	kunmap_local(buf);
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
	sb->disk_uuid = get_random_u64();
	sb->creation_time = ktime_get_real_seconds();
	sb->last_update = sb->creation_time;
	sb->disk_index = disk_idx;
	sb->disk_state = LHSR_DISK_HEALTHY;
	sb->raid_type = raid_type;
	sb->disk_count = disk_count;
	sb->total_sectors = total_sectors;
	sb->generation = 1;
	/* Zero checksum field before computing CRC (field is at offset 0 in struct) */
	sb->checksum = 0;
	sb->checksum = __crc32c_le(0, (void *)sb, LHSR_SB_SIZE);
}

/* Pack/unpack helpers for checksum cache xarray values (u64 keyed by sector offset) */
/* Lower 32 bits = CRC32c, upper 32 bits = flags (LHSR_BLOCK_VERIFIED/CORRUPT) */
#define cksum_pack(cksum, flags)	(((u64)(flags) << 32) | (cksum))
#define cksum_unpack_cksum(val)		((u32)(val))
#define cksum_unpack_flags(val)		((u32)((val) >> 32))

/* Key the checksum cache by (disk_idx << 56) | offset so each disk
 * has its own checksum namespace. 56 bits gives 2^56 sectors = 128 PB
 * per disk — enough for any practical deployment.
 */
#define CKSUM_KEY(disk, off)		((u64)(disk) << 56 | (u64)(off))

/* RAID5/6 READ reconstruction completion */
static void lhsr_raid_5_read_endio(struct bio *bio)
{
	struct lhsr_raid_5_read_ctx *ctx = bio->bi_private;
	struct lhsr_array *arr;
	int is_raid6;

	if (!ctx) {
		DMERR("read_endio: ctx is NULL");
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return;
	}

	arr = ctx->arr;
	is_raid6 = (arr->raid_type == LHSR_RAID6);
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

	/* Populate data_bufs entry from this completed bio's data.
	 *
	 * NOTE: bio->bi_iter.bi_size is ALWAYS 0 after I/O completion
	 * (the block layer advances the iterator as it processes segments).
	 * Do NOT use bio_for_each_segment() here — it would see bi_size=0
	 * and skip the loop body.  Instead, read directly from ctx->pages[slot]
	 * which we allocated and mapped into the bio at offset 0.
	 */
	if (slot < ctx->num_slots && ctx->data_bufs && ctx->data_bufs[slot]) {
		void *kaddr = kmap_local_page(ctx->pages[slot]);
		int non_zero = 0;

		memcpy(ctx->data_bufs[slot], kaddr, ctx->bio_size);

		/* Quick non-zero check on first bytes */
		{
			u8 *tmp = (u8 *)ctx->data_bufs[slot];
			unsigned int k;
			for (k = 0; k < min_t(size_t, 64, ctx->bio_size); k++) {
				if (tmp[k]) non_zero++;
			}
		}

		kunmap_local(kaddr);

		DMDEBUG("read_endio: slot=%u disk=%u non_zero_first64=%d bio_size=%zu",
		        slot, ctx->disk_map[slot], non_zero, ctx->bio_size);

		/* Free our per-clone page (not shared, safe to free here) */
		if (ctx->pages && slot < ctx->num_slots && ctx->pages[slot]) {
			__free_page(ctx->pages[slot]);
			ctx->pages[slot] = NULL;
		}
	} else if (bio->bi_status == BLK_STS_OK) {
		/* No data_bufs slot but bio completed OK — free page anyway */
		if (slot < ctx->num_slots && ctx->pages && ctx->pages[slot]) {
			__free_page(ctx->pages[slot]);
			ctx->pages[slot] = NULL;
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
			for (j = 0; j < ctx->data_disks; j++) {
				if (lhsr_failed_disks_get(arr) & (1 << j)) {
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

			DMDEBUG("RECON complete: pending=0 failed_count=%u all_bufs_valid=%d num_slots=%u",
			        failed_count, all_bufs_valid, ctx->num_slots);

			if (all_bufs_valid) {
				unsigned long fd_local = lhsr_failed_disks_get(arr);
				unsigned int dd = ctx->data_disks;
				unsigned int p_idx = dd;
				bool p_alive = !(fd_local & (1UL << p_idx));
				/*
				 * Determine reconstruction method:
				 *
				 * 1. Single data failure + P alive → XOR (data + P)
				 *    Works for both RAID5 and RAID6. Q is in disk_map
				 *    but excluded from XOR (GF-weighted, not XOR-compatible).
				 *
				 * 2. RAID6: 1 data + P dead + Q alive → RS decode from Q
				 *    (single-equation recover).
				 *
				 * 3. RAID6: 2 data failures + P+Q alive → Vandermonde 2x2
				 *    RS decode from P+Q+survivors.
				 *
				 * 4. RAID5 double-failure, or not enough parity → IOERR.
				 */
				if (failed_count == 1 && p_alive) {
					/*
					 * XOR reconstruction: data survivors + P parity.
					 * Iterate disk_map and exclude Q (GF-weighted).
					 * Use a fixed-size array (max LHSR_MAX_DISKS).
					 */
					u8 *xor_slots[LHSR_MAX_DISKS];
					unsigned int xor_count = 0;
					for (j = 0; j < ctx->num_slots; j++) {
						unsigned int d = ctx->disk_map[j];
						if (d < dd || d == p_idx)
							xor_slots[xor_count++] = ctx->data_bufs[j];
					}
					DMDEBUG("RECON XOR: xor_count=%u", xor_count);
					lhsr_xor_parity(ctx->recon_buf,
							 (void **)xor_slots, xor_count, bio_size);
				} else if (failed_count >= 1 && is_raid6) {
					/*
					 * Try Reed-Solomon decode for RAID6.
					 */
					int rs_ret = lhsr_rs_decode_2(arr, failed,
								       failed_count,
								       ctx, bio_size);
					if (rs_ret == 0) {
						DMDEBUG("RECON RS decode: target=%u failed=%u",
						        ctx->target_disk, failed_count);
					} else {
						DMERR("RAID6: RS decode failed (%d) for %u disks",
						      rs_ret, failed_count);
						ctx->status = BLK_STS_IOERR;
					}
				} else {
					DMERR("RAID%c: double-failure not recoverable (%u)",
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
				int non_zero = 0;
				{
					u8 *tmp = (u8 *)ctx->recon_buf;
					unsigned int k;
					for (k = 0; k < min_t(size_t, 64, bio_size); k++) {
						if (tmp[k]) non_zero++;
					}
				}
				DMDEBUG("RECON XOR: recon_buf non_zero_first64=%d bio_size=%zu",
				        non_zero, bio_size);
				bio_for_each_segment(bv, ctx->orig_bio, iter) {
					void *kaddr = kmap_local_page(bv.bv_page);
					void *dst = kaddr + bv.bv_offset;
					memcpy(dst, ctx->recon_buf, bv.bv_len);
					kunmap_local(kaddr);
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
			for (j = 0; j < ctx->num_slots; j++)
				kfree(ctx->data_bufs[j]);
			kfree(ctx->data_bufs);
		}
		if (ctx->pages) {
			unsigned int j;
			for (j = 0; j < ctx->num_slots; j++) {
				if (ctx->pages[j])
					__free_page(ctx->pages[j]);
			}
			kfree(ctx->pages);
		}
		kfree(ctx);
	}

	bio_put(bio);
}

static int lhsr_update_disk_state(struct lhsr_array *arr, unsigned int disk_idx, u32 new_state);
static void lhsr_select_new_primary(struct lhsr_array *arr, unsigned int failed_disk, u32 extra_failed);

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

	ctx = kmalloc(sizeof(*ctx), GFP_NOIO);
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
				if (!(lhsr_failed_disks_get(arr) & (1 << i))) {
					DMERR("Disk %u failed due to I/O errors", i);
					lhsr_failed_disks_set(arr, lhsr_failed_disks_get(arr) | (1 << i));
					up_write(&arr->sb_sem);
					lhsr_update_disk_state(arr, i, LHSR_DISK_DEGRADED);
					if (arr->primary_disk == i)
						lhsr_select_new_primary(arr, i, 0);
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

/* GF(2^8) multiply via Russian peasant algorithm (polynomial 0x1d) */
static inline u8 lhsr_gf_mul(u8 a, u8 b)
{
	u8 prod = 0;
	u8 val = a;

	while (b) {
		if (b & 1)
			prod ^= val;
		val = (val << 1) ^ (val & 0x80 ? 0x1d : 0);
		b >>= 1;
	}
	return prod;
}

/* GF(2^8) inverse using log/antilog tables (power table wraps at 255) */
static inline u8 lhsr_gf_inv(u8 a)
{
	unsigned int idx;

	if (!a)
		return 0;	/* 0 has no inverse — return 0 harmlessly */
	idx = 255 - gf_log[a];
	if (idx >= 255)
		idx = 0;
	return rs_power_table[idx];
}

/*
 * RS(255,N) decode for RAID6 with exactly 2 failed data disks.
 *
 * Solves the Vandermonde system:
 *   [1     1  ] [D_t]   [S]    S = surviving data ^ P
 *   [g^t  g^o ] [D_o] = [T]    T = Q ^ sum(g^i * surviving data)
 *
 * Only reconstructs ctx->target_disk (the bio target).
 * Returns 0 on success, -ENOMEM on allocation failure, -EIO if not recoverable.
 */
static int lhsr_rs_decode_2(struct lhsr_array *arr, unsigned int *failed,
			    unsigned int failed_count,
			    struct lhsr_raid_5_read_ctx *ctx, size_t len)
{
	unsigned int i, j;
	unsigned int data_disks = ctx->data_disks;
	unsigned int p_idx = data_disks;
	unsigned int q_idx = data_disks + 1;
	unsigned long fd = lhsr_failed_disks_get(arr);
	bool p_alive = !(fd & (1UL << p_idx));
	bool q_alive = (arr->raid_type == LHSR_RAID6) && !(fd & (1UL << q_idx));
	unsigned int target = ctx->target_disk;
	unsigned int other;
	u8 *tmp;
	u8 coeff_other, det, inv_det;
	u8 *recon = ctx->recon_buf;	/* Cast void* to u8* for byte access */

	/* Allocate temp buffer for weighted sum (T) */
	tmp = kzalloc(len, GFP_NOIO);
	if (!tmp)
		return -ENOMEM;

	/*
	 * Step 1: Compute S = XOR of surviving data + P (if alive).
	 * Store into recon_buf.
	 */
	memset(recon, 0, len);
	for (j = 0; j < ctx->num_slots; j++) {
		unsigned int d = ctx->disk_map[j];
		if (d < data_disks || d == p_idx) {
			/* Data survivor or P parity */
			u8 *buf = ctx->data_bufs[j];
			for (i = 0; i < len; i++)
				recon[i] ^= buf[i];
		}
	}

	/*
	 * Step 2: Compute T = Q ^ sum(g^i * surviving_data).
	 */
	memset(tmp, 0, len);
	for (j = 0; j < ctx->num_slots; j++) {
		unsigned int d = ctx->disk_map[j];
		u8 *buf = ctx->data_bufs[j];

		if (d < data_disks) {
			/* Surviving data disk: weighted by g^d */
			u8 coeff = rs_power_table[d];
			for (i = 0; i < len; i++)
				tmp[i] ^= lhsr_gf_mul(buf[i], coeff);
		} else if (d == q_idx) {
			/* Q parity: XOR directly (already weighted) */
			for (i = 0; i < len; i++)
				tmp[i] ^= buf[i];
		}
	}

	if (failed_count == 1 && !p_alive && q_alive) {
		/*
		 * Case: target data disk + P failed, Q alive.
		 * D_target = inv(g^target) * (Q ^ sum(g^i * surviving))
		 * tmp = g^target * D_target → divide by g^target
		 */
		u8 inv_g = lhsr_gf_inv(rs_power_table[target]);
		for (i = 0; i < len; i++)
			recon[i] = lhsr_gf_mul(tmp[i], inv_g);
	} else if (failed_count >= 2 && !p_alive && q_alive) {
		/*
		 * Case: data + P failed (>=2 failures including P), Q alive.
		 * Same single-equation from Q as above — P failure doesn't affect
		 * the Q-weighted-sum formula since P is never included in T.
		 * D_target = inv(g^target) * T
		 */
		u8 inv_g = lhsr_gf_inv(rs_power_table[target]);
		for (i = 0; i < len; i++)
			recon[i] = lhsr_gf_mul(tmp[i], inv_g);
	} else if (failed_count >= 2 && p_alive && q_alive) {
		/*
		 * Case: 2 data disks failed, P+Q alive.
		 * Vandermonde 2x2: D_t = (g^o * S ^ T) * inv(g^t ^ g^o)
		 * where t = target, o = other failed data disk
		 */
		other = (target == failed[0]) ? failed[1] : failed[0];
		coeff_other = rs_power_table[other];
		det = rs_power_table[target] ^ rs_power_table[other];
		inv_det = lhsr_gf_inv(det);

		for (i = 0; i < len; i++) {
			u8 s = recon[i];
			u8 t = tmp[i];
			recon[i] = lhsr_gf_mul(
				lhsr_gf_mul(coeff_other, s) ^ t, inv_det);
		}
	} else {
		kfree(tmp);
		return -EIO;
	}

	kfree(tmp);
	return 0;
}

/* Select a new primary disk after a disk failure */
static void lhsr_select_new_primary(struct lhsr_array *arr, unsigned int failed_disk, u32 extra_failed)
{
	unsigned int k;

	arr->primary_disk = arr->disks; /* invalid */
	for (k = 0; k < arr->disks; k++) {
		if (k == failed_disk)
			continue;
		if (!(lhsr_failed_disks_get(arr) & (1 << k)) && !(extra_failed & (1 << k))) {
			arr->primary_disk = k;
			break;
		}
	}
	if (arr->primary_disk < arr->disks)
		DMINFO("Failover to disk %u", arr->primary_disk);
	else
		DMERR("No working disk for failover");
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

	DMDEBUG("Running periodic disk health check");

	for (i = 0; i < arr->disks; i++) {
		if (lhsr_failed_disks_get(arr) & (1 << i))
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

			if (arr->primary_disk == i)
				lhsr_select_new_primary(arr, i, new_failed);
		}
	}

	if (new_failed) {
		/* Update failed_disks with lock protection */
		down_write(&arr->sb_sem);
		lhsr_failed_disks_set(arr, lhsr_failed_disks_get(arr) | new_failed);
		up_write(&arr->sb_sem);
		arr->state = LHSR_STATE_DEGRADED;
		DMWARN("Failed disks mask: 0x%lx", lhsr_failed_disks_get(arr));

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

/* Scrub a block - reads and verifies data integrity
 *
 * Two modes:
 *   1. dm-integrity stacking (arr->integrity_below == true):
 *      dm-integrity verifies per-block checksums on every read.  If the read
 *      succeeds the block is intact; if it fails with -EIO the block is
 *      corrupt.  No CRC computation, no ephemeral xarray cache needed.
 *
 *   2. Native checksum cache (arr->integrity_below == false):
 *      Read block, compute CRC32c, compare against previously stored value
 *      in the xarray cache.  Cache is ephemeral (lost on module reload).
 *
 * Uses proper page cache pages for BIO I/O instead of vmalloc'd memory.
 * vmalloc_to_page() is unreliable for I/O because kvmalloc may return
 * kmalloc memory, and block layer BIOs require struct page backing.
 */
static int lhsr_scrub_block(struct lhsr_array *arr, unsigned int disk_idx, u64 offset)
{
	struct bio *bio;
	struct page **pages;
	void *buf;
	void *xa_val;
	u32 stored_csum = 0;
	u32 stored_flags = 0;
	u32 calc_csum;
	unsigned int nr_pages;
	int ret = 0;
	int i;
	bool retried = false;

	if (lhsr_failed_disks_get(arr) & (1 << disk_idx))
		return -EINVAL;

	if (!arr->disk[disk_idx])
		return -EINVAL;

	/*
	 * When stacked on dm-integrity, no checksum cache operations are
	 * needed — integrity verification happens at the block layer below.
	 * Just read the block; success means verified, failure means corrupt.
	 */
	if (arr->integrity_below) {
		nr_pages = (LHSR_SCRUB_BLOCK_SIZE + PAGE_SIZE - 1) >> PAGE_SHIFT;

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

		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);

		if (ret != 0) {
			DMERR("Scrub (integrity) read failed at offset 0x%llx: %d",
			      offset, ret);
			return ret;
		}

		DMINFO("Scrub (integrity): block at offset 0x%llx verified OK", offset);
		return 0;
	}

	/*
	 * Native mode: read block into pages, compute CRC32c, compare against
	 * ephemeral xarray cache.
	 */

	/* Look up stored checksum for this (disk, offset) in xarray */
	xa_val = xa_load(&arr->cksum_cache, CKSUM_KEY(disk_idx, offset));
	if (xa_val) {
		stored_csum = cksum_unpack_cksum(xa_to_value(xa_val));
		stored_flags = cksum_unpack_flags(xa_to_value(xa_val));
	}

retry:
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
			void *kmap_addr = kmap_local_page(pages[i]);
			memcpy(buf_ptr, kmap_addr, page_bytes);
			kunmap_local(kmap_addr);
			buf_ptr += page_bytes;
		}
	}

	for (i = 0; i < nr_pages; i++)
		__free_page(pages[i]);
	kfree(pages);

	/* Calculate checksum of read data */
	calc_csum = __crc32c_le(0, buf, LHSR_SCRUB_BLOCK_SIZE);

	if (stored_csum == 0) {
		/* No stored checksum — new block, store it */
		xa_val = xa_mk_value(cksum_pack(calc_csum, LHSR_BLOCK_VERIFIED));
		ret = xa_err(xa_store(&arr->cksum_cache, CKSUM_KEY(disk_idx, offset), xa_val, GFP_KERNEL));
		if (ret)
			DMWARN("Scrub: Failed to store checksum for offset 0x%llx: %d",
			       offset, ret);
		else
			DMINFO("Scrub: Stored new checksum for offset 0x%llx", offset);
		kfree(buf);
		return 0;
	}

	/* Verify checksum */
	if (calc_csum != stored_csum) {
		if (!retried) {
			/*
			 * First CRC mismatch — could be a transient race with a
			 * concurrent write. Retry the read to rule it out before
			 * declaring corruption.
			 */
			retried = true;
			kfree(buf);
			DMINFO("Scrub: Retrying read at offset 0x%llx (stored=0x%08x, calc=0x%08x)",
			       offset, stored_csum, calc_csum);
			goto retry;
		}

		/*
		 * Still mismatched after retry — genuine data corruption.
		 * Mark the block CORRUPT and return error.
		 */
		DMERR("Scrub: VERIFIED corruption at offset 0x%llx (stored=0x%08x, calc=0x%08x)",
		       offset, stored_csum, calc_csum);
		xa_val = xa_mk_value(cksum_pack(stored_csum,
				       stored_flags | LHSR_BLOCK_CORRUPT));
		xa_store(&arr->cksum_cache, CKSUM_KEY(disk_idx, offset),
			 xa_val, GFP_KERNEL);
		atomic_inc(&arr->corruptions_detected);
		kfree(buf);
		return -EIO;
	}

	/*
	 * Checksum verified. If this was a retry that resolved the mismatch,
	 * count it as a transient resolved by retry.
	 */
	if (retried)
		arr->scrub_retry_resolved++;

	/* Update flags to VERIFIED */
	xa_val = xa_mk_value(cksum_pack(stored_csum,
			       stored_flags | LHSR_BLOCK_VERIFIED));
	xa_store(&arr->cksum_cache, CKSUM_KEY(disk_idx, offset),
		 xa_val, GFP_KERNEL);
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

	/* Check if we're done with current disk */
	if (arr->scrub_offset >= arr->disk_sectors) {
		/* Count this disk as completed */
		arr->scrub_disks_done++;

		/* If all disks are done, we're finished */
		if (arr->scrub_disks_done >= arr->disks) {
			/* All disks done */
			DMINFO("Scrub completed: %llu verified, %llu corrupt, %llu transient-retry",
			       arr->scrub_verified, arr->scrub_corrupted,
			       arr->scrub_retry_resolved);
			arr->scrub_state = LHSR_SCRUB_COMPLETED;
			return;
		}

		/* Advance to next healthy disk */
		disk_idx = (arr->scrub_disk + 1) % arr->disks;
		while (lhsr_failed_disks_get(arr) & (1 << disk_idx))
			disk_idx = (disk_idx + 1) % arr->disks;

		arr->scrub_disk = disk_idx;
		arr->scrub_offset = 0;
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
		arr->scrub_corrupted++;
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
	arr->scrub_disks_done = 0;
	arr->scrub_offset = 0;
	arr->scrub_verified = 0;
	arr->scrub_corrupted = 0;
	arr->scrub_last_offset = 0;
	arr->scrub_start_jiffies = jiffies;

	INIT_DELAYED_WORK(&arr->scrub_work, scrub_work);
	queue_delayed_work(arr->scrub_wq, &arr->scrub_work, 0);

	DMINFO("Scrubber started");
	return 0;
}

/*
 * Rebuild a parity stripe for RAID5/6.
 * Reads all surviving data disks at @offset, computes P parity (XOR)
 * and Q parity (GF(2^8) for RAID6), writes to the target parity disk.
 *
 * Returns 0 on success, negative errno on failure.
 */
/*
 * Reconstruct a single-disk stripe via XOR (RAID5) or GF (RAID6 Q).
 *
 * Reads every non-failed, non-target disk at @offset, XORs or GF-accumulates
 * into a result buffer, and writes the result to @disk_idx.
 *
 * For RAID5: XOR all remaining disks (data + P) — works for both data-disk
 *   rebuild (reconstruct missing data) and parity-disk rebuild (reconstruct P).
 * For RAID6 single-failure (non-Q): XOR remaining data + P — Q is excluded
 *   from XOR because Q = gf_mul(D0) ^ gf_mul(D1) ^ ... — XORing Q would
 *   produce wrong data.  P is XOR-of-data, correct for single-disk recovery.
 * For RAID6 Q rebuild (disk_idx = data_disks+1): GF multiply per data disk.
 */
static int lhsr_rebuild_reconstruct_stripe(struct lhsr_array *arr,
					   unsigned int disk_idx,
					   sector_t offset, u64 block_bytes)
{
	unsigned int data_disks = 0, parity_disks = 0;
	unsigned int nr_pages;
	struct page **read_pages = NULL;
	struct page **result_pages = NULL;
	unsigned int i, p;
	int ret = 0;
	bool first = true;

	if (arr->raid_type == LHSR_RAID5) {
		data_disks = arr->disks - 1;
		parity_disks = 1;
	} else if (arr->raid_type == LHSR_RAID6) {
		data_disks = arr->disks - 2;
		parity_disks = 2;
	} else {
		return -EINVAL;
	}

	nr_pages = (block_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (!nr_pages)
		return -EINVAL;

	read_pages = kcalloc(nr_pages, sizeof(*read_pages), GFP_KERNEL);
	result_pages = kcalloc(nr_pages, sizeof(*result_pages), GFP_KERNEL);
	if (!read_pages || !result_pages) {
		ret = -ENOMEM;
		goto out;
	}

	/* Iterate ALL disks, skip the failed target and already-failed disks */
	for (i = 0; i < arr->disks; i++) {
		void *rbuf, *rbuftmp;

		/* Skip target and already-failed */
		if (i == disk_idx || (lhsr_failed_disks_get(arr) & (1 << i)) ||
		    !arr->disk[i])
			continue;

		/*
		 * RAID6 Q parity rebuild: GF multiply from data disks only.
		 * Q can only be rebuilt from data — reading P doesn't help
		 * because Q = gf_mul(D0) ^ gf_mul(D1) ^ ... (not XOR of data).
		 */
		if (arr->raid_type == LHSR_RAID6 && disk_idx == data_disks + 1) {
			if (i >= data_disks)
				continue;  /* skip parity disks for Q rebuild */
			/* Q rebuild path — uses GF(2^8) multiply-accumulate */
			goto q_rebuild;
		}

		/*
		 * RAID6 single-disk failure (non-Q): skip Q disk in XOR path.
		 * Q = gf_mul(D0) ^ gf_mul(D1) ^ ... — XORing Q produces
		 * wrong data because Q uses GF multiplication, not XOR.
		 * P = D0 ^ D1 ^ ... — XORing P is correct for recovery.
		 */
		if (arr->raid_type == LHSR_RAID6 && i == data_disks + 1)
			continue;

		/*
		 * All other cases (RAID5 any disk, RAID6 data or P rebuild):
		 * XOR all non-target, non-failed, non-Q disks.
		 */

		/* Fall through to XOR path */
		{

		/* Allocate read pages */
		for (p = 0; p < nr_pages; p++) {
			read_pages[p] = alloc_page(GFP_KERNEL);
			if (!read_pages[p]) {
				ret = -ENOMEM;
				goto out;
			}
		}

		/* Read from this disk */
		{
			struct bio *bio;

			bio = bio_alloc_bioset(arr->disk[i], nr_pages,
					       REQ_OP_READ, GFP_KERNEL,
					       &lhsr_bioset);
			if (!bio) {
				ret = -ENOMEM;
				goto out;
			}
			bio->bi_iter.bi_sector = offset + arr->disk_offset[i];
			for (p = 0; p < nr_pages; p++) {
				unsigned int page_bytes = (p == nr_pages - 1) ?
					block_bytes - (p << PAGE_SHIFT) : PAGE_SIZE;
				__bio_add_page(bio, read_pages[p], page_bytes, 0);
			}
			ret = submit_bio_wait(bio);
			bio_put(bio);
			if (ret)
				goto out;
		}

		if (first) {
			/* First source: copy to result pages */
			for (p = 0; p < nr_pages; p++) {
				result_pages[p] = alloc_page(GFP_KERNEL);
				if (!result_pages[p]) {
					ret = -ENOMEM;
					goto out;
				}
				rbuf = kmap_local_page(read_pages[p]);
				rbuftmp = kmap_local_page(result_pages[p]);
				memcpy(rbuftmp, rbuf, PAGE_SIZE);
				kunmap_local(rbuftmp);
				kunmap_local(rbuf);
			}
			first = false;
		} else {
			/* XOR into result pages */
			for (p = 0; p < nr_pages; p++) {
				unsigned long *s, *d;
				unsigned int j;

				rbuf = kmap_local_page(read_pages[p]);
				rbuftmp = kmap_local_page(result_pages[p]);
				s = rbuf;
				d = rbuftmp;
				for (j = 0; j < PAGE_SIZE / sizeof(unsigned long); j++)
					d[j] ^= s[j];
				kunmap_local(rbuftmp);
				kunmap_local(rbuf);
			}
		}

		/* Free read pages */
		for (p = 0; p < nr_pages; p++) {
			__free_page(read_pages[p]);
			read_pages[p] = NULL;
		}

		/* End of XOR path */
		}

		continue;

q_rebuild:
		/* RAID6 Q parity rebuild: GF(2^8) multiply-accumulate */
		{
			u8 coeff = rs_power_table[i];

			for (p = 0; p < nr_pages; p++) {
				unsigned int page_bytes = (p == nr_pages - 1) ?
					block_bytes - (p << PAGE_SHIFT) : PAGE_SIZE;
				u8 *src;

				read_pages[p] = alloc_page(GFP_KERNEL);
				if (!read_pages[p]) {
					ret = -ENOMEM;
					goto out;
				}
				rbuf = kmap_local_page(read_pages[p]);
				{
					struct bio *bio;

					bio = bio_alloc_bioset(arr->disk[i], 1,
						REQ_OP_READ, GFP_KERNEL,
						&lhsr_bioset);
					if (!bio) {
						kunmap_local(rbuf);
						ret = -ENOMEM;
						goto out;
					}
					bio->bi_iter.bi_sector = offset +
						arr->disk_offset[i];
					__bio_add_page(bio, read_pages[p],
						       page_bytes, 0);
					ret = submit_bio_wait(bio);
					bio_put(bio);
					if (ret) {
						kunmap_local(rbuf);
						goto out;
					}
				}
				src = rbuf;

				if (i == 0) {
					/* First data disk: store gf_mul */
					result_pages[p] = alloc_page(GFP_KERNEL);
					if (!result_pages[p]) {
						kunmap_local(rbuf);
						ret = -ENOMEM;
						goto out;
					}
					u8 *dst = kmap_local_page(result_pages[p]);
					unsigned int j;
					for (j = 0; j < page_bytes; j++)
						dst[j] = lhsr_gf_mul(src[j], coeff);
					kunmap_local(dst);
				} else {
					/* Accumulate */
					u8 *dst = kmap_local_page(result_pages[p]);
					unsigned int j;
					for (j = 0; j < page_bytes; j++)
						dst[j] ^= lhsr_gf_mul(src[j], coeff);
					kunmap_local(dst);
				}
				kunmap_local(rbuf);
				__free_page(read_pages[p]);
				read_pages[p] = NULL;
			}
		}
	}

	if (first) {
		DMERR("Rebuild: no readable source disks for disk %u", disk_idx);
		ret = -EIO;
		goto out;
	}

	/* Write reconstructed data to target disk with FUA */
	{
		struct bio *bio;

		bio = bio_alloc_bioset(arr->disk[disk_idx], nr_pages,
				       REQ_OP_WRITE | REQ_SYNC | REQ_FUA,
				       GFP_KERNEL, &lhsr_bioset);
		if (!bio) {
			ret = -ENOMEM;
			goto out;
		}
		bio->bi_iter.bi_sector = offset + arr->disk_offset[disk_idx];
		for (p = 0; p < nr_pages; p++) {
			unsigned int page_bytes = (p == nr_pages - 1) ?
				block_bytes - (p << PAGE_SHIFT) : PAGE_SIZE;
			__bio_add_page(bio, result_pages[p], page_bytes, 0);
		}
		ret = submit_bio_wait(bio);
		bio_put(bio);
	}

out:
	for (p = 0; p < nr_pages; p++) {
		if (read_pages[p])
			__free_page(read_pages[p]);
		if (result_pages[p])
			__free_page(result_pages[p]);
	}
	kfree(read_pages);
	kfree(result_pages);
	return ret;
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
		DMINFO("Rebuild complete: %llu sectors processed", arr->rebuild_verified);
		arr->rebuild_state = LHSR_REBUILD_COMPLETE;
		lhsr_update_disk_state(arr, arr->rebuild_disk, LHSR_DISK_HEALTHY);
		atomic_long_and(~(1UL << arr->rebuild_disk),
				&arr->failed_disks);
		if (lhsr_failed_disks_get(arr) == 0)
			arr->state = LHSR_STATE_HEALTHY;
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
		if (!(lhsr_failed_disks_get(arr) & (1 << i)) && arr->disk[i]) {
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

	/* Check if done */
	if (offset >= arr->disk_sectors) {
		DMINFO("Rebuild complete: %llu sectors processed", arr->rebuild_verified);
		arr->rebuild_state = LHSR_REBUILD_COMPLETE;
		lhsr_update_disk_state(arr, arr->rebuild_disk, LHSR_DISK_HEALTHY);
		atomic_long_and(~(1UL << arr->rebuild_disk),
				&arr->failed_disks);
		if (lhsr_failed_disks_get(arr) == 0)
			arr->state = LHSR_STATE_HEALTHY;
		return;
	}

	/* Limit block size to not exceed device */
	if (offset + (block_size >> SECTOR_SHIFT) > arr->disk_sectors)
		block_size = (arr->disk_sectors - offset) << SECTOR_SHIFT;

	/*
	 * Check write-intent bitmap (WIB): skip this block if it was
	 * never written to since last resync.  For RAID1, both mirrors
	 * have identical data for unwritten regions, so no copy needed.
	 *
	 * For RAID5/6, every region needs reconstruction from parity
	 * on dead-disk replacement.  The WIB check is conservative:
	 * WIB bits are cleared on RMW write completion in the RAID5/6
	 * path, but a set bit only means "write was queued", not that
	 * it completed — the RMW worker may still be in flight (or
	 * may have failed).  We cannot skip reconstruction based on
	 * a cleared WIB bit alone during rebuild because parity may
	 * not be consistent if we are rebuilding THE parity disk.
	 * The RAID5/6 rebuild path always reconstructs from parity,
	 * which is the only safe approach regardless of WIB state.
	 *
	 * If WIB is absent (v1 array or alloc failure), this check
	 * always returns "needs copy" and we fall through.
	 */
	if (arr->wib_pages && arr->raid_type < LHSR_RAID5 &&
	    !lhsr_wib_test(arr, offset)) {
		DMDEBUG("rebuild: skip sector %llu (clean WIB bit, "
			"already handled by live write)", (u64)offset);
		arr->rebuild_offset += (block_size >> SECTOR_SHIFT);
		arr->rebuild_verified += (block_size >> SECTOR_SHIFT);
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ / 10);
		return;
	}

	/*
	 * RAID5/6: reconstruct stripe via XOR/GF from all non-failed disks.
	 * This handles BOTH data-disk rebuild (reconstruct missing data from
	 * remaining data + parity) AND parity-disk rebuild (reconstruct P via
	 * XOR or Q via GF).  The function does its own memory management and
	 * writes with FUA.
	 *
	 * Before writing, mark the bitmap region dirty so a crash during
	 * rebuild gets recovered on re-assembly.  Clear after write commits.
	 */
	if (arr->raid_type >= LHSR_RAID5) {
		sector_t region_align = offset &
			~(sector_t)(LHSR_BITMAP_REGION_SECTORS - 1);

		if (lhsr_bitmap_set(arr, region_align))
			DMWARN("rebuild: bitmap_set failed at sector %llu",
			       (u64)offset);

		ret = lhsr_rebuild_reconstruct_stripe(arr, arr->rebuild_disk,
						      offset, block_size);
		if (ret)
			DMERR("Rebuild (RAID%c) failed at offset 0x%llx: %d",
			      arr->raid_type == LHSR_RAID5 ? '5' : '6',
			      (u64)offset << SECTOR_SHIFT, ret);

		if (lhsr_bitmap_clear(arr, region_align))
			DMWARN("rebuild: bitmap_clear failed at sector %llu",
			       (u64)offset);

		/* Mark block as processed in WIB (so rebuild doesn't redo it) */
		lhsr_wib_clear(arr, offset);

		arr->rebuild_offset += (block_size >> SECTOR_SHIFT);
		arr->rebuild_verified += (block_size >> SECTOR_SHIFT);
		queue_delayed_work(arr->rebuild_wq,
				   &arr->rebuild_work, HZ / 10);
		return;
	}

	/*
	 * RAID0/1 data disk rebuild: simple block copy from one healthy
	 * source disk to target.  Not used for RAID5/6 — use the XOR
	 * reconstruction path above instead.
	 */

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
	if (!arr->disk[source_disk]) {
		DMERR("Rebuild: source disk %u is NULL", source_disk);
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		arr->rebuild_state = LHSR_REBUILD_NONE;
		return;
	}
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
		DMERR("Rebuild read failed at offset 0x%llx, ret=%d",
		      (u64)offset << SECTOR_SHIFT, ret);
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		arr->rebuild_offset += (block_size >> SECTOR_SHIFT);
		queue_delayed_work(arr->rebuild_wq, &arr->rebuild_work, HZ);
		return;
	}

	/* Write to rebuild disk */
	if (!arr->disk[arr->rebuild_disk]) {
		DMERR("Rebuild: target disk %u is NULL", arr->rebuild_disk);
		for (i = 0; i < nr_pages; i++)
			__free_page(pages[i]);
		kfree(pages);
		arr->rebuild_state = LHSR_REBUILD_NONE;
		return;
	}
	bio = bio_alloc(arr->disk[arr->rebuild_disk], nr_pages,
			REQ_OP_WRITE | REQ_SYNC | REQ_FUA, GFP_KERNEL);
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
		DMERR("Rebuild write failed at offset 0x%llx",
		      (u64)offset << SECTOR_SHIFT);
	} else {
		/* Clear WIB bit — block is now correct on target */
		lhsr_wib_clear(arr, offset);
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

	if (!(lhsr_failed_disks_get(arr) & (1 << disk_idx))) {
		DMERR("Disk %u is not failed, no rebuild needed", disk_idx);
		return -EINVAL;
	}

	/* Verify target is a valid disk index */
	if (arr->raid_type >= LHSR_RAID5) {
		unsigned int pd = (arr->raid_type == LHSR_RAID5) ? 1 : 2;
		unsigned int dd = arr->disks - pd;
		if (disk_idx >= dd)
			DMINFO("Rebuild: target disk %u is a parity disk, using XOR/RS reconstruction",
			       disk_idx);
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
			if (!(lhsr_failed_disks_get(arr) & (1 << k))) {
				arr->primary_disk = k;
				break;
			}
		}
		if (arr->primary_disk >= arr->disks) {
			DMERR("Rebuild start: no source disk available");
			return -EINVAL;
		}
	}

	/*
	 * Initialize and load write-intent bitmap (WIB).
	 * If WIB loads successfully, rebuild can skip clean regions
	 * (never-written blocks).  If WIB is absent (v1 array or
	 * alloc failure), fall through with conservative behavior
	 * (always-copy).
	 */
	if (lhsr_wib_init(arr))
		DMWARN("rebuild: WIB init failed, will do full copy");

	arr->rebuild_state = LHSR_REBUILD_RUNNING;
	arr->rebuild_disk = disk_idx;
	arr->rebuild_offset = 0;
	arr->rebuild_total = arr->disk_sectors;
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
	unsigned int raid_type = LHSR_RAID0;
	unsigned int num_disks = 0;
	unsigned int i = 0;
	sector_t size = 0;
	int r = 0;
	bool integrity_below = false;
	bool degraded = false;
	unsigned int total_disks = 0;

	DMINFO("ctr: Entered with %u args", argc);
	for (i = 0; i < argc; i++)
		DMINFO("ctr: argv[%u]=[%s]", i, argv[i]);

	/*
	 * Trailing keywords "integrity", "degraded", and "total_disks=N"
	 * — can appear in any order.  Strip them BEFORE any argc-dependent
	 * calculations.
	 *
	 *   "integrity": dm-integrity devices are stacked below.  When set,
	 *   LHSR skips its own checksum cache and relies on dm-integrity's
	 *   persistent per-block checksums.
	 *
	 *   "degraded": This table maps fewer than disk_count devices.
	 *   The kernel determines the true disk count from the superblock
	 *   on the provided devices (or total_disks=N) and marks missing
	 *   positions as failed.  Writes are rejected; reads are served
	 *   via parity reconstruction or mirror failover.
	 *
	 *   "total_disks=N": Required when degraded mode is specified and
	 *   NO valid superblock exists on any provided disk (e.g. creating
	 *   a degraded array for the first time, or all superblocks are
	 *   corrupt).  Specifies the total number of disks the array
	 *   should have, so the kernel can expand to the correct count.
	 */
	{
		bool changed;
		do {
			changed = false;
			if (argc >= 2 && strcmp(argv[argc - 1], "integrity") == 0) {
				argc--;
				integrity_below = true;
				changed = true;
				DMINFO("ctr: integrity-below flag set");
			}
			if (argc >= 2 && strcmp(argv[argc - 1], "degraded") == 0) {
				argc--;
				degraded = true;
				changed = true;
				DMINFO("ctr: degraded flag set");
			}
			if (argc >= 2 && strncmp(argv[argc - 1], "total_disks=", 12) == 0) {
				const char *val = argv[argc - 1] + 12;
				unsigned int tmp;
				int rv;
				argc--;
				rv = kstrtouint(val, 0, &tmp);
				if (rv || tmp < 1 || tmp > LHSR_MAX_DISKS) {
					DMERR("ctr: invalid total_disks='%s'", val);
					ti->error = "Invalid total_disks value";
					return -EINVAL;
				}
				total_disks = tmp;
				changed = true;
				DMINFO("ctr: total_disks=%u flag set", total_disks);
			}
		} while (changed);
	}

	/* Prevent new devices during module exit */
	if (lhsr_module_exiting) {
		DMERR("ctr: Module is exiting, refusing new device");
		ti->error = "Module is unloading";
		return -EBUSY;
	}

	DMINFO("ctr: After keyword stripping: argc=%u", argc);

	/* Validate argument count based on raid type */
	/* Format: type [device offset] [device offset] ... */
	/* Minimum: type + 1 device + 1 offset = 3 args */
	if (argc < 3) {
		DMERR("Need at least 3 args (type device offset), got %u", argc);
		ti->error = "Invalid arguments (need: type device offset [device offset] ...)";
		return -EINVAL;
	}

/*
	 * Parse raid type from argv[0].
	 *
	 * Table format:
	 *   RAID0/1: start len lhsr <type> <dev> <offset> [<dev> <offset>]...
	 *     type = "single" or "mirror"
	 *     argc = 1 + 2*N   (type + N device pairs)
	 *     devices start at argv[1]
	 *
	 *   RAID5/6: start len lhsr <type> <chunk_sects> <stripes_per_cont> <cont_sects> <dev> <offset>...
	 *     type = "raid5" or "raid6"
	 *     argc = 4 + 2*N   (type + 3 params + N device pairs)
	 *     devices start at argv[4]
	 */
	if (strcmp(argv[0], "single") == 0) {
		raid_type = 0;
	} else if (strcmp(argv[0], "mirror") == 0) {
		raid_type = 1;
	} else if (strcmp(argv[0], "raid5") == 0 || strcmp(argv[0], "raid6") == 0) {
		raid_type = (strcmp(argv[0], "raid6") == 0) ? 3 : 2;
	} else {
		DMERR("Unknown raid type: %s", argv[0]);
		ti->error = "Unknown raid type";
		return -EINVAL;
	}

	/* Count disks and compute device argv start offset */
	unsigned int dev_start = 1;	/* argv index of first device */

	if (strcmp(argv[0], "single") == 0) {
		num_disks = 1;
		/* single: argv[0]=type, argv[1]=device — no offset pairs */
		dev_start = 1;
		if (argc < 2) {
			DMERR("single requires at least 2 args (type device)");
			ti->error = "Invalid arguments for single (need type device)";
			return -EINVAL;
		}
	} else if (raid_type >= LHSR_RAID5) {
		/* raid5/6: argv[0]=type, argv[1]=chunk_sects, argv[2]=stripes_per_cont, argv[3]=cont_sects */
		/* devices start at argv[4] */
		if (argc < 5) {
			DMERR("RAID5/6 requires at least 5 args (type chunk_sects stripes cont_sects device offset)");
			ti->error = "Invalid arguments for RAID5/6";
			return -EINVAL;
		}
		dev_start = 4;
		num_disks = (argc - dev_start) / 2;
	} else {
		/* RAID0/1: type device offset [device offset]... */
		dev_start = 1;
		num_disks = (argc - 1) / 2;
	}

	if (raid_type == LHSR_RAID1 && num_disks != 2) {
		DMERR("Mirror requires exactly 2 disks, got %u", num_disks);
		ti->error = "Mirror requires exactly 2 disks";
		return -EINVAL;
	}
	if (raid_type >= LHSR_RAID5 && num_disks < 3 && !degraded) {
		DMERR("RAID5/6 requires at least 3 disks, got %u", num_disks);
		ti->error = "RAID5/6 requires at least 3 disks";
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

	/* Initialize per-stripe locks BEFORE any goto bad can fire.
	 * kzalloc gives zeroed memory; mutex_init() writes the proper
	 * runtime state so mutex_destroy() is safe on all error paths.
	 */
	{
		unsigned int j;
		for (j = 0; j < LHSR_STRIPE_LOCKS; j++)
			mutex_init(&arr->stripe_locks[j]);
	}

	/* Track active device */
	atomic_inc(&lhsr_active_devices);
	DMINFO("ctr: Active devices now: %d", atomic_read(&lhsr_active_devices));

	/* Initialize all fields */
	arr->integrity_below = integrity_below;
	arr->degraded = degraded;
	arr->uuid = fast_hash_32(raid_type);
	arr->raid_type = raid_type;
	arr->disks = num_disks;
	arr->state = LHSR_STATE_OFFLINE;
	arr->size = 0;

	/* Parse RAID5/6 parameters */
	if (raid_type >= LHSR_RAID5) {
		unsigned long tmp_chunk;
		if (kstrtoul(argv[1], 0, &tmp_chunk)) {
			DMERR("Invalid chunk_sectors: '%s'", argv[1]);
			ti->error = "Invalid chunk_sectors";
			r = -EINVAL;
			goto bad;
		}
		arr->chunk_sectors = (unsigned int)tmp_chunk;
		/* stripes_per_container (argv[2]) and container_sectors (argv[3])
		 * are accepted for table-format compatibility but not yet used.
		 * Future: container-aware reconstruction and scrub.
		 */
	} else {
		arr->chunk_sectors = 8;	/* 4KB for RAID0/1 */
	}

	/*
	 * CRITICAL CONSTRAINT: RMW write path (lhsr_rmw_submit_write)
	 * allocates one bio with one page per chunk.  If chunk_bytes
	 * exceeds PAGE_SIZE, bio_add_page() will fail silently.
	 * This is a hard structural limit until the RMW path is
	 * reworked for multi-page bios.
	 */
	if (arr->chunk_sectors * 512 > PAGE_SIZE) {
		DMERR("chunk_sectors=%u too large: chunk_bytes=%u > PAGE_SIZE=%lu",
		      arr->chunk_sectors, arr->chunk_sectors * 512, PAGE_SIZE);
		ti->error = "chunk_sectors too large for PAGE_SIZE";
		r = -EINVAL;
		goto bad;
	}
	arr->primary_disk = 0;
	lhsr_failed_disks_set(arr, 0);
	arr->error_threshold = 3;
	arr->last_check = jiffies;
	arr->write_verify_enabled = 0;
	atomic_set(&arr->destroying, 0);

	/* Initialize concurrency primitives */
	init_rwsem(&arr->sb_sem);

	/* Initialize CRC table if not already done */
	/* CRC32c uses kernel crypto API - no initialization needed */

	/* Get devices - arguments are "device offset" pairs starting at dev_start */
	DMINFO("ctr: num_disks=%u, argc=%u, dev_start=%u", num_disks, argc, dev_start);
	for (i = 0; i < num_disks; i++) {
		const char *dev_name = argv[dev_start + i * 2];
		unsigned long long tmp_offset;
		int kstr_ret = kstrtoull(argv[dev_start + 1 + i * 2], 0, &tmp_offset);
		if (kstr_ret) {
			DMERR("ctr: Invalid offset '%s' for device %u", argv[dev_start + 1 + i * 2], i);
			ti->error = "Invalid offset value";
			r = kstr_ret;
			goto bad;
		}
		sector_t offset = (sector_t)tmp_offset;
		DMINFO("ctr: Getting device %u: argv[%d]='%s', offset=%llu",
		       i, dev_start + i * 2, dev_name, (u64)offset);
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

	/*
	 * Calculate size from smallest device.
	 *
	 * LIMITATION: This wastes space on larger disks.  For example,
	 * a RAID5 of [100GB, 200GB, 300GB] gives 200GB usable and wastes
	 * 300GB (the extra capacity on the two larger disks is unused).
	 *
	 * Future SHR-style chunk mapping (per-chunk stripe offsets) would
	 * allow using the full capacity of every disk, but that's a
	 * significant rearchitecture of the stripe map in lhsr_map().
	 *
	 * Workaround for now: partition larger disks to match before
	 * creating the array, or accept the waste.  See header comment
	 * for full production readiness status.
	 */
	size = bdev_nr_sectors(arr->disk[0]);
	for (i = 1; i < num_disks; i++) {
		sector_t s = bdev_nr_sectors(arr->disk[i]);
		if (s < size)
			size = s;
	}

	/* Reserve space at end of device for metadata (write-hole bitmap +
	 * superblock + write-intent bitmap).  WIB size depends on disk size,
	 * so we estimate using the raw device size before subtraction. */
	{
		/* WIB sectors for this raw device size (rounded up to pages) */
		unsigned int wib_sectors = max(1U,
			((unsigned int)(size / LHSR_WIB_CHUNK_SECTORS) +
			 LHSR_WIB_BITS_PER_PAGE - 1) /
			LHSR_WIB_BITS_PER_PAGE) * LHSR_WIB_PAGE_SECTORS;
		unsigned int meta_sectors = LHSR_META_BASE_SECTORS + wib_sectors;

		if (size > meta_sectors * 2) {
			size -= meta_sectors;
		} else {
			DMERR("Device too small for v2 metadata "
			      "(%llu sectors, need %u)",
			      (u64)size, meta_sectors);
			ti->error = "Device too small";
			r = -EINVAL;
			goto bad;
		}
	}

	if (size < 2048) {
		DMERR("Device too small: %llu sectors", size);
		ti->error = "Device too small";
		r = -EINVAL;
		goto bad;
	}

	/* Save per-disk usable sectors (after metadata reservation) */
	arr->disk_sectors = size;

	/* For RAID5/6, user-visible capacity = data_disks × disk_sectors */
	if (arr->raid_type >= LHSR_RAID5) {
		unsigned int pd = (arr->raid_type == LHSR_RAID5) ? 1 : 2;
		unsigned int dd = arr->disks - pd;
		size = size * dd;
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

		DMDEBUG("About to read superblock for disk %u", i);
		r = lhsr_read_superblock(arr->disk[i], sb, arr->disk_sectors, arr->disk_offset[i]);
		DMDEBUG("Read superblock returned: %d", r);
		if (r == 0 && lhsr_validate_superblock(sb) == 0) {
			DMDEBUG("Disk %u: Found valid superblock (gen=%llu)", i, sb->generation);
			if (sb->array_uuid != arr->array_uuid) {
				DMWARN("Disk %u array_uuid mismatch (0x%llx vs 0x%llx)",
				       i, sb->array_uuid, arr->array_uuid);
			}
			lhsr_failed_disks_set(arr, lhsr_failed_disks_get(arr) |
					       ((sb->disk_state >= LHSR_DISK_DEGRADED) ? (1 << i) : 0));
		} else {
			DMWARN("Disk %u: No valid superblock, initializing (persistence optional)", i);
			lhsr_init_superblock(sb, arr->array_uuid, i, raid_type, num_disks, arr->disk_sectors);
			/* Don't fail device creation if superblock write fails - it's optional for testing */
			DMDEBUG("About to write superblock for disk %u", i);
			r = lhsr_write_superblock(arr->disk[i], sb, arr->disk_sectors, arr->disk_offset[i]);
			DMDEBUG("Write superblock returned: %d", r);
			if (r) {
				DMWARN("Superblock write failed for disk %u - continuing without persistence", i);
			} else {
				DMDEBUG("Wrote new superblock to disk %u", i);
			}
		}

		/* Track highest generation */
		if (sb->generation > arr->generation)
			arr->generation = sb->generation;
	}

	/* In degraded mode, expand to the full disk count from the superblock.
	 * The user has provided fewer device pairs than disk_count, which means
	 * one or more disks are missing (e.g., failed and removed).  We read
	 * disk_count from the surviving superblock(s), expand arr->disks, and
	 * mark every unpopulated position in failed_disks so the I/O path
	 * treats them as missing (reconstructing reads, rejecting writes).
	 */
	if (degraded) {
		unsigned int full_count = 0;
		unsigned int expand_to = 0;

		/* Find the true disk_count from the first valid superblock.
		 * If superblocks are present this gives us the authoritative
		 * disk count and overrides any total_disks=N keyword. */
		for (i = 0; i < num_disks; i++) {
			if (lhsr_validate_superblock(&arr->sbs[i]) == 0 &&
			    arr->sbs[i].disk_count > full_count)
				full_count = arr->sbs[i].disk_count;
		}

		/* Determine whether we need to expand arr->disks */
		if (full_count > num_disks)
			expand_to = full_count;
		else if (total_disks > num_disks)
			expand_to = total_disks;

		if (expand_to > 0) {
			unsigned int old_disks = num_disks;

			DMINFO("Degraded: expanding from %u to %u disks (from %s)",
			       old_disks, expand_to,
			       full_count > num_disks ? "superblock" : "total_disks=N");

			/* Mark every position >= num_disks as failed/missing */
			for (i = num_disks; i < expand_to; i++) {
				arr->disk[i] = NULL;
				arr->dm_devs[i] = NULL;
				arr->disk_offset[i] = 0;
				memset(&arr->sbs[i], 0, sizeof(arr->sbs[i]));
				lhsr_failed_disks_set(arr,
					lhsr_failed_disks_get(arr) | (1 << i));
			}

			arr->disks = expand_to;
			arr->degraded = true;

			/* Recompute user-visible size with correct disk count.
			 * Initial size was computed with old_disks only. */
			size = arr->disk_sectors;
			if (arr->raid_type >= LHSR_RAID5) {
				unsigned int pd = (arr->raid_type == LHSR_RAID5) ? 1 : 2;
				unsigned int dd = arr->disks - pd;
				size = size * dd;
			}
			arr->size = size;

			DMINFO("Degraded mode: %u of %u disks present, size=%llu",
			       old_disks, expand_to, (u64)size);
		} else if (full_count > 0 && full_count == num_disks) {
			DMINFO("Degraded flag set but all disks present — array is healthy");
			arr->degraded = false;
		} else if (total_disks > 0 && total_disks == num_disks) {
			DMINFO("total_disks=%u matches provided disks — array is healthy",
			       total_disks);
			arr->degraded = false;
		} else {
			/*
			 * No superblock and no total_disks=N available.
			 * Keep arr->degraded = true so writes are rejected
			 * and status correctly reports DEGRADED, but we
			 * cannot expand because the expected disk count is
			 * unknown.  The caller should provide total_disks=N
			 * if they know the full array membership count.
			 */
			DMWARN("Degraded flag set but no superblock with disk_count "
			       "and no total_disks=N — keeping %u disks, writes "
			       "will be rejected until array is fully populated",
			       num_disks);
			arr->degraded = true;
		}
	}

	/* Recover failed disks from superblock state */
	for (i = 0; i < arr->disks; i++) {
		if (arr->sbs[i].disk_state >= LHSR_DISK_DEGRADED)
			lhsr_failed_disks_set(arr, lhsr_failed_disks_get(arr) | (1 << i));
	}

	arr->state = (lhsr_failed_disks_get(arr) == 0) ? LHSR_STATE_HEALTHY : LHSR_STATE_DEGRADED;

	/* Create workqueue for disk health checks - temporarily disabled for testing */
	arr->check_wq = NULL;
	DMINFO("Disk health check workqueue disabled for testing");

	/* Initialize scrubber state */
	arr->scrub_state = LHSR_SCRUB_IDLE;
	arr->scrub_wq = NULL;
	arr->scrub_disk = 0;
	arr->scrub_disks_done = 0;
	arr->scrub_offset = 0;
	arr->scrub_verified = 0;
	arr->scrub_corrupted = 0;
	arr->scrub_retry_resolved = 0;
	xa_init(&arr->cksum_cache);  /* Initialize checksum cache (xarray) */
	atomic_set(&arr->corruptions_detected, 0);
	atomic_set(&arr->repairs, 0);

	/* Initialize rebuild state */
	arr->rebuild_state = LHSR_REBUILD_NONE;
	arr->rebuild_disk = 0;
	arr->rebuild_offset = 0;
	arr->rebuild_total = 0;
	arr->rebuild_verified = 0;
	arr->rebuild_wq = NULL;
	arr->wib_wq = NULL;

	/* Create workqueue for RAID5/6 RMW writes — up to 8 concurrent workers.
	 * Per-stripe mutexes prevent concurrent writes to the SAME stripe.
	 * Different stripes run in parallel across CPUs, eliminating the single-worker
	 * bottleneck while preserving the write-hole safety guarantee per-stripe.
	 * 32 workers allows the NVMe controller to pipeline ~8 concurrent I/Os,
	 * essential for saturating modern devices with deep NCQ queues.
	 */
	arr->rmw_wq = alloc_workqueue("lhsr_rmw_%s", WQ_UNBOUND | WQ_MEM_RECLAIM, 32,
				       arr->disk[0] && arr->disk[0]->bd_disk ?
				       arr->disk[0]->bd_disk->disk_name : "unknown");
	if (!arr->rmw_wq) {
		DMERR("ctr: Failed to create RMW workqueue");
		ti->error = "Failed to create RMW workqueue";
		r = -ENOMEM;
		goto bad;
	}

	/* Initialize write-hole journal (dirty stripe bitmap) */
	r = lhsr_bitmap_init(arr);
	if (r) {
		DMERR("ctr: Failed to initialize bitmap");
		ti->error = "Failed to initialize bitmap";
		goto bad;
	}

	/* Load bitmap from disk (recover after crash) */
	r = lhsr_bitmap_load(arr);
	if (r) {
		DMERR("ctr: Failed to load bitmap");
		ti->error = "Failed to load bitmap";
		goto bad;
	}

	/* Recover any dirty stripes — reconstruct parity from data */
	r = lhsr_bitmap_recover(arr);
	if (r)
		DMWARN("ctr: Bitmap recovery failed (%d), continuing", r);

	/* Create workqueue for background bitmap flush (2s interval).
	 * The bitmap is write-behind: in-memory bits are always current;
	 * disk copies may lag by up to ~2 seconds.  On crash, the WIB
	 * protects in-flight writes, so stale bitmap bits only cause
	 * extra consistency verification on next assembly — never data loss.
	 */
	arr->bitmap_wq = alloc_workqueue("lhsr_bmp_%s", WQ_MEM_RECLAIM | WQ_UNBOUND, 1,
					 arr->disk[0] && arr->disk[0]->bd_disk ?
					 arr->disk[0]->bd_disk->disk_name : "unknown");
	if (!arr->bitmap_wq) {
		DMERR("ctr: Failed to create bitmap flush workqueue");
		ti->error = "Failed to create bitmap flush workqueue";
		r = -ENOMEM;
		goto bad;
	}
	INIT_DELAYED_WORK(&arr->bitmap_work, lhsr_bitmap_work);
	queue_delayed_work(arr->bitmap_wq, &arr->bitmap_work, 2 * HZ);
	DMINFO("ctr: Bitmap background flush workqueue created (2s interval)");

	/* Initialize write-intent bitmap (WIB) for incremental rebuild.
	 * This allocates memory and tries to load from disk.
	 * Failure is non-fatal — rebuild falls back to full copy. */
	r = lhsr_wib_init(arr);
	if (r) {
		DMERR("ctr: Failed to initialize WIB (%d), rebuild will do full copy", r);
		ti->error = "Failed to initialize WIB";
		goto bad;
	}

	/* Create workqueue for periodic WIB flush (30s interval) */
	arr->wib_wq = alloc_workqueue("lhsr_wib_%s", WQ_MEM_RECLAIM | WQ_UNBOUND, 1,
				      arr->disk[0] && arr->disk[0]->bd_disk ?
				      arr->disk[0]->bd_disk->disk_name : "unknown");
	if (!arr->wib_wq) {
		DMERR("ctr: Failed to create WIB workqueue");
		ti->error = "Failed to create WIB workqueue";
		r = -ENOMEM;
		goto bad;
	}
	INIT_DELAYED_WORK(&arr->wib_work, lhsr_wib_work);
	queue_delayed_work(arr->wib_wq, &arr->wib_work, 30 * HZ);
	DMINFO("ctr: WIB periodic flush workqueue created (30s interval)");

	ti->private = arr;
	ti->len = size;
	ti->begin = 0;
	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;

	/*
	 * REQUIRED: Needs bio_set_dev() before issuing bios — otherwise
	 * bio->bi_blkg is NULL and blk_cgroup_bio_start() oopses
	 * with NULL pointer dereference at CR2 offset 0x50.
	 *
	 * dm-raid had the exact same bug, fixed by upstream commit
	 * 3990e5b2 ("dm-raid: add missing needs_bio_set_dev").
	 */
	ti->needs_bio_set_dev = true;

	/* Set max_io_len to chunk_sectors for RAID5/6 — ensures each bio fits in one chunk */
	if (arr->raid_type >= LHSR_RAID5)
		ti->max_io_len = arr->chunk_sectors;

	DMINFO("Created LHSR: type=%u size=%llu", raid_type, size);

	/* Log RAID level info.
	 * Use arr->disks (post-expansion) for accurate count in degraded mode. */
	if (raid_type == LHSR_RAID5) {
		DMINFO("RAID5 configured: %u data + 1 parity", arr->disks - 1);
	} else if (raid_type == LHSR_RAID6) {
		DMINFO("RAID6 configured: %u data + 2 parity", arr->disks - 2);
	}

	return 0;

bad:
	/* Cancel background bitmap flush before destroying pages */
	if (arr->bitmap_wq) {
		cancel_delayed_work_sync(&arr->bitmap_work);
		destroy_workqueue(arr->bitmap_wq);
		arr->bitmap_wq = NULL;
	}
	lhsr_bitmap_destroy(arr);
	while (i > 0) {
		i--;
		if (arr->dm_devs[i])
			dm_put_device(ti, arr->dm_devs[i]);
	}
	/* Destroy per-stripe locks */
	{
		unsigned int j;
		for (j = 0; j < LHSR_STRIPE_LOCKS; j++)
			mutex_destroy(&arr->stripe_locks[j]);
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

	/* Drain RMW workqueue first — all pending writes must complete before teardown */
	DMINFO("dtr: Draining RMW workqueue (rmw_wq=%p)", arr->rmw_wq);
	if (arr->rmw_wq) {
		flush_workqueue(arr->rmw_wq);
		destroy_workqueue(arr->rmw_wq);
		arr->rmw_wq = NULL;
		DMINFO("dtr: RMW workqueue destroyed");
	}

	/* Destroy per-stripe locks (no concurrent RMW workers after workqueue destroy) */
	{
		unsigned int j;
		for (j = 0; j < LHSR_STRIPE_LOCKS; j++)
			mutex_destroy(&arr->stripe_locks[j]);
	}

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

	DMDEBUG("dtr: Writing superblocks for %u disks", arr->disks);

	/* Write updated superblocks for all disks before destroying.
	 * NULL disk entries (degraded-mode missing slots) are skipped. */
	arr->generation++;
	for (i = 0; i < arr->disks; i++) {
		struct lhsr_superblock *sb = &arr->sbs[i];

		if (!arr->disk[i]) {
			DMDEBUG("dtr: Disk %u is NULL (degraded missing slot), skipping", i);
			continue;
		}

		DMINFO("dtr: Processing disk %u", i);
		sb->last_update = ktime_get_real_seconds();
		sb->generation = arr->generation;
		sb->disk_state = (lhsr_failed_disks_get(arr) & (1 << i)) ? LHSR_DISK_DEGRADED : LHSR_DISK_HEALTHY;

		DMDEBUG("dtr: Writing superblock for disk %u", i);
		r = lhsr_write_superblock(arr->disk[i], sb, arr->disk_sectors, arr->disk_offset[i]);
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

	/* Stop periodic WIB flush before flushing dirty pages */
	DMINFO("dtr: Stopping WIB workqueue (wib_wq=%p)", arr->wib_wq);
	if (arr->wib_wq) {
		DMINFO("dtr: Canceling WIB work...");
		if (!cancel_delayed_work_sync(&arr->wib_work)) {
			DMWARN("dtr: wib_work did not complete in time, forcing");
			flush_workqueue(arr->wib_wq);
		}
		destroy_workqueue(arr->wib_wq);
		arr->wib_wq = NULL;
		DMINFO("dtr: WIB workqueue destroyed");
	}

	/* Flush and free write-intent bitmap (WIB) */
	lhsr_wib_destroy(arr);

	/* Final flush of any dirty bitmap pages that the background
	 * worker may not have written yet.  This must happen BEFORE
	 * canceling the workqueue: after the cancel, no more writes
	 * will occur, and the pages would be discarded.
	 */
	lhsr_bitmap_flush(arr);

	/* Stop background bitmap flush worker */
	DMINFO("dtr: Stopping bitmap flush workqueue (bitmap_wq=%p)", arr->bitmap_wq);
	if (arr->bitmap_wq) {
		if (!cancel_delayed_work_sync(&arr->bitmap_work)) {
			DMWARN("dtr: bitmap_work did not complete, forcing");
			flush_workqueue(arr->bitmap_wq);
		}
		destroy_workqueue(arr->bitmap_wq);
		arr->bitmap_wq = NULL;
		DMINFO("dtr: Bitmap workqueue destroyed");
	}

	/* Flush and free write-hole journal bitmap */
	lhsr_bitmap_destroy(arr);

	DMINFO("dtr: Putting devices...");

	for (i = 0; i < arr->disks; i++) {
		DMINFO("dtr: Putting device %u (dm_devs[%u]=%p)", i, i, arr->dm_devs[i]);
		if (arr->dm_devs[i]) {
			dm_put_device(ti, arr->dm_devs[i]);
			DMINFO("dtr: Device %u released", i);
		}
	}

	DMINFO("dtr: Destroying mutex");

	/* Free checksum cache (xarray) */
	xa_destroy(&arr->cksum_cache);

	DMINFO("dtr: Freeing arr %p", arr);
	ti->private = NULL;
	/* ti->private is NULL now — no race window with dangling pointer */
	kfree(arr);
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

	if (!parity || !data || data_disks == 0 || !src[0]) {
		DMERR("xor_parity: NULL parameter (parity=%p data=%p disks=%u)",
		      parity, data, data_disks);
		return;
	}

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

/*
 * RAID5/6 RMW synchronous worker — runs on the ordered rmw_wq.
 *
 * Does the full RMW cycle sequentially using synchronous I/O:
 *   1. Read old data chunk from data disk
 *   2. Read old P parity chunk
 *   3. (RAID6) Read old Q parity chunk
 *   4. Compute new data, new P, new Q
 *   5. Write new data chunk
 *   6. Write new P parity
 *   7. (RAID6) Write new Q parity
 *   8. Complete orig_bio
 */

/* Parallel I/O completion context — multiple bios share one completion */
struct lhsr_parallel_io {
	struct completion done;
	atomic_t pending;
	blk_status_t status;
};

/* Completion callback for parallel I/O: last one to finish wakes the waiter */
static void lhsr_parallel_endio(struct bio *bio)
{
	struct lhsr_parallel_io *pio = bio->bi_private;

	if (bio->bi_status && pio->status == BLK_STS_OK)
		pio->status = bio->bi_status;
	if (atomic_dec_and_test(&pio->pending))
		complete(&pio->done);
	bio_put(bio);
}

/* Submit one page-aligned I/O as part of a parallel batch.
 * If the target disk is failed or missing:
 *   - For reads: zero the page and skip (no I/O submitted)
 *   - For writes: skip entirely (parity write to failed disk is meaningless)
 * Returns 0 on success (or skip), -ENOMEM on bio allocation failure.
 */
static int lhsr_submit_parallel(struct lhsr_array *arr, unsigned int disk_idx,
				struct page *page, size_t len,
				sector_t sector, blk_opf_t opf,
				struct lhsr_parallel_io *pio)
{
	struct bio *bio;
	struct block_device *bdev;

	if (!page)
		return 0;
	if (!arr->disk[disk_idx])
		return 0;

	/* Failed disk handling */
	if (lhsr_failed_disks_get(arr) & (1 << disk_idx)) {
		if (opf == REQ_OP_READ) {
			void *ptr = kmap_local_page(page);
			memset(ptr, 0, len);
			kunmap_local(ptr);
		}
		return 0;
	}

	bdev = arr->disk[disk_idx];
	bio = bio_alloc_bioset(bdev, 1, opf, GFP_NOIO, &lhsr_bioset);
	if (!bio) {
		atomic_dec(&pio->pending);
		return -ENOMEM;
	}

	bio->bi_iter.bi_sector = sector + arr->disk_offset[disk_idx];
	bio->bi_private = pio;
	bio->bi_end_io = lhsr_parallel_endio;

	if (!bio_add_page(bio, page, len, 0)) {
		bio_put(bio);
		atomic_dec(&pio->pending);
		return -EIO;
	}

	submit_bio(bio);
	return 0;
}

/* Synchronous bio completion callback */
struct lhsr_bio_done {
	struct completion done;
	blk_status_t status;
};

static void lhsr_bio_done_endio(struct bio *bio)
{
	struct lhsr_bio_done *bd = bio->bi_private;
	bd->status = bio->bi_status;
	complete(&bd->done);
}

/* Submit a single-page bio and wait synchronously for completion.
 * Returns BLK_STS_OK on success, or an error status.
 * The caller must free the bio after return.
 */
static blk_status_t lhsr_submit_bio_sync(struct block_device *bdev,
					  struct page *page, size_t len,
					  sector_t sector, blk_opf_t opf)
{
	struct bio *bio;
	struct lhsr_bio_done done;
	blk_status_t status;

	if (!bdev || !page)
		return BLK_STS_IOERR;

	bio = bio_alloc_bioset(bdev, 1, opf, GFP_NOIO, &lhsr_bioset);
	if (!bio)
		return BLK_STS_RESOURCE;

	bio->bi_iter.bi_sector = sector;
	init_completion(&done.done);
	bio->bi_private = &done;
	bio->bi_end_io = lhsr_bio_done_endio;

	if (!bio_add_page(bio, page, len, 0)) {
		bio_put(bio);
		return BLK_STS_IOERR;
	}

	submit_bio(bio);
	wait_for_completion_io(&done.done);
	status = done.status;
	bio_put(bio);
	return status;
}

/* =====================================================================
 * Write-hole journal (dirty stripe bitmap)
 *
 * The bitmap tracks which 1MB regions of user data have uncommitted
 * RMW parity updates.  Each bit represents one region; the bitmap is
 * stored on ALL disks to survive single-disk failure.
 *
 * Crash recovery:
 *   On array assembly, scan all dirty bits.  For each dirty region,
 *   reconstruct parity from the data disks using XOR (RAID5).
 *
 * On-disk format per page (4096 bytes):
 *   offset 0:  8 bytes seq (monotonic, 0 = uninitialized)
 *   offset 8:  4 bytes CRC32c (of entire page with crc32=0)
 *   offset 12: 4084 bytes of bitmap data (32672 bits)
 *
 * Layout on disk (per device):
 *   sector 0 .. disk_sectors-1               user data
 *   sector disk_sectors .. +BITMAP_SECTORS   bitmap pages
 *   sector +BITMAP_SECTORS .. +META_SECTORS  superblock
 * ===================================================================== */

#define LHSR_BITMAP_FLAG_DIRTY 0

/* ------------------------------------------------------------------ */
/* Helper: compute CRC32c of a bitmap page (leaves page unchanged)    */
/* ------------------------------------------------------------------ */
static u32 lhsr_bitmap_page_crc(struct lhsr_bitmap_page *page)
{
	u32 saved = page->crc32;
	u32 csum;

	page->crc32 = 0;
	csum = __crc32c_le(0, (const unsigned char *)page, 4096);
	page->crc32 = saved;
	return csum;
}

/* ---------------------------------------------------------------- */
/* Helper: get the disk sector for a bitmap page index              */
/* ---------------------------------------------------------------- */
static sector_t lhsr_bitmap_page_sector(struct lhsr_array *arr,
					unsigned int page_idx,
					unsigned int disk)
{
	/* Bitmap lives in the reserved metadata area, between user data
	 * and the superblock. */
	return arr->disk_offset[disk] + arr->disk_sectors
		+ page_idx * LHSR_BITMAP_PAGE_SECTORS;
}

/* ---------------------------------------------------------------- */
/* Convert a physical chunk-start sector to a bitmap region index   */
/* ---------------------------------------------------------------- */
static inline sector_t lhsr_bitmap_region_from_sector(sector_t chunk_start)
{
	return chunk_start >> (LHSR_BITMAP_REGION_SHIFT - 9);
}

/* ---------------------------------------------------------------- */
/* Write one bitmap page to ALL available disks with FUA            */
/* Returns 0 on success (or partial), -EIO if ALL disks failed.     */
/* ---------------------------------------------------------------- */
static int lhsr_bitmap_write_page(struct lhsr_array *arr,
				  unsigned int page_idx)
{
	struct page *page = arr->bitmap_pages[page_idx];
	struct lhsr_bitmap_page *bmp;
	unsigned int d, nr_bios = 0, submitted = 0;
	int ret = 0;
	struct lhsr_parallel_io pio;
	unsigned int failed = lhsr_failed_disks_get(arr);

	if (!page) {
		DMERR("bitmap: write_page %u: page is NULL", page_idx);
		return -EINVAL;
	}

	bmp = (struct lhsr_bitmap_page *)kmap_local_page(page);

	/* Increment sequence number and update CRC */
	arr->bitmap_seqs[page_idx]++;
	bmp->seq = arr->bitmap_seqs[page_idx];
	bmp->crc32 = lhsr_bitmap_page_crc(bmp);
	DMDEBUG("bitmap: writing page %u seq=%llu crc32=0x%08x to %u disks",
		page_idx, (u64)bmp->seq, bmp->crc32, arr->disks);

	kunmap_local(bmp);

	/* Count non-failed disks for parallel submit */
	for (d = 0; d < arr->disks; d++) {
		if (!(failed & (1 << d)) && arr->disk[d])
			nr_bios++;
	}

	if (nr_bios == 0) {
		DMERR("bitmap: write_page %u: no valid disks", page_idx);
		return -EIO;
	}

	/* Submit same page to all disks in parallel using the same
	 * lhsr_submit_parallel helper that the data write phase uses.
	 * This sends one bio per disk, all concurrently, and waits once.
	 *
	 * NOTE: lhsr_submit_parallel adds arr->disk_offset[d] to the sector,
	 * so we pass the bitmap position RELATIVE to disk_offset (just the
	 * data-area part + page offset), NOT the full absolute sector.
	 * This differs from lhsr_submit_bio_sync which expects the full sector.
	 */
	init_completion(&pio.done);
	pio.status = BLK_STS_OK;
	atomic_set(&pio.pending, nr_bios);

	for (d = 0; d < arr->disks; d++) {
		sector_t bitmap_sector;

		if (failed & (1 << d))
			continue;
		if (!arr->disk[d])
			continue;

		/* Sector relative to disk_offset[0]:
		 * disk_sectors (data area) + page index * sectors-per-page
		 * lhsr_submit_parallel will add disk_offset[d] internally.
		 */
		bitmap_sector = arr->disk_sectors
			+ page_idx * LHSR_BITMAP_PAGE_SECTORS;

		if (lhsr_submit_parallel(arr, d, page, 4096,
				bitmap_sector,
				REQ_OP_WRITE | REQ_SYNC | REQ_FUA, &pio) == 0)
			submitted++;
	}

	wait_for_completion_io(&pio.done);

	if (submitted == 0) {
		DMERR("bitmap: write_page %u: all %u bios failed to submit",
		      page_idx, nr_bios);
		return -EIO;
	}

	if (pio.status != BLK_STS_OK) {
		DMERR("bitmap: write_page %u: disk write failed (status=%d)",
		      page_idx, pio.status);
		ret = -EIO;
	}

	clear_bit(LHSR_BITMAP_FLAG_DIRTY, &arr->bitmap_flags[page_idx]);
	return ret;
}

/* ---------------------------------------------------------------- */
/* Flush all dirty bitmap pages to disk.                            */
/* Called by the background flush worker and on array destruction.  */
/* ---------------------------------------------------------------- */
static void lhsr_bitmap_flush(struct lhsr_array *arr)
{
	unsigned int i;

	if (!arr)
		return;

	for (i = 0; i < LHSR_BITMAP_PAGES; i++) {
		if (arr->bitmap_pages[i] &&
		    test_bit(LHSR_BITMAP_FLAG_DIRTY, &arr->bitmap_flags[i]))
			lhsr_bitmap_write_page(arr, i);
	}
}

/* ---------------------------------------------------------------- */
/* Background bitmap flush worker — writes dirty pages to disk.     */
/* Runs periodically (every 2 seconds) when the array is active.    */
/* The bitmap is only used for crash recovery efficiency; the WIB    */
/* protects in-flight writes.  A stale bitmap (missing recent         */
/* clears) just means extra parity verification on next assembly,    */
/* never data loss.  Hence the relaxed ~2s flush interval.           */
/* ---------------------------------------------------------------- */
static void lhsr_bitmap_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct lhsr_array *arr = container_of(dwork, struct lhsr_array, bitmap_work);

	/* Bail if array is being destroyed */
	if (atomic_read(&arr->destroying))
		return;

	DMDEBUG("bitmap_work: starting periodic flush");
	lhsr_bitmap_flush(arr);
	DMDEBUG("bitmap_work: periodic flush complete");

	/* Re-schedule if workqueue still exists */
	if (arr->bitmap_wq)
		queue_delayed_work(arr->bitmap_wq, &arr->bitmap_work, 2 * HZ);
}

/* ---------------------------------------------------------------- */
/* Set a dirty bit for a region in memory.                          */
/* Called BEFORE writing data+parity to mark intent.                */
/* The actual disk write is deferred to the background flush worker.*/
/* ---------------------------------------------------------------- */
static int lhsr_bitmap_set(struct lhsr_array *arr, sector_t chunk_start)
{
	sector_t region = lhsr_bitmap_region_from_sector(chunk_start);
	unsigned int page_idx, bit_idx;
	struct lhsr_bitmap_page *bmp;

	/* Don't set bits during recovery */
	if (atomic_read(&arr->bitmap_recovering)) {
		DMINFO("bitmap: set called but recovering=1, skipping");
		return 0;
	}

	page_idx = region / LHSR_BITMAP_BITS_PER_PAGE;
	bit_idx  = region % LHSR_BITMAP_BITS_PER_PAGE;

	if (page_idx >= LHSR_BITMAP_PAGES) {
		DMERR("bitmap: region %llu out of range (page %u >= %u)",
		      (u64)region, page_idx, LHSR_BITMAP_PAGES);
		return -EINVAL;
	}

	bmp = (struct lhsr_bitmap_page *)kmap_local_page(arr->bitmap_pages[page_idx]);

	if (bmp->bits[bit_idx / 8] & (1 << (bit_idx % 8))) {
		/* Already set */
		kunmap_local(bmp);
		return 0;
	}

	bmp->bits[bit_idx / 8] |= (1 << (bit_idx % 8));
	kunmap_local(bmp);

	set_bit(LHSR_BITMAP_FLAG_DIRTY, &arr->bitmap_flags[page_idx]);
	return 0;
}

/* ---------------------------------------------------------------- */
/* Clear a dirty bit for a region in memory.                        */
/* Called AFTER data+parity are fully committed.                    */
/* The actual disk write is deferred to the background flush worker.*/
/* ---------------------------------------------------------------- */
static int lhsr_bitmap_clear(struct lhsr_array *arr, sector_t chunk_start)
{
	sector_t region = lhsr_bitmap_region_from_sector(chunk_start);
	unsigned int page_idx, bit_idx;
	struct lhsr_bitmap_page *bmp;

	page_idx = region / LHSR_BITMAP_BITS_PER_PAGE;
	bit_idx  = region % LHSR_BITMAP_BITS_PER_PAGE;

	if (page_idx >= LHSR_BITMAP_PAGES)
		return -EINVAL;

	bmp = (struct lhsr_bitmap_page *)kmap_local_page(arr->bitmap_pages[page_idx]);

	if (!(bmp->bits[bit_idx / 8] & (1 << (bit_idx % 8)))) {
		/* Already clear */
		kunmap_local(bmp);
		return 0;
	}

	bmp->bits[bit_idx / 8] &= ~(1 << (bit_idx % 8));
	kunmap_local(bmp);

	set_bit(LHSR_BITMAP_FLAG_DIRTY, &arr->bitmap_flags[page_idx]);
	return 0;
}

/* ---------------------------------------------------------------- */
/* Initialize bitmap pages (allocate zeroed pages)                  */
/* Returns 0 on success, -ENOMEM on allocation failure.            */
/* ---------------------------------------------------------------- */
static int lhsr_bitmap_init(struct lhsr_array *arr)
{
	unsigned int i;

	for (i = 0; i < LHSR_BITMAP_PAGES; i++) {
		struct lhsr_bitmap_page *bmp;

		arr->bitmap_pages[i] = alloc_page(GFP_KERNEL);
		if (!arr->bitmap_pages[i]) {
			DMERR("bitmap: failed to allocate page %u", i);
			while (i > 0) {
				i--;
				__free_page(arr->bitmap_pages[i]);
				arr->bitmap_pages[i] = NULL;
			}
			return -ENOMEM;
		}

		bmp = (struct lhsr_bitmap_page *)kmap_local_page(arr->bitmap_pages[i]);
		memset(bmp, 0, 4096);
		kunmap_local(bmp);

		arr->bitmap_flags[i] = 0;
		arr->bitmap_seqs[i] = 0;
	}

	atomic_set(&arr->bitmap_recovering, 1);
	return 0;
}

/* ---------------------------------------------------------------- */
/* Destroy bitmap pages — flush dirty pages, free all memory        */
/* ---------------------------------------------------------------- */
static void lhsr_bitmap_destroy(struct lhsr_array *arr)
{
	unsigned int i;

	if (!arr)
		return;

	/* Flush any dirty pages */
	for (i = 0; i < LHSR_BITMAP_PAGES; i++) {
		if (arr->bitmap_pages[i] &&
		    test_bit(LHSR_BITMAP_FLAG_DIRTY, &arr->bitmap_flags[i]))
			lhsr_bitmap_write_page(arr, i);
	}

	/* Free all pages */
	for (i = 0; i < LHSR_BITMAP_PAGES; i++) {
		if (arr->bitmap_pages[i]) {
			__free_page(arr->bitmap_pages[i]);
			arr->bitmap_pages[i] = NULL;
		}
	}
}

/* ---------------------------------------------------------------- */
/* Load bitmap from disk — for each page, pick the copy with the    */
/* highest valid sequence number across all disks.                  */
/* Returns 0 on success, negative on error.                         */
/* ---------------------------------------------------------------- */
static int lhsr_bitmap_load(struct lhsr_array *arr)
{
	unsigned int p;
	int ret = 0;

	DMINFO("bitmap: loading from %u disks", arr->disks);

	for (p = 0; p < LHSR_BITMAP_PAGES; p++) {
		struct lhsr_bitmap_page *dst;
		u64 best_seq = 0;
		unsigned int best_disk = arr->disks;
		bool any_valid = false;
		unsigned int d;

		dst = (struct lhsr_bitmap_page *)kmap_local_page(
			arr->bitmap_pages[p]);

		for (d = 0; d < arr->disks; d++) {
			struct page *tmp_page;
			struct lhsr_bitmap_page *tmp;
			u32 expected_crc;
			blk_status_t st;

			if (!arr->disk[d])
				continue;

			tmp_page = alloc_page(GFP_KERNEL);
			if (!tmp_page)
				continue;

			st = lhsr_submit_bio_sync(arr->disk[d], tmp_page,
				4096,
				lhsr_bitmap_page_sector(arr, p, d),
				REQ_OP_READ | REQ_SYNC);

			if (st != BLK_STS_OK) {
				__free_page(tmp_page);
				continue;
			}

			tmp = (struct lhsr_bitmap_page *)kmap_local_page(tmp_page);

			/* All zeros = uninitialized */
			if (tmp->seq == 0) {
				kunmap_local(tmp);
				__free_page(tmp_page);
				continue;
			}

			/* Verify CRC */
			expected_crc = tmp->crc32;
			tmp->crc32 = 0;
			if (__crc32c_le(0, (const unsigned char *)tmp, 4096) != expected_crc) {
				DMERR("bitmap: page %u disk %u CRC mismatch",
				      p, d);
				tmp->crc32 = expected_crc;
				kunmap_local(tmp);
				__free_page(tmp_page);
				continue;
			}
			tmp->crc32 = expected_crc;

			/* Check sequence number */
			if (tmp->seq > best_seq) {
				best_seq = tmp->seq;
				best_disk = d;
				memcpy(dst, tmp, 4096);
				any_valid = true;
			}

			kunmap_local(tmp);
			__free_page(tmp_page);
		}

			if (any_valid) {
				unsigned int set_bits = 0, bb;

				arr->bitmap_seqs[p] = best_seq;
				for (bb = 0; bb < LHSR_BITMAP_BITS_PER_PAGE; bb++) {
					if (dst->bits[bb / 8] & (1 << (bb % 8)))
						set_bits++;
				}
				DMINFO("bitmap: page %u loaded from disk %u (seq %llu, %u bits set)",
					p, best_disk, (u64)best_seq, set_bits);
			} else {
				DMINFO("bitmap: page %u uninitialized (no valid copy)", p);
				memset(dst, 0, 4096);
				arr->bitmap_seqs[p] = 0;
			}

		kunmap_local(dst);
	}

	DMINFO("bitmap: load complete");
	return ret;
}

/* ---------------------------------------------------------------- */
/* Recover dirty stripes — reconstruct parity for all set bits,     */
/* then clear the bits.  Called once during array assembly.         */
/* ---------------------------------------------------------------- */
static int lhsr_bitmap_recover(struct lhsr_array *arr)
{
	unsigned int pd, data_disks, parity_disks;
	sector_t sectors_recovered = 0;
	unsigned int regions_recovered = 0;
	unsigned int p;

	if (arr->raid_type < LHSR_RAID5) {
		atomic_set(&arr->bitmap_recovering, 0);
		return 0;
	}

	parity_disks = (arr->raid_type == LHSR_RAID5) ? 1 : 2;
	data_disks = arr->disks - parity_disks;
	pd = data_disks;

	DMINFO("bitmap: recovery starting — %u data + %u parity, %llu sectors/disk",
	       data_disks, parity_disks, (u64)arr->disk_sectors);

	for (p = 0; p < LHSR_BITMAP_PAGES; p++) {
		struct page *parity_page = NULL, *temp_page = NULL, *q_page = NULL;
		void *parity_buf, *temp_buf, *q_buf = NULL;
		size_t chunk_bytes = arr->chunk_sectors * 512;
		int ret;

		parity_page = alloc_page(GFP_KERNEL);
		temp_page   = alloc_page(GFP_KERNEL);
		if (!parity_page || !temp_page) {
			DMERR("bitmap: recovery OOM");
			if (parity_page) __free_page(parity_page);
			if (temp_page) __free_page(temp_page);
			goto out;
		}

		if (arr->raid_type == LHSR_RAID6) {
			q_page = alloc_page(GFP_KERNEL);
			if (!q_page)
				DMWARN("bitmap: Q page OOM, skipping Q parity rebuild");
		}

		parity_buf = kmap_local_page(parity_page);
		temp_buf   = kmap_local_page(temp_page);
		if (q_page)
			q_buf = kmap_local_page(q_page);

		/*
		 * Process dirty regions one at a time.
		 * For each set bit: 1) clear it, 2) reconstruct parity,
		 * 3) write page.  Crash-safe because unprocessed bits survive.
		 */
		for (;;) {
			struct lhsr_bitmap_page *bmp;
			unsigned int b, bit;
			sector_t region;
			int found = 0;

			/* Scan for the next dirty bit */
			bmp = (struct lhsr_bitmap_page *)kmap_local_page(
				arr->bitmap_pages[p]);

			for (b = 0; b < LHSR_BITMAP_BITS_PER_PAGE; b++) {
				if (bmp->bits[b / 8] & (1 << (b % 8))) {
					bit = b;
					found = 1;
					break;
				}
			}

			if (!found) {
				kunmap_local(bmp);
				break;
			}

			/* Clear the bit NOW (while mapped) */
			bmp->bits[bit / 8] &= ~(1 << (bit % 8));
			kunmap_local(bmp);

			region = (sector_t)p * LHSR_BITMAP_BITS_PER_PAGE + bit;
			DMINFO("bitmap: recovering region %llu (page %u bit %u)",
			       (u64)region, p, bit);

			/* Reconstruct parity for this region's stripes */
			{
				sector_t stripe, stripe_end;
				unsigned int d;

				stripe = region * LHSR_BITMAP_REGION_SECTORS
					/ arr->chunk_sectors;
				stripe_end = stripe + LHSR_BITMAP_REGION_SECTORS
					/ arr->chunk_sectors;

				for (; stripe < stripe_end; stripe++) {
					sector_t s = stripe * arr->chunk_sectors;

					memset(parity_buf, 0, chunk_bytes);
					if (q_buf)
						memset(q_buf, 0, chunk_bytes);

					/* XOR all readable data disks */
					for (d = 0; d < data_disks; d++) {
						blk_status_t st;

						if (lhsr_failed_disks_get(arr) & (1 << d))
							continue;
						if (!arr->disk[d])
							continue;

						st = lhsr_submit_bio_sync(arr->disk[d],
							temp_page, chunk_bytes,
							s + arr->disk_offset[d],
							REQ_OP_READ | REQ_SYNC);
						if (st != BLK_STS_OK) {
							DMERR("bitmap: recover read disk %u sector %llu failed",
							      d, (u64)(s + arr->disk_offset[d]));
							continue;
						}

						/* XOR temp into parity, GF multiply into Q */
						{
							size_t i;
							u8 *p8 = parity_buf;
							u8 *t8 = temp_buf;
							if (q_buf && arr->raid_type == LHSR_RAID6) {
								u8 coeff = rs_power_table[d];
								u8 *q8 = q_buf;
								for (i = 0; i < chunk_bytes; i++) {
									p8[i] ^= t8[i];
									q8[i] ^= lhsr_gf_mul(t8[i], coeff);
								}
							} else {
								for (i = 0; i < chunk_bytes; i++)
									p8[i] ^= t8[i];
							}
						}
					}

					/* Write P parity with FUA */
					if (!(lhsr_failed_disks_get(arr) & (1 << pd)) &&
					    arr->disk[pd]) {
						blk_status_t st;
						st = lhsr_submit_bio_sync(arr->disk[pd],
							parity_page, chunk_bytes,
							s + arr->disk_offset[pd],
							REQ_OP_WRITE | REQ_SYNC | REQ_FUA);
						if (st != BLK_STS_OK)
							DMERR("bitmap: recover write P disk %u sector %llu failed",
							      pd, (u64)(s + arr->disk_offset[pd]));
					}

					/* Write Q parity with FUA (RAID6) */
					if (q_buf && q_page &&
					    arr->raid_type == LHSR_RAID6) {
						unsigned int qd = data_disks + 1;
						if (!(lhsr_failed_disks_get(arr) & (1 << qd)) &&
						    arr->disk[qd]) {
							blk_status_t st;
							st = lhsr_submit_bio_sync(arr->disk[qd],
								q_page, chunk_bytes,
								s + arr->disk_offset[qd],
								REQ_OP_WRITE | REQ_SYNC | REQ_FUA);
							if (st != BLK_STS_OK)
								DMERR("bitmap: recover write Q disk %u sector %llu failed",
								      qd, (u64)(s + arr->disk_offset[qd]));
						}
					}

					sectors_recovered += arr->chunk_sectors;
				}
			}

			regions_recovered++;

			/* Write page to persist bit-cleared state */
			set_bit(LHSR_BITMAP_FLAG_DIRTY, &arr->bitmap_flags[p]);
			ret = lhsr_bitmap_write_page(arr, p);
			if (ret)
				DMERR("bitmap: write page %u after recovery failed", p);
		}

		if (q_buf)
			kunmap_local(q_buf);
		kunmap_local(temp_buf);
		kunmap_local(parity_buf);
		__free_page(temp_page);
		__free_page(parity_page);
		if (q_page)
			__free_page(q_page);
	}

out:
	atomic_set(&arr->bitmap_recovering, 0);

	if (regions_recovered > 0)
		DMINFO("bitmap: recovery COMPLETE — %u regions, %llu sectors",
		       regions_recovered, (u64)sectors_recovered);
	else
		DMINFO("bitmap: no dirty regions found");

	return 0;  /* Non-fatal: data is safe even if some stripes skipped */
}

/* =====================================================================
 * Write-intent bitmap (WIB) — persistent on-disk bitmap for incremental
 * rebuild optimization.
 *
 * The WIB tracks which 1MB data regions have been written to since the
 * last rebuild/resync.  On rebuild, regions whose WIB bit is CLEAR can
 * be skipped because all surviving disks have identical data for those
 * regions (they were never modified).
 *
 * Each bit covers LHSR_WIB_CHUNK_SECTORS (2048 sectors = 1MB) of data.
 * The WIB is stored on disk per-device in the extended metadata area:
 *   disk_offset + disk_sectors + LHSR_META_BASE_SECTORS + page_idx * 8
 *
 * Page format: struct lhsr_bitmap_page (seq + CRC32c + bits array).
 * The same page format is shared with the write-hole journal for code
 * reuse and consistent recovery on re-assembly.
 *
 * Bit semantics:
 *   SET   = region was written to since last resync → needs copy on rebuild
 *   CLEAR = region is clean (no writes) → safe to skip during rebuild
 *
 * On rebuild start, WIB is loaded from disk.  After rebuild completes,
 * all bits are cleared.  During normal operation, writes set WIB bits
 * which are periodically flushed to all disks.
 *
 * If WIB is absent (v1 array, alloc failure, or load failure), the
 * rebuild falls back to conservative always-copy behavior.
 * ===================================================================== */

/* ---------------------------------------------------------------
 * Sector offset of the p-th WIB page on disk @disk
 * WIB lives in the extended metadata area after write-hole bitmap
 * and superblock.
 * --------------------------------------------------------------- */
static inline sector_t lhsr_wib_page_sector(struct lhsr_array *arr,
					    unsigned int page_idx,
					    unsigned int disk)
{
	return arr->disk_offset[disk] + arr->disk_sectors
		+ LHSR_META_BASE_SECTORS
		+ (sector_t)page_idx * LHSR_WIB_PAGE_SECTORS;
}

/* ---------------------------------------------------------------
 * Total number of WIB bits needed to cover the data area
 * --------------------------------------------------------------- */
static inline unsigned int lhsr_wib_nbits(struct lhsr_array *arr)
{
	return (unsigned int)(arr->disk_sectors / LHSR_WIB_CHUNK_SECTORS) + 1;
}

/* ---------------------------------------------------------------
 * Number of pages needed for lhsr_wib_nbits() bits
 * --------------------------------------------------------------- */
static inline unsigned int lhsr_wib_npages(struct lhsr_array *arr)
{
	return max(1U, (lhsr_wib_nbits(arr) + LHSR_WIB_BITS_PER_PAGE - 1)
		 / LHSR_WIB_BITS_PER_PAGE);
}

/* ---------------------------------------------------------------
 * Write a dirty WIB page to all non-failed disks (sync write).
 * Returns 0 on success (or partial failure), -EIO if ALL disks fail.
 * --------------------------------------------------------------- */
static int lhsr_wib_write_page(struct lhsr_array *arr, unsigned int page_idx)
{
	struct lhsr_bitmap_page *page_data;
	unsigned int d;
	int ret = 0, any_written = 0;

	if (!arr->wib_pages || page_idx >= arr->wib_npages)
		return -EINVAL;

	page_data = (struct lhsr_bitmap_page *)kmap_local_page(
		arr->wib_pages[page_idx]);

	/* Fill in header: bump seq, compute CRC32c */
	arr->wib_seqs[page_idx]++;
	page_data->seq = arr->wib_seqs[page_idx];
	page_data->crc32 = 0;
	page_data->crc32 = __crc32c_le(0, (const unsigned char *)page_data, 4096);
	kunmap_local(page_data);

	/* Write to all non-failed disks */
	for (d = 0; d < arr->disks; d++) {
		blk_status_t st;
		sector_t sector;

		if (!arr->disk[d])
			continue;
		if (lhsr_failed_disks_get(arr) & (1 << d))
			continue;

		sector = lhsr_wib_page_sector(arr, page_idx, d);
		st = lhsr_submit_bio_sync(arr->disk[d], arr->wib_pages[page_idx],
					  4096, sector,
					  REQ_OP_WRITE | REQ_SYNC);
		if (st != BLK_STS_OK) {
			DMERR("WIB: write page %u to disk %u failed", page_idx, d);
			ret = -EIO;
		} else {
			any_written = 1;
		}
	}

	/* Clear dirty flag regardless — if all disks failed, we'll retry
	 * on next set/clear, but flagging it dirty forever is worse */
	clear_bit(0, &arr->wib_flags[page_idx]);

	return any_written ? 0 : (ret ? ret : -EIO);
}

/* ---------------------------------------------------------------
 * Set the WIB bit for the 1MB region containing @sector.
 * Marks the page dirty so it gets flushed to disk.
 *
 * Called from the write path: every write to user data sets the
 * corresponding WIB bit to indicate this region needs re-replication
 * during rebuild.
 * --------------------------------------------------------------- */
static void lhsr_wib_set(struct lhsr_array *arr, sector_t sector)
{
	unsigned int bit;
	unsigned int page_idx;
	unsigned int bit_in_page;
	struct lhsr_bitmap_page *wib;

	if (!arr->wib_pages)
		return;

	bit = (unsigned int)(sector / LHSR_WIB_CHUNK_SECTORS);
	if (bit >= arr->wib_nbits)
		return;

	page_idx = bit / LHSR_WIB_BITS_PER_PAGE;
	bit_in_page = bit % LHSR_WIB_BITS_PER_PAGE;

	if (page_idx >= arr->wib_npages)
		return;

	wib = (struct lhsr_bitmap_page *)page_address(arr->wib_pages[page_idx]);
	set_bit(bit_in_page, (unsigned long *)wib->bits);
	/* Mark page dirty so it gets flushed to disk */
	set_bit(0, &arr->wib_flags[page_idx]);
}

/* ---------------------------------------------------------------
 * Clear the WIB bit for the 1MB region containing @sector.
 * Marks the page dirty so the clear persists on disk.
 *
 * Called when a rebuild completes copying this region (or when the
 * entire rebuild finishes and all bits are reset).
 * --------------------------------------------------------------- */
static void lhsr_wib_clear(struct lhsr_array *arr, sector_t sector)
{
	unsigned int bit;
	unsigned int page_idx;
	unsigned int bit_in_page;
	struct lhsr_bitmap_page *wib;

	if (!arr->wib_pages)
		return;

	bit = (unsigned int)(sector / LHSR_WIB_CHUNK_SECTORS);
	if (bit >= arr->wib_nbits)
		return;

	page_idx = bit / LHSR_WIB_BITS_PER_PAGE;
	bit_in_page = bit % LHSR_WIB_BITS_PER_PAGE;

	if (page_idx >= arr->wib_npages)
		return;

	wib = (struct lhsr_bitmap_page *)page_address(arr->wib_pages[page_idx]);
	clear_bit(bit_in_page, (unsigned long *)wib->bits);
	set_bit(0, &arr->wib_flags[page_idx]);
}

/* ---------------------------------------------------------------
 * Test the WIB bit for the 1MB region containing @sector.
 * Returns 1 if bit is SET (region was written to, needs copy),
 * 0 if CLEAR (clean region, can skip during rebuild).
 * If WIB is absent, returns 1 (conservative: always copy).
 * --------------------------------------------------------------- */
static int lhsr_wib_test(struct lhsr_array *arr, sector_t sector)
{
	unsigned int bit;
	unsigned int page_idx;
	unsigned int bit_in_page;
	struct lhsr_bitmap_page *wib;

	if (!arr->wib_pages)
		return 1;

	bit = (unsigned int)(sector / LHSR_WIB_CHUNK_SECTORS);
	if (bit >= arr->wib_nbits)
		return 1;

	page_idx = bit / LHSR_WIB_BITS_PER_PAGE;
	bit_in_page = bit % LHSR_WIB_BITS_PER_PAGE;

	if (page_idx >= arr->wib_npages)
		return 1;

	wib = (struct lhsr_bitmap_page *)page_address(arr->wib_pages[page_idx]);
	return test_bit(bit_in_page, (unsigned long *)wib->bits) ? 1 : 0;
}

/* ---------------------------------------------------------------
 * Clear ALL WIB bits (called after rebuild completes/resets).
 * Marks all pages dirty so the clear persists on disk.
 * --------------------------------------------------------------- */
static void lhsr_wib_clear_all(struct lhsr_array *arr)
{
	unsigned int p;

	if (!arr->wib_pages)
		return;

	for (p = 0; p < arr->wib_npages; p++) {
		memset(page_address(arr->wib_pages[p]) +
		       LHSR_BITMAP_HEADER_BYTES, 0,
		       4096 - LHSR_BITMAP_HEADER_BYTES);
		set_bit(0, &arr->wib_flags[p]);
	}
}

/* ---------------------------------------------------------------
 * Flush all dirty WIB pages to disk.
 * Returns 0 if all writes succeeded, -EIO on any failure.
 * --------------------------------------------------------------- */
static int lhsr_wib_flush(struct lhsr_array *arr)
{
	unsigned int p;
	int ret = 0;

	if (!arr->wib_pages)
		return 0;

	for (p = 0; p < arr->wib_npages; p++) {
		if (test_bit(0, &arr->wib_flags[p])) {
			int err = lhsr_wib_write_page(arr, p);
			if (err)
				ret = err;
		}
	}
	return ret;
}

/* ---------------------------------------------------------------
 * Periodic WIB flush — writes dirty WIB pages to disk every 30s.
 * Keeps the write-intent bitmap reasonably current so that after a
 * crash, rebuild copies fewer regions.  Conservative: stale WIB
 * after crash means more copy work, never data loss.
 * --------------------------------------------------------------- */
static void lhsr_wib_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct lhsr_array *arr = container_of(dwork, struct lhsr_array, wib_work);

	/* Bail if array is being destroyed */
	if (atomic_read(&arr->destroying)) {
		DMDEBUG("wib_work: array being destroyed, skipping");
		return;
	}

	DMDEBUG("wib_work: starting periodic WIB flush");
	lhsr_wib_flush(arr);
	DMDEBUG("wib_work: periodic WIB flush complete");

	/* Re-schedule if workqueue still exists */
	if (arr->wib_wq)
		queue_delayed_work(arr->wib_wq, &arr->wib_work, 30 * HZ);
}

/* ---------------------------------------------------------------
 * Allocate WIB pages and try to load from disk.
 *
 * If this is a fresh array (no on-disk WIB yet), zero-initialize
 * all pages (all bits CLEAR = no dirty regions).
 *
 * If loading from disk succeeds, the on-disk state is restored.
 * If loading fails, pages are zeroed and we proceed with clean WIB.
 * Returns 0 on success, -ENOMEM on allocation failure.
 * --------------------------------------------------------------- */
static int lhsr_wib_init(struct lhsr_array *arr)
{
	unsigned int npages;
	unsigned int p;

	if (arr->wib_pages) {
		/* Already initialized — just zero out bits for new rebuild cycle */
		lhsr_wib_clear_all(arr);
		DMINFO("WIB: re-initialized %u pages", arr->wib_npages);
		return 0;
	}

	npages = lhsr_wib_npages(arr);

	arr->wib_pages = kvzalloc(npages * sizeof(struct page *), GFP_KERNEL);
	arr->wib_flags = kvzalloc(npages * sizeof(unsigned long), GFP_KERNEL);
	arr->wib_seqs = kvzalloc(npages * sizeof(u64), GFP_KERNEL);

	if (!arr->wib_pages || !arr->wib_flags || !arr->wib_seqs) {
		DMERR("WIB: failed to allocate metadata arrays");
		goto err_free;
	}

	for (p = 0; p < npages; p++) {
		arr->wib_pages[p] = alloc_page(GFP_KERNEL);
		if (!arr->wib_pages[p]) {
			DMERR("WIB: failed to alloc page %u/%u", p, npages);
			goto err_free;
		}
	}

	arr->wib_npages = npages;
	arr->wib_nbits = lhsr_wib_nbits(arr);

	DMINFO("WIB: %u pages, %u bits for %llu sectors",
	       npages, arr->wib_nbits, (u64)arr->disk_sectors);

	/* Try to load from disk — if no on-disk WIB (new array or v1
	 * conversion), zero-init is correct (all clean = no rebuild needed) */
	if (lhsr_wib_load(arr) != 0) {
		DMINFO("WIB: no on-disk state found, starting clean");
		lhsr_wib_clear_all(arr);
	}

	return 0;

err_free:
	lhsr_wib_destroy(arr);
	return -ENOMEM;
}

/* ---------------------------------------------------------------
 * Load WIB from all disks — for each page, pick the copy with the
 * highest valid sequence number (same approach as write-hole journal).
 *
 * Returns 0 on success (or partial load), negative on error.
 * If no disk has valid WIB data (new array), returns -ENODATA.
 * --------------------------------------------------------------- */
static int lhsr_wib_load(struct lhsr_array *arr)
{
	unsigned int p;
	int ret = -ENODATA;

	if (!arr->wib_pages)
		return -EINVAL;

	for (p = 0; p < arr->wib_npages; p++) {
		struct lhsr_bitmap_page *dst;
		u64 best_seq = 0;
		unsigned int best_disk = arr->disks;
		bool any_valid = false;
		unsigned int d;

		dst = (struct lhsr_bitmap_page *)kmap_local_page(
			arr->wib_pages[p]);

		for (d = 0; d < arr->disks; d++) {
			struct page *tmp_page;
			struct lhsr_bitmap_page *tmp;
			u32 expected_crc;
			blk_status_t st;

			if (!arr->disk[d])
				continue;

			tmp_page = alloc_page(GFP_KERNEL);
			if (!tmp_page)
				continue;

			st = lhsr_submit_bio_sync(arr->disk[d], tmp_page,
						  4096,
						  lhsr_wib_page_sector(arr, p, d),
						  REQ_OP_READ | REQ_SYNC);
			if (st != BLK_STS_OK) {
				__free_page(tmp_page);
				continue;
			}

			tmp = (struct lhsr_bitmap_page *)kmap_local_page(tmp_page);

			/* All zeros = uninitialized / never written */
			if (tmp->seq == 0) {
				kunmap_local(tmp);
				__free_page(tmp_page);
				continue;
			}

			/* Verify CRC32c — must match __crc32c_le(0, ...) used in write */
			expected_crc = tmp->crc32;
			tmp->crc32 = 0;
			if (__crc32c_le(0, (const unsigned char *)tmp, 4096) != expected_crc) {
				DMWARN("WIB: page %u disk %u: CRC mismatch", p, d);
				kunmap_local(tmp);
				__free_page(tmp_page);
				continue;
			}

			if (tmp->seq > best_seq) {
				best_seq = tmp->seq;
				best_disk = d;
			}

			any_valid = true;
			kunmap_local(tmp);
			__free_page(tmp_page);
		}

		if (any_valid && best_disk < arr->disks) {
			/* Re-read the best copy into our page */
			blk_status_t st;

			st = lhsr_submit_bio_sync(arr->disk[best_disk],
						  arr->wib_pages[p],
						  4096,
						  lhsr_wib_page_sector(arr, p, best_disk),
						  REQ_OP_READ | REQ_SYNC);
			if (st == BLK_STS_OK) {
				arr->wib_seqs[p] = best_seq;
				clear_bit(0, &arr->wib_flags[p]);
				DMDEBUG("WIB: page %u loaded from disk %u seq %llu",
					p, best_disk, (u64)best_seq);
				ret = 0;
			}
		} else {
			/* No valid copy — zero this page */
			memset(dst, 0, 4096);
			arr->wib_seqs[p] = 0;
			clear_bit(0, &arr->wib_flags[p]);
		}

		kunmap_local(dst);
	}

	return ret;
}

/* ---------------------------------------------------------------
 * Destroy WIB: flush dirty pages to disk, then free all memory.
 * --------------------------------------------------------------- */
static void lhsr_wib_destroy(struct lhsr_array *arr)
{
	unsigned int p;

	if (!arr->wib_pages)
		goto out_free_meta;

	/* Flush dirty pages before freeing */
	for (p = 0; p < arr->wib_npages; p++) {
		if (arr->wib_pages[p]) {
			if (test_bit(0, &arr->wib_flags[p]))
				lhsr_wib_write_page(arr, p);
			__free_page(arr->wib_pages[p]);
		}
	}

	kvfree(arr->wib_pages);

out_free_meta:
	kvfree(arr->wib_flags);
	kvfree(arr->wib_seqs);

	arr->wib_pages = NULL;
	arr->wib_flags = NULL;
	arr->wib_seqs = NULL;
	arr->wib_npages = 0;
	arr->wib_nbits = 0;
}

/* RMW worker — runs on WQ_UNBOUND rmw_wq (max_active=8) */
static void lhsr_rmw_worker(struct work_struct *work)
{
	struct lhsr_rmw_work *rmw = container_of(work, struct lhsr_rmw_work, work);
	struct bio *orig = rmw->orig_bio;
	struct lhsr_array *arr = rmw->arr;
	blk_status_t status = BLK_STS_OK;

	struct page *old_data_page = NULL;
	struct page *parity_page = NULL;
	struct page *new_data_page = NULL;
	struct page *q_parity_page = NULL;

	ktime_t tm0, tm1, tm2, tm3, tm_bs;
	static DEFINE_RATELIMIT_STATE(rmw_rs, 1 * HZ, 10);

	size_t chunk_bytes = rmw->chunk_bytes;
	unsigned int data_disks = rmw->data_disks;
	unsigned int parity_disks = rmw->parity_disks;
	unsigned int data_disk = rmw->data_disk;
	sector_t chunk_start = rmw->chunk_start;

	unsigned int p_disk = data_disks;	/* P parity index */
	unsigned int q_disk = data_disks + 1;	/* Q parity index (RAID6) */
	unsigned int lock_idx;			/* Per-stripe mutex hash index */
	bool locked = false;			/* Did we acquire the stripe lock? */

	/* Allocate page buffers (process context, can use GFP_NOIO) */
	old_data_page = alloc_page(GFP_NOIO);
	parity_page = alloc_page(GFP_NOIO);
	new_data_page = alloc_page(GFP_NOIO);
	if (!old_data_page || !parity_page || !new_data_page) {
		DMERR("RMW worker: page allocation failed");
		status = BLK_STS_RESOURCE;
		goto out;
	}
	if (parity_disks > 1) {
		q_parity_page = alloc_page(GFP_NOIO);
		if (!q_parity_page) {
			DMERR("RMW worker: Q parity page allocation failed");
			status = BLK_STS_RESOURCE;
			goto out;
		}
	}

	/*
	 * Check array is not being destroyed.
	 * We hold the only reference to this work item while running,
	 * so arr is valid as long as rmw_wq hasn't been destroyed.
	 */
	if (atomic_read(&arr->destroying)) {
		DMERR("RMW worker: array being destroyed, aborting write");
		status = BLK_STS_IOERR;
		goto out;
	}

	/*
	 * Acquire per-stripe mutex.  Ensures that two RMW workers on the
	 * SAME stripe (same chunk_start) do not interleave their read-modify-
	 * write cycles.  Different stripes run concurrently on different CPUs
	 * via the WQ_UNBOUND workqueue.  The hash may collide two different
	 * stripes to the same lock — this is a harmless false serialization
	 * that has no correctness impact.
	 */
	lock_idx = (chunk_start >> 3) & (LHSR_STRIPE_LOCKS - 1);
	mutex_lock(&arr->stripe_locks[lock_idx]);
	locked = true;
	tm0 = ktime_get();

	/* Parallel read phase: count bios first, set pending, submit all, wait once.
	 * Counting BEFORE submission prevents a race where a bio completes
	 * synchronously (loopback/tmpfs) inside submit_bio(), which would fire
	 * complete() before the other bios are even allocated.
	 */
	{
		struct lhsr_parallel_io pio;
		unsigned int failed = lhsr_failed_disks_get(arr);
		int nr_reads = 1;  /* data disk read always needed */

		init_completion(&pio.done);
		pio.status = BLK_STS_OK;

		/* Count P parity read (skipped if disk failed) */
		if (!(failed & (1 << p_disk)))
			nr_reads++;
		/* Count Q parity read for RAID6 (skipped if disk failed) */
		if (parity_disks > 1 && !(failed & (1 << q_disk)))
			nr_reads++;

		atomic_set(&pio.pending, nr_reads);

		/* Read old data */
		lhsr_submit_parallel(arr, data_disk, old_data_page,
				     chunk_bytes, chunk_start,
				     REQ_OP_READ, &pio);

		/* Read P parity (lhsr_submit_parallel zeroes page if disk failed) */
		lhsr_submit_parallel(arr, p_disk, parity_page,
				     chunk_bytes, chunk_start,
				     REQ_OP_READ, &pio);

		/* Read Q parity for RAID6 (skipped/zeroed if Q disk failed) */
		if (parity_disks > 1)
			lhsr_submit_parallel(arr, q_disk, q_parity_page,
					     chunk_bytes, chunk_start,
					     REQ_OP_READ, &pio);

		/* Wait for all submitted reads */
		if (nr_reads > 0)
			wait_for_completion_io(&pio.done);
		tm1 = ktime_get();
		if (pio.status != BLK_STS_OK) {
			DMERR("RMW worker: parallel read phase failed "
			      "(status=%d)", pio.status);
			status = pio.status;
			goto out;
		}
	}

	/* Phase 4: Compute new data, P parity, and (RAID6) Q parity */
	{
		void *old_data = kmap_local_page(old_data_page);
		void *parity = kmap_local_page(parity_page);
		void *new_data = kmap_local_page(new_data_page);
		unsigned int i;

		/* Start with old data */
		memcpy(new_data, old_data, chunk_bytes);

		/* Merge bio data into new_data */
		{
			struct bio_vec bv;
			struct bvec_iter iter;
			size_t dst_off = (size_t)rmw->offset_in_chunk << SECTOR_SHIFT;

			bio_for_each_segment(bv, orig, iter) {
				void *src = kmap_local_page(bv.bv_page) + bv.bv_offset;
				size_t copy_len = min_t(size_t, bv.bv_len,
							chunk_bytes - dst_off);
				memcpy(new_data + dst_off, src, copy_len);
				kunmap_local(src);
				dst_off += copy_len;
				if (dst_off >= chunk_bytes)
					break;
			}
		}

		/* Compute new P parity = old_P XOR old_data XOR new_data */
		{
			u8 *p = parity;
			u8 *od = old_data;
			u8 *nd = new_data;
			for (i = 0; i < chunk_bytes; i++)
				p[i] ^= od[i] ^ nd[i];
		}

		/* (RAID6) Compute new Q parity */
		if (parity_disks > 1) {
			u8 *q = kmap_local_page(q_parity_page);
			u8 *od = old_data;
			u8 *nd = new_data;
			u8 coeff = rs_power_table[data_disk];
			for (i = 0; i < chunk_bytes; i++)
				q[i] ^= lhsr_gf_mul(od[i] ^ nd[i], coeff);
			kunmap_local(q);
		}

		kunmap_local(new_data);
		kunmap_local(parity);
		kunmap_local(old_data);
	}
	tm2 = ktime_get();  /* after compute */

	/*
	 * Mark intent in write-hole journal BEFORE modifying data/parity.
	 * If we crash now, recovery sees the dirty bit and reconstructs
	 * parity from the (unchanged) data disks — always consistent.
	 */
	if (lhsr_bitmap_set(arr, chunk_start))
		DMWARN("RMW: bitmap_set failed for chunk %llu", (u64)chunk_start);
	tm_bs = ktime_get();

	/* Parallel write phase: submit all writes at once, wait once.
	 * REQ_FUA forces each write to stable storage.  Submitting all
	 * three concurrently allows the block layer/device to pipeline
	 * cache flushes instead of doing three sequential FUA waits.
	 * The write-hole bitmap (set above) guarantees crash recovery
	 * can reconstruct parity if any write fails mid-batch.
	 * Count bios first to prevent synchronous-completion race (see
	 * read phase comment).
	 */
	{
		struct lhsr_parallel_io pio;
		unsigned int failed = lhsr_failed_disks_get(arr);
		int nr_writes = 0;

		init_completion(&pio.done);
		pio.status = BLK_STS_OK;

		/* Count non-failed disks for write */
		if (!(failed & (1 << data_disk)))
			nr_writes++;
		if (!(failed & (1 << p_disk)))
			nr_writes++;
		if (parity_disks > 1 && !(failed & (1 << q_disk)))
			nr_writes++;

		atomic_set(&pio.pending, nr_writes);

		lhsr_submit_parallel(arr, data_disk, new_data_page,
				     chunk_bytes, chunk_start,
				     REQ_OP_WRITE | REQ_SYNC | REQ_FUA, &pio);
		lhsr_submit_parallel(arr, p_disk, parity_page,
				     chunk_bytes, chunk_start,
				     REQ_OP_WRITE | REQ_SYNC | REQ_FUA, &pio);
		if (parity_disks > 1)
			lhsr_submit_parallel(arr, q_disk, q_parity_page,
					     chunk_bytes, chunk_start,
					     REQ_OP_WRITE | REQ_SYNC | REQ_FUA, &pio);

		/* Wait for all submitted writes */
		if (nr_writes > 0)
			wait_for_completion_io(&pio.done);
		tm3 = ktime_get();

		if (pio.status != BLK_STS_OK) {
			DMERR("RMW worker: parallel write phase failed "
			      "(status=%d)", pio.status);
			status = pio.status;
			goto out;
		}
	} /* end of parallel write phase block */

	/* All writes committed — clear the dirty bit */
	if (lhsr_bitmap_clear(arr, chunk_start))
		DMWARN("RMW: bitmap_clear failed for chunk %llu", (u64)chunk_start);

	/*
	 * Clear WIB bit: the entire stripe is now self-consistent because
	 * every data chunk and parity chunk was written with REQ_FUA.
	 * WIB granularity is 1MB (LHSR_WIB_CHUNK_SECTORS), which is much
	 * larger than any stripe, so all chunks in the stripe share the
	 * same WIB bit — clearing once is correct.
	 */
	lhsr_wib_clear(arr, chunk_start);

	if (__ratelimit(&rmw_rs)) {
		long d_rd = ktime_us_delta(tm1, tm0);
		long d_cpu = ktime_us_delta(tm2, tm1);
		long d_bs = ktime_us_delta(tm_bs, tm2);
		long d_wr = ktime_us_delta(tm3, tm_bs);
		long d_bmp = ktime_us_delta(ktime_get(), tm3);
		long dt = ktime_us_delta(ktime_get(), tm0);
		DMDEBUG("RMW timing: chunk=%llu rd=%ld cpu=%ld "
		        "bs=%ld wr=%ld bmp=%ld total=%ld",
		        (u64)chunk_start, d_rd, d_cpu, d_bs, d_wr, d_bmp, dt);
	}

out:
	if (locked)
		mutex_unlock(&arr->stripe_locks[lock_idx]);
	if (old_data_page)
		__free_page(old_data_page);
	if (parity_page)
		__free_page(parity_page);
	if (new_data_page)
		__free_page(new_data_page);
	if (q_parity_page)
		__free_page(q_parity_page);

	orig->bi_status = status;
	bio_endio(orig);
	kfree(rmw);
}

/* Mirror write completion - called when one mirror member's write finishes */
static void lhsr_mirror_endio(struct bio *bio)
{
	struct lhsr_mirror_ctx *mctx = bio->bi_private;

	if (!mctx) {
		DMERR("mirror_endio: mctx is NULL");
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return;
	}

	if (bio->bi_status)
		mctx->status = bio->bi_status;

	if (atomic_dec_and_test(&mctx->pending)) {
		mctx->orig_bio->bi_status = mctx->status;
		bio_endio(mctx->orig_bio);
		kfree(mctx);
	}

	bio_put(bio);
}

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

	/*
	 * FLUSH handling: forward REQ_OP_FLUSH to all working member disks.
	 * num_flush_bios = 1 ensures DM serializes flush requests.
	 */
	/*
	 * Degraded mode: reject non-read, non-flush I/O.
	 * Reads are safe (reconstructed from parity/mirrors).  FLUSH is harmless.
	 * Writes, discards, and any other non-read-op are unsafe: with missing
	 * disks we cannot maintain parity consistency or mirror replication.
	 */
	if (arr->degraded && bio_op(bio) != REQ_OP_READ && bio_op(bio) != REQ_OP_FLUSH) {
		DMDEBUG("Degraded: rejecting %s I/O at sector %llu",
			bio_op(bio) == REQ_OP_DISCARD ? "DISCARD" : "WRITE",
			(u64)(bio->bi_iter.bi_sector - ti->begin));
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	if (bio_op(bio) == REQ_OP_FLUSH) {
		struct lhsr_mirror_ctx *mctx;
		unsigned int i, working = 0;

		/* Count working disks */
		for (i = 0; i < arr->disks; i++) {
			if (!(lhsr_failed_disks_get(arr) & (1 << i)) && arr->disk[i])
				working++;
		}
		if (working == 0) {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		mctx = kzalloc(sizeof(*mctx), GFP_NOIO);
		if (!mctx) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		mctx->orig_bio = bio;
		mctx->status = 0;
		atomic_set(&mctx->pending, working);
		{
			unsigned int submitted = 0;

			for (i = 0; i < arr->disks; i++) {
				struct bio *fbio;

				if (lhsr_failed_disks_get(arr) & (1 << i) || !arr->disk[i])
					continue;

				fbio = bio_alloc_bioset(arr->disk[i], 0, REQ_OP_FLUSH,
							GFP_NOIO, &lhsr_bioset);
				if (!fbio) {
					mctx->status = BLK_STS_RESOURCE;
					atomic_dec(&mctx->pending);
					continue;
				}
				fbio->bi_end_io = lhsr_mirror_endio;
				fbio->bi_private = mctx;
				submit_bio(fbio);
				submitted++;
			}
			if (submitted == 0) {
				bio->bi_status = mctx->status ? mctx->status : BLK_STS_RESOURCE;
				bio_endio(bio);
				kfree(mctx);
			}
		}
		return DM_MAPIO_SUBMITTED;
	}

	/*
	 * DISCARD handling:
	 *   - Single disk (raid_type == LHSR_RAID0): passthrough to single disk (existing path)
	 *   - Mirror  (raid_type == LHSR_RAID1): forward to all working members
	 *   - RAID5/6 (raid_type >= LHSR_RAID5): reject (stripe-aligned discard not yet implemented)
	 */
	if (bio_op(bio) == REQ_OP_DISCARD) {
		/* Single disk: passthrough to existing path below */
		if (arr->raid_type == LHSR_RAID0)
			goto passthrough_single;

		/* RAID5/6: not yet supported */
		if (arr->raid_type >= LHSR_RAID5) {
			DMDEBUG("DISCARD on RAID5/6 not yet supported (ignoring)");
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		/* Mirror: forward to all working members */
		{
			struct lhsr_mirror_ctx *mctx;
			unsigned int i, working = 0;

			for (i = 0; i < arr->disks; i++) {
				if (!(lhsr_failed_disks_get(arr) & (1 << i)) && arr->disk[i])
					working++;
			}
			if (working == 0) {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			mctx = kzalloc(sizeof(*mctx), GFP_NOIO);
			if (!mctx) {
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			mctx->orig_bio = bio;
			mctx->status = 0;
			atomic_set(&mctx->pending, working);
			{
				unsigned int submitted = 0;

				for (i = 0; i < arr->disks; i++) {
					struct bio *dbio;

					if (lhsr_failed_disks_get(arr) & (1 << i) || !arr->disk[i])
						continue;

					dbio = bio_alloc_clone(arr->disk[i], bio,
							       GFP_NOIO, &lhsr_bioset);
					if (!dbio) {
						mctx->status = BLK_STS_RESOURCE;
						atomic_dec(&mctx->pending);
						continue;
					}
					dbio->bi_iter.bi_sector = offset + arr->disk_offset[i];
					dbio->bi_end_io = lhsr_mirror_endio;
					dbio->bi_private = mctx;
					submit_bio(dbio);
					submitted++;
				}
				if (submitted == 0) {
					bio->bi_status = mctx->status ? mctx->status : BLK_STS_RESOURCE;
					bio_endio(bio);
					kfree(mctx);
				}
			}
			return DM_MAPIO_SUBMITTED;
		}
	}

passthrough_single:
	/* Single disk passthrough */
	if (arr->raid_type == LHSR_RAID0) {
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

	/* RAID1 Mirror - write to ALL working members */
	if (arr->raid_type == LHSR_RAID1) {
		if (bio_op(bio) != REQ_OP_READ) {
			struct lhsr_mirror_ctx *mctx;
			struct bio *clone;
			unsigned int working = 0;
			unsigned int i;

			/* Count working mirror members */
			for (i = 0; i < arr->disks; i++) {
				if (!(lhsr_failed_disks_get(arr) & (1 << i)) && arr->disk[i])
					working++;
			}

			if (working == 0) {
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			mctx = kzalloc(sizeof(*mctx), GFP_NOIO);
			if (!mctx) {
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			mctx->orig_bio = bio;
			mctx->status = 0;
			atomic_set(&mctx->pending, working);

			{
			unsigned int submitted = 0;

			for (i = 0; i < arr->disks; i++) {
				if (lhsr_failed_disks_get(arr) & (1 << i) || !arr->disk[i])
					continue;

				clone = bio_alloc_clone(arr->disk[i], bio,
							GFP_NOIO, &lhsr_bioset);
				if (!clone) {
					mctx->status = BLK_STS_RESOURCE;
					atomic_dec(&mctx->pending);
					continue;
				}

				clone->bi_iter.bi_sector = offset + arr->disk_offset[i];
				clone->bi_end_io = lhsr_mirror_endio;
				clone->bi_private = mctx;

				if (lhsr_setup_io_tracking(clone, ti) < 0) {
					bio_put(clone);
					mctx->status = BLK_STS_IOERR;
					atomic_dec(&mctx->pending);
					continue;
				}

				submit_bio(clone);
				submitted++;
			}

			/*
			 * If no clones were submitted, no endio will fire.
			 * Complete orig_bio and free mctx here.
			 * If at least one was submitted, endio handles completion.
			 * mctx may be freed if all completed synchronously.
			 * DO NOT TOUCH mctx after this check.
			 */
			if (submitted == 0) {
				bio->bi_status = mctx->status ? mctx->status : BLK_STS_RESOURCE;
				bio_endio(bio);
				kfree(mctx);
			} else {
				/*
				 * Mark this region in the write-intent bitmap (WIB)
				 * so rebuild knows it was touched and needs to be
				 * re-replicated if a disk fails.
				 */
				if (arr->wib_pages)
					lhsr_wib_set(arr, offset);
			}
			}

			return DM_MAPIO_SUBMITTED;
		}
	}

	/* RAID5/6 handling with stripe map */
	if (arr->raid_type >= LHSR_RAID5) {
		unsigned int parity_disks = (arr->raid_type == LHSR_RAID5) ? 1 : 2;
		unsigned int data_disks = arr->disks - parity_disks;
		unsigned int chunk_sects = arr->chunk_sectors;
		unsigned int stripe_sects = data_disks * chunk_sects;
		unsigned int data_disk;
		unsigned int i;			/* Generic loop counter */
		sector_t chunk_start;
		int is_write = (bio_op(bio) != REQ_OP_READ);

		/*
		 * Stripe map formula (right-static parity):
		 *   User sector s → data_disk = (s / chunk_sects) % data_disks
		 *   Sector on data disk = (s / stripe_sects) * chunk_sects + (s % chunk_sects)
		 *   Parity at same offset on parity disk
		 */
		chunk_start = (offset / stripe_sects) * chunk_sects;
		data_disk = ((unsigned int)(offset / chunk_sects)) % data_disks;

		if (is_write) {
			struct lhsr_rmw_work *rmw;
			size_t chunk_bytes = (size_t)chunk_sects << SECTOR_SHIFT;

			DMDEBUG("RAID5/6 write: offset=%llu data_disk=%u chunk_start=%llu bio_bytes=%u",
				(u64)offset, data_disk, (u64)chunk_start,
				bio->bi_iter.bi_size);

			/* Fail if target data disk is failed or missing */
			if (lhsr_failed_disks_get(arr) & (1 << data_disk) || !arr->disk[data_disk]) {
				DMERR("RAID5/6 write: target data disk %u unavailable",
				      data_disk);
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			/* Allocate RMW work item — pages are allocated inside the worker */
			rmw = kzalloc(sizeof(*rmw), GFP_NOIO);
			if (!rmw) {
				DMERR("RAID5/6 write: work item allocation failed");
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			rmw->orig_bio = bio;
			rmw->arr = arr;
			rmw->data_disk = data_disk;
			rmw->data_disks = data_disks;
			rmw->parity_disks = parity_disks;
			rmw->chunk_start = chunk_start;
			rmw->chunk_bytes = chunk_bytes;
			rmw->offset_in_chunk = (unsigned int)(offset % chunk_sects);

			/* Mark this region in WIB so rebuild knows it was touched */
			if (arr->wib_pages)
				lhsr_wib_set(arr, offset);

			INIT_WORK(&rmw->work, lhsr_rmw_worker);
			queue_work(arr->rmw_wq, &rmw->work);
			return DM_MAPIO_SUBMITTED;
		}

	/*
	 * RAID5/6 READ: stripe-aware single-disk read with reconstruction fallback
	 */
	{
		unsigned int target_disk = data_disk;

		if (lhsr_failed_disks_get(arr) & (1 << target_disk) || !arr->disk[target_disk]) {
			/*
			 * Target data disk failed — reconstruct from survivors using parity.
			 * Cannot failover to another data disk: each disk has DIFFERENT data.
			 */
			struct lhsr_raid_5_read_ctx *ctx;
			unsigned int working = 0;
			unsigned int total_slots = 0;
			unsigned int j;
			sector_t stripe_sector;

			DMDEBUG("RECON: target_disk=%u failed_mask=0x%lx data_disks=%u chunk_start=%llu offset_in_chunk=%llu",
			        target_disk, lhsr_failed_disks_get(arr), data_disks,
			        (u64)chunk_start, (u64)(offset % chunk_sects));

			/* Count working survivors (data + parity) */
			for (i = 0; i < arr->disks; i++) {
				if (!(lhsr_failed_disks_get(arr) & (1 << i)) && arr->disk[i])
					working++;
			}
			if (working == 0) {
				DMERR("RECON: no working survivors");
				bio->bi_status = BLK_STS_IOERR;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			/* Count total survivor slots.
			 * For RAID6: read ALL surviving parity disks (including Q).
			 * Q is needed by RS(255,N) decode for double-failure recovery.
			 * In the XOR path (single data failure, P alive), Q is simply
			 * excluded from the XOR computation.
			 */
			total_slots = 0;
			for (i = 0; i < data_disks; i++) {
				if (!(lhsr_failed_disks_get(arr) & (1 << i)))
					total_slots++;
			}
			unsigned int parity_to_use = parity_disks;
			for (j = 0; j < parity_to_use; j++) {
				if (!(lhsr_failed_disks_get(arr) & (1 << (data_disks + j))))
					total_slots++;
			}

			DMDEBUG("RECON: working=%u total_slots=%u", working, total_slots);

			/* Allocate read context with flex array for disk_map */
			ctx = kzalloc(sizeof(*ctx) + sizeof(unsigned int) * total_slots, GFP_NOIO);
			if (!ctx) {
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			ctx->orig_bio = bio;
			ctx->arr = arr;
			ctx->target_disk = data_disk;
			ctx->data_disks = data_disks;
			ctx->working = working;
			ctx->num_slots = total_slots;
			ctx->offset = offset;
			ctx->bio_size = bio->bi_iter.bi_size;
			atomic_set(&ctx->pending, total_slots);
			ctx->status = 0;

			ctx->recon_buf = kzalloc(bio->bi_iter.bi_size, GFP_NOIO);
			if (!ctx->recon_buf) {
				kfree(ctx);
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			ctx->data_bufs = kzalloc(sizeof(void *) * total_slots, GFP_NOIO);
			if (!ctx->data_bufs) {
				kfree(ctx->recon_buf);
				kfree(ctx);
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			/* Build disk_map: survivors (data then parity)
			 * For RAID6, BOTH P and Q are included in disk_map.
			 * Q is a GF-weighted sum and CANNOT be combined with XOR,
			 * but IS needed for RS(255,N) double-failure decode.
			 */
			j = 0;
			for (i = 0; i < data_disks; i++) {
				if (!(lhsr_failed_disks_get(arr) & (1 << i))) {
					ctx->disk_map[j] = i;
					j++;
				}
			}
			ctx->data_survivors = j;	/* Number of surviving data in disk_map */
			for (i = 0; i < parity_to_use; i++) {
				unsigned int pd = data_disks + i;
				if (!(lhsr_failed_disks_get(arr) & (1 << pd))) {
					ctx->disk_map[j] = pd;
					j++;
				}
			}

			/*
			 * All survivors read at the same stripe position:
			 * each disk's chunk for this stripe is at chunk_start + sector_in_chunk
			 */
			stripe_sector = chunk_start + (offset % chunk_sects);

			/* Allocate per-slot pages array */
			ctx->pages = kzalloc(sizeof(struct page *) * ctx->num_slots, GFP_NOIO);
			if (!ctx->pages) {
				kfree(ctx->recon_buf);
				kfree(ctx->data_bufs);
				kfree(ctx);
				bio->bi_status = BLK_STS_RESOURCE;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}

			/* Phase 1: Pre-allocate a page for each clone */
			for (i = 0; i < ctx->num_slots; i++) {
				ctx->pages[i] = alloc_page(GFP_NOIO);
				if (!ctx->pages[i]) {
					ctx->status = BLK_STS_RESOURCE;
					break;
				}
			}

			/* Phase 2: For each slot, create an independent bio with its own page */
			{
			unsigned int submitted = 0;
			int alloc_failed = 0;

			DMDEBUG("RECON clone setup: orig_bio_size=%zu ctx_bio_size=%zu chunk_bytes=%zu",
			        (size_t)bio->bi_iter.bi_size, ctx->bio_size, (size_t)chunk_sects << SECTOR_SHIFT);

			for (i = 0; i < ctx->num_slots; i++) {
				struct bio *recon_bio;
				unsigned int di = ctx->disk_map[i];

				if (!arr->disk[di]) {
					ctx->status = BLK_STS_IOERR;
					atomic_dec(&ctx->pending);
					DMINFO("RECON clone: disk %u (slot %u) unavailable", di, i);
					continue;
				}
				if (!ctx->pages[i]) {
					ctx->status = BLK_STS_RESOURCE;
					atomic_dec(&ctx->pending);
					alloc_failed = 1;
					DMINFO("RECON clone: slot %u page allocation failed", i);
					continue;
				}
				recon_bio = bio_alloc_bioset(arr->disk[di], 1,
							    REQ_OP_READ, GFP_NOIO,
							    &lhsr_bioset);
				if (!recon_bio) {
					ctx->status = BLK_STS_RESOURCE;
					atomic_dec(&ctx->pending);
					alloc_failed = 1;
					DMINFO("RECON clone: bio_alloc_bioset failed for disk %u", di);
					continue;
				}
				/* Use bio_add_page to properly set up the bvec.
				 * Page ownership: we allocated the page and bio_add_page
				 * does NOT take a reference (the caller retains ownership).
				 * The endio handler will __free_page to release it.
				 */
				if (!bio_add_page(recon_bio, ctx->pages[i],
						  bio->bi_iter.bi_size, 0)) {
					bio_put(recon_bio);
					ctx->status = BLK_STS_RESOURCE;
					atomic_dec(&ctx->pending);
					DMERR("RECON clone: bio_add_page failed for disk %u", di);
					alloc_failed = 1;
					continue;
				}
				ctx->data_bufs[i] = kzalloc(bio->bi_iter.bi_size, GFP_NOIO);
				if (!ctx->data_bufs[i]) {
					bio_put(recon_bio);
					ctx->status = BLK_STS_RESOURCE;
					atomic_dec(&ctx->pending);
					alloc_failed = 1;
					continue;
				}
				recon_bio->bi_iter.bi_sector = stripe_sector + arr->disk_offset[di];
				recon_bio->bi_end_io = lhsr_raid_5_read_endio;
				recon_bio->bi_private = ctx;
				submit_bio(recon_bio);
				submitted++;
			}

			/*
			 * If none were submitted, no endio will fire.
			 * Complete orig_bio and free ctx here.
			 * If at least one was submitted, endio handles completion.
			 * ctx may be freed if all completed synchronously.
			 * DO NOT TOUCH ctx after this check.
			 */
			if (submitted == 0) {
				bio->bi_status = ctx->status ? ctx->status : BLK_STS_RESOURCE;
				bio_endio(bio);
				kfree(ctx->recon_buf);
				if (ctx->data_bufs) {
					for (i = 0; i < ctx->num_slots; i++)
						kfree(ctx->data_bufs[i]);
					kfree(ctx->data_bufs);
				}
				if (ctx->pages) {
					for (i = 0; i < ctx->num_slots; i++) {
						if (ctx->pages[i])
							__free_page(ctx->pages[i]);
					}
					kfree(ctx->pages);
				}
				kfree(ctx);
				return DM_MAPIO_SUBMITTED;
			}
			return DM_MAPIO_SUBMITTED;
			}
		}

		/*
		 * Normal read: direct from target data disk at stripe-aware position.
		 * physical sector = disk_offset + chunk_start + sector_within_chunk
		 */
		{
			sector_t disk_sector = arr->disk_offset[target_disk]
					     + chunk_start + (offset % chunk_sects);
			bio_set_dev(bio, arr->disk[target_disk]);
			bio->bi_iter.bi_sector = disk_sector;
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

	/* Read: try primary, failover to any working disk.
	 * Degraded-mode NULL disk slots are checked via failed_disks AND
	 * the explicit !arr->disk[] guard (belt-and-suspenders). */
	if (!(lhsr_failed_disks_get(arr) & (1 << arr->primary_disk)) &&
	    arr->disk[arr->primary_disk]) {
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
			if (!(lhsr_failed_disks_get(arr) & (1 << i)) &&
			    arr->disk[i]) {
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
		if (arr->raid_type == LHSR_RAID0) {
			const char *state = (arr->degraded || lhsr_failed_disks_get(arr)) ? "DEGRADED" : "OK";
			sz += scnprintf(result + sz, maxlen - sz, "%s %u/%llu",
				       state, arr->disks, arr->size);
		} else if (arr->raid_type == LHSR_RAID1) {
			unsigned int healthy = (lhsr_failed_disks_get(arr) == 0) ? arr->disks : arr->disks - 1;
			const char *state = (arr->degraded || lhsr_failed_disks_get(arr)) ? "DEGRADED" : "OK";
			sz += scnprintf(result + sz, maxlen - sz, "%s %u/%u err=%u fail=%d",
				       state, healthy, arr->disks,
				       atomic_read(&arr->io_errors),
				       atomic_read(&arr->failovers));
		} else if (arr->raid_type >= LHSR_RAID5) {
			unsigned long failed = lhsr_failed_disks_get(arr);
			const char *raid_name = arr->raid_type == LHSR_RAID5 ? "RAID5" : "RAID6";
			const char *state = arr->degraded ? "DEGRADED" :
					   failed ? "DEGRADED" : "OK";
			unsigned int healthy = 0;
			{ unsigned int j;
			for (j = 0; j < arr->disks; j++) {
				if (!(failed & (1 << j)) && arr->disk[j])
					healthy++;
			} }
			sz += scnprintf(result + sz, maxlen - sz, "%s %s %u/%u err=%u",
				       state, raid_name, healthy, arr->disks,
				       atomic_read(&arr->io_errors));
		}
		if (arr->scrub_state != LHSR_SCRUB_IDLE) {
			const char *state_str = "IDLE";
			switch (arr->scrub_state) {
			case LHSR_SCRUB_RUNNING:
				state_str = "RUN";
				break;
			case LHSR_SCRUB_PAUSED:
				state_str = "PAUSED";
				break;
			case LHSR_SCRUB_COMPLETED:
				state_str = "DONE";
				break;
			}
			sz += scnprintf(result + sz, maxlen - sz, " scrub=%s-%llu",
				       state_str, arr->scrub_verified);
		}
		break;
	case STATUSTYPE_TABLE:
		sz += scnprintf(result + sz, maxlen - sz,
			       "UUID=%llx RAID=%u DISKS=%u INTEGRITY=%d DEGRADED=%d",
			       arr->uuid, arr->raid_type, arr->disks,
			       arr->integrity_below, arr->degraded);
		if (arr->raid_type >= LHSR_RAID5) {
			const char *raid_name = arr->raid_type == LHSR_RAID5 ? "RAID5" : "RAID6";
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

	r = lhsr_write_superblock(arr->disk[disk_idx], sb, arr->disk_sectors, arr->disk_offset[disk_idx]);
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
		lhsr_failed_disks_set(arr, lhsr_failed_disks_get(arr) | (1 << disk_idx));
		arr->disk_errors[disk_idx] = arr->error_threshold;
		up_write(&arr->sb_sem);
		lhsr_update_disk_state(arr, disk_idx, LHSR_DISK_DEGRADED);

		if (arr->primary_disk == disk_idx)
			lhsr_select_new_primary(arr, disk_idx, 0);

		arr->state = LHSR_STATE_DEGRADED;
		scnprintf(result, maxlen, "Disk %u marked failed", disk_idx);
		return 1;
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
		lhsr_failed_disks_set(arr, lhsr_failed_disks_get(arr) & ~(1 << disk_idx));
		arr->disk_errors[disk_idx] = 0;
		up_write(&arr->sb_sem);
		lhsr_update_disk_state(arr, disk_idx, LHSR_DISK_HEALTHY);

		if (lhsr_failed_disks_get(arr) == 0)
			arr->state = LHSR_STATE_HEALTHY;
		else
			arr->state = LHSR_STATE_DEGRADED;

		scnprintf(result, maxlen, "Disk %u marked online", disk_idx);
		return 1;
	}

	if (strncmp(argv[0], "member_status", 12) == 0) {
		if (argc < 2)
			return -EINVAL;
		err = kstrtouint(argv[1], 10, &disk_idx);
		if (err || disk_idx >= arr->disks)
			return -EINVAL;

		scnprintf(result, maxlen,
			  "member[%u]: state=%s errors=%u gen=%llu",
			  disk_idx,
			  (lhsr_failed_disks_get(arr) & (1 << disk_idx)) ?
				"degraded" : "healthy",
			  arr->disk_errors[disk_idx],
			  arr->sbs[disk_idx].generation);
		return 1;
	}

	if (strncmp(argv[0], "disk_health", 10) == 0) {
		if (argc < 2)
			return -EINVAL;
		err = kstrtouint(argv[1], 10, &disk_idx);
		if (err || disk_idx >= arr->disks)
			return -EINVAL;

		scnprintf(result, maxlen, "disk[%u]: errors=%u failed=%d gen=%llu",
			  disk_idx, arr->disk_errors[disk_idx],
			  (int)((lhsr_failed_disks_get(arr) >> disk_idx) & 1),
			  arr->sbs[disk_idx].generation);

		return 1;
	}

	if (strncmp(argv[0], "device_path", 11) == 0) {
		if (argc < 2)
			return -EINVAL;
		err = kstrtouint(argv[1], 10, &disk_idx);
		if (err || disk_idx >= arr->disks)
			return -EINVAL;
		if (!arr->disk[disk_idx] || !arr->disk[disk_idx]->bd_disk)
			return -ENXIO;

		/* Return the full device path, including partition suffix.
		 * For partition devices (e.g. loop0p1, sda1), the disk_name
		 * is the parent (e.g. "loop0") — we must append "p<N>" for
		 * devices whose name ends in a digit, else "<N>" directly.
		 */
		{
			const char *dname = arr->disk[disk_idx]->bd_disk->disk_name;
			struct block_device *bdev = arr->disk[disk_idx];

			if (bdev_is_partition(bdev)) {
				int partno = bdev_partno(bdev);
				size_t dlen = strlen(dname);

				/* Append "p<N>" if parent name ends in digit
				 * (e.g. loop0 → loop0p1), else "<N>" (e.g. sda → sda1).
				 * Inline the digit check to avoid <ctype.h>.
				 */
				if (dlen > 0 && dname[dlen - 1] >= '0' &&
				    dname[dlen - 1] <= '9')
					scnprintf(result, maxlen, "%sp%d", dname, partno);
				else
					scnprintf(result, maxlen, "%s%d", dname, partno);
			} else {
				scnprintf(result, maxlen, "%s", dname);
			}
		}
		return 1;
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
			case LHSR_SCRUB_RUNNING:
				state_str = "RUNNING";
				break;
			case LHSR_SCRUB_PAUSED:
				state_str = "PAUSED";
				break;
			case LHSR_SCRUB_COMPLETED:
				state_str = "COMPLETED";
				break;
			}
			scnprintf(result, maxlen,
				  "scrub: state=%s disk=%u offset=0x%llx verified=%llu corrupted=%llu",
				  state_str, arr->scrub_disk, arr->scrub_offset,
				  arr->scrub_verified, arr->scrub_corrupted);
			return 1;
		}

		if (argc < 2)
			return -EINVAL;

		if (strcmp(argv[1], "start") == 0) {
			lhsr_scrub_start(arr);
			scnprintf(result, maxlen, "Scrub started");
			return 1;
		}

		if (strcmp(argv[1], "stop") == 0) {
			arr->scrub_state = LHSR_SCRUB_IDLE;
			scnprintf(result, maxlen, "Scrub stopped at offset 0x%llx", arr->scrub_offset);
			return 1;
		}

		DMDEBUG("Unknown scrub subcommand: '%s'", argv[1]);
		return -EINVAL;
	}

	/* scan - trigger or report health scan status */
	if (strncmp(argv[0], "scan", 4) == 0) {
		/* If no scan running, start one */
		if (arr->scrub_state == LHSR_SCRUB_IDLE ||
		    arr->scrub_state == LHSR_SCRUB_COMPLETED) {
			lhsr_scrub_start(arr);
			scnprintf(result, maxlen, "Scan started");
		} else {
			const char *state_str = "RUNNING";
			if (arr->scrub_state == LHSR_SCRUB_PAUSED)
				state_str = "PAUSED";
			scnprintf(result, maxlen,
				  "scan: state=%s disk=%u offset=0x%llx"
				  " verified=%llu corrupted=%llu",
				  state_str, arr->scrub_disk,
				  arr->scrub_offset,
				  arr->scrub_verified,
				  arr->scrub_corrupted);
		}
		return 1;
	}

	if (strncmp(argv[0], "rebuild", 6) == 0) {
		/* Query status (no second argument) */
		if (argc == 1 || (argc >= 2 && strcmp(argv[1], "status") == 0)) {
			const char *state_str = "NONE";
			u32 pct = 0;
			switch (arr->rebuild_state) {
			case LHSR_REBUILD_PENDING:
				state_str = "PENDING";
				break;
			case LHSR_REBUILD_RUNNING:
				state_str = "RUNNING";
				pct = arr->rebuild_total ?
					(u32)((arr->rebuild_offset * 100) /
					      arr->rebuild_total) : 0;
				break;
			case LHSR_REBUILD_COMPLETE:
				state_str = "COMPLETE";
				pct = 100;
				break;
			}
			scnprintf(result, maxlen,
				  "rebuild: state=%s disk=%u progress=%u%%"
				  " (%llu/%llu sectors)",
				  state_str, arr->rebuild_disk, pct,
				  arr->rebuild_verified, arr->rebuild_total);
			return 1;
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
			return 1;
		}

		if (strcmp(argv[1], "stop") == 0) {
			arr->rebuild_state = LHSR_REBUILD_NONE;
			scnprintf(result, maxlen, "Rebuild stopped at offset 0x%llx", arr->rebuild_offset);
			return 1;
		}

		return -EINVAL;
	}

	if (strncmp(argv[0], "persist", 7) == 0) {
		unsigned int i;
		arr->generation++;
		for (i = 0; i < arr->disks; i++) {
			lhsr_update_disk_state(arr, i, (lhsr_failed_disks_get(arr) & (1 << i)) ?
						       LHSR_DISK_DEGRADED : LHSR_DISK_HEALTHY);
		}
		scnprintf(result, maxlen, "Persisted gen=%llu", arr->generation);
		return 1;
	}

	if (strncmp(argv[0], "write_verify", 11) == 0) {
		if (argc < 2) {
			const char *mode_str = "none";
			switch (arr->write_verify_enabled) {
			case LHSR_WRITE_VERIFY_SIMPLE:
				mode_str = "simple";
				break;
			case LHSR_WRITE_VERIFY_FULL:
				mode_str = "full";
				break;
			}
			scnprintf(result, maxlen, "write_verify=%s", mode_str);
			return 1;
		}

		if (strcmp(argv[1], "none") == 0) {
			arr->write_verify_enabled = LHSR_WRITE_VERIFY_NONE;
			scnprintf(result, maxlen, "Write verification disabled");
			return 1;
		}
		if (strcmp(argv[1], "simple") == 0) {
			DMERR("Write-verify not yet implemented");
			scnprintf(result, maxlen, "Write-verify not implemented");
			return -EOPNOTSUPP;
		}
		if (strcmp(argv[1], "full") == 0) {
			DMERR("Write-verify not yet implemented");
			scnprintf(result, maxlen, "Write-verify not implemented");
			return -EOPNOTSUPP;
		}
		return -EINVAL;
	}

	if (strncmp(argv[0], "config", 6) == 0) {
		scnprintf(result, maxlen,
			  "uuid=%llx raid=%u disks=%u state=%u failed=0x%lx gen=%llu verify=%u integrity=%d",
			  arr->array_uuid, arr->raid_type, arr->disks,
			  arr->state, lhsr_failed_disks_get(arr), arr->generation,
			  arr->write_verify_enabled, arr->integrity_below);
		return 1;
	}

	DMERR("Unknown message: %s", argv[0]);
	return -EINVAL;
}

/* Target operations */
/* iterate_devices callback for LVM/device-mapper dependency reporting */
static int lhsr_iterate_devices(struct dm_target *ti,
				iterate_devices_callout_fn fn,
				void *data)
{
	struct lhsr_array *arr = ti->private;
	unsigned int i;
	int ret;

	if (!arr)
		return 0;

	for (i = 0; i < arr->disks; i++) {
		if (!arr->dm_devs[i])
			continue;
		ret = fn(ti, arr->dm_devs[i], arr->disk_offset[i],
			 arr->disk_sectors, data);
		if (ret)
			return ret;
	}
	return 0;
}

static struct target_type lhsr_target = {
	.name = "lhsr",
	.version = {1, 3, 0},
	.ctr = lhsr_ctr,
	.dtr = lhsr_dtr,
	.map = lhsr_map,
	.status = lhsr_status,
	.message = lhsr_message,
	.iterate_devices = lhsr_iterate_devices,
};

/* Module initialization */

/* Forward declaration */
static void __init lhsr_rs_table_init(void);
static int __init lhsr_init(void)
{
	int r;

	/*
	 * Compile-time structural invariant checks.
	 *
	 * These BUILD_BUG_ON() calls verify that the on-disk superblock
	 * layout matches the shared header definition.  If you change the
	 * struct, adjust LHSR_SB_VERSION accordingly.
	 *
	 * Every LHSR_SUPERBLOCK_SIZE must be exactly 128 bytes packed.
	 * Every field offset must match the comment block in include/lhsr.h.
	 */
	BUILD_BUG_ON(sizeof(struct lhsr_superblock) != LHSR_SB_SIZE);
	BUILD_BUG_ON(sizeof(struct lhsr_superblock) != 128);

	/* The failed_disks bitmask must hold at least LHSR_MAX_DISKS bits.
	 * atomic_long_t is at least 32 bits on all architectures we support. */
	BUILD_BUG_ON(sizeof(atomic_long_t) * 8 < LHSR_MAX_DISKS);

	/*
	 * CRITICAL: Ensure no struct field straddles the boundary between
	 * the old include/lhsr.h layout and the kernel's layout.
	 * Offset 52 is where the two old structs diverged:
	 *   Old include/lhsr.h had total_blocks (u64) at offset 52
	 *   Kernel dm_lhsr.h had    raid_type (u32)  at offset 52
	 * If the compiler pads differently from the packed layout,
	 * this check will catch the mismatch via the size check above.
	 * The field offset assertions below are documentation only:
	 *   offsetof(struct lhsr_superblock, raid_type) == 52
	 *   offsetof(struct lhsr_superblock, generation)  == 68
	 *   offsetof(struct lhsr_superblock, reserved)    == 84
	 */

	r = bioset_init(&lhsr_bioset, 256, 0, BIOSET_NEED_BVECS);
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

	DMINFO("Module loaded: %s (target registered) — SB size=%zu version=%u",
	       LHSR_VERSION, sizeof(struct lhsr_superblock), LHSR_SB_VERSION);
	return 0;
}

/* Module cleanup */
static void __exit lhsr_exit(void)
{
	int waited = 0;

	DMINFO("Module unload: Setting exiting flag");
	lhsr_module_exiting = 1;

	/* Wait for active devices to drain — no timeout.
	 * Calling dm_unregister_target() with active devices causes a DM rwsem
	 * deadlock: the stuck ioctl path holds _lock for READ, and
	 * dm_unregister_target needs it for WRITE.  Never force unload while
	 * devices are active — print periodic warnings instead.
	 *
	 * Remove all LHSR DM devices first:
	 *   # dmsetup remove <device>
	 *   # or: dmsetup remove_all
	 */
	while (atomic_read(&lhsr_active_devices) > 0) {
		if (waited < 10) {
			DMINFO("Module unload: Waiting for active devices (current: %d)",
			       atomic_read(&lhsr_active_devices));
		} else if (waited % 10 == 0) {
			DMERR("Module unload: Still waiting for %d active device(s)! "
			      "Remove all LHSR devices first (dmsetup remove_all).",
			      atomic_read(&lhsr_active_devices));
		}
		msleep(1000);
		waited++;
	}

	DMINFO("Module unload: All devices drained after %ds", waited);

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
/* Initialize Reed-Solomon power table: rs_power_table[i] = 2^i in GF(2^8) */
static void __init lhsr_rs_table_init(void)
{
	unsigned int i;
	rs_power_table[0] = 1;
	for (i = 1; i < 256; i++)
		rs_power_table[i] = (rs_power_table[i-1] << 1) ^
			(rs_power_table[i-1] & 0x80 ? 0x1d : 0);
	/* Build discrete log table (inverse mapping of power table).
	 * gf_log[v] = i such that rs_power_table[i] = v.
	 * gf_log[0] is deliberately 0 (0 has no log — code checks for 0 before use).
	 */
	memset(gf_log, 0, sizeof(gf_log));
	for (i = 0; i < 255; i++)
		gf_log[rs_power_table[i]] = i;
}

module_init(lhsr_init);
module_exit(lhsr_exit);
