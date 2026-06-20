/*
 * LHSR SHR (Synology Hybrid RAID) — Data Structures
 *
 * SHR is a partition layout algorithm for mixing disks of different sizes
 * in a redundant RAID configuration.  It splits large disks into multiple
 * equal-sized partitions, groups same-sized partitions into RAID tiers,
 * and merges tiers with LVM.
 *
 * This is a PURELY USERSPACE feature.  Zero kernel module changes required.
 *
 * License: GPLv3
 */

#ifndef LHSR_SHR_H
#define LHSR_SHR_H

#include <stdint.h>
#include <stdbool.h>

/* ===================================================================
 * Constants
 * =================================================================== */

#define SHR_MAX_DISKS        32   /* Maximum physical disks */
#define SHR_MAX_TIERS        32   /* Maximum RAID tiers */
#define SHR_MAX_PARTITIONS   256  /* Max partitions across all disks */
#define SHR_ALIGNMENT        2048 /* Default partition alignment (1 MB) */
#define SHR_MIN_PARTITION    1048576 /* Default min partition (512 MB) */

/* ===================================================================
 * Data Structures
 *
 * "Bad programmers worry about the code. Good programmers worry about
 *  data structures and their relationships."         — Linus Torvalds
 *
 * The partition is the fundamental unit.  Every partition in a tier
 * is exactly the same size.  The disk is
 * the physical container.  The layout is the complete picture.
 * =================================================================== */

/* One partition on one disk.  All partitions in a tier have equal size. */
struct shr_partition {
	unsigned int    disk_idx;       /* Index into disks[] */
	uint64_t        offset_sectors; /* Sector offset on this disk */
	uint64_t        size_sectors;   /* Size in sectors (same across tier) */
	unsigned int    tier;           /* Which tier this belongs to */
};

/* One RAID tier.  All partitions are same size, same RAID level. */
struct shr_tier {
	unsigned int    partition_count;    /* Number of member partitions */
	unsigned int    raid_type;          /* 2=RAID5, 3=RAID6 */
	unsigned int    parity_per_tier;    /* 1 for SHR-1, 2 for SHR-2 */
	uint64_t        partition_size;     /* Size of each member */
	uint64_t        usable_sectors;     /* Usable = size * (members - parity) */
	unsigned int    md_idx;             /* md device number (0, 1, 2...) */
};

/* One physical disk. */
struct shr_disk {
	char            path[256];          /* e.g., /dev/sdb */
	uint64_t        total_sectors;      /* Total capacity */
	uint64_t        available_sectors;  /* Remaining after tier allocations */
};

/* The complete SHR layout. */
struct shr_layout {
	unsigned int        disk_count;
	unsigned int        tier_count;
	unsigned int        parity_per_tier;    /* 1 or 2 */
	uint64_t            alignment;          /* Sector alignment */
	uint64_t            min_partition;      /* Minimum partition size */
	struct shr_disk     disks[SHR_MAX_DISKS];
	struct shr_tier     tiers[SHR_MAX_TIERS];
	unsigned int        partition_count;
	struct shr_partition partitions[SHR_MAX_PARTITIONS];
	uint64_t            total_raw_sectors;
	uint64_t            total_usable_sectors;
};

/* ===================================================================
 * API
 * =================================================================== */

/*
 * Compute the optimal SHR partition layout for a given set of disks.
 *
 * disks      : Array of disk total_sectors (sorted by size)
 * disk_count : Number of disks (must be >= 3 for SHR-1, >= 4 for SHR-2)
 * parity     : 1 for SHR-1 (single parity), 2 for SHR-2 (dual parity)
 * alignment  : Sector alignment (0 = default SHR_ALIGNMENT)
 * min_part   : Minimum partition size (0 = default SHR_MIN_PARTITION)
 * layout     : Output: computed layout
 *
 * Returns 0 on success, -1 on error (prints reason to stderr).
 */
int shr_plan_layout(const uint64_t *sizes, unsigned int disk_count,
		    unsigned int parity, uint64_t alignment,
		    uint64_t min_part, struct shr_layout *layout);

/*
 * Print the plan in human-readable format.
 */
void shr_print_plan(const struct shr_layout *layout,
		    const char * const *disk_paths);

/*
 * Print shell commands to execute the plan.
 * Generates sgdisk + dmsetup (LHSR) + LVM commands.
 */
void shr_print_commands(const struct shr_layout *layout,
			const char * const *disk_paths);

/*
 * Execute the SHR layout: partition disks, create RAID arrays,
 * set up LVM.  This is DESTRUCTIVE — all data on the specified
 * devices will be lost.
 *
 * Returns 0 on success, 1 on error.
 */
int cmd_shr_create(int argc, char **argv);

/*
 * Show current SHR status: discover SHR tiers from system state,
 * display their health, member disks, and any LVM built on top.
 *
 * No arguments needed — scans /dev/md/ for shr_tier_* arrays
 * and checks LVM for VGs/LVs on those arrays.
 *
 * Returns 0 on success, 1 on error.
 */
int cmd_shr_status(int argc, char **argv);
int cmd_shr_destroy(int argc, char **argv);
int cmd_shr_scrub(int argc, char **argv);
int cmd_shr_disk(int argc, char **argv);
int cmd_shr_rebuild(int argc, char **argv);
int cmd_shr_expand(int argc, char **argv);
int cmd_shr_grow(int argc, char **argv);

#endif /* LHSR_SHR_H */
