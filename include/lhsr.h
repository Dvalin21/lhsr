/*
 * LHSR - Linux Hybrid Self-Healing RAID
 * Shared On-Disk Format Definitions
 *
 * THIS IS THE SINGLE SOURCE OF TRUTH for on-disk superblock layout.
 * Both kernel module and userspace tools MUST include this header.
 * All three copies (include/lhsr.h, kernel/dm-lhsr/dm_lhsr.h,
 * userspace/recovery/lhsr-scan.c) were unified here.
 *
 * If you change this struct, update LHSR_SB_VERSION.
 * If you add fields, use reserved[] space or increment version.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#ifndef LHSR_H
#define LHSR_H

/* ===================================================================
 * Type compatibility layer
 *
 * Kernel builds:  <linux/types.h> provides __u8/__u32/__u64
 * Userspace:      <linux/types.h> when on Linux (provides __u* types
 *                 directly), otherwise <stdint.h> + manual typedefs.
 *
 * The on-disk struct uses __u* types so the same header works in both
 * contexts.  Add new compat types here as needed — do NOT duplicate
 * the struct in kernel and userspace separately.
 * =================================================================== */
#ifdef __KERNEL__
#include <linux/types.h>
#else
/*
 * Userspace on Linux: include <linux/types.h> which provides
 * __u8/__u16/__u32/__u64 with the same layout as the kernel
 * (__u64 = unsigned long long, not unsigned long).
 *
 * Userspace on non-Linux: use <stdint.h> fallback.
 */
#ifdef __linux__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif /* __linux__ */
#endif /* __KERNEL__ */

/* ===================================================================
 * Magic & Version
 * =================================================================== */

#define LHSR_MAGIC             "LHSRDISK"
#define LHSR_MAGIC_LEN         8

/* Superblock on-disk format version.
 * Increment when the packed struct layout changes or when the metadata
 * area layout changes (e.g. new sections added after the superblock).
 * The kernel module rejects superblocks with version != LHSR_SB_VERSION.
 *
 * Version history:
 *   1 — Initial format: write-hole bitmap + superblock in 272-sector
 *       metadata area at end of device.
 *   2 — Added write-intent bitmap (WIB) after superblock in extended
 *       metadata area.  LHSR_SB_FLAG_WIB set in flags field. */
#define LHSR_SB_VERSION        2

/* Superblock size in bytes (must match packed struct layout below) */
#define LHSR_SB_SIZE           128

/* Backward-compat aliases for code written against the old kernel header.
 * New code should use the LHSR_MAGIC / LHSR_MAGIC_LEN names. */
#define LHSR_SB_MAGIC          LHSR_MAGIC
#define LHSR_SB_MAGIC_LEN      LHSR_MAGIC_LEN

/* Superblock location: stored inside the per-disk reserved metadata area.
 * Primary at the start of the reserved space, backup further in. */
#define LHSR_SB_SECTORS        16   /* 16 sectors (8 KB) reserved at end of device */

/* ===================================================================
 * Superblock flags (stored in flags field)
 * =================================================================== */

#define LHSR_SB_FLAG_WIB              0x00000001   /* Write-intent bitmap present */

/* ===================================================================
 * Write-hole journal (dirty stripe bitmap)
 *
 * The bitmap is stored between the user data area and the superblock.
 * Each 4KB page has a 12-byte header (seq + CRC) followed by bits.
 * Each bit covers LHSR_BITMAP_REGION_BYTES of user data.
 *
 * Total metadata (v1): LHSR_META_SECTORS = LHSR_BITMAP_TOTAL_SECTORS + LHSR_SB_SECTORS
 * =================================================================== */
#define LHSR_BITMAP_REGION_SHIFT      20   /* 2^20 bytes = 1MB per region bit */
#define LHSR_BITMAP_REGION_SECTORS    (1 << (LHSR_BITMAP_REGION_SHIFT - 9))  /* 2048 */
#define LHSR_BITMAP_PAGES             32   /* 32 pages = 128KB bitmap */
#define LHSR_BITMAP_HEADER_BYTES      12   /* 8 bytes seq + 4 bytes CRC */
#define LHSR_BITMAP_BITS_PER_PAGE     ((4096 - LHSR_BITMAP_HEADER_BYTES) * 8)  /* 32672 */
#define LHSR_BITMAP_PAGE_SECTORS      (4096 / 512)  /* 8 */
#define LHSR_BITMAP_TOTAL_SECTORS     (LHSR_BITMAP_PAGES * LHSR_BITMAP_PAGE_SECTORS)  /* 256 */

/* ===================================================================
 * Write-intent bitmap (WIB)
 *
 * Tracks which 1MB data regions have been written to since the last
 * rebuild/resync.  On rebuild, regions whose WIB bit is CLEAR can be
 * skipped because all surviving disks have identical data for those
 * regions (they were never modified).
 *
 * Uses the same page-level format as the write-hole journal:
 *   4KB page with 12-byte header (seq + CRC32c), then 4084 bytes of
 *   bit data.  Each bit covers LHSR_WIB_CHUNK_SECTORS of user data.
 *
 * Layout on disk (v2+, per device):
 *   sector 0 .. disk_sectors-1               user data
 *   sector disk_sectors .. +255              write-hole bitmap pages
 *   sector disk_sectors+256 .. +271          superblock (primary + backup)
 *   sector disk_sectors+272 .. +271+WIB_SZ   write-intent bitmap pages
 *
 * WIB size (sectors) = wib_pages * LHSR_WIB_PAGE_SECTORS
 * where wib_pages is computed from disk_sectors at array creation.
 * See also: LHSR_WIB_SECTORS() macro below.
 * =================================================================== */
#define LHSR_WIB_CHUNK_SHIFT            20   /* 2^20 bytes = 1MB per bit */
#define LHSR_WIB_CHUNK_SECTORS          (1 << (LHSR_WIB_CHUNK_SHIFT - 9))  /* 2048 */
#define LHSR_WIB_PAGE_SECTORS           8    /* Same as write-hole journal page */
#define LHSR_WIB_BITS_PER_PAGE          ((4096 - LHSR_BITMAP_HEADER_BYTES) * 8)  /* 32672 */

/* Compute the total v2 metadata sectors including the write-intent bitmap.
 * disk_sectors is the user data area size (before adding WIB).
 * Returns total metadata sectors including write-hole bitmap, superblock, and WIB. */
#define LHSR_META2_SECTORS(disk_sectors)						\
	(LHSR_BITMAP_TOTAL_SECTORS + LHSR_SB_SECTORS +					\
	 (max(1U, (unsigned int)((disk_sectors) / LHSR_WIB_CHUNK_SECTORS +		\
	  LHSR_WIB_BITS_PER_PAGE - 1) / LHSR_WIB_BITS_PER_PAGE) * LHSR_WIB_PAGE_SECTORS))

/* v2 base metadata (write-hole bitmap + superblock, no WIB) */
#define LHSR_META_BASE_SECTORS          (LHSR_BITMAP_TOTAL_SECTORS + LHSR_SB_SECTORS)  /* 272 */

/* ===================================================================
 * RAID types stored in superblock raid_type field
 *
 * These values are ON-DISK.  Do not renumber.
 * LHSR_RAID_SHR and LHSR_RAID_SHR2 are NOT implemented in the kernel
 * module — they are reserved for future userspace-only mapping on top
 * of equal-sized mdadm + LVM (Synology SHR model).
 * =================================================================== */

#define LHSR_RAID_SINGLE       0
#define LHSR_RAID_MIRROR       1
#define LHSR_RAID5             2
#define LHSR_RAID6             3
#define LHSR_RAID_SHR          4
#define LHSR_RAID_SHR2         5

/* Convenience aliases used internally in kernel module */
#define LHSR_RAID0             LHSR_RAID_SINGLE
#define LHSR_RAID1             LHSR_RAID_MIRROR

/* ===================================================================
 * Per-disk states stored in superblock disk_state field
 *
 * These values are ON-DISK.  Do not renumber.
 * =================================================================== */

#define LHSR_DISK_HEALTHY      0
#define LHSR_DISK_DEGRADED     1
#define LHSR_DISK_FAILED       2
#define LHSR_DISK_REBUILDING   3

/* ===================================================================
 * Array runtime states (not on-disk, but shared between kernel and
 * userspace for status reporting).
 * =================================================================== */

#define LHSR_STATE_OFFLINE     0
#define LHSR_STATE_ONLINE      1
#define LHSR_STATE_HEALTHY     2
#define LHSR_STATE_DEGRADED    3

/* ===================================================================
 * Scrubber states (shared between kernel module and userspace tools)
 * =================================================================== */

#define LHSR_SCRUB_IDLE        0
#define LHSR_SCRUB_RUNNING     1
#define LHSR_SCRUB_PAUSED      2
#define LHSR_SCRUB_COMPLETED   3

/* ===================================================================
 * Rebuild states
 * =================================================================== */

#define LHSR_REBUILD_NONE      0
#define LHSR_REBUILD_PENDING   1
#define LHSR_REBUILD_RUNNING   2
#define LHSR_REBUILD_COMPLETE  3

/* ===================================================================
 * ON-DISK SUPERBLOCK — SINGLE SOURCE OF TRUTH
 *
 * All 128 bytes.  Packed.  If you change this struct you MUST:
 *   1. Increment LHSR_SB_VERSION
 *   2. Add a BUILD_BUG_ON(sizeof(struct lhsr_superblock) == LHSR_SB_SIZE)
 *      in the kernel module
 *   3. Update the userspace recovery tool's layout
 *
 * Fields at a glance (packed offsets):
 *   0:8   magic         "LHSRDISK"
 *   8:4   version       LHSR_SB_VERSION
 *  12:8   array_uuid    array identifier
 *  20:8   disk_uuid     per-disk unique ID
 *  28:8   creation_time seconds since epoch
 *  36:8   last_update   seconds since epoch
 *  44:4   disk_index    this disk's position in the array
 *  48:4   disk_state    LHSR_DISK_* value
 *  52:4   raid_type     LHSR_RAID_* value
 *  56:4   disk_count    total number of disks in array
 *  60:8   total_sectors per-disk usable sector count
 *  68:8   generation    monotonic counter for crash consistency
 *  76:4   checksum      CRC32c of whole struct with this field zeroed
 *  80:4   flags         LHSR_SB_FLAG_* bits (e.g. LHSR_SB_FLAG_WIB)
 *  84:44  reserved      future expansion (zeroed on write)
 *
 * Total: 128 bytes.
 * =================================================================== */

struct lhsr_superblock {
	__u8  magic[8];		/* "LHSRDISK" */
	__u32 version;		/* LHSR_SB_VERSION */
	__u64 array_uuid;
	__u64 disk_uuid;
	__u64 creation_time;
	__u64 last_update;
	__u32 disk_index;
	__u32 disk_state;	/* LHSR_DISK_* */
	__u32 raid_type;	/* LHSR_RAID_* */
	__u32 disk_count;
	__u64 total_sectors;
	__u64 generation;
	__u32 checksum;		/* CRC32c, zeroed before compute */
	__u32 flags;
	__u8  reserved[44];
} __attribute__((packed));

/* ===================================================================
 * Checksum algorithms (for future per-block checksum metadata)
 * =================================================================== */

#define LHSR_CHECKSUM_CRC32C    0
#define LHSR_CHECKSUM_BLAKE3    1
#define LHSR_CHECKSUM_XXHASH64  2

/* ===================================================================
 * Block flags (for checksum cache)
 * =================================================================== */

#define LHSR_BLOCK_VERIFIED     0x01
#define LHSR_BLOCK_CORRUPT      0x02

/* ===================================================================
 * Write verification modes (shared with kernel)
 * =================================================================== */

#define LHSR_WRITE_VERIFY_NONE   0
#define LHSR_WRITE_VERIFY_SIMPLE 1
#define LHSR_WRITE_VERIFY_FULL   2

/* ===================================================================
 * Corruption severity levels
 * =================================================================== */

#define LHSR_CORRUPT_NONE        0
#define LHSR_CORRUPT_SINGLE_BIT  1
#define LHSR_CORRUPT_MULTI_BIT   2
#define LHSR_CORRUPT_FULL_BLOCK  3

/* ===================================================================
 * Corruption log entry (stored in metadata area — on-disk format)
 * =================================================================== */

struct lhsr_corruption_entry {
	__u64 block_offset;
	__u32 disk_index;
	__u32 severity;
	__u64 detected_time;
	__u32 expected_checksum;
	__u32 actual_checksum;
	__u8  resolved;
	__u8  padding[7];
} __attribute__((packed));

/* ===================================================================
 * Defaults
 * =================================================================== */

#define LHSR_DEFAULT_BLOCK_SIZE  (128 * 1024)	/* 128 KB */
#define LHSR_DEFAULT_CHUNK_SECTORS  8		/* 4 KB */
#define LHSR_MAX_DISKS              32		/* Current limit; increase with atomic_long_t */

#endif /* LHSR_H */
