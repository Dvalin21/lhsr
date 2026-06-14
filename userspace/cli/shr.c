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
#include <dirent.h>
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

/* ===================================================================
 * Phase 6.2 — "lhsrctl shr create"
 *
 * Executes the SHR layout: partitions disks, creates RAID arrays
 * (mdadm or LHSR dmsetup), sets up LVM.
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

/* Step 3: Create RAID arrays (mdadm or LHSR dmsetup) */
static int shr_create_raid(struct shr_layout *layout,
			   const char **disk_paths, int use_lhsr)
{
	unsigned int i, j;
	char cmd[16384];

	printf("\n--- Step 3: Create RAID arrays ---\n");

	for (i = 0; i < layout->tier_count; i++) {
		struct shr_tier *t = &layout->tiers[i];
		int raid_level = (t->raid_type == 2) ? 5 : 6;

		if (use_lhsr) {
			/* LHSR: dmsetup create */
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
		} else {
			/* mdadm: build member list */
			int pos = 0;

			pos = snprintf(cmd, sizeof(cmd),
				"mdadm --create /dev/md/shr_tier_%u "
				"--level=%d --raid-devices=%u --bitmap=none",
				i, raid_level, t->partition_count);

			for (j = 0; j < layout->partition_count; j++) {
				char pdev[512];
				if (layout->partitions[j].tier != i)
					continue;
				unsigned int di = layout->partitions[j].disk_idx;
				unsigned int pn = layout->partitions[j].tier + 1;
				shr_get_part_dev(disk_paths[di], pn, pdev, sizeof(pdev));
				pos += snprintf(cmd + pos, sizeof(cmd) - pos,
						" %s", pdev);
				if ((size_t)pos >= sizeof(cmd) - 64) {
					fprintf(stderr, "Error: mdadm command too long\n");
					return -1;
				}
			}
			if (shr_run_cmd("%s", cmd) < 0)
				return -1;

			/* Wait for md device to appear */
			char mddev[64];
			snprintf(mddev, sizeof(mddev), "/dev/md/shr_tier_%u", i);
			shr_wait_for_part(mddev, 0, 5000);
		}
	}
	return 0;
}

/* Step 4: LVM — PVs, VG, LV */
static int shr_create_lvm(struct shr_layout *layout, int use_lhsr)
{
	unsigned int i;

	printf("\n--- Step 4: LVM setup ---\n");

	for (i = 0; i < layout->tier_count; i++) {
		if (use_lhsr) {
			if (shr_run_cmd("pvcreate /dev/mapper/shr_tier_%u",
					i) < 0)
				return -1;
		} else {
			if (shr_run_cmd("pvcreate /dev/md/shr_tier_%u",
					i) < 0)
				return -1;
		}
	}

	/* Create VG on first tier */
	printf("\n# Creating volume group 'shr_vg' ...\n");
	if (use_lhsr) {
		if (shr_run_cmd("vgcreate shr_vg /dev/mapper/shr_tier_0") < 0)
			return -1;
	} else {
		if (shr_run_cmd("vgcreate shr_vg /dev/md/shr_tier_0") < 0)
			return -1;
	}

	/* Extend VG with remaining tiers */
	for (i = 1; i < layout->tier_count; i++) {
		if (use_lhsr) {
			if (shr_run_cmd("vgextend shr_vg /dev/mapper/shr_tier_%u",
					i) < 0)
				return -1;
		} else {
			if (shr_run_cmd("vgextend shr_vg /dev/md/shr_tier_%u",
					i) < 0)
				return -1;
		}
	}

	/* Create LV spanning all available space */
	printf("\n# Creating logical volume ...\n");
	if (shr_run_cmd("lvcreate -l 100%%FREE -n shr_vol shr_vg") < 0)
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
	int use_lhsr = 0;
	int force = 0;
	int ret = 1;
	int opt_consumed = 1; /* skip "create" */
	char confirm[64];
	unsigned int i;
	struct shr_layout layout;

	/* Parse options: --parity N, --lhsr, --mdadm, --force */
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
		} else if (strcmp(argv[i], "--force") == 0) {
			force = 1;
			opt_consumed++;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
			fprintf(stderr, "Usage: lhsrctl shr create [--parity 1|2] "
				"[--lhsr|--mdadm] [--force] <device>...\n");
			return 1;
		}
	}

	disk_count = argc - opt_consumed;
	if (disk_count < 3) {
		fprintf(stderr, "Error: SHR requires at least 3 disks\n");
		fprintf(stderr, "Usage: lhsrctl shr create [--parity 1|2] "
			"[--lhsr|--mdadm] [--force] <device>...\n");
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

	/* Sort by size ascending for algorithm */
	qsort(sizes, disk_count, sizeof(uint64_t), cmp_u64_asc);

	/* Compute layout */
	if (shr_plan_layout(sizes, disk_count, parity, 0, 0, &layout) < 0)
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

	/* Step 3: Create RAID arrays */
	if (shr_create_raid(&layout, disk_paths, use_lhsr) < 0) {
		fprintf(stderr, "\nFAILED at Step 3 (RAID creation).\n"
			"Rollback: wipe partition superblocks with "
			"`mdadm --zero-superblock` on each partition.\n");
		goto out;
	}

	/* Step 4: LVM setup */
	if (shr_create_lvm(&layout, use_lhsr) < 0) {
		fprintf(stderr, "\nFAILED at Step 4 (LVM setup).\n"
			"Rollback:\n"
			"  lvremove shr_vg/shr_vol\n"
			"  vgremove shr_vg\n"
			"  pvremove on each RAID device\n"
			"  mdadm --stop /dev/md/shr_tier_*\n");
		goto out;
	}

	/* Success */
	printf("\n============================================================\n");
	printf("  SHR LAYOUT CREATED SUCCESSFULLY\n");
	printf("============================================================\n");
	printf("\nVolume Group:   shr_vg");
	printf("\nLogical Volume: shr_vg/shr_vol");
	printf("\n");
	if (use_lhsr)
		printf("\nRAID mode: LHSR (self-healing via dmsetup)\n");
	else
		printf("\nRAID mode: mdadm\n");
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

/* Entry from /dev/md/ directory scan. */
struct md_entry {
	char name[64];
	int md_num;    /* kernel md device number, -1 if unknown */
};

/* List /dev/md/<name> entries matching a prefix.  Returns count. */
static int scan_md_devices(const char *prefix,
			   struct md_entry *entries, int max)
{
	DIR *dir;
	struct dirent *de;
	int count = 0;

	dir = opendir("/dev/md");
	if (!dir)
		return 0;

	while ((de = readdir(dir)) != NULL && count < max) {
		if (de->d_name[0] == '.')
			continue;
		if (strncmp(de->d_name, prefix, strlen(prefix)) != 0)
			continue;
		snprintf(entries[count].name, sizeof(entries[count].name),
			 "%.63s", de->d_name);

		/* Resolve symlink to get md device number */
		char link[256];
		char full[512];
		snprintf(full, sizeof(full), "/dev/md/%s", de->d_name);
		ssize_t n = readlink(full, link, sizeof(link) - 1);
		if (n > 0) {
			link[n] = '\0';
			const char *p = strrchr(link, '/');
			if (p) p++;
			else p = link;
			if (p[0] == 'm' && p[1] == 'd')
				entries[count].md_num = atoi(p + 2);
		}
		count++;
	}
	closedir(dir);
	return count;
}

int cmd_shr_status(int argc, char **argv)
{
	struct md_entry mdents[16];
	int ntiers;
	unsigned int i;
	int found_lvm = 0;

	(void)argc;
	(void)argv;

	printf("LHSR SHR Status\n");
	printf("===============\n\n");

	/* ---- Scan SHR tiers ---- */
	ntiers = scan_md_devices("shr_tier_", mdents, 16);

	if (ntiers == 0) {
		printf("No SHR tiers found.\n");
		printf("  (Looked in /dev/md/ for shr_tier_* devices)\n\n");
		printf("No LVM VG created by SHR found.\n");
		printf("Use 'lhsrctl shr create' to create an SHR layout.\n");
		return 1;
	}

	printf("Tiers: %d\n\n", ntiers);

	/* ---- Print each tier ---- */
	for (i = 0; i < (unsigned int)ntiers; i++) {
		char sysfs[256];
		char buf[256];
		char level[64] = "?";
		int raid_disks = 0;
		int degraded = 0;
		uint64_t array_sectors = 0;
		char sync_action[64] = "?";
		char sync_progress[256] = "";
		char metadata[64] = "?";
		DIR *sdir;
		struct dirent *de;
		int member_count = 0;

		printf("  Tier %d\n", (int)i);
		printf("    Device: /dev/md/%s", mdents[i].name);
		if (mdents[i].md_num >= 0)
			printf(" (md%d)", mdents[i].md_num);
		printf("\n");

		if (mdents[i].md_num < 0)
			goto skip_sysfs;

		/* Read sysfs attributes */
		snprintf(sysfs, sizeof(sysfs), "/sys/block/md%d/md/level",
			 mdents[i].md_num);
		if (read_sysfs(sysfs, buf, sizeof(buf)) == 0)
			snprintf(level, sizeof(level), "%.63s", buf);

		snprintf(sysfs, sizeof(sysfs), "/sys/block/md%d/md/raid_disks",
			 mdents[i].md_num);
		if (read_sysfs(sysfs, buf, sizeof(buf)) == 0)
			raid_disks = atoi(buf);

		snprintf(sysfs, sizeof(sysfs), "/sys/block/md%d/md/degraded",
			 mdents[i].md_num);
		if (read_sysfs(sysfs, buf, sizeof(buf)) == 0)
			degraded = atoi(buf);

		snprintf(sysfs, sizeof(sysfs), "/sys/block/md%d/md/array_size",
			 mdents[i].md_num);
		if (read_sysfs(sysfs, buf, sizeof(buf)) == 0) {
			array_sectors = strtoull(buf, NULL, 10);
			/* "default" means full device — read size directly */
			if (array_sectors == 0 && buf[0] == 'd')
				array_sectors = 0; /* flag to fall through */
		}
		if (array_sectors == 0) {
			/* Fallback: read block device size */
			snprintf(sysfs, sizeof(sysfs),
				 "/sys/block/md%d/size",
				 mdents[i].md_num);
			if (read_sysfs(sysfs, buf, sizeof(buf)) == 0)
				array_sectors = strtoull(buf, NULL, 10);
		}

		snprintf(sysfs, sizeof(sysfs),
			 "/sys/block/md%d/md/sync_action",
			 mdents[i].md_num);
		if (read_sysfs(sysfs, buf, sizeof(buf)) == 0)
			snprintf(sync_action, sizeof(sync_action),
				 "%.63s", buf);

		snprintf(sysfs, sizeof(sysfs),
			 "/sys/block/md%d/md/sync_completed",
			 mdents[i].md_num);
		if (read_sysfs(sysfs, buf, sizeof(buf)) == 0 && buf[0])
			snprintf(sync_progress, sizeof(sync_progress),
				 " (%.127s)", buf);

		snprintf(sysfs, sizeof(sysfs),
			 "/sys/block/md%d/md/metadata_version",
			 mdents[i].md_num);
		if (read_sysfs(sysfs, buf, sizeof(buf)) == 0)
			snprintf(metadata, sizeof(metadata),
				 "%.63s", buf);

		printf("    RAID level: %s\n", level);
		printf("    Disks: %d configured, %d failed\n",
		       raid_disks, degraded);
		printf("    Size: %lu sectors (%.1f GiB)\n",
		       (unsigned long)array_sectors,
		       (double)array_sectors * 512 / (1024*1024*1024));
		printf("    Metadata: %s\n", metadata);
		printf("    Sync: %s%s\n", sync_action, sync_progress);

		/* List member partitions */
		snprintf(sysfs, sizeof(sysfs),
			 "/sys/block/md%d/slaves/",
			 mdents[i].md_num);
		sdir = opendir(sysfs);
		if (sdir) {
			printf("    Members:\n");
			while ((de = readdir(sdir)) != NULL) {
				if (de->d_name[0] == '.')
					continue;
				printf("      - %s\n", de->d_name);
				member_count++;
			}
			closedir(sdir);
		}
		if (member_count == 0)
			printf("    Members: (none)\n");
		printf("\n");

skip_sysfs: ;
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
						const char *type = "unknown";
						struct stat pv_st;
						unsigned int j;

						/* Check if PV is a known SHR tier */
						if (stat(pv, &pv_st) == 0) {
							for (j = 0; j < (unsigned int)ntiers; j++) {
								char md_path[128];
								struct stat md_st;
								snprintf(md_path, sizeof(md_path),
									 "/dev/md/%.63s",
									 mdents[j].name);
								if (stat(md_path, &md_st) == 0 &&
								    major(pv_st.st_rdev) ==
								    major(md_st.st_rdev) &&
								    minor(pv_st.st_rdev) ==
								    minor(md_st.st_rdev)) {
									type = "SHR tier";
									break;
								}
							}
						}
						if (strcmp(type, "unknown") == 0 &&
						    strstr(pv, "md"))
							type = "md device";
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
