/*
 * LHSR Recovery Tool - Scan disks for LHSR arrays and attempt recovery
 *
 * Uses struct lhsr_superblock from the shared project header (include/lhsr.h),
 * the single source of truth for on-disk layout.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <time.h>

/*
 * include/lhsr.h handles type compatibility automatically:
 *   __KERNEL__  → <linux/types.h>
 *   __linux__   → <linux/types.h>
 *   otherwise   → <stdint.h> + manual typedefs
 */
#include "../../include/lhsr.h"

/* Disk states (user-visible strings) */
static const char *disk_state_str(uint32_t state)
{
	switch (state) {
	case LHSR_DISK_HEALTHY:    return "HEALTHY";
	case LHSR_DISK_DEGRADED:   return "DEGRADED";
	case LHSR_DISK_FAILED:     return "FAILED";
	case LHSR_DISK_REBUILDING: return "REBUILDING";
	default:                   return "UNKNOWN";
	}
}

/* RAID types (user-visible strings) */
static const char *raid_type_str(uint32_t type)
{
	switch (type) {
	case LHSR_RAID_SINGLE: return "SINGLE";
	case LHSR_RAID_MIRROR: return "MIRROR";
	case LHSR_RAID5:       return "RAID5";
	case LHSR_RAID6:       return "RAID6";
	case LHSR_RAID_SHR:    return "SHR";
	case LHSR_RAID_SHR2:   return "SHR2";
	default:               return "UNKNOWN";
	}
}

/* CRC32c - Castagnoli (userspace implementation, kernel uses crypto API) */
static uint32_t crc32c(uint8_t *buf, size_t len)
{
	uint32_t crc = 0xFFFFFFFF;
	static uint32_t table[256];
	static int table_init = 0;
	int i, j;

	if (!table_init) {
		for (i = 0; i < 256; i++) {
			uint32_t c = i;
			for (j = 0; j < 8; j++)
				c = (c >> 1) ^ (c & 1 ? 0x82F63B78 : 0);
			table[i] = c;
		}
		table_init = 1;
	}

	while (len--)
		crc = (crc >> 8) ^ table[(crc ^ *buf++) & 0xFF];

	return crc ^ 0xFFFFFFFF;
}

/*
 * Superblock locations — must match kernel module's layout in
 * lhsr_write_superblock/lhsr_read_superblock (dm-lhsr.c):
 *
 *   Primary: last usable sector (disk_offset + array_size)
 *   Backup:  primary + (LHSR_SB_SECTORS / 2)
 *
 * LHSR_SB_SECTORS = 16 sectors (8 KB) reserved at the end of the device.
 * Total device sectors = disk_size / 512.
 * Primary sector = total_sectors - LHSR_SB_SECTORS.
 * Backup sector  = total_sectors - LHSR_SB_SECTORS + (LHSR_SB_SECTORS / 2).
 */
#define LHSR_SCAN_SECTORS          (LHSR_SB_SECTORS)
#define LHSR_SCAN_PRIMARY_SECTOR(disk_sectors) ((disk_sectors) - LHSR_SCAN_SECTORS)
#define LHSR_SCAN_BACKUP_SECTOR(disk_sectors)  ((disk_sectors) - LHSR_SCAN_SECTORS + (LHSR_SCAN_SECTORS / 2))

static int read_superblock(int fd, off_t offset, struct lhsr_superblock *sb, int verify)
{
	ssize_t ret;
	uint32_t stored_csum, calc_csum;

	ret = pread(fd, sb, sizeof(*sb), offset);
	if (ret != (ssize_t)sizeof(*sb)) {
		fprintf(stderr, "Failed to read superblock at offset %ld: %s\n",
			(long)offset, strerror(errno));
		return -1;
	}

	/* Verify magic */
	if (memcmp(sb->magic, LHSR_MAGIC, LHSR_MAGIC_LEN) != 0) {
		return -1;  /* No LHSR superblock */
	}

	if (!verify)
		return 0;

	/* Verify checksum */
	stored_csum = sb->checksum;
	sb->checksum = 0;
	calc_csum = crc32c((uint8_t *)sb, sizeof(*sb));

	if (stored_csum != calc_csum) {
		fprintf(stderr, "Checksum mismatch: expected 0x%08x, got 0x%08x\n",
			stored_csum, calc_csum);
		/* Restore checksum before returning so caller sees the original value */
		sb->checksum = stored_csum;
		return -1;
	}

	/* Restore checksum so caller can display it */
	sb->checksum = stored_csum;
	return 0;
}

static int scan_disk(const char *device, int verbose, int do_recovery)
{
	int fd;
	struct lhsr_superblock sb_primary, sb_backup;
	struct stat st;
	uint64_t disk_bytes = 0;
	uint64_t total_sectors = 0;
	uint64_t primary_sector;
	uint64_t backup_sector;
	off_t primary_off;
	off_t backup_off;
	int found = 0;
	int ret;

	(void)do_recovery;

	if (verbose)
		printf("Scanning %s...\n", device);

	fd = open(device, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
		return -1;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		return -1;
	}

	/*
	 * Get device size: block devices use BLKGETSIZE64 ioctl;
	 * regular files use st_size.
	 */
	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &disk_bytes) < 0) {
			close(fd);
			return -1;
		}
	} else {
		if (st.st_size <= 0) {
			close(fd);
			return -1;
		}
		disk_bytes = (uint64_t)st.st_size;
	}

	total_sectors = disk_bytes / 512;

	/* No room for a superblock? */
	if (total_sectors <= LHSR_SCAN_SECTORS + 1) {
		close(fd);
		return -1;
	}

	primary_sector = LHSR_SCAN_PRIMARY_SECTOR(total_sectors);
	backup_sector  = LHSR_SCAN_BACKUP_SECTOR(total_sectors);

	primary_off = (off_t)(primary_sector * 512);
	backup_off  = (off_t)(backup_sector * 512);

	/* Try primary superblock */
	ret = read_superblock(fd, primary_off, &sb_primary, 1);
	if (ret == 0) {
		printf("\n=== LHSR Array Found ===\n");
		printf("Device: %s\n", device);
		printf("Location: PRIMARY @ 0x%lx\n", primary_off);
		printf("Array UUID: 0x%016llx\n", (unsigned long long)sb_primary.array_uuid);
		printf("Disk UUID: 0x%016llx\n", (unsigned long long)sb_primary.disk_uuid);
		printf("Disk Index: %u\n", sb_primary.disk_index);
		printf("Disk State: %s (0x%x)\n", disk_state_str(sb_primary.disk_state), sb_primary.disk_state);
		printf("RAID Type: %s (%u)\n", raid_type_str(sb_primary.raid_type), sb_primary.raid_type);
		printf("Disk Count: %u\n", sb_primary.disk_count);
		printf("Total Sectors: %llu (%.2f GB)\n",
		       (unsigned long long)sb_primary.total_sectors,
		       (double)sb_primary.total_sectors / 2048 / 1024);
		printf("Generation: %llu\n", (unsigned long long)sb_primary.generation);
		/* Copy packed members to aligned local variables */
		time_t creation_time = (time_t)sb_primary.creation_time;
		time_t last_update = (time_t)sb_primary.last_update;
		printf("Created: %s", ctime(&creation_time));
		printf("Updated: %s", ctime(&last_update));
		printf("Checksum: 0x%08x [VALID]\n", sb_primary.checksum);
		printf("Primary at gen %llu\n", (unsigned long long)sb_primary.generation);
		found = 1;
	}

	/* Try backup superblock */
	ret = read_superblock(fd, backup_off, &sb_backup, 1);
	if (ret == 0) {
		if (!found) {
			printf("\n=== LHSR Array Found (Backup Only) ===\n");
			printf("Device: %s\n", device);
		}
		printf("\nLocation: BACKUP @ 0x%lx\n", (long)backup_off);
		printf("Array UUID: 0x%016llx\n", (unsigned long long)sb_backup.array_uuid);
		printf("Generation: %llu\n", (unsigned long long)sb_backup.generation);
		printf("Checksum: 0x%08x [VALID]\n", sb_backup.checksum);

		if (found) {
			if (sb_primary.generation > sb_backup.generation)
				printf("Note: Primary is newer (gen %llu vs %llu)\n",
				       (unsigned long long)sb_primary.generation,
				       (unsigned long long)sb_backup.generation);
			else if (sb_backup.generation > sb_primary.generation)
				printf("Note: Backup is newer - data may be stale on primary!\n");
		}
		found = 1;
	}

	if (verbose && !found)
		printf("  No LHSR superblock found\n");

	close(fd);
	return found ? 0 : 1;
}

static void usage(const char *prog)
{
	printf("Usage: %s [options] [device...]\n", prog);
	printf("\nOptions:\n");
	printf("  -v        Verbose output\n");
	printf("  -r        Attempt automatic recovery\n");
	printf("  -x        Hexdump superblock contents\n");
	printf("  -h        Show this help\n");
	printf("\nExamples:\n");
	printf("  %s /dev/sdb          # Scan single disk\n", prog);
	printf("  %s /dev/sdb /dev/sdc # Scan multiple disks\n", prog);
	printf("  %s -v                # Scan all disks verbose\n", prog);
}

int main(int argc, char **argv)
{
	int opt;
	int verbose = 0;
	int i;
	int found = 0;
	int err = 0;

	printf("LHSR Recovery Scanner v1.0\n");
	printf("===========================\n\n");

	while ((opt = getopt(argc, argv, "vxrh")) != -1) {
		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		case 'r':
			/* Recovery mode: hint for lhsrctl recover */
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "No devices specified. Use -h for help.\n");
		return 1;
	}

	for (i = optind; i < argc; i++) {
		if (scan_disk(argv[i], verbose, 0) == 0)
			found++;
		else
			err++;
	}

	printf("\n=== Summary ===\n");
	printf("Scanned: %d devices\n", argc - optind);
	printf("Found LHSR arrays: %d\n", found);
	printf("No LHSR data: %d\n", err);

	if (found) {
		printf("\n=== Recovery Note ===\n");
		printf("Arrays detected. To attempt assembly, use:\n");
		printf("  # lhsrctl recover %s", argv[optind]);
		for (i = optind + 1; i < argc; i++)
			printf(" %s", argv[i]);
		printf("\n\nOr generate the dmsetup command manually:\n");
		printf("  # lhsrctl recover <devices> | grep 'dmsetup create'\n");
	}

	return 0;
}
