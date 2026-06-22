/*
 * LHSR Kernel Module Header
 *
 * Includes on-disk format definitions from the shared project header.
 * The struct lhsr_superblock is defined ONCE in include/lhsr.h.
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

/*
 * Include on-disk format definitions.
 * The kernel Makefile adds -I$(src)/../include so <lhsr.h> resolves to
 * the project root's include/lhsr.h — the single source of truth for
 * struct lhsr_superblock, LHSR_RAID_* constants, LHSR_DISK_* constants,
 * LHSR_SB_SIZE, LHSR_SB_VERSION, and all other on-disk layout values.
 */
#include <lhsr.h>

/* Workqueue timeout in jiffies (5 seconds) */
#define LHSR_WORKQUEUE_TIMEOUT (5 * HZ)

/* Scrubber block size (128 KB = 2^17 bytes = 256 sectors) */
#define LHSR_SCRUB_BLOCK_SIZE      (128 * 1024)
#define LHSR_SCRUB_BLOCK_SIZE_BITS 17		/* log2 of block size in bytes */
#define LHSR_SCRUB_BLOCK_SECTORS   (LHSR_SCRUB_BLOCK_SIZE >> SECTOR_SHIFT)

/* Write-hole journal — dirty stripe bitmap page (on-disk format) */
struct lhsr_bitmap_page {
	u64 seq;			/* Monotonic write sequence (0 = uninitialized) */
	u32 crc32;			/* CRC32c of entire page with this field zeroed */
	u8  bits[LHSR_BITMAP_BITS_PER_PAGE / 8];  /* Bit array */
} __packed;

/* Forward declarations for RAID5/6 read reconstruction */
struct lhsr_rmw_ctx;
struct lhsr_array;

/* Pack/unpack helpers for checksum cache xarray values (u64 keyed by sector offset) */
/* Lower 32 bits = CRC32c, upper 32 bits = flags (LHSR_BLOCK_VERIFIED/CORRUPT) */
#define cksum_pack(cksum, flags)	(((u64)(flags) << 32) | (cksum))
#define cksum_unpack_cksum(val)		((u32)(val))
#define cksum_unpack_flags(val)		((u32)((val) >> 32))

/* Array structure - the core runtime data structure */
struct lhsr_array {
	/* Configuration - set once, read-only during I/O */
	u64 uuid;
	unsigned int raid_type;		/* LHSR_RAID_* value */
	unsigned int disks;
	sector_t size;
	unsigned int chunk_sectors;	/* Stripe chunk size in sectors (default 8 = 4KB) */
	sector_t disk_sectors;		/* Per-disk usable sectors (after SB reservation) */
	struct block_device *disk[32];
	struct dm_dev *dm_devs[32];
	sector_t disk_offset[32];	/* Per-disk offset in sectors */
	u32 state;
	u32 primary_disk;
	atomic_long_t failed_disks;	/* Bitmask of failed disks (atomic for hot-path read) */

	/* Concurrency control */
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
	u32 scrub_disks_done;
	u64 scrub_offset;
	u64 scrub_blocks;
	u64 scrub_verified;
	u64 scrub_corrupted;
	u64 scrub_retry_resolved;
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

	/* RAID5/6 RMW write concurrency:
	 * - Regular (non-ordered) workqueue allows multiple concurrent RMW workers
	 * - Per-stripe mutex hash prevents concurrent writes to the SAME stripe
	 *   (stripe_locks[idx]) where idx = (chunk_start / 8) % LHSR_STRIPE_LOCKS
	 */
	#define LHSR_STRIPE_LOCKS 128
	struct workqueue_struct *rmw_wq;
	struct mutex stripe_locks[LHSR_STRIPE_LOCKS];

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

	/* Checksum cache for scrubber (xarray keyed by sector offset) */
	struct xarray cksum_cache;

	/* dm-integrity stacking (persistent checksums) */
	bool integrity_below;				/* Stacked on dm-integrity devices */
	bool degraded;					/* Assembled with fewer disks than disk_count */

	/* Write-hole journal — dirty stripe bitmap */
	struct page *bitmap_pages[LHSR_BITMAP_PAGES];	/* In-memory pages */
	unsigned long bitmap_flags[LHSR_BITMAP_PAGES];	/* Bit 0 = dirty */
	u64 bitmap_seqs[LHSR_BITMAP_PAGES];		/* Current seq per page */
	atomic_t bitmap_recovering;			/* 1 = recovery in progress */
	struct workqueue_struct *bitmap_wq;		/* Background bitmap flush workqueue */
	struct delayed_work bitmap_work;		/* Background bitmap flush work */

	/* Write-intent bitmap (WIB) — persistent on-disk bitmap for
	 * incremental rebuild.  Tracks which 1MB regions were written to
	 * since last rebuild/resync.  On rebuild, regions whose WIB bit
	 * is CLEAR can be skipped (all surviving disks have identical data).
	 *
	 * Allocated in constructor; freed in lhsr_dtr().
	 * Stored on disk in extended metadata area (v2 superblock format).
	 * Pages use same seq+CRC32c format as write-hole journal.
	 *
	 * See include/lhsr.h for on-disk layout and constants.
	 */
	struct page **wib_pages;			/* Array of page ptrs, NULL when absent */
	unsigned long *wib_flags;			/* Per-page flag bits (bit 0 = dirty) */
	u64 *wib_seqs;					/* Per-page sequence numbers */
	unsigned int wib_npages;			/* Number of allocated pages */
	unsigned int wib_nbits;				/* Total number of valid bits */
	struct workqueue_struct *wib_wq;		/* WIB periodic flush workqueue */
	struct delayed_work wib_work;			/* WIB periodic flush work */
};

/* Accessor helpers for failed_disks bitmask (concurrent hot-path field) */
static inline unsigned long lhsr_failed_disks_get(struct lhsr_array *arr)
{
	return atomic_long_read(&arr->failed_disks);
}

static inline void lhsr_failed_disks_set(struct lhsr_array *arr, unsigned long val)
{
	atomic_long_set(&arr->failed_disks, val);
}

#endif /* DM_LHSR_H */
