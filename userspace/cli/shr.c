/*
 * LHSR SHR (Synology Hybrid RAID) — Plan Subcommand
 *
 * Computes the optimal partition layout for N disks of varying sizes,
 * using the greedy tiering algorithm: smallest-disk-first, one RAID
 * tier per iteration.  Prints the plan and the sgdisk + mdadm/dmsetup
 * + LVM commands to execute it.
 *
 * This is a planning tool only — it does NOT touch any block device.
 *
 * "Bad programmers worry about the code. Good programmers worry about
 *  data structures and their relationships."         — Linus Torvalds
 *
 * License: GPLv3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <errno.h>

#include "shr.h"

/* ===================================================================
 * Internal helpers
 * =================================================================== */

static int cmp_u64_asc(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;
	if (x < y) return -1;
	if (x > y) return 1;
	return 0;
}

static const char *raid_type_name(unsigned int t)
{
	switch (t) {
	case 2: return "RAID5";
	case 3: return "RAID6";
	case 1: return "RAID1";
	default: return "UNKNOWN";
	}
}

/* ===================================================================
 * Plan layout computation
 *
 * Algorithm: Greedy Tiering
 *
 * 1. Reserve alignment sectors at start of each disk for GPT
 * 2. Sort disks by size ascending (smallest first for capacity calc)
 * 3. While >= min_disks_for_parity disks have available >= min_partition:
 *    a. partition_size = min(available) among qualifying disks
 *    b. partition_size = round DOWN to alignment boundary
 *    c. Allocate partition on each qualifying disk at its next offset
 *    d. Reduce each disk's available by partition_size
 *    e. Remove disks with available < min_partition from qualifying set
 * 4. Report remaining space as unusable
 * =================================================================== */

int shr_plan_layout(const uint64_t *sizes, unsigned int disk_count,
		    unsigned int parity, uint64_t alignment,
		    uint64_t min_part, struct shr_layout *layout)
{
	unsigned int min_disks;
	uint64_t disk_offset[SHR_MAX_DISKS];
	unsigned int i, j;

	if (!sizes || !layout)
		return -1;
	if (disk_count < 3) {
		fprintf(stderr, "Error: SHR requires at least 3 disks, got %u\n",
			disk_count);
		return -1;
	}
	if (parity != 1 && parity != 2) {
		fprintf(stderr, "Error: parity must be 1 (SHR-1) or 2 (SHR-2), got %u\n",
			parity);
		return -1;
	}
	if (disk_count > SHR_MAX_DISKS) {
		fprintf(stderr, "Error: too many disks (%u, max %u)\n",
			disk_count, SHR_MAX_DISKS);
		return -1;
	}

	min_disks = (parity == 1) ? 3 : 4;
	if (disk_count < min_disks) {
		fprintf(stderr, "Error: SHR-%u requires at least %u disks, got %u\n",
			parity, min_disks, disk_count);
		return -1;
	}

	if (alignment == 0)
		alignment = SHR_ALIGNMENT;
	if (min_part == 0)
		min_part = SHR_MIN_PARTITION;

	/* Initialize layout */
	memset(layout, 0, sizeof(*layout));
	layout->disk_count = disk_count;
	layout->parity_per_tier = parity;
	layout->alignment = alignment;
	layout->min_partition = min_part;

	/* Populate disk info — note: sizes must already be sorted */
	for (i = 0; i < disk_count; i++) {
		layout->disks[i].total_sectors = sizes[i];
		/* Reserve GPT at front */
		layout->disks[i].available_sectors = sizes[i] - alignment;
		disk_offset[i] = alignment;
		layout->total_raw_sectors += sizes[i];
	}

	/* Check each disk has room after alignment */
	for (i = 0; i < disk_count; i++) {
		if (layout->disks[i].available_sectors < min_part) {
			fprintf(stderr, "Error: Disk %u too small (%llu sectors "
				"after alignment, need %llu)\n",
				i, (unsigned long long)layout->disks[i].available_sectors,
				(unsigned long long)min_part);
			return -1;
		}
	}

	/*
	 * Greedy tier allocation.
	 *
	 * We track which disks are still "active" (have >= min_part remaining).
	 * Each iteration finds the smallest available_sectors among active disks,
	 * rounds down to alignment, and carves that many sectors from each
	 * active disk as a partition for the new tier.
	 */
	{
		bool active[SHR_MAX_DISKS];
		unsigned int active_count;
		unsigned int tier_idx = 0;

		for (i = 0; i < disk_count; i++)
			active[i] = true;
		active_count = disk_count;

		while (active_count >= min_disks) {
			uint64_t min_avail = (uint64_t)-1;
			uint64_t part_size;

			/* Find smallest available among active disks */
			for (i = 0; i < disk_count; i++) {
				if (!active[i])
					continue;
				if (layout->disks[i].available_sectors < min_avail)
					min_avail = layout->disks[i].available_sectors;
			}

			/* Round partition size down to alignment */
			part_size = min_avail;
			part_size -= part_size % alignment;

			/* Must be at least min_partition */
			if (part_size < min_part)
				break;

			/* Create tier */
			layout->tiers[tier_idx].partition_count = active_count;
			layout->tiers[tier_idx].raid_type = (parity == 1) ? 2 : 3; /* RAID5 or RAID6 */
			layout->tiers[tier_idx].parity_per_tier = parity;
			layout->tiers[tier_idx].partition_size = part_size;
			layout->tiers[tier_idx].usable_sectors =
				part_size * (active_count - parity);
			layout->tiers[tier_idx].md_idx = tier_idx;

			/* Allocate partitions on each active disk */
			for (i = 0; i < disk_count; i++) {
				uint64_t this_part_size;

				if (!active[i])
					continue;

				/* If disk has less than part_size left,
				 * allocate what's left (rounded down) */
				if (layout->disks[i].available_sectors < part_size) {
					this_part_size = layout->disks[i].available_sectors;
					this_part_size -= this_part_size % alignment;
				} else {
					this_part_size = part_size;
				}

				/* This disk is saturated */
				if (this_part_size < min_part) {
					active[i] = false;
					active_count--;
					continue;
				}

				/* Record partition */
				j = layout->partition_count;
				if (j >= SHR_MAX_PARTITIONS) {
					fprintf(stderr, "Error: too many partitions (%u max)\n",
						SHR_MAX_PARTITIONS);
					return -1;
				}
				layout->partitions[j].disk_idx = i;
				layout->partitions[j].offset_sectors = disk_offset[i];
				layout->partitions[j].size_sectors = this_part_size;
				layout->partitions[j].tier = tier_idx;
				layout->partition_count++;

				/* Advance */
				disk_offset[i] += this_part_size;
				layout->disks[i].available_sectors -= this_part_size;

				/* Check if disk is exhausted */
				if (layout->disks[i].available_sectors < min_part) {
					active[i] = false;
					active_count--;
				}
			}

			tier_idx++;
		}

		layout->tier_count = tier_idx;
	}

	/* Compute total usable capacity */
	for (i = 0; i < layout->tier_count; i++)
		layout->total_usable_sectors += layout->tiers[i].usable_sectors;

	return 0;
}

/* ===================================================================
 * Print the plan
 * =================================================================== */

void shr_print_plan(const struct shr_layout *layout,
		    const char * const *disk_paths)
{
	unsigned int i, j;
	uint64_t total_unused = 0;

	printf("\nLHSR SHR Layout Plan");
	printf("\n====================");
	printf("\nMode: SHR-%u (%u parity per tier)",
	       layout->parity_per_tier, layout->parity_per_tier);
	printf("\nAlignment: %llu sectors (%llu KB)",
	       (unsigned long long)layout->alignment,
	       (unsigned long long)(layout->alignment / 2));
	printf("\nMin partition: %llu sectors (%llu MB)",
	       (unsigned long long)layout->min_partition,
	       (unsigned long long)(layout->min_partition / 2048));
	printf("\nTotal raw capacity: %llu sectors",
	       (unsigned long long)layout->total_raw_sectors);
	printf("\nTotal usable capacity: %llu sectors",
	       (unsigned long long)layout->total_usable_sectors);
	printf("\nTiers: %u", layout->tier_count);
	printf("\n\n");

	/* Disk summary */
	printf("Disks");
	printf("\n-----");
	printf("\n%3s  %-20s  %12s  %12s  %s", "#", "Device", "Size", "Tiers", "Partitions");
	printf("\n");
	for (i = 0; i < layout->disk_count; i++) {
		unsigned int parts_on_disk = 0;
		unsigned int tiers_on_disk = 0;
		unsigned int last_tier = (unsigned int)-1;

		for (j = 0; j < layout->partition_count; j++) {
			if (layout->partitions[j].disk_idx != i)
				continue;
			parts_on_disk++;
			if (layout->partitions[j].tier != last_tier) {
				tiers_on_disk++;
				last_tier = layout->partitions[j].tier;
			}
		}
		printf("%3u  %-20s  %12llu  %12u  %u",
		       i,
		       disk_paths ? disk_paths[i] : "?",
		       (unsigned long long)layout->disks[i].total_sectors,
		       tiers_on_disk,
		       parts_on_disk);

		/* Show unused space */
		if (layout->disks[i].available_sectors > layout->alignment) {
			uint64_t unused = layout->disks[i].available_sectors;
			unused -= layout->alignment;
			printf("  (%llu unused)",
			       (unsigned long long)unused);
			total_unused += unused;
		}
		printf("\n");
	}

	/* Tier breakdown */
	printf("\nTiers");
	printf("\n-----");
	printf("\n%5s  %10s  %10s  %8s  %8s  %s",
	       "Tier", "Type", "Members", "PartSz", "Usable", "Disks");
	printf("\n");
	for (i = 0; i < layout->tier_count; i++) {
		const struct shr_tier *t = &layout->tiers[i];
		char members_str[64];

		snprintf(members_str, sizeof(members_str), "%u disks", t->partition_count);
		printf("%5u  %10s  %10s  %8llu  %8llu  ",
		       i,
		       raid_type_name(t->raid_type),
		       members_str,
		       (unsigned long long)t->partition_size,
		       (unsigned long long)t->usable_sectors);

		/* List which disks */
		int first = 1;
		for (j = 0; j < layout->partition_count; j++) {
			if (layout->partitions[j].tier != i)
				continue;
			if (!first) printf(",");
			printf("d%u", layout->partitions[j].disk_idx);
			first = 0;
		}
		printf("\n");
	}

	/* Per-disk partition map */
	printf("\nPartition Map");
	printf("\n-------------");
	for (i = 0; i < layout->disk_count; i++) {
		printf("\nDisk %u (%s):", i,
		       disk_paths ? disk_paths[i] : "?");
		int has_part = 0;
		for (j = 0; j < layout->partition_count; j++) {
			if (layout->partitions[j].disk_idx != i)
				continue;
			has_part = 1;
			printf("\n  Partition %2u: offset=%12llu  size=%12llu  tier=%u",
			       layout->partitions[j].tier + 1,
			       (unsigned long long)layout->partitions[j].offset_sectors,
			       (unsigned long long)layout->partitions[j].size_sectors,
			       layout->partitions[j].tier);
		}
		if (!has_part)
			printf("  (no partitions)");
	}

	/* Unused space summary */
	if (total_unused > 0) {
		printf("\n\nUnused Capacity: %llu sectors (%llu MB)",
		       (unsigned long long)total_unused,
		       (unsigned long long)(total_unused / 2048));
		printf("\n  Disks smaller than the largest tier cannot contribute");
		printf("\n  further space once their remaining capacity drops below");
		printf("\n  the minimum partition threshold.");
	}

	printf("\n\n");
}

/* ===================================================================
 * Print shell commands to execute the plan
 * =================================================================== */

void shr_print_commands(const struct shr_layout *layout,
			const char * const *disk_paths, int use_lhsr)
{
	unsigned int i, j;
	unsigned int part_num[SHR_MAX_DISKS];
	char size_str[64];

	/* Track partition numbering per disk (1-based for sgdisk) */
	for (i = 0; i < layout->disk_count; i++)
		part_num[i] = 0;

	printf("# ============================================================");
	printf("\n# LHSR SHR — Execute the following commands as root");
	printf("\n#");
	printf("\n# Review the plan before running anything.");
	printf("\n# Commands are idempotent where possible.");
	printf("\n# ============================================================");
	printf("\n\n");

	/* ---- Step 1: Partition the disks ---- */
	printf("# Step 1: Partition disks with sgdisk");
	printf("\n# WARNING: This ERASES all data on these disks!");
	printf("\n");
	for (i = 0; i < layout->disk_count; i++) {
		const char *dev = disk_paths ? disk_paths[i] : "(device)";

		printf("\n# Disk %u: %s (%llu sectors total)", i, dev,
		       (unsigned long long)layout->disks[i].total_sectors);

		/* Count partitions on this disk */
		unsigned int pcount = 0;
		for (j = 0; j < layout->partition_count; j++) {
			if (layout->partitions[j].disk_idx == i)
				pcount++;
		}

		if (pcount == 0) {
			printf("\n#  (no partitions — disk too small to contribute)");
			continue;
		}

		printf("\nsgdisk --zap-all %s \\", dev);
		unsigned int pn = 1;
		for (j = 0; j < layout->partition_count; j++) {
			if (layout->partitions[j].disk_idx != i)
				continue;

		snprintf(size_str, sizeof(size_str), "+%llus",
			 (unsigned long long)layout->partitions[j].size_sectors);
		printf("\n  --new=%u:0:%s \\", pn, size_str);
			printf("\n  --typecode=%u:fd00 \\", pn); /* Linux RAID */
			pn++;
		}
		printf("\n  --backup=/tmp/shr_gpt_backup_%s.dump", dev + 5);
		/* Remove leading /dev/ if present */
		printf("\n");
		part_num[i] = pn - 1;
	}

	printf("\n# Verify partitions:");
	for (i = 0; i < layout->disk_count; i++) {
		if (part_num[i] > 0) {
			const char *dev = disk_paths ? disk_paths[i] : "(device)";
			printf("\nsgdisk --print %s", dev);
		}
	}
	printf("\n\n");

	/* ---- Step 2: Wait for kernel to see partitions ---- */
	printf("# Step 2: Wait for partition table re-read");
	printf("\nfor dev in");
	for (i = 0; i < layout->disk_count; i++) {
		if (part_num[i] > 0)
			printf(" %s", disk_paths ? disk_paths[i] : "");
	}
	printf("; do");
	printf("\n  partprobe \"$dev\" || blockdev --rereadpt \"$dev\"");
	printf("\ndone");
	printf("\nsleep 1");
	printf("\n\n");

	/* ---- Step 3: Create RAID arrays ---- */
	printf("# Step 3: Create RAID arrays for each tier");
	printf("\n");
	for (i = 0; i < layout->tier_count; i++) {
		const struct shr_tier *t = &layout->tiers[i];

		if (use_lhsr) {
			/* LHSR mode: use dmsetup create */
			char table[4096];
			int table_pos = 0;
			int dev_count = 0;

			snprintf(size_str, sizeof(size_str), "%llu",
				 (unsigned long long)(t->partition_size * t->partition_count));

			if (t->raid_type == 2) {
				/* RAID5: need chunk_sects, stripe_depth, cont_sects */
				table_pos = snprintf(table, sizeof(table),
					"\"0 %llu lhsr raid5 8 1 8",
					(unsigned long long)t->usable_sectors);
			} else {
				table_pos = snprintf(table, sizeof(table),
					"\"0 %llu lhsr raid6 8 1 8",
					(unsigned long long)t->usable_sectors);
			}

			/* Add partition devices */
			for (j = 0; j < layout->partition_count; j++) {
				if (layout->partitions[j].tier != i)
					continue;
				unsigned int di = layout->partitions[j].disk_idx;
				const char *dev = disk_paths ? disk_paths[di] : "(device)";
				unsigned int pn = layout->partitions[j].tier + 1;
				table_pos += snprintf(table + table_pos,
					sizeof(table) - table_pos,
					" %sp%u 0",
					dev, pn);
				dev_count++;
			}
			table_pos += snprintf(table + table_pos,
				sizeof(table) - table_pos, "\"");
			table[sizeof(table) - 1] = '\0';

			printf("dmsetup create shr_tier_%u --table %s\n", i, table);
		} else {
			/* mdadm mode */
			char members[4096];
			int mpos = 0;

			int level = (t->raid_type == 2) ? 5 : 6;

			for (j = 0; j < layout->partition_count; j++) {
				if (layout->partitions[j].tier != i)
					continue;
				unsigned int di = layout->partitions[j].disk_idx;
				const char *dev = disk_paths ? disk_paths[di] : "(device)";
				unsigned int pn = layout->partitions[j].tier + 1;
				mpos += snprintf(members + mpos, sizeof(members) - mpos,
					" %sp%u", dev, pn);
			}
			members[sizeof(members) - 1] = '\0';

			printf("mdadm --create /dev/md/shr_tier_%d \\\n", i);
			printf("  --level=%d --raid-devices=%u%s\n",
			       level, t->partition_count, members);
		}
	}
	printf("\n");

	/* ---- Step 4: LVM setup ---- */
	printf("# Step 4: LVM — initialize PVs, create VG, create LV");
	printf("\n");
	for (i = 0; i < layout->tier_count; i++) {
		if (use_lhsr) {
			printf("pvcreate /dev/mapper/shr_tier_%u\n", i);
		} else {
			printf("pvcreate /dev/md/shr_tier_%u\n", i);
		}
	}
	printf("\n# Create VG on first tier");
	if (use_lhsr) {
		printf("\nvgcreate shr_vg /dev/mapper/shr_tier_0");
	} else {
		printf("\nvgcreate shr_vg /dev/md/shr_tier_0");
	}
	printf("\n# Extend VG with remaining tiers");
	for (i = 1; i < layout->tier_count; i++) {
		if (use_lhsr) {
			printf("\nvgextend shr_vg /dev/mapper/shr_tier_%u", i);
		} else {
			printf("\nvgextend shr_vg /dev/md/shr_tier_%u", i);
		}
	}
	printf("\n\n# Create LV spanning all available space");
	printf("\nlvcreate -l 100%%FREE -n shr_vol shr_vg");
	printf("\n\n# Format (user's choice):");
	printf("\n# mkfs.ext4 /dev/shr_vg/shr_vol");
	printf("\n# mkfs.xfs  /dev/shr_vg/shr_vol");
	printf("\n# mkfs.btrfs /dev/shr_vg/shr_vol");
	printf("\n\n# Mount:");
	printf("\n# mount /dev/shr_vg/shr_vol /mnt/storage");

	printf("\n\n");
}

/* ===================================================================
 * Entry point for "lhsrctl shr plan"
 * =================================================================== */

int cmd_shr_plan(int argc, char **argv)
{
	uint64_t *sizes = NULL;
	const char **disk_paths = NULL;
	unsigned int disk_count = 0;
	int parity = 1; /* SHR-1 default */
	int use_lhsr = 0;
	int ret = 1;
	int opt_consumed = 1; /* argv[0] = "plan", count it */
	unsigned int i;

	/* Parse options: --parity N, --lhsr, --mdadm */
	for (i = 1; i < (unsigned int)argc; i++) {
		if (strcmp(argv[i], "--parity") == 0 && i + 1 < (unsigned int)argc) {
			parity = atoi(argv[++i]);
			if (parity != 1 && parity != 2) {
				fprintf(stderr, "Error: parity must be 1 or 2\n");
				return 1;
			}
			opt_consumed += 2;
		} else if (strcmp(argv[i], "--lhsr") == 0) {
			use_lhsr = 1;
			opt_consumed++;
		} else if (strcmp(argv[i], "--mdadm") == 0) {
			use_lhsr = 0;
			opt_consumed++;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
			fprintf(stderr, "Usage: lhsrctl shr plan [--parity 1|2] [--lhsr|--mdadm] <device>...\n");
			return 1;
		}
	}

	disk_count = argc - opt_consumed;
	if (disk_count < 3) {
		fprintf(stderr, "Error: SHR requires at least 3 disks\n");
		fprintf(stderr, "Usage: lhsrctl shr plan [--parity 1|2] [--lhsr|--mdadm] <device>...\n");
		return 1;
	}

	sizes = calloc(disk_count, sizeof(uint64_t));
	disk_paths = calloc(disk_count, sizeof(char *));
	if (!sizes || !disk_paths) {
		fprintf(stderr, "Error: out of memory\n");
		goto out;
	}

	/* Read device sizes */
	for (i = 0; i < disk_count; i++) {
		int fd;
		uint64_t sz;
		const char *dev = argv[opt_consumed + i];

		disk_paths[i] = dev;
		fd = open(dev, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Error: cannot open %s: %s\n",
				dev, strerror(errno));
			goto out;
		}
		if (ioctl(fd, BLKGETSIZE64, &sz) < 0) {
			fprintf(stderr, "Error: cannot get size of %s: %s\n",
				dev, strerror(errno));
			close(fd);
			goto out;
		}
		close(fd);
		sizes[i] = sz / 512; /* Convert bytes to sectors */
	}

	/* Sort disks by size ascending (smallest first) */
	qsort(sizes, disk_count, sizeof(uint64_t), cmp_u64_asc);

	/* Compute layout */
	struct shr_layout layout;
	if (shr_plan_layout(sizes, disk_count, parity, 0, 0, &layout) < 0)
		goto out;

	/* Print plan */
	shr_print_plan(&layout, disk_paths);

	/* Print commands */
	printf("Execution Commands");
	printf("\n==================\n");
	shr_print_commands(&layout, disk_paths, use_lhsr);

	ret = 0;

out:
	free(sizes);
	free(disk_paths);
	return ret;
}
