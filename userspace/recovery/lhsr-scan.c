/*
 * LHSR Recovery Tool - Scan disks for LHSR arrays and attempt recovery
 *
 * Uses struct lhsr_superblock from the shared project header (include/lhsr.h),
 * the single source of truth for on-disk layout.
 *
 * Mode of operations:
 *   lhsr-scan /dev/sdb /dev/sdc   → scan listed disks (legacy text output)
 *   lhsr-scan --deep /dev/sdb     → deep scan metadata area
 *   lhsr-scan --json /dev/sdb     → machine-readable JSON output
 *   lhsr-scan -v                  → auto-detect and scan all block devices
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
#include <dirent.h>
#include <stdint.h>
#include <time.h>

/* LHSR_META2_SECTORS macro uses max() — define it if not already available */
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

/*
 * include/lhsr.h handles type compatibility automatically:
 *   __KERNEL__  → <linux/types.h>
 *   __linux__   → <linux/types.h>
 *   otherwise   → <stdint.h> + manual typedefs
 */
#include "../../include/lhsr.h"

/* ---------- Constants ---------- */

/* Max superblock copies we track per disk during deep scan */
#define MAX_SB_COPIES           64

/* Deep scan: step size in sectors (16 sectors = 8KB = 1 superblock) */
#define DEEP_SCAN_STEP_SECTORS  16

/*
 * Deep scan range: scan the last DEEP_SCAN_RANGE_SECTORS of the device.
 * Default 1MB = 2048 sectors (enough to cover metadata + margin).
 */
#define DEEP_SCAN_RANGE_SECTORS 2048

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

/* Position labels */
static const char *pos_str(int pos)
{
	switch (pos) {
	case 0:  return "PRIMARY";
	case 1:  return "BACKUP";
	default: return "DEEP";
	}
}

/* ---------- CRC32c ---------- */

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

/* ---------- Superblock location helpers ---------- */

/*
 * Primary at the end of the device.
 * Backup is halfway into the superblock reservation.
 */
#define LHSR_SCAN_SECTORS          (LHSR_SB_SECTORS)
#define LHSR_SCAN_PRIMARY_SECTOR(disk_sectors) ((disk_sectors) - LHSR_SCAN_SECTORS)
#define LHSR_SCAN_BACKUP_SECTOR(disk_sectors)  ((disk_sectors) - LHSR_SCAN_SECTORS + (LHSR_SCAN_SECTORS / 2))

/* ---------- Superblock read ---------- */

/*
 * Read a superblock at the given byte offset.
 * If verify is set, validates magic AND CRC32c checksum.
 * Returns 0 on success, -1 on any failure.
 * On success with checksum failure: returns -1 but sb is still populated.
 */
static int read_superblock(int fd, off_t offset, struct lhsr_superblock *sb, int verify)
{
	ssize_t ret;
	uint32_t stored_csum, calc_csum;

	memset(sb, 0, sizeof(*sb));
	ret = pread(fd, sb, sizeof(*sb), offset);
	if (ret != (ssize_t)sizeof(*sb))
		return -1;

	/* Verify magic */
	if (memcmp(sb->magic, LHSR_MAGIC, LHSR_MAGIC_LEN) != 0)
		return -1;

	if (!verify)
		return 0;

	/* Verify checksum */
	stored_csum = sb->checksum;
	sb->checksum = 0;
	calc_csum = crc32c((uint8_t *)sb, sizeof(*sb));

	if (stored_csum != calc_csum) {
		sb->checksum = stored_csum;  /* restore for display */
		return -1;
	}

	sb->checksum = stored_csum;
	return 0;
}

/*
 * Variant: read superblock and return whether checksum is valid.
 * Populates sb on any magic match.
 */
static int read_sb_with_csum(int fd, off_t offset, struct lhsr_superblock *sb,
			      int *csum_ok)
{
	int ret = read_superblock(fd, offset, sb, 0);
	if (ret != 0) {
		*csum_ok = 0;
		return -1;
	}
	/* Magic found — now check checksum */
	uint32_t stored = sb->checksum;
	sb->checksum = 0;
	uint32_t calc = crc32c((uint8_t *)sb, sizeof(*sb));
	sb->checksum = stored;
	*csum_ok = (stored == calc);
	return 0;
}

/* ---------- Device helpers ---------- */

static int get_device_size(int fd, uint64_t *sectors)
{
	struct stat st;
	uint64_t bytes = 0;

	if (fstat(fd, &st) < 0)
		return -1;

	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &bytes) < 0)
			return -1;
	} else {
		if (st.st_size <= 0)
			return -1;
		bytes = (uint64_t)st.st_size;
	}

	*sectors = bytes / 512;
	return 0;
}

/*
 * Auto-detect block devices: scan /dev/sd* and /dev/vd*.
 * Returns array of paths (caller must free), *count filled.
 */
static char **auto_detect_devices(int *count)
{
	const char *dirs[] = { "/dev", NULL };
	const char *patterns[] = { "sd", "vd", "nvme", NULL };
	char **devs = NULL;
	int n = 0, cap = 0;
	int p, d;

	*count = 0;

	for (d = 0; dirs[d]; d++) {
		DIR *dp = opendir(dirs[d]);
		if (!dp)
			continue;

		struct dirent *entry;
		while ((entry = readdir(dp)) != NULL) {
			for (p = 0; patterns[p]; p++) {
				int plen = strlen(patterns[p]);
				if (strncmp(entry->d_name, patterns[p], plen) != 0)
					continue;

				/* Must be a whole disk: sdX, vdX, nvmeXnY */
				char name[512];
				snprintf(name, sizeof(name), "/dev/%s", entry->d_name);

				/* Check it's a block device */
				struct stat st;
				if (stat(name, &st) < 0 || !S_ISBLK(st.st_mode))
					continue;

				/* Filter out partitions: skip if digit in name after prefix */
				const char *rest = entry->d_name + plen;
				int has_digit = 0;
				for (const char *r = rest; *r; r++) {
					if (*r >= '0' && *r <= '9') {
						has_digit = 1;
						break;
					}
				}
				if (has_digit)
					continue;  /* partition, skip */

				/* Add to list */
				if (n >= cap) {
					cap = cap ? cap * 2 : 32;
					char **tmp = realloc(devs, cap * sizeof(char *));
					if (!tmp) goto oom;
					devs = tmp;
				}
				devs[n] = strdup(name);
				if (!devs[n]) goto oom;
				n++;
				break;  /* matched one pattern, no need to check others */
			}
		}
		closedir(dp);
	}

	*count = n;
	return devs;

oom:
	for (int i = 0; i < n; i++)
		free(devs[i]);
	free(devs);
	*count = 0;
	return NULL;
}

/* ---------- Scan a single disk ---------- */

/*
 * Scan a single disk device.
 *
 * Output modes (not mutually exclusive):
 *   human=1: legacy text output to stdout
 *   json=1:  JSON fragment to fp_json (caller handles commas/braces)
 *   deep=1:  scan metadata area at DEEP_SCAN_STEP_SECTORS granularity
 *
 * Returns: 0 = found LHSR data, 1 = no LHSR data, -1 = error
 */
struct sb_entry {
	uint64_t sector;
	int position;       /* 0=primary, 1=backup, 2+=deep */
	int csum_valid;
	struct lhsr_superblock sb;
};

static int scan_disk_full(const char *device, int human, int json,
			   FILE *fp_json, int deep)
{
	struct lhsr_superblock sb_primary, sb_backup;
	uint64_t total_sectors = 0;
	uint64_t primary_sector, backup_sector;
	off_t primary_off, backup_off;
	int found = 0;

	/* Collected superblock entries for this disk */
	struct sb_entry entries[MAX_SB_COPIES];
	int n_entries = 0;

	int fd = open(device, O_RDONLY);
	if (fd < 0) {
		if (human)
			fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
		return -1;
	}

	if (get_device_size(fd, &total_sectors) < 0) {
		close(fd);
		return -1;
	}

	/* No room for metadata? */
	if (total_sectors <= LHSR_SCAN_SECTORS + 1) {
		close(fd);
		return -1;
	}

	primary_sector = LHSR_SCAN_PRIMARY_SECTOR(total_sectors);
	backup_sector  = LHSR_SCAN_BACKUP_SECTOR(total_sectors);
	primary_off = (off_t)(primary_sector * 512);
	backup_off  = (off_t)(backup_sector * 512);

	/* ---- Primary ---- */
	{
		int csum_ok;
		if (read_sb_with_csum(fd, primary_off, &sb_primary, &csum_ok) == 0) {
			if (n_entries < MAX_SB_COPIES) {
				entries[n_entries].sector = primary_sector;
				entries[n_entries].position = 0;
				entries[n_entries].csum_valid = csum_ok;
				memcpy(&entries[n_entries].sb, &sb_primary, sizeof(sb_primary));
				n_entries++;
			}
		}
	}

	/* ---- Backup ---- */
	{
		int csum_ok;
		if (read_sb_with_csum(fd, backup_off, &sb_backup, &csum_ok) == 0) {
			if (n_entries < MAX_SB_COPIES) {
				entries[n_entries].sector = backup_sector;
				entries[n_entries].position = 1;
				entries[n_entries].csum_valid = csum_ok;
				memcpy(&entries[n_entries].sb, &sb_backup, sizeof(sb_backup));
				n_entries++;
			}
		}
	}

	/* ---- Deep scan: scan metadata area at finer granularity ---- */
	if (deep) {
		/* Determine scan range: last DEEP_SCAN_RANGE_SECTORS of device,
		 * but at minimum the metadata reservation area. */
		uint64_t meta_start = total_sectors;
		uint64_t meta_sectors = LHSR_META2_SECTORS(total_sectors);

		if (meta_sectors < DEEP_SCAN_RANGE_SECTORS) {
			uint64_t range = DEEP_SCAN_RANGE_SECTORS;
			if (range > total_sectors)
				range = total_sectors;
			meta_start = total_sectors - range;
		} else {
			meta_start = total_sectors - meta_sectors;
		}

		/* Scan backwards from end of device */
		uint64_t scan_sector = total_sectors - LHSR_SCAN_SECTORS;
		while (scan_sector > meta_start && n_entries < MAX_SB_COPIES) {
			/* Skip primary and backup (already scanned) */
			if (scan_sector == primary_sector || scan_sector == backup_sector) {
				scan_sector -= DEEP_SCAN_STEP_SECTORS;
				continue;
			}

			off_t off = (off_t)(scan_sector * 512);
			int csum_ok;
			struct lhsr_superblock sb;
			if (read_sb_with_csum(fd, off, &sb, &csum_ok) == 0) {
				entries[n_entries].sector = scan_sector;
				entries[n_entries].position = 2;  /* DEEP */
				entries[n_entries].csum_valid = csum_ok;
				memcpy(&entries[n_entries].sb, &sb, sizeof(sb));
				n_entries++;
			}

			scan_sector -= DEEP_SCAN_STEP_SECTORS;
		}
	}

	close(fd);

	found = (n_entries > 0);

	/* ---- Output ---- */

	if (json) {
		/* Find best entry (highest generation with valid checksum) */
		int best = -1;
		uint64_t best_gen = 0;
		for (int i = 0; i < n_entries; i++) {
			if (!entries[i].csum_valid)
				continue;
			if (best < 0 || entries[i].sb.generation > best_gen) {
				best = i;
				best_gen = entries[i].sb.generation;
			}
		}

		fprintf(fp_json, "  {\n");
		fprintf(fp_json, "    \"device\": \"%s\",\n", device);
		fprintf(fp_json, "    \"total_sectors\": %llu,\n",
			(unsigned long long)total_sectors);
		fprintf(fp_json, "    \"found\": %s,\n", found ? "true" : "false");
		if (found) {
			fprintf(fp_json, "    \"superblocks\": [\n");
			for (int i = 0; i < n_entries; i++) {
				struct lhsr_superblock *sb = &entries[i].sb;
				fprintf(fp_json,
					"      {\"sector\":%llu,\"position\":\"%s\","
					"\"csum_valid\":%s,\"generation\":%llu,"
					"\"version\":%u,\"raid_type\":\"%s\","
					"\"disk_index\":%u,\"disk_count\":%u,"
					"\"disk_state\":\"%s\","
					"\"array_uuid\":\"0x%016llx\","
					"\"disk_uuid\":\"0x%016llx\"}%s\n",
					(unsigned long long)entries[i].sector,
					pos_str(entries[i].position),
					entries[i].csum_valid ? "true" : "false",
					(unsigned long long)sb->generation,
					sb->version,
					raid_type_str(sb->raid_type),
					sb->disk_index, sb->disk_count,
					disk_state_str(sb->disk_state),
					(unsigned long long)sb->array_uuid,
					(unsigned long long)sb->disk_uuid,
					(i < n_entries - 1) ? "," : "");
			}
			fprintf(fp_json, "    ],\n");
			if (best >= 0) {
				struct lhsr_superblock *sb = &entries[best].sb;
				fprintf(fp_json,
					"    \"best_generation\": %llu,\n"
					"    \"array_uuid\": \"0x%016llx\",\n"
					"    \"raid_type\": \"%s\",\n"
					"    \"disk_count\": %u,\n"
					"    \"disk_index\": %u,\n"
					"    \"disk_state\": \"%s\"\n",
					(unsigned long long)sb->generation,
					(unsigned long long)sb->array_uuid,
					raid_type_str(sb->raid_type),
					sb->disk_count, sb->disk_index,
					disk_state_str(sb->disk_state));
			}
		}
		fprintf(fp_json, "  }");
	}

	if (human) {
		if (!found) {
			printf("No LHSR superblock found on %s\n", device);
			return 1;
		}

		printf("\n=== LHSR Array Found on %s ===\n", device);
		for (int i = 0; i < n_entries; i++) {
			struct lhsr_superblock *sb = &entries[i].sb;
			printf("  SB#%d: %s @ sector %llu",
			       i, pos_str(entries[i].position),
			       (unsigned long long)entries[i].sector);
			if (!entries[i].csum_valid)
				printf(" [CHECKSUM FAILED]");
			printf("\n");
			printf("    Generation: %llu  |  Version: %u\n",
			       (unsigned long long)sb->generation, sb->version);
			printf("    Array UUID: 0x%016llx  |  Disk UUID: 0x%016llx\n",
			       (unsigned long long)sb->array_uuid,
			       (unsigned long long)sb->disk_uuid);
			printf("    Disk %u of %u  |  State: %s  |  RAID: %s\n",
			       sb->disk_index, sb->disk_count,
			       disk_state_str(sb->disk_state),
			       raid_type_str(sb->raid_type));
			printf("    Total sectors: %llu (%.2f GB)\n",
			       (unsigned long long)sb->total_sectors,
			       (double)sb->total_sectors / 2048.0 / 1024.0);
			time_t ct = (time_t)sb->creation_time;
			time_t lu = (time_t)sb->last_update;
			printf("    Created: %s", ctime(&ct));
			printf("    Updated: %s", ctime(&lu));
			printf("    Checksum: 0x%08x [%s]\n",
			       sb->checksum,
			       entries[i].csum_valid ? "VALID" : "INVALID");
		}

		/* Cross-reference if multiple copies */
		if (n_entries > 1) {
			uint64_t max_gen = 0, min_gen = UINT64_MAX;
			int n_csum_ok = 0, n_csum_bad = 0;
			for (int i = 0; i < n_entries; i++) {
				if (entries[i].sb.generation > max_gen)
					max_gen = entries[i].sb.generation;
				if (entries[i].sb.generation < min_gen)
					min_gen = entries[i].sb.generation;
				if (entries[i].csum_valid)
					n_csum_ok++;
				else
					n_csum_bad++;
			}
			printf("  Cross-ref: %d copies (%d valid, %d corrupt)"
			       "  |  gen range %llu..%llu\n",
			       n_entries, n_csum_ok, n_csum_bad,
			       (unsigned long long)min_gen,
			       (unsigned long long)max_gen);
			if (n_csum_bad > 0)
				printf("  *** %d superblock(s) have checksum errors — "
				       "possible corruption\n", n_csum_bad);
		}
	}

	return found ? 0 : 1;
}

/* ---------- Usage ---------- */

static void usage(const char *prog)
{
	printf("Usage: %s [options] [device...]\n", prog);
	printf("\nOptions:\n");
	printf("  -v            Verbose / auto-detect mode (scan all block devices)\n");
	printf("  --deep        Deep scan metadata area (finds orphaned superblocks)\n");
	printf("  -j, --json    JSON output (machine-parseable)\n");
	printf("  -r            Recovery mode (reserved for lhsrctl recover)\n");
	printf("  -h            Show this help\n");
	printf("\nIf no devices given and -v is set, auto-detects /dev/sd*, /dev/vd*, /dev/nvme*.\n");
	printf("\nExamples:\n");
	printf("  %s /dev/sdb              # Scan single disk\n", prog);
	printf("  %s /dev/sdb /dev/sdc     # Scan multiple disks\n", prog);
	printf("  %s --json /dev/sdb       # JSON output\n", prog);
	printf("  %s --deep /dev/sdb       # Deep scan\n", prog);
	printf("  %s -v                    # Auto-detect + scan all\n", prog);
}

/* ---------- Main ---------- */

int main(int argc, char **argv)
{
	int verbose = 0;
	int json_mode = 0;
	int deep = 0;
	int i;
	int found = 0;
	int err = 0;

	/* Parse options — handle both short and long forms */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0)
			verbose = 1;
		else if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0)
			json_mode = 1;
		else if (strcmp(argv[i], "--deep") == 0)
			deep = 1;
		else if (strcmp(argv[i], "-r") == 0)
			; /* reserved */
		else if (strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	/* Collect device paths (non-option arguments) */
	char **devices = NULL;
	int n_devargs = 0;

	for (i = 1; i < argc; i++) {
		if (argv[i][0] != '-') {
			n_devargs++;
			char **tmp = realloc(devices, n_devargs * sizeof(char *));
			if (!tmp) {
				fprintf(stderr, "Out of memory\n");
				free(devices);
				return 1;
			}
			devices = tmp;
			devices[n_devargs - 1] = argv[i];
		}
	}

	/* Auto-detect if no devices specified and -v is set */
	if (n_devargs == 0) {
		if (verbose) {
			int count;
			char **auto_devs = auto_detect_devices(&count);
			if (!auto_devs || count == 0) {
				fprintf(stderr, "No block devices found to scan\n");
				free(auto_devs);
				free(devices);
				return 1;
			}
			free(devices);
			devices = auto_devs;
			n_devargs = count;

			if (!json_mode) {
				printf("Auto-detected %d block device(s):\n", count);
				for (i = 0; i < count; i++)
					printf("  %s\n", devices[i]);
			}
		} else {
			if (!json_mode)
				fprintf(stderr, "No devices specified. Use -v for auto-detect or -h for help.\n");
			free(devices);
			return 1;
		}
	}

	/* JSON preamble */
	FILE *json_fp = NULL;
	int first_device_json = 1;
	if (json_mode) {
		json_fp = stdout;
		fprintf(json_fp, "{\n");
		fprintf(json_fp, "  \"tool\": \"lhsr-scan\",\n");
		fprintf(json_fp, "  \"version\": \"2.0\",\n");
		fprintf(json_fp, "  \"devices\": [\n");
	}

	/* Scan each device */
	for (i = 0; i < n_devargs; i++) {
		if (json_mode) {
			if (!first_device_json)
				fprintf(json_fp, ",\n");
			first_device_json = 0;
		}

		if (!json_mode && verbose)
			printf("\nScanning %s...\n", devices[i]);

		int res;
		if (json_mode || deep)
			res = scan_disk_full(devices[i],
					     !json_mode,     /* human output if not json */
					     json_mode,
					     json_fp,
					     deep);
		else
			res = scan_disk_full(devices[i], 1, 0, NULL, 0);

		if (res == 0)
			found++;
		else if (res == 1)
			err++;
	}

	/* JSON postamble */
	if (json_mode) {
		fprintf(json_fp, "\n  ],\n");
		fprintf(json_fp, "  \"summary\": {\n");
		fprintf(json_fp, "    \"scanned\": %d,\n", n_devargs);
		fprintf(json_fp, "    \"found\": %d,\n", found);
		fprintf(json_fp, "    \"no_data\": %d\n", err);
		fprintf(json_fp, "  }\n");
		fprintf(json_fp, "}\n");
	}

	/* Human summary */
	if (!json_mode) {
		printf("\n=== Summary ===\n");
		printf("Scanned: %d devices\n", n_devargs);
		printf("Found LHSR arrays: %d\n", found);
		printf("No LHSR data: %d\n", err);

		if (found) {
			printf("\n=== Recovery Note ===\n");
			printf("Arrays detected. To attempt assembly, use:\n");
			printf("  # lhsrctl recover <devices>\n");
		}
	}

	free(devices);
	return found ? 0 : 1;
}
