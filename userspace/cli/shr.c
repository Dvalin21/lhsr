/*
 * LHSR SHR (Synology Hybrid RAID) — Plan and Create Subcommands
 *
 * Computes the optimal partition layout for N disks of varying sizes,
 * using the greedy tiering algorithm: smallest-disk-first, one RAID
 * tier per iteration.
 *
 * "shr plan"  — prints the plan and shell commands (no disk writes)
 * "shr create" — executes partitioning, RAID creation, and LVM setup
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
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <lhsr.h>
#include <linux/fs.h>
#include <errno.h>

#include "shr.h"

/* Forward declarations */
static int shr_disk_replace(const char *tier_name,
			    unsigned int disk_idx, const char *new_dev);

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

static int cmp_disk_info(const void *a, const void *b)
{
	const struct {
		uint64_t size;
		const char *path;
	} *const da = a, *const db = b;
	if (da->size < db->size) return -1;
	if (da->size > db->size) return 1;
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
 * 3. While >= 2 disks have available >= min_partition:
 *    a. pick the RAID type the remaining disk count supports
 *       (SHR-1: RAID5 at 3+, RAID1 at exactly 2; SHR-2: RAID6 at 4+ only)
 *    b. partition_size = min(available) among qualifying disks
 *    c. partition_size = round DOWN to alignment boundary
 *    d. Allocate partition on each qualifying disk at its next offset
 *    e. Reduce each disk's available by partition_size
 *    f. Remove disks with available < min_partition from qualifying set
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
		/* Reserve GPT at front AND back (alignment sectors each) */
		if (sizes[i] > alignment * 2)
			layout->disks[i].available_sectors = sizes[i] - alignment * 2;
		else
			layout->disks[i].available_sectors = 0;
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

		while (active_count >= 2) {
			uint64_t min_avail = (uint64_t)-1;
			uint64_t part_size;
			unsigned int tier_raid_type;
			unsigned int data_members;

			/*
			 * Choose the RAID type this tier can support.
			 *
			 * SHR-1: RAID5 while >= 3 disks remain, then a final
			 * RAID1 mirror on the last 2. Without the mirror tier
			 * the tail of a mixed set is stranded — 8/6/4/4/2 TB
			 * loses 6 TB raw (2 TB usable) because the loop used
			 * to stop at min_disks.
			 *
			 * SHR-2 promises 2-disk fault tolerance on every tier,
			 * so it must NOT degrade to RAID5/RAID1 at the tail.
			 */
			if (parity == 2) {
				if (active_count < 4)
					break;
				tier_raid_type = 3;              /* RAID6 */
				data_members = active_count - 2;
			} else if (active_count >= 3) {
				tier_raid_type = 2;              /* RAID5 */
				data_members = active_count - 1;
			} else {
				tier_raid_type = LHSR_RAID1;     /* mirror */
				data_members = 1;
			}

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
			layout->tiers[tier_idx].raid_type = tier_raid_type;
			layout->tiers[tier_idx].parity_per_tier =
				(tier_raid_type == LHSR_RAID1) ? 1 : parity;
			layout->tiers[tier_idx].partition_size = part_size;
			layout->tiers[tier_idx].usable_sectors =
				part_size * data_members;
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
			uint64_t unused = layout->disks[i].available_sectors - layout->alignment;
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
			const char * const *disk_paths)
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

	/* ---- Step 3: Create RAID arrays via LHSR dmsetup ---- */
	printf("# Step 3: Create RAID arrays for each tier (LHSR)");
	printf("\n");
	for (i = 0; i < layout->tier_count; i++) {
		const struct shr_tier *t = &layout->tiers[i];
		char table[4096];
		int table_pos = 0;
		int dev_count = 0;

		snprintf(size_str, sizeof(size_str), "%llu",
			 (unsigned long long)(t->partition_size * t->partition_count));

		if (t->raid_type == 2) {
			table_pos = snprintf(table, sizeof(table),
				"\"0 %llu lhsr raid5 8 1 8",
				(unsigned long long)t->usable_sectors);
		} else {
			table_pos = snprintf(table, sizeof(table),
				"\"0 %llu lhsr raid6 8 1 8",
				(unsigned long long)t->usable_sectors);
		}

		/* Add partition devices: <partition_path> <offset_0> */
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
	}
	printf("\n");

	/* ---- Step 4: LVM setup ---- */
	printf("# Step 4: LVM — initialize PVs, create VG, create LV");
	printf("\n");
	for (i = 0; i < layout->tier_count; i++)
		printf("pvcreate /dev/mapper/shr_tier_%u\n", i);
	printf("\n# Create VG on first tier");
	printf("\nvgcreate shr_vg /dev/mapper/shr_tier_0");
	printf("\n# Extend VG with remaining tiers");
	for (i = 1; i < layout->tier_count; i++)
		printf("\nvgextend shr_vg /dev/mapper/shr_tier_%u", i);
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
	int ret = 1;
	int opt_consumed = 1; /* argv[0] = "plan", count it */
	unsigned int i;

	/* Parse options: --parity N */
	for (i = 1; i < (unsigned int)argc; i++) {
		if (strcmp(argv[i], "--parity") == 0 && i + 1 < (unsigned int)argc) {
			parity = atoi(argv[++i]);
			if (parity != 1 && parity != 2) {
				fprintf(stderr, "Error: parity must be 1 or 2\n");
				return 1;
			}
			opt_consumed += 2;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
			fprintf(stderr, "Usage: lhsrctl shr plan [--parity 1|2] <device>...\n");
			return 1;
		}
	}

	disk_count = argc - opt_consumed;
	if (disk_count < 3) {
		fprintf(stderr, "Error: SHR requires at least 3 disks\n");
		fprintf(stderr, "Usage: lhsrctl shr plan [--parity 1|2] <device>...\n");
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

	/* Print commands (LHSR dmsetup mode) */
	printf("Execution Commands");
	printf("\n==================\n");
	shr_print_commands(&layout, disk_paths);

	ret = 0;

out:
	free(sizes);
	free(disk_paths);
	return ret;
}

/* ===================================================================
 * Phase 6.2 — "lhsrctl shr create"
 *
 * Executes the SHR layout: partitions disks, creates RAID arrays
 * (LHSR dmsetup), sets up LVM.
 *
 * This is DESTRUCTIVE. All data on the specified disks will be lost.
 * =================================================================== */

/* ---- helpers ---- */

static int shr_run_cmd(const char *fmt, ...)
{
	char cmd[4096];
	va_list ap;
	int ret;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);

	printf("  $ %s\n", cmd);
	fflush(stdout);

	ret = system(cmd);
	if (ret == -1) {
		fprintf(stderr, "  Error: fork failed\n");
		return -1;
	}
	if (!WIFEXITED(ret) || WEXITSTATUS(ret) != 0) {
		fprintf(stderr, "  Error: exit code %d\n",
			WIFEXITED(ret) ? WEXITSTATUS(ret) : -1);
		return -1;
	}
	return 0;
}

/* Build partition device path from a base device and partition number.
 * Handles both /dev/sda → /dev/sda1 and /dev/loop5 → /dev/loop5p1. */
static char *shr_get_part_dev(const char *dev, int part_num,
			      char *buf, size_t sz)
{
	size_t len = strlen(dev);
	char fmt[16];

	if (len > 0 && isdigit((unsigned char)dev[len - 1]))
		snprintf(fmt, sizeof(fmt), "%%sp%%d");
	else
		snprintf(fmt, sizeof(fmt), "%%s%%d");
	snprintf(buf, sz, fmt, dev, part_num);
	return buf;
}

/* Poll for partition device to appear, up to timeout_ms.
 * If part_num is 0, poll for the base device path directly. */
static int shr_wait_for_part(const char *dev, int part_num, int timeout_ms)
{
	char pdev[512];
	struct stat st;
	int waited = 0;

	if (part_num > 0)
		shr_get_part_dev(dev, part_num, pdev, sizeof(pdev));
	else
		snprintf(pdev, sizeof(pdev), "%s", dev);

	while (waited < timeout_ms) {
		if (stat(pdev, &st) == 0 && S_ISBLK(st.st_mode))
			return 0;
		usleep(100000); /* 100ms */
		waited += 100;
	}
	fprintf(stderr, "  Error: %s did not appear after %d ms\n",
		pdev, timeout_ms);
	return -1;
}

/* ---- validation ---- */

/* Check that a device path is safe to pass to shell commands.
 * Must exist, be a block device, not contain shell metacharacters. */
static int shr_validate_device(const char *dev)
{
	struct stat st;
	const char *p;

	if (stat(dev, &st) < 0) {
		fprintf(stderr, "Error: cannot access %s: %s\n",
			dev, strerror(errno));
		return -1;
	}
	if (!S_ISBLK(st.st_mode)) {
		fprintf(stderr, "Error: %s is not a block device\n", dev);
		return -1;
	}
	/* Reject shell metacharacters in device path */
	for (p = dev; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '/' &&
		    *p != '-' && *p != '_' && *p != '.') {
			fprintf(stderr, "Error: invalid character in device path '%s'\n",
				dev);
			return -1;
		}
	}
	return 0;
}

/* Warn that a disk already has partitions or md superblock.
 * Returns non-zero if we should abort. */
static int shr_warn_used_device(const char *dev)
{
	char pdev[512];
	struct stat st;
	int i;

	/* Check for existing partition devices */
	for (i = 1; i <= 4; i++) {
		shr_get_part_dev(dev, i, pdev, sizeof(pdev));
		if (stat(pdev, &st) == 0) {
			fprintf(stderr, "Warning: %s already has partitions "
				"(e.g. %s)\n", dev, pdev);
			fprintf(stderr, "  Use --force to override, or wipe first.\n");
			return -1;
		}
	}
	/* Check if device is mounted */
	{
		FILE *fp;
		char line[1024];
		int mounted = 0;

		fp = fopen("/proc/mounts", "r");
		if (fp) {
			while (fgets(line, sizeof(line), fp)) {
				if (strstr(line, dev)) {
					mounted = 1;
					break;
				}
			}
			fclose(fp);
		}
		if (mounted) {
			fprintf(stderr, "Error: %s appears to be mounted\n", dev);
			return -1;
		}
	}
	return 0;
}

/* ---- execution steps ---- */

/* Step 1: Partition all disks with sgdisk */
static int shr_create_partitions(struct shr_layout *layout,
				 const char **disk_paths)
{
	unsigned int i, j;

	printf("\n--- Step 1: Partition disks ---\n");

	for (i = 0; i < layout->disk_count; i++) {
		const char *dev = disk_paths[i];
		unsigned int pn = 1;

		/* Wipe existing partition table */
		printf("\n# Disk %u: %s\n", i, dev);
		if (shr_run_cmd("sgdisk --zap-all '%s'", dev) < 0)
			return -1;

		/* Create partitions for each tier this disk participates in */
		for (j = 0; j < layout->partition_count; j++) {
			uint64_t size;

			if (layout->partitions[j].disk_idx != i)
				continue;

			size = layout->partitions[j].size_sectors;
			if (shr_run_cmd("sgdisk --new=%u:0:+%llus "
					"--typecode=%u:fd00 '%s'",
					pn, (unsigned long long)size,
					pn, dev) < 0)
				return -1;
			pn++;
		}
	}
	return 0;
}

/* Step 2: Wait for partition table re-read and partition devices */
static int shr_create_wait_partitions(struct shr_layout *layout,
				      const char **disk_paths)
{
	unsigned int i;

	printf("\n--- Step 2: Wait for partitions ---\n");

	for (i = 0; i < layout->disk_count; i++) {
		const char *dev = disk_paths[i];
		unsigned int pn;
		unsigned int j;

		/* Count partitions on this disk */
		unsigned int pcount = 0;
		for (j = 0; j < layout->partition_count; j++) {
			if (layout->partitions[j].disk_idx == i)
				pcount++;
		}
		if (pcount == 0)
			continue;

		/* Check if the first partition device already exists
		 * (e.g. loop devices with losetup -P auto-create them).
		 * If so, skip re-read entirely. */
		{
			char first_part[512];
			struct stat st;
			shr_get_part_dev(dev, 1, first_part, sizeof(first_part));
			if (stat(first_part, &st) == 0 && S_ISBLK(st.st_mode)) {
				printf("  Partition %s already exists, skipping re-read\n",
				       first_part);
				goto wait_phase;
			}
		}

		/* Re-read partition table */
		printf("  Re-reading partition table on %s ...\n", dev);
		fflush(stdout);

		if (shr_run_cmd("partprobe '%s' 2>/dev/null || "
				"blockdev --rereadpt '%s' 2>/dev/null || "
				"partx -a '%s' 2>/dev/null",
				dev, dev, dev) < 0) {
			fprintf(stderr, "  Warning: partition re-read failed "
				"for %s, trying partx -a\n", dev);
			shr_run_cmd("partx -a '%s'", dev);
		}

wait_phase:
		/* Wait for each partition device to appear */
		for (pn = 1; pn <= pcount; pn++) {
			if (shr_wait_for_part(dev, pn, 5000) < 0)
				return -1;
		}
	}
	return 0;
}

/* Step 3: Create RAID arrays via LHSR dmsetup */
static int shr_create_raid(struct shr_layout *layout,
			   const char **disk_paths)
{
	unsigned int i, j;
	char cmd[16384];

	printf("\n--- Step 3: Create RAID arrays (LHSR) ---\n");

	for (i = 0; i < layout->tier_count; i++) {
		struct shr_tier *t = &layout->tiers[i];
		int raid_level = (t->raid_type == 2) ? 5 : 6;
		int pos;

		pos = snprintf(cmd, sizeof(cmd),
			"dmsetup create shr_tier_%u --table "
			"\"0 %llu lhsr raid%c 8 1 8",
			i, (unsigned long long)t->usable_sectors,
			raid_level + '0');

		for (j = 0; j < layout->partition_count; j++) {
			char pdev[512];
			if (layout->partitions[j].tier != i)
				continue;
			unsigned int di = layout->partitions[j].disk_idx;
			unsigned int pn = layout->partitions[j].tier + 1;
			shr_get_part_dev(disk_paths[di], pn, pdev, sizeof(pdev));
			pos += snprintf(cmd + pos, sizeof(cmd) - pos,
					" %s 0", pdev);
			if ((size_t)pos >= sizeof(cmd) - 64) {
				fprintf(stderr, "Error: dmsetup table too long\n");
				return -1;
			}
		}
		snprintf(cmd + pos, sizeof(cmd) - pos, "\"");
		if (shr_run_cmd("%s", cmd) < 0)
			return -1;
	}
	return 0;
}

/* Step 4: LVM — PVs, VG, LV */
static int shr_create_lvm(struct shr_layout *layout)
{
	unsigned int i;
	/* LVM config filter: only accept /dev/mapper/shr_tier_*
	 * This prevents LVM from detecting the underlying RAID member partitions
	 * as duplicate PVs (the PV metadata gets striped across members). */
	const char *lvm_filter =
		"devices { "
		"preferred_names=[\"^/dev/mapper/\"] "
		"filter = [ \"a|/dev/mapper/shr_tier_.*|\", \"r|.*|\" ] "
		"}";

	printf("\n--- Step 4: LVM setup ---\n");

	for (i = 0; i < layout->tier_count; i++) {
		if (shr_run_cmd("pvcreate --config '%s' "
				"/dev/mapper/shr_tier_%u",
				lvm_filter, i) < 0)
			return -1;
	}

	/* Create VG on first tier */
	printf("\n# Creating volume group 'shr_vg' ...\n");
	if (shr_run_cmd("vgcreate --config '%s' "
			"shr_vg /dev/mapper/shr_tier_0",
			lvm_filter) < 0)
		return -1;

	/* Extend VG with remaining tiers */
	for (i = 1; i < layout->tier_count; i++) {
		if (shr_run_cmd("vgextend --config '%s' "
				"shr_vg /dev/mapper/shr_tier_%u",
				lvm_filter, i) < 0)
			return -1;
	}

	/* Create LV spanning all available space */
	printf("\n# Creating logical volume ...\n");
	if (shr_run_cmd("lvcreate --config '%s' "
			"-l 100%%FREE -n shr_vol --yes shr_vg",
			lvm_filter) < 0)
		return -1;

	return 0;
}

/* ---- entry point ---- */

int cmd_shr_create(int argc, char **argv)
{
	uint64_t *sizes = NULL;
	const char **disk_paths = NULL;
	unsigned int disk_count = 0;
	int parity = 1;
	int force = 0;
	int yes = 0;
	uint64_t min_part = 0; /* 0 = use default (SHR_MIN_PARTITION) */
	int ret = 1;
	int opt_consumed = 1; /* skip "create" */
	char confirm[64];
	unsigned int i;
	struct shr_layout layout;

	/* Parse options: --parity N, --force, --yes, --min-part */
	for (i = 1; i < (unsigned int)argc; i++) {
		if (strcmp(argv[i], "--parity") == 0 && i + 1 < (unsigned int)argc) {
			parity = atoi(argv[++i]);
			if (parity != 1 && parity != 2) {
				fprintf(stderr, "Error: parity must be 1 or 2\n");
				return 1;
			}
			opt_consumed += 2;
		} else if (strcmp(argv[i], "--force") == 0) {
			force = 1;
			opt_consumed++;
		} else if (strcmp(argv[i], "--yes") == 0) {
			yes = 1;
			opt_consumed++;
		} else if (strcmp(argv[i], "--min-part") == 0 && i + 1 < (unsigned int)argc) {
			min_part = strtoull(argv[++i], NULL, 10);
			if (min_part == 0) {
				fprintf(stderr, "Error: invalid --min-part value\n");
				return 1;
			}
			opt_consumed += 2;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
			fprintf(stderr, "Usage: lhsrctl shr create [--parity 1|2] "
				"[--force] [--yes] [--min-part N] "
				"<device>...\n");
			return 1;
		}
	}

	disk_count = argc - opt_consumed;
	if (disk_count < 3) {
		fprintf(stderr, "Error: SHR requires at least 3 disks\n");
		fprintf(stderr, "Usage: lhsrctl shr create [--parity 1|2] "
			"[--force] [--yes] [--min-part N] "
			"<device>...\n");
		return 1;
	}

	/* Validate all devices */
	for (i = 0; i < disk_count; i++) {
		const char *dev = argv[opt_consumed + i];
		if (shr_validate_device(dev) < 0)
			return 1;
	}

	sizes = calloc(disk_count, sizeof(uint64_t));
	disk_paths = calloc(disk_count, sizeof(char *));
	if (!sizes || !disk_paths) {
		fprintf(stderr, "Error: out of memory\n");
		goto out;
	}

	/* Read device sizes */
	printf("Scanning devices ...\n");
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
		sizes[i] = sz / 512;
	}

	/* Sort by size ascending — keep disk_paths in sync */
	{
		struct disk_info {
			uint64_t size;
			const char *path;
		} *info = malloc(disk_count * sizeof(struct disk_info));
		if (!info) {
			fprintf(stderr, "Error: out of memory\n");
			goto out;
		}
		for (i = 0; i < disk_count; i++) {
			info[i].size = sizes[i];
			info[i].path = disk_paths[i];
		}
		qsort(info, disk_count, sizeof(struct disk_info),
		      cmp_disk_info);
		for (i = 0; i < disk_count; i++) {
			sizes[i] = info[i].size;
			disk_paths[i] = info[i].path;
		}
		free(info);
	}

	/* Compute layout */
	if (shr_plan_layout(sizes, disk_count, parity, 0, min_part, &layout) < 0)
		goto out;

	/* Print plan */
	shr_print_plan(&layout, disk_paths);

	/* Pre-flight checks */
	printf("\n--- Pre-flight checks ---\n");
	for (i = 0; i < disk_count; i++) {
		if (shr_warn_used_device(disk_paths[i]) < 0) {
			if (!force) {
				fprintf(stderr, "Use --force to override.\n");
				goto out;
			}
			fprintf(stderr, "  (--force override)\n");
		}
	}

	/* Confirmation */
	if (!yes) {
		printf("\n============================================================\n");
		printf("  DESTRUCTIVE OPERATION — ALL DATA ON THESE DISKS WILL BE LOST\n");
		printf("============================================================\n");
		printf("Type 'YES' to proceed, anything else to abort: ");
		fflush(stdout);
		if (!fgets(confirm, sizeof(confirm), stdin)) {
			fprintf(stderr, "Aborted.\n");
			goto out;
		}
		/* Remove trailing newline */
		{
			size_t clen = strlen(confirm);
			if (clen > 0 && confirm[clen - 1] == '\n')
				confirm[clen - 1] = '\0';
		}
		if (strcmp(confirm, "YES") != 0) {
			printf("Aborted.\n");
			goto out;
		}
	}

	printf("\nExecuting SHR layout ...\n");

	/* Step 1: Partition disks */
	if (shr_create_partitions(&layout, disk_paths) < 0) {
		fprintf(stderr, "\nFAILED at Step 1 (partitioning).\n"
			"Rollback: run `sgdisk --zap-all` on each device.\n");
		goto out;
	}

	/* Step 2: Wait for partitions */
	if (shr_create_wait_partitions(&layout, disk_paths) < 0) {
		fprintf(stderr, "\nFAILED at Step 2 (partition re-read).\n"
			"Rollback: partitions may exist; check with sgdisk --print.\n"
			"Run `partx -a` on each device manually.\n");
		goto out;
	}

	/* Step 3: Create RAID arrays (LHSR dmsetup) */
	if (shr_create_raid(&layout, disk_paths) < 0) {
		fprintf(stderr, "\nFAILED at Step 3 (RAID creation).\n"
			"Rollback: use `dmsetup remove shr_tier_*` on each tier "
			"then wipe partitions with sgdisk --zap-all.\n");
		goto out;
	}

	/* Step 4: LVM setup */
	if (shr_create_lvm(&layout) < 0) {
		fprintf(stderr, "\nFAILED at Step 4 (LVM setup).\n"
			"Rollback:\n"
			"  lvremove shr_vg/shr_vol\n"
			"  vgremove shr_vg\n"
			"  pvremove /dev/mapper/shr_tier_*\n"
			"  dmsetup remove shr_tier_*\n");
		goto out;
	}

	/* Success */
	printf("\n============================================================\n");
	printf("  SHR LAYOUT CREATED SUCCESSFULLY\n");
	printf("============================================================\n");
	printf("\nVolume Group:   shr_vg");
	printf("\nLogical Volume: shr_vg/shr_vol");
	printf("\n");
	printf("\nRAID mode: LHSR (self-healing via dmsetup)\n");
	printf("\nFormat and mount:\n");
	printf("  # mkfs.ext4 /dev/shr_vg/shr_vol\n");
	printf("  # mount /dev/shr_vg/shr_vol /mnt/storage\n");
	printf("\n");

	ret = 0;

out:
	free(sizes);
	free(disk_paths);
	return ret;
}

/* ===================================================================
 * Phase 6.3: shr status — discover and display current SHR layout
 * =================================================================== */

/* Read a sysfs attribute file into buf.  Returns 0 on success, -1 on error. */
static int read_sysfs(const char *path, char *buf, size_t bufsz)
{
	int fd, n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, bufsz - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	/* Strip trailing newline */
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';
	return 0;
}

/* Run a command and capture its stdout into buf.
 * Returns exit code (0 on success), -1 on popen failure. */
static int shr_capture(const char *cmd, char *buf, size_t bufsz)
{
	FILE *fp;
	size_t n;

	fp = popen(cmd, "r");
	if (!fp)
		return -1;
	n = fread(buf, 1, bufsz - 1, fp);
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';
	buf[n] = '\0';
	return pclose(fp);
}

/* Send a dmsetup message to a dm target and capture the result.
 * Format: dmsetup message <device> <sector> <args...>
 * Returns 0 on success, -1 on error.
 * The result buffer receives the response (stripped of trailing newline).
 */
static int dm_msg(const char *dm_name, const char *msg,
		  char *result, size_t result_sz)
{
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/dmsetup message '%.63s' 0 %s 2>/dev/null || true",
		 dm_name, msg);
	return shr_capture(cmd, result, result_sz);
}

/* Entry from dmsetup ls for LHSR dm targets. */
struct dm_entry {
	char name[64];  /* e.g. shr_tier_0 */
};

/* Scan dmsetup for SHR tier devices (LHSR mode).
 * Returns count of entries found (max 'max').
 */
static int scan_dm_tiers(const char *prefix,
			 struct dm_entry *entries, int max)
{
	FILE *fp;
	char line[256];
	int count = 0;
	int plen = strlen(prefix);

	fp = popen("/usr/sbin/dmsetup ls 2>/dev/null || true", "r");
	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp) && count < max) {
		/* dmsetup ls output: "name\t(major:minor)" or "name (major:minor)"
		 * Find the first tab/space to separate name from (major:minor) */
		char *sep = line;
		while (*sep && *sep != ' ' && *sep != '\t')
			sep++;
		if (!*sep)
			continue;
		*sep = '\0';

		if (strncmp(line, prefix, plen) != 0)
			continue;

		snprintf(entries[count].name, sizeof(entries[count].name),
			 "%.63s", line);
		count++;
	}
	pclose(fp);
	return count;
}

int cmd_shr_status(int argc, char **argv)
{
	struct dm_entry dments[16];
	int ntiers;
	unsigned int i;
	int found_lvm = 0;

	(void)argc;
	(void)argv;

	printf("LHSR SHR Status\n");
	printf("===============\n\n");

	/* ---- Scan SHR tiers (LHSR dm) ---- */
	ntiers = scan_dm_tiers("shr_tier_", dments, 16);

	if (ntiers == 0) {
		printf("No SHR tiers found.\n");
		printf("  (Scanned dmsetup for shr_tier_* devices)\n\n");
		printf("No LVM VG created by SHR found.\n");
		printf("Use 'lhsrctl shr create' to create an SHR layout.\n");
		return 1;
	}

	printf("Tiers: %d\n\n", ntiers);

	/* ---- Print each tier (LHSR dm) ---- */
	for (i = 0; i < (unsigned int)ntiers; i++) {
		char buf[256];
		char dm_path[256];
		char cfg[512] = "";
		uint64_t dm_sectors = 0;
		unsigned int raid_type = 0;
		unsigned int n_disks = 0;
		int gen = 0;
		unsigned int state = 0;
		unsigned int j;

		snprintf(dm_path, sizeof(dm_path),
			 "/dev/mapper/%.63s", dments[i].name);
		printf("  Tier %d (LHSR)\n", (int)i);
		printf("    Device: %s\n", dm_path);

		/* Query kernel config */
		if (dm_msg(dments[i].name, "config",
			   cfg, sizeof(cfg)) == 0 && cfg[0]) {
			/* Parse key=value pairs */
			char *kv = cfg;
			while (kv && *kv) {
				char *next = strchr(kv, ' ');
				if (next) *next = '\0';
				if (strncmp(kv, "raid=", 5) == 0)
					raid_type = atoi(kv + 5);
				else if (strncmp(kv, "disks=", 6) == 0)
					n_disks = atoi(kv + 6);
				else if (strncmp(kv, "state=", 6) == 0)
					state = atoi(kv + 6);
				else if (strncmp(kv, "gen=", 4) == 0)
					gen = atoi(kv + 4);
				if (next) {
					*next = ' ';
					kv = next + 1;
				} else break;
			}
		}

		/* dm device major:minor from dmsetup info */
		{
			char info_cmd[512];
			char info[256] = "";
			snprintf(info_cmd, sizeof(info_cmd),
				 "/usr/sbin/dmsetup info -c "
				 "--noheadings -o major,minor "
				 "'%.63s' 2>/dev/null",
				 dments[i].name);
			shr_capture(info_cmd, info, sizeof(info));
			if (info[0])
				printf("    Kernel dev: %s\n", info);
		}

		/* RAID info from kernel config */
		{
			const char *raid_name = "?";
			switch (raid_type) {
			case LHSR_RAID5: raid_name = "RAID5"; break;
			case LHSR_RAID6: raid_name = "RAID6"; break;
			case LHSR_RAID_MIRROR: raid_name = "RAID1"; break;
			case LHSR_RAID_SINGLE: raid_name = "SINGLE"; break;
			}
			const char *state_str = "?";
			switch (state) {
			case LHSR_STATE_HEALTHY:
				state_str = "HEALTHY"; break;
			case LHSR_STATE_DEGRADED:
				state_str = "DEGRADED"; break;
			case LHSR_STATE_ONLINE:
				state_str = "ONLINE"; break;
			case LHSR_STATE_OFFLINE:
				state_str = "OFFLINE"; break;
			}
			printf("    RAID: %s  Disks: %u  "
			       "State: %s  gen=%d\n",
			       raid_name, n_disks, state_str, gen);
		}

		/* Read block device size via sysfs */
		{
			char sysfs_path[256];
			snprintf(sysfs_path, sizeof(sysfs_path),
				 "/sys/block/dm-%s/size",
				 dments[i].name + 9);
			if (read_sysfs(sysfs_path, buf,
				       sizeof(buf)) == 0)
				dm_sectors = strtoull(buf, NULL, 10);
		}
		/* Fallback: dmsetup table size */
		if (dm_sectors == 0) {
			char sz_cmd[512];
			char sz[256] = "";
			snprintf(sz_cmd, sizeof(sz_cmd),
				 "/usr/sbin/dmsetup table "
				 "'%.63s' 2>/dev/null | "
				 "awk '{print $2}'",
				 dments[i].name);
			shr_capture(sz_cmd, sz, sizeof(sz));
			if (sz[0])
				dm_sectors = strtoull(sz, NULL, 10);
		}
		printf("    Size: %lu sectors (%.1f GiB)\n",
		       (unsigned long)dm_sectors,
		       (double)dm_sectors * 512
		       / (1024*1024*1024));

		/* Member disk health */
		if (n_disks > 0 && n_disks <= 32) {
			printf("    Members:\n");
			for (j = 0; j < n_disks; j++) {
				char mem[256] = "";
				char msgbuf[128];
				snprintf(msgbuf, sizeof(msgbuf),
					 "member_status %u", j);
				if (dm_msg(dments[i].name, msgbuf,
					   mem, sizeof(mem)) == 0
				    && mem[0]) {
					printf("      disk%u: %s\n",
					       j, mem);
				} else {
					printf("      disk%u: "
					       "(no response)\n", j);
				}
			}
		}

		/* Scrub status */
		{
			char scrub[256] = "";
			if (dm_msg(dments[i].name, "scrub",
				   scrub, sizeof(scrub)) == 0
			    && scrub[0]) {
				printf("    Scrub: %s\n", scrub);
			}
		}
		printf("\n");
	}

	/* ---- Discover LVM and mount topology ---- */
	printf("LVM\n");
	printf("---\n");

	{
		char buf[8192];
		int rv;

		/* Show VGs */
		rv = shr_capture(
			"lvm vgs --noheadings -o vg_name,vg_size,"
			"pv_count,lv_count 2>/dev/null || true",
			buf, sizeof(buf));
		(void)rv;

		if (buf[0] == '\0') {
			printf("  No LVM VGs found.\n");
		} else {
			found_lvm = 1;
			printf("  VGs:\n");
			char *line = buf;
			while (line && *line) {
				char *nl = strchr(line, '\n');
				if (nl) *nl = '\0';
				char *p = line;
				while (*p == ' ') p++;
				if (*p) {
					char vg[64], vsz[64];
					int pvc = 0, lvc = 0;
					if (sscanf(p, "%63s %63s %d %d",
						   vg, vsz, &pvc, &lvc) >= 2) {
						printf("    %s (%s, %d PVs, "
						       "%d LVs)\n",
						       vg, vsz, pvc, lvc);
					}
				}
				if (nl) line = nl + 1;
				else break;
			}
		}

		/* Show LVs with mount point info */
		if (found_lvm) {
			rv = shr_capture(
				"lvm lvs --noheadings -o lv_name,vg_name,"
				"lv_size,lv_attr 2>/dev/null || true",
				buf, sizeof(buf));
			if (buf[0]) {
				printf("  LVs:\n");
				char *line = buf;
				while (line && *line) {
					char *nl = strchr(line, '\n');
					if (nl) *nl = '\0';
					char *p = line;
					while (*p == ' ') p++;
					if (*p) {
						char lv[64], vg[64],
						     lsz[64], attr[16];
						if (sscanf(p, "%63s %63s "
							   "%63s %15s",
							   lv, vg, lsz,
							   attr) >= 3) {
							char mnt[256] = "";
							char mpt[512];
							snprintf(mpt, sizeof(mpt),
								"findmnt -n -o "
								"TARGET "
								"/dev/%s/%s "
								"2>/dev/null "
								"|| true",
								vg, lv);
							shr_capture(mpt, mnt,
								sizeof(mnt));
							printf("    %s/%s (%s, "
							       "attr=%s)",
							       vg, lv, lsz,
							       attr);
							if (mnt[0])
								printf(" -> %s",
								       mnt);
							printf("\n");
						}
					}
					if (nl) line = nl + 1;
					else break;
				}
			}
		}

		/* Show PVs with type annotation */
		rv = shr_capture(
			"lvm pvs --noheadings -o pv_name,vg_name,pv_size "
			"2>/dev/null || true",
			buf, sizeof(buf));
		if (buf[0]) {
			printf("  PVs:\n");
			char *line = buf;
			while (line && *line) {
				char *nl = strchr(line, '\n');
				if (nl) *nl = '\0';
				char *p = line;
				while (*p == ' ') p++;
				if (*p) {
					char pv[256], vg[64], psz[64];
					if (sscanf(p, "%255s %63s %63s",
						   pv, vg, psz) >= 1) {
						const char *type = "disk";
						struct stat pv_st;
						unsigned int j;

						/* Check if PV is a known SHR tier */
						if (stat(pv, &pv_st) == 0) {
							for (j = 0; j < (unsigned int)ntiers; j++) {
								char dm_path[128];
								struct stat dm_st;
								snprintf(dm_path, sizeof(dm_path),
									 "/dev/mapper/%.63s",
									 dments[j].name);
								if (stat(dm_path, &dm_st) == 0 &&
								    major(pv_st.st_rdev) ==
								    major(dm_st.st_rdev) &&
								    minor(pv_st.st_rdev) ==
								    minor(dm_st.st_rdev)) {
									type = "SHR tier";
									break;
								}
							}
						}
						printf("    %s -> %s (%s)"
						       " [%s]\n",
						       pv,
						       vg[0] ? vg : "(none)",
						       psz, type);
					}
				}
				if (nl) line = nl + 1;
				else break;
			}
		}
	}

	if (!found_lvm && ntiers > 0) {
		printf("No LVM VG found on SHR tiers.\n");
		printf("The tiers exist but are not yet part of "
		       "an LVM volume.\n");
	}

	printf("\n");
	return 0;
}

/* ===================================================================
 * shr destroy — tear down an existing SHR layout
 *
 * Scans for SHR tiers (/dev/md/shr_tier_*), discovers LVM on them,
 * and destroys everything in dependency order.
 * =================================================================== */
int cmd_shr_destroy(int argc, char **argv)
{
	struct dm_entry dments[16];
	int ntiers;
	int force = 0;
	unsigned int i;
	int ret = 0;
	int yes = 0;
	char confirm[64];
	char vg_names[16][64];
	int nvgs = 0;

	/* Parse --force, --yes */
	for (i = 1; i < (unsigned int)argc; i++) {
		if (strcmp(argv[i], "--force") == 0)
			force = 1;
		else if (strcmp(argv[i], "--yes") == 0)
			yes = 1;
		else {
			fprintf(stderr,
				"Usage: lhsrctl shr destroy [--force] [--yes]\n");
			return 1;
		}
	}

	/* Scan LHSR SHR tiers */
	ntiers = scan_dm_tiers("shr_tier_", dments, 16);
	if (ntiers == 0) {
		printf("No LHSR SHR tiers found.\n");
		return 1;
	}

	printf("LHSR SHR Destroy\n");
	printf("================\n");
	printf("Tiers to destroy: %d\n\n", ntiers);

	/* LVM config filter for LHSR mode */
	const char *lvm_filter =
		"devices { "
		"preferred_names=[\"^/dev/mapper/\"] "
		"filter = [ \"a|/dev/mapper/shr_tier_.*|\", \"r|.*|\" ] "
		"}";

	/* Discover LVM on each tier */
	for (i = 0; i < (unsigned int)ntiers; i++) {
		char tier_path[256];
		char buf[512];
		char result[256] = "";

		snprintf(tier_path, sizeof(tier_path),
			 "/dev/mapper/%.63s", dments[i].name);
		printf("  %s (%s) [LHSR]\n",
		       dments[i].name, tier_path);

		snprintf(buf, sizeof(buf),
			 "pvs --config '%s' --noheadings "
			 "-o vg_name "
			 "'%.255s' 2>/dev/null || true",
			 lvm_filter, tier_path);
		shr_capture(buf, result, sizeof(result));

		/* Parse VG name from pvs output */
		{
			char *p = result;
			char vg[64] = "";
			while (*p == ' ' || *p == '\t') p++;
			if (*p) {
				snprintf(vg, sizeof(vg), "%.63s", p);
				size_t len = strlen(vg);
				while (len > 0 && (vg[len-1] == ' ' ||
						   vg[len-1] == '\t' ||
						   vg[len-1] == '\n'))
					vg[--len] = '\0';
			}

			if (vg[0]) {
				printf("    -> LVM VG: %s\n", vg);
				int found = 0;
				unsigned int j;
				for (j = 0; j < (unsigned int)nvgs; j++) {
					if (strcmp(vg_names[j], vg) == 0) {
						found = 1;
						break;
					}
				}
				if (!found && nvgs < 16) {
					snprintf(vg_names[nvgs], 64, "%.63s",
						 vg);
					nvgs++;
				}
			} else {
				printf("    -> not in LVM\n");
			}
		}
	}

	/* Show LVs and mounts within each VG */
	if (nvgs > 0) {
		printf("\nLVM VGs to remove: %d\n", nvgs);
		for (i = 0; i < (unsigned int)nvgs; i++) {
			printf("  %s\n", vg_names[i]);
			char cmd[1024];
			char result[4096];
			snprintf(cmd, sizeof(cmd),
				 "lvs --config '%s' --noheadings "
				 "-o lv_path "
				 "'%.63s' 2>/dev/null || true",
				 lvm_filter, vg_names[i]);
			if (shr_capture(cmd, result, sizeof(result)) == 0
			    && result[0]) {
				char *line = result;
				while (line && *line) {
					char *nl = strchr(line, '\n');
					if (nl) *nl = '\0';
					char *p = line;
					while (*p == ' ') p++;
					if (*p) {
						char mntbuf[256] = "";
						char find_cmd[1024];
						snprintf(find_cmd,
							 sizeof(find_cmd),
							 "findmnt -n -o TARGET "
							 "'%.255s' "
							 "2>/dev/null "
							 "|| true", p);
						shr_capture(find_cmd, mntbuf,
							    sizeof(mntbuf));
						if (mntbuf[0])
							printf("    %s -> "
							       "mounted %s\n",
							       p, mntbuf);
						else
							printf("    %s\n", p);
					}
					if (nl) line = nl + 1;
					else break;
				}
			}
		}
	}

	/* Confirmation */
	if (!force && !yes) {
		printf("\n======================================"
		       "==========================\n");
		printf("  DESTRUCTIVE — DATA ON SHR TIERS "
		       "WILL BE LOST\n");
		printf("======================================"
		       "==========================\n");
		printf("Type 'YES' to proceed, anything else "
		       "to abort: ");
		fflush(stdout);
		if (!fgets(confirm, sizeof(confirm), stdin)) {
			printf("Aborted.\n");
			return 1;
		}
		{
			size_t clen = strlen(confirm);
			if (clen > 0 && confirm[clen - 1] == '\n')
				confirm[clen - 1] = '\0';
		}
		if (strcmp(confirm, "YES") != 0) {
			printf("Aborted.\n");
			return 1;
		}
	}

	printf("\nExecuting SHR destroy (best-effort) ...\n");

	/* ---- Teardown: reverse of create order ---- */

	/* Step 1: Unmount all LVs and remove them */
	for (i = 0; i < (unsigned int)nvgs; i++) {
		char cmd[4096];
		char result[8192];

		snprintf(cmd, sizeof(cmd),
			 "lvs --config '%s' --noheadings "
			 "-o lv_path "
			 "'%.63s' 2>/dev/null || true",
			 lvm_filter, vg_names[i]);
		if (shr_capture(cmd, result, sizeof(result)) == 0
		    && result[0]) {
			char *line = result;
			while (line && *line) {
				char *nl = strchr(line, '\n');
				if (nl) *nl = '\0';
				char *p = line;
				while (*p == ' ') p++;
				if (*p) {
					/* Unmount */
					char mntbuf[256] = "";
					snprintf(cmd, sizeof(cmd),
						 "findmnt -n -o TARGET "
						 "'%.255s' "
						 "2>/dev/null || true",
						 p);
					shr_capture(cmd, mntbuf,
						    sizeof(mntbuf));
					if (mntbuf[0]) {
						printf("  umount %s ... ",
						       mntbuf);
						char umnt[1024];
						snprintf(umnt, sizeof(umnt),
							 "umount '%.255s' "
							 ">/dev/null 2>&1",
							 mntbuf);
						if (system(umnt) == 0)
							printf("OK\n");
						else
							printf("FAIL "
							       "(ignored)\n");
					}
					/* Remove LV */
					printf("  lvremove %s ... ", p);
					snprintf(cmd, sizeof(cmd),
						 "lvremove --config '%s' "
						 "-f '%.255s' "
						 ">/dev/null 2>&1",
						 lvm_filter, p);
					if (system(cmd) == 0)
						printf("OK\n");
					else
						printf("FAIL (ignored)\n");
				}
				if (nl) line = nl + 1;
				else break;
			}
		}
	}

	/* Step 2: Remove VGs */
	for (i = 0; i < (unsigned int)nvgs; i++) {
		char cmd[1024];
		printf("  vgremove %s ... ", vg_names[i]);
		snprintf(cmd, sizeof(cmd),
			 "vgremove --config '%s' -f '%.63s' "
			 ">/dev/null 2>&1",
			 lvm_filter, vg_names[i]);
		if (system(cmd) == 0)
			printf("OK\n");
		else
			printf("FAIL (ignored)\n");
	}

	/* Step 3: Remove PVs and dm devices */
	for (i = 0; i < (unsigned int)ntiers; i++) {
		char dm_path[256];
		char cmd[1024];
		snprintf(dm_path, sizeof(dm_path),
			 "/dev/mapper/%.63s", dments[i].name);

		/* Wipe PV label */
		printf("  pvremove %s ... ", dm_path);
		snprintf(cmd, sizeof(cmd),
			 "pvremove --config '%s' "
			 "-ff '%.255s' >/dev/null 2>&1",
			 lvm_filter, dm_path);
		if (system(cmd) == 0)
			printf("OK\n");
		else
			printf("FAIL (ignored)\n");

		/* Remove dm device */
		printf("  dmsetup remove %s ... ", dments[i].name);
		snprintf(cmd, sizeof(cmd),
			 "/usr/sbin/dmsetup remove '%.63s' "
			 "2>/dev/null", dments[i].name);
		if (system(cmd) == 0)
			printf("OK\n");
		else
			printf("FAIL (ignored)\n");
	}

	printf("\nSHR destroy complete.\n");
	return ret;
}
int cmd_shr_disk(int argc, char **argv)
{
	struct dm_entry dments[16];
	int ntiers;
	const char *action;
	unsigned int disk_idx;

	if (argc < 1) {
		fprintf(stderr, "Usage: lhsrctl shr disk <fail|online|list|replace>\n");
		return 1;
	}
	action = argv[0];

	/* "replace <tier> <disk_idx> <new_dev>" uses disk index directly */
	if (strcmp(action, "replace") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: lhsrctl shr disk "
				"replace <tier> <disk_idx> <new_dev>\n");
			return 1;
		}
		/* Parse disk index */
		{
			char *end;
			long val = strtol(argv[2], &end, 10);
			if (*end || val < 0 || val >= 32) {
				fprintf(stderr, "Invalid disk index '%s' (0-31)\n",
					argv[2]);
				return 1;
			}
			disk_idx = (unsigned int)val;
		}
		return shr_disk_replace(argv[1], disk_idx, argv[3]);
	}

	if (argc < 2) {
		fprintf(stderr, "Usage: lhsrctl shr disk <fail|online> <idx>\n");
		return 1;
	}

	/* Validate and parse disk index */
	{
		char *end;
		long val = strtol(argv[1], &end, 10);
		if (*end || val < 0 || val >= 32) {
			fprintf(stderr, "Invalid disk index '%s' (0-31)\n", argv[1]);
			return 1;
		}
		disk_idx = (unsigned int)val;
	}

	ntiers = scan_dm_tiers("shr_tier_", dments, 16);
	if (ntiers == 0) {
		printf("No LHSR SHR tiers found.\n");
		return 1;
	}

	{
		int i;
		for (i = 0; i < ntiers; i++) {
			char resp[512] = "";
			char msgbuf[256];

			if (strcmp(action, "fail") == 0)
				snprintf(msgbuf, sizeof(msgbuf),
					 "disk_fail %u", disk_idx);
			else if (strcmp(action, "online") == 0)
				snprintf(msgbuf, sizeof(msgbuf),
					 "disk_online %u", disk_idx);
			else {
				fprintf(stderr, "Unknown action '%s'. "
					"Use 'fail', 'online', or 'replace'.\n",
					action);
				return 1;
			}

			printf("%s: %s disk%u ... ", dments[i].name,
			       action, disk_idx);
			fflush(stdout);

			if (dm_msg(dments[i].name, msgbuf,
				  resp, sizeof(resp)) == 0 && resp[0]) {
				printf("%s\n", resp);
			} else {
				printf("FAILED\n");
				return 1;
			}
		}
	}
	return 0;
}

/*
 * SHR rebuild — control/resync disk rebuild.
 *
 * Usage: lhsrctl shr rebuild <start <idx>|stop|status>
 */
int cmd_shr_rebuild(int argc, char **argv)
{
	struct dm_entry dments[16];
	int ntiers;
	const char *action;

	if (argc < 1) {
		fprintf(stderr, "Usage: lhsrctl shr rebuild "
			"<start <idx>|stop|status>\n");
		return 1;
	}
	action = argv[0];

	ntiers = scan_dm_tiers("shr_tier_", dments, 16);
	if (ntiers == 0) {
		printf("No LHSR SHR tiers found.\n");
		return 1;
	}

	{
		int i;
		for (i = 0; i < ntiers; i++) {
			char resp[512] = "";
			char msgbuf[256];

			if (strcmp(action, "start") == 0) {
				if (argc < 2) {
					fprintf(stderr, "Usage: lhsrctl shr "
						"rebuild start <disk_idx>\n");
					return 1;
				}
				snprintf(msgbuf, sizeof(msgbuf),
					 "rebuild start %s", argv[1]);
			} else if (strcmp(action, "stop") == 0) {
				snprintf(msgbuf, sizeof(msgbuf),
					 "rebuild stop");
			} else {
				snprintf(msgbuf, sizeof(msgbuf),
					 "rebuild status");
			}

			printf("%s: %s", dments[i].name, action);
			if (strcmp(action, "start") == 0)
				printf(" disk%s", argv[1]);
			printf(" ... ");
			fflush(stdout);

			if (dm_msg(dments[i].name, msgbuf,
				  resp, sizeof(resp)) == 0 && resp[0]) {
				printf("%s\n", resp);
			} else {
				printf("FAILED\n");
				return 1;
			}
		}
	}
	return 0;
}
/*
 * SHR scrub — control/resync health check via kernel dm messages.
 *
 * Usage: lhsrctl shr scrub [start|stop|status]
 *
 * Without arguments, reports current scrub state for all discovered tiers.
 */
int cmd_shr_scrub(int argc, char **argv)
{
	struct dm_entry dments[16];
	int ntiers;
	int i;

	ntiers = scan_dm_tiers("shr_tier_", dments, 16);
	if (ntiers == 0) {
		printf("No LHSR SHR tiers found.\n");
		return 1;
	}

	/* Determine action */
	const char *action = "status";
	if (argc > 0)
		action = argv[0];

	for (i = 0; i < ntiers; i++) {
		char resp[512] = "";
		char msgbuf[256];

		if (strcmp(action, "start") == 0) {
			snprintf(msgbuf, sizeof(msgbuf), "scrub start");
		} else if (strcmp(action, "stop") == 0) {
			snprintf(msgbuf, sizeof(msgbuf), "scrub stop");
		} else {
			snprintf(msgbuf, sizeof(msgbuf), "scrub status");
		}

		printf("%s: %s ... ", dments[i].name, action);
		fflush(stdout);

		if (dm_msg(dments[i].name, msgbuf, resp, sizeof(resp)) == 0
		    && resp[0]) {
			printf("%s\n", resp);
		} else {
			printf("(no response)\n");
		}
	}
	return 0;
}

/* ===================================================================
 * shr expand — add new disk(s) to an existing SHR array
 *
 * Scans for existing LHSR SHR tiers and their LVM VG, creates a new
 * LHSR tier from the new disk(s), and extends the VG/LV.
 *
 * RAID type determined by disk count:
 *   1 disk  → SINGLE   (no redundancy, just adds capacity)
 *   2 disks → RAID1    (mirror)
 *   3+ disks → RAID5   (single parity)
 *
 * Usage:  lhsrctl shr expand [--force] <device>...
 *
 * Raw block devices are used directly as RAID members — no partition
 * table needed.
 * =================================================================== */
int cmd_shr_expand(int argc, char **argv)
{
	uint64_t *disk_sizes = NULL;
	unsigned int new_count = 0;
	int force = 0;
	int yes = 0;
	int ret = 1;
	int opt_consumed = 0;
	unsigned int i;
	char confirm[64];

	/* LVM config filter: only accept /dev/mapper/shr_tier_* */
	const char *lvm_filter =
		"devices { "
		"preferred_names=[\"^/dev/mapper/\"] "
		"filter = [ \"a|/dev/mapper/shr_tier_.*|\", \"r|.*|\" ] "
		"}";

	/* ---- Parse options ---- */
	for (i = 0; i < (unsigned int)argc; i++) {
		if (strcmp(argv[i], "--force") == 0) {
			force = 1;
			opt_consumed++;
		} else if (strcmp(argv[i], "--yes") == 0) {
			yes = 1;
			opt_consumed++;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Error: unknown option '%s'\n",
				argv[i]);
			fprintf(stderr, "Usage: lhsrctl shr expand "
				"[--force] [--yes] <device>...\n");
			return 1;
		}
	}

	new_count = argc - opt_consumed;
	if (new_count == 0) {
		fprintf(stderr, "Error: no new disk devices specified\n");
		fprintf(stderr, "Usage: lhsrctl shr expand "
			"[--force] [--yes] <device>...\n");
		return 1;
	}

	/* Point directly into argv — no allocation needed */
	const char **new_disks = (const char **)(argv + opt_consumed);

	disk_sizes = calloc(new_count, sizeof(uint64_t));
	if (!disk_sizes) {
		fprintf(stderr, "Error: out of memory\n");
		return 1;
	}

	/* ---- Validate new disks and read sizes ---- */
	printf("Scanning new devices ...\n");
	for (i = 0; i < new_count; i++) {
		int fd;
		uint64_t sz;

		if (shr_validate_device(new_disks[i]) < 0)
			goto out;

		fd = open(new_disks[i], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Error: cannot open %s: %s\n",
				new_disks[i], strerror(errno));
			goto out;
		}
		if (ioctl(fd, BLKGETSIZE64, &sz) < 0) {
			fprintf(stderr, "Error: cannot get size of %s: %s\n",
				new_disks[i], strerror(errno));
			close(fd);
			goto out;
		}
		close(fd);
		disk_sizes[i] = sz / 512;  /* convert bytes to sectors */
		printf("  %s: %llu sectors (%.1f GiB)\n",
		       new_disks[i],
		       (unsigned long long)disk_sizes[i],
		       (double)disk_sizes[i] * 512 / (1024*1024*1024));
	}

	/* ---- Scan existing LHSR tiers ---- */
	struct dm_entry dments[16];
	int ntiers = scan_dm_tiers("shr_tier_", dments, 16);
	if (ntiers == 0) {
		fprintf(stderr, "Error: no existing SHR tiers found.\n");
		fprintf(stderr, "Use 'lhsrctl shr create' to create "
			"a new SHR layout.\n");
		goto out;
	}

	printf("\nExisting LHSR tiers: %d\n", ntiers);
	for (i = 0; i < (unsigned int)ntiers; i++)
		printf("  %s\n", dments[i].name);

	/* ---- Find the LVM VG on existing tiers ---- */
	char vg_name[64] = "";
	{
		char buf[4096];
		int rv;
		rv = shr_capture(
			"lvm pvs --noheadings -o pv_name,vg_name "
			"2>/dev/null | "
			"grep '^[[:space:]]*/dev/mapper/shr_tier_' "
			"|| true",
			buf, sizeof(buf));
		(void)rv;
		if (buf[0]) {
			char pv[256], vg[64];
			char *nl = strchr(buf, '\n');
			if (nl)
				*nl = '\0';
			if (sscanf(buf, "%255s %63s", pv, vg) >= 2)
				snprintf(vg_name, sizeof(vg_name),
					 "%s", vg);
		}
	}
	if (!vg_name[0]) {
		fprintf(stderr, "Error: no LVM VG found on existing "
			"SHR tiers.\n");
		fprintf(stderr, "The tiers must be part of an LVM VG "
			"before expansion.\n");
		goto out;
	}
	printf("LVM volume group: %s\n", vg_name);

	/* ---- Determine next tier index ---- */
	unsigned int next_tier = 0;
	for (i = 0; i < (unsigned int)ntiers; i++) {
		unsigned int idx;
		if (sscanf(dments[i].name, "shr_tier_%u", &idx) == 1) {
			if (idx >= next_tier)
				next_tier = idx + 1;
		}
	}

	/* ---- Determine RAID type from disk count ---- */
	unsigned int raid_type;
	const char *raid_name;
	unsigned int parity_per_tier;
	if (new_count >= 3) {
		raid_type = LHSR_RAID5;
		raid_name = "RAID5";
		parity_per_tier = 1;
	} else if (new_count == 2) {
		raid_type = LHSR_RAID_MIRROR;
		raid_name = "RAID1";
		parity_per_tier = 1;  /* N-1 = 1 for 2-disk mirror */
	} else {
		raid_type = LHSR_RAID_SINGLE;
		raid_name = "SINGLE";
		parity_per_tier = 0;
	}

	/* ---- Calculate tier size ---- */
	uint64_t tier_sectors = disk_sizes[0];
	for (i = 1; i < new_count; i++) {
		if (disk_sizes[i] < tier_sectors)
			tier_sectors = disk_sizes[i];
	}
	/* Align down to SHR alignment */
	tier_sectors = (tier_sectors / SHR_ALIGNMENT) * SHR_ALIGNMENT;

	/* Usable = size * (members - parity) */
	uint64_t usable_sectors;
	if (parity_per_tier > 0 && new_count > parity_per_tier)
		usable_sectors = tier_sectors * (new_count - parity_per_tier);
	else
		usable_sectors = tier_sectors;

	/* ---- Print plan ---- */
	printf("\nSHR Expansion Plan\n");
	printf("==================\n");
	printf("Mode:               LHSR\n");
	printf("New disks:          %u\n", new_count);
	printf("New tier:           %s (%s)\n", raid_name,
	       new_count == 1 ? "no redundancy" :
	       new_count == 2 ? "mirrored" : "single parity");
	printf("Tier index:         %u\n", next_tier);
	printf("Tier device:        /dev/mapper/shr_tier_%u\n", next_tier);
	printf("Tier size:          %llu sectors (%.1f GiB)\n",
	       (unsigned long long)tier_sectors,
	       (double)tier_sectors * 512 / (1024*1024*1024));
	printf("Usable capacity:    %llu sectors (%.1f GiB)\n",
	       (unsigned long long)usable_sectors,
	       (double)usable_sectors * 512 / (1024*1024*1024));
	printf("\nLVM actions:\n");
	printf("  pvcreate /dev/mapper/shr_tier_%u\n", next_tier);
	printf("  vgextend %s /dev/mapper/shr_tier_%u\n",
	       vg_name, next_tier);
	printf("  lvextend -l +100%%FREE %s/shr_vol\n", vg_name);

	/* ---- Confirmation ---- */
	if (!yes) {
		printf("\nThis operation will modify LVM state.\n");
		printf("Type 'YES' to proceed, anything else to abort: ");
		fflush(stdout);
		if (!fgets(confirm, sizeof(confirm), stdin)) {
			printf("Aborted.\n");
			goto out;
		}
		{
			size_t clen = strlen(confirm);
			if (clen > 0 && confirm[clen - 1] == '\n')
				confirm[clen - 1] = '\0';
		}
		if (strcmp(confirm, "YES") != 0) {
			printf("Aborted.\n");
			goto out;
		}
	}

	/* ---- Execute ---- */
	printf("\nExecuting SHR expansion ...\n");

	/* Step 1: Check for stale /dev/mapper/shr_tier_N */
	{
		char stale_path[64];
		struct stat st;
		snprintf(stale_path, sizeof(stale_path),
			 "/dev/mapper/shr_tier_%u", next_tier);
		if (stat(stale_path, &st) == 0 && S_ISBLK(st.st_mode)) {
			fprintf(stderr, "Error: %s already exists.\n",
				stale_path);
			fprintf(stderr, "Remove it first with "
				"'dmsetup remove shr_tier_%u' "
				"or use --force.\n", next_tier);
			if (!force)
				goto out;
			fprintf(stderr, "  (--force override: "
				"removing existing device)\n");
			shr_run_cmd(
				"/usr/sbin/dmsetup remove "
				"shr_tier_%u 2>/dev/null; true",
				next_tier);
		}
	}

	/* Step 2: Pre-flight checks */
	printf("\n--- Pre-flight checks ---\n");
	for (i = 0; i < new_count; i++) {
		if (shr_warn_used_device(new_disks[i]) < 0) {
			if (!force) {
				fprintf(stderr,
					"Use --force to override.\n");
				goto out;
			}
			fprintf(stderr, "  (--force override)\n");
		}
	}

	/* Step 3: Create RAID tier */
	printf("\n--- Step 3: Create RAID tier ---\n");

	{
		char cmd[16384];
		int pos;

		if (raid_type == LHSR_RAID_SINGLE) {
			/* SINGLE: type + device + offset (kernel requires >=3 args) */
			pos = snprintf(cmd, sizeof(cmd),
				"dmsetup create shr_tier_%u --table "
				"\"0 %llu lhsr single %s 0\"",
				next_tier,
				(unsigned long long)tier_sectors,
				new_disks[0]);
			if (shr_run_cmd("%s", cmd) < 0)
				goto out;
		} else if (raid_type == LHSR_RAID_MIRROR) {
			/* MIRROR: type + dev/offset pairs */
			pos = snprintf(cmd, sizeof(cmd),
				"dmsetup create shr_tier_%u --table "
				"\"0 %llu lhsr mirror",
				next_tier,
				(unsigned long long)tier_sectors);
			for (i = 0; i < new_count; i++) {
				pos += snprintf(cmd + pos,
					sizeof(cmd) - pos,
					" %s 0", new_disks[i]);
				if ((size_t)pos >= sizeof(cmd) - 64) {
					fprintf(stderr, "Error: "
						"dmsetup table too long\n");
					goto out;
				}
			}
			snprintf(cmd + pos, sizeof(cmd) - pos, "\"");
			if (shr_run_cmd("%s", cmd) < 0)
				goto out;
		} else {
			/* RAID5/6: fixed kernel params (chunk_sects=8,
			 * stripes_per_cont=1, cont_sects=8) + dev/offset pairs */
			int raid_char = (raid_type == LHSR_RAID5) ? '5' : '6';
			pos = snprintf(cmd, sizeof(cmd),
				"dmsetup create shr_tier_%u --table "
				"\"0 %llu lhsr raid%c 8 1 8",
				next_tier,
				(unsigned long long)tier_sectors,
				raid_char);
			for (i = 0; i < new_count; i++) {
				pos += snprintf(cmd + pos,
					sizeof(cmd) - pos,
					" %s 0", new_disks[i]);
				if ((size_t)pos >= sizeof(cmd) - 64) {
					fprintf(stderr, "Error: "
						"dmsetup table too long\n");
					goto out;
				}
			}
			snprintf(cmd + pos, sizeof(cmd) - pos, "\"");
			if (shr_run_cmd("%s", cmd) < 0)
				goto out;
		}
	}

	/* Step 4: Wait for tier device */
	{
		char tier_path[64];
		snprintf(tier_path, sizeof(tier_path),
			 "/dev/mapper/shr_tier_%u", next_tier);
		if (shr_wait_for_part(tier_path, 0, 5000) < 0)
			goto out;
	}

	/* Step 5: PV and VG extend */
	printf("\n--- Step 5: LVM — extend VG ---\n");
	{
		char tier_dev[64];
		snprintf(tier_dev, sizeof(tier_dev),
			 "/dev/mapper/shr_tier_%u", next_tier);

		if (shr_run_cmd("pvcreate --config '%s' '%s'",
				lvm_filter, tier_dev) < 0)
			goto out;
		if (shr_run_cmd("vgextend --config '%s' "
				"'%s' '%s'",
				lvm_filter,
				vg_name, tier_dev) < 0)
			goto out;
	}

	/* Step 6: Extend LV */
	printf("\n--- Step 6: LVM — extend LV ---\n");
	{
		char lv_path[128];
		snprintf(lv_path, sizeof(lv_path), "%s/shr_vol", vg_name);
		/* allow_changes_with_duplicate_pvs=1 needed because LVM
		 * detects PV UUID fragments in RAID5-striped data on the
		 * underlying member devices.  The LHSR filter prevents LVM
		 * from USING those devices, but LVM still blocks metadata
		 * commits when it sees duplicates. */
		if (shr_run_cmd("lvextend --config "
				"'devices { allow_changes_with_duplicate_pvs=1 "
				"preferred_names=[\"^/dev/mapper/\"] "
				"filter = [ \"a|/dev/mapper/shr_tier_.*|\", "
				"\"r|.*|\" ] }' "
				"-l +100%%FREE '%s' --yes",
				lv_path) < 0) {
			fprintf(stderr, "Warning: lvextend failed. "
				"You can run it manually:\n");
			fprintf(stderr, "  lvextend -l +100%%FREE %s --yes\n",
				lv_path);
		}
	}

	/* ---- Success ---- */
	printf("\n===========================================================\n");
	printf("  SHR EXPANSION SUCCESSFUL\n");
	printf("===========================================================\n");
	printf("\nAdded %u new disk(s)", new_count);
	printf(" as %s tier /dev/mapper/shr_tier_%u\n",
	       raid_name, next_tier);
	printf("Volume group: %s\n", vg_name);
	printf("Tier size: %llu sectors (%.1f GiB)\n",
	       (unsigned long long)tier_sectors,
	       (double)tier_sectors * 512 / (1024*1024*1024));
	printf("New usable capacity: %llu sectors (%.1f GiB)\n",
	       (unsigned long long)usable_sectors,
	       (double)usable_sectors * 512 / (1024*1024*1024));
	printf("\nIf the filesystem supports online resize:\n");
	printf("  ext4:  resize2fs /dev/%s/shr_vol\n", vg_name);
	printf("  xfs:   xfs_growfs /mount/point\n");
	printf("\n");

	ret = 0;

out:
	free(disk_sizes);
	return ret;
}

/*
 * Replace a disk in an LHSR SHR tier.
 *
 * Usage (via cmd_shr_disk):
 *   shr disk replace <tier_name> <disk_idx> <new_dev>
 *
 * Uses the kernel module's device_path message to get the current device
 * for each disk index, builds a new dm table, loads/resumes, then starts
 * rebuild.
 */
static int shr_disk_replace(const char *tier_name,
			    unsigned int disk_idx, const char *new_dev)
{
	char cmd[4096];
	char resp[2048];
	char table_line[4096];
	unsigned long long size = 0;
	int raid_type = -1;
	int disks = 0;
		/* Kernel disk name is at most 32 bytes (DISK_NAME_LEN) */
		char dev_paths[32][64];
	int i;

	printf("Replacing disk %u in '%s' with %s\n",
	       disk_idx, tier_name, new_dev);

	/* 1. Verify new device exists and is a block device */
	{
		struct stat st;
		if (stat(new_dev, &st) < 0) {
			fprintf(stderr, "Error: '%s' does not exist\n", new_dev);
			return 1;
		}
		if (!S_ISBLK(st.st_mode)) {
			fprintf(stderr, "Error: '%s' is not a block device\n",
				new_dev);
			return 1;
		}
	}

	/* 2. Get config: raid=N disks=N */
	if (dm_msg(tier_name, "config", resp, sizeof(resp)) < 0 || !resp[0]) {
		fprintf(stderr, "Error: cannot get config from '%s'\n",
			tier_name);
		return 1;
	}
	{
		char work[1024];
		snprintf(work, sizeof(work), "%.1000s", resp);
		char *tok = strtok(work, " \t");
		while (tok) {
			int val;
			if (sscanf(tok, "raid=%d", &val) == 1)
				raid_type = val;
			else if (sscanf(tok, "disks=%d", &val) == 1)
				disks = val;
			tok = strtok(NULL, " \t");
		}
	}
	if (raid_type < 0 || disks <= 0) {
		fprintf(stderr, "Error: cannot parse config: '%s'\n", resp);
		return 1;
	}
	if (disk_idx >= (unsigned int)disks) {
		fprintf(stderr, "Error: disk index %u out of range (0-%d)\n",
			disk_idx, disks - 1);
		return 1;
	}
	printf("  RAID type=%d, disks=%d\n", raid_type, disks);

	/* 3. Get array size from dmsetup table (first 2 tokens: "0 <size> ...") */
	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/dmsetup table '%.63s' 2>/dev/null", tier_name);
	if (shr_capture(cmd, table_line, sizeof(table_line)) < 0 ||
	    !table_line[0]) {
		fprintf(stderr, "Error: cannot get table for '%s'\n", tier_name);
		return 1;
	}
	{
		char twork[4096];
		snprintf(twork, sizeof(twork), "%.3000s", table_line);
		/* Strip "name: " prefix if present */
		char *p = twork;
		char *colon = strchr(p, ':');
		if (colon) {
			p = colon + 1;
			while (*p == ' ') p++;
		}
		char *tok = strtok(p, " \t");
		if (tok) tok = strtok(NULL, " \t"); /* skip start=0 */
		if (tok)
			size = strtoull(tok, NULL, 10);
	}
	if (size == 0) {
		fprintf(stderr, "Error: cannot parse size from: '%s'\n",
			table_line);
		return 1;
	}
	printf("  Array size: %llu sectors\n", size);

	/* 4. Get device path for each disk index */
	for (i = 0; i < disks; i++) {
		char msgbuf[64];
		char dp_resp[256];
		snprintf(msgbuf, sizeof(msgbuf), "device_path %d", i);
		if (dm_msg(tier_name, msgbuf, dp_resp, sizeof(dp_resp)) < 0 ||
		    !dp_resp[0]) {
			fprintf(stderr, "Error: cannot get device path "
				"for disk %d\n", i);
			return 1;
		}
		/* Strip trailing whitespace/newline */
		{
			char *nl = dp_resp;
			while (*nl) nl++;
			while (nl > dp_resp &&
			       (nl[-1] == '\n' || nl[-1] == '\r' ||
				nl[-1] == ' '))
				*--nl = '\0';
		}
		/* Convert to partition path (kernel returns parent disk name,
		 * e.g. "loop0" but we need "/dev/loop0p1" for the dm table) */
		{
			char part_str[64];
			shr_get_part_dev(dp_resp, 1, part_str, sizeof(part_str));
			snprintf(dev_paths[i], sizeof(dev_paths[i]),
				 "/dev/%.31s", part_str);
		}
		printf("  Disk %d: %s\n", i, dev_paths[i]);
	}

	/* 5. Build new table line */
	{
		char new_table[8192];
		int pos = 0;

		if (raid_type == LHSR_RAID_SINGLE ||
		    raid_type == LHSR_RAID_MIRROR) {
			const char *type_str =
				(raid_type == LHSR_RAID_SINGLE)
				? "single" : "mirror";
			pos = snprintf(new_table, sizeof(new_table),
				      "0 %llu lhsr %s", size, type_str);
			for (i = 0; i < disks; i++) {
				const char *dev = (i == (int)disk_idx)
					? new_dev : dev_paths[i];
				pos += snprintf(new_table + pos,
						sizeof(new_table) - pos,
						" %s 0", dev);
				if (pos >= (int)sizeof(new_table) - 64) {
					fprintf(stderr,
						"Error: table too long\n");
					return 1;
				}
			}
		} else if (raid_type >= LHSR_RAID5) {
			const char *type_str =
				(raid_type == LHSR_RAID5)
				? "raid5" : "raid6";
			int parity = (raid_type == LHSR_RAID6) ? 2 : 1;
			pos = snprintf(new_table, sizeof(new_table),
				      "0 %llu lhsr %s 8 %d %d",
				      size, type_str, disks, parity);
			for (i = 0; i < disks; i++) {
				const char *dev = (i == (int)disk_idx)
					? new_dev : dev_paths[i];
				pos += snprintf(new_table + pos,
						sizeof(new_table) - pos,
						" %s 0", dev);
				if (pos >= (int)sizeof(new_table) - 64) {
					fprintf(stderr,
						"Error: table too long\n");
					return 1;
				}
			}
		} else {
			fprintf(stderr, "Error: unsupported RAID type %d\n",
				raid_type);
			return 1;
		}

		printf("  New table: %s\n", new_table);

		/* 6. Suspend the tier (required before load) */
		printf("  Suspending %s ... ", tier_name);
		fflush(stdout);
		snprintf(cmd, sizeof(cmd),
			 "/usr/sbin/dmsetup suspend '%.63s' 2>&1",
			 tier_name);
		if (system(cmd) != 0) {
			fprintf(stderr, "FAILED (suspend)\n");
			return 1;
		}
		printf("OK\n");

		/* 7. Load new table */
		printf("  Loading new table ... ");
		fflush(stdout);
		{
			char tmpfile[] = "/tmp/shr_dm_table_XXXXXX";
			int fd = mkstemp(tmpfile);
			if (fd < 0) {
				perror("  Failed to create temp file");
				return 1;
			}
			FILE *f = fdopen(fd, "w");
			if (!f) {
				close(fd);
				unlink(tmpfile);
				perror("  Failed to open temp file");
				return 1;
			}
			fprintf(f, "%s\n", new_table);
			fclose(f);

			snprintf(cmd, sizeof(cmd),
				 "/usr/sbin/dmsetup load '%.63s' < '%s' 2>&1",
				 tier_name, tmpfile);
			int rc = system(cmd);
			unlink(tmpfile);

			if (rc != 0) {
				fprintf(stderr,
					"FAILED (dmsetup load returned %d)\n",
					rc);
				/* Try to resume so device is not stuck */
				snprintf(cmd, sizeof(cmd),
					 "/usr/sbin/dmsetup resume "
					 "'%.63s' 2>&1", tier_name);
				system(cmd);
				return 1;
			}
		}
		printf("OK\n");

		/* 8. Resume tier */
		printf("  Resuming %s ... ", tier_name);
		fflush(stdout);
		snprintf(cmd, sizeof(cmd),
			 "/usr/sbin/dmsetup resume '%.63s' 2>&1",
			 tier_name);
		if (system(cmd) != 0) {
			fprintf(stderr, "FAILED\n");
			return 1;
		}
		printf("OK\n");

		/* 9. Mark replaced disk for rebuild */
		printf("  Marking disk %u for rebuild ... ", disk_idx);
		fflush(stdout);
		snprintf(cmd, sizeof(cmd),
			 "/usr/sbin/dmsetup message '%.63s' 0 "
			 "disk_fail %u 2>&1", tier_name, disk_idx);
		if (system(cmd) != 0) {
			fprintf(stderr, "FAILED\n");
			return 1;
		}
		printf("OK\n");

		/* 10. Start rebuild */
		printf("  Starting rebuild of disk %u ... ", disk_idx);
		fflush(stdout);
		snprintf(cmd, sizeof(cmd),
			 "/usr/sbin/dmsetup message '%.63s' 0 "
			 "rebuild start %u 2>&1", tier_name, disk_idx);
		if (system(cmd) != 0) {
			fprintf(stderr, "FAILED\n");
			return 1;
		}

		/* 10. Show initial rebuild status */
		if (dm_msg(tier_name, "rebuild status",
			   resp, sizeof(resp)) == 0 && resp[0]) {
			printf("OK\n  Rebuild: %s\n", resp);
		} else {
			printf("OK\n");
		}
	}

	return 0;
}


/*
 * Grow an SHR tier by adding one or more disks.
 *
 * NOT IMPLEMENTED for LHSR.  Online RAID reshape requires kernel changes
 * that have not been implemented in the LHSR kernel module.
 *
 * Use 'lhsrctl shr expand' to add capacity via a new tier instead.
 */
int cmd_shr_grow(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr,
		"Error: 'shr grow' applies to mdadm-based SHR only.\n"
		"LHSR does not support online RAID growth.\n"
		"Use 'lhsrctl shr expand' to add capacity "
		"via a new tier.\n");
	return 1;
}
