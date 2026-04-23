/*
 * LHSR Recovery Tool - Scan disks for LHSR arrays and attempt recovery
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

#include "../../lib/raid_engine.h"

/* LHSR Superblock - must match kernel version */
#define LHSR_SB_MAGIC         "LHSRDISK"
#define LHSR_SB_MAGIC_LEN     8
#define LHSR_SB_VERSION       1
#define LHSR_SB_PRIMARY_OFF   (4 * 1024 * 1024)
#define LHSR_SB_BACKUP_OFF(disk_size) ((disk_size) - (8 * 1024 * 1024))
#define LHSR_SB_SIZE          128

struct lhsr_sb {
    uint8_t     magic[8];
    uint32_t    version;
    uint64_t    array_uuid;
    uint64_t    disk_uuid;
    uint64_t    creation_time;
    uint64_t    last_update;
    uint32_t    disk_index;
    uint32_t    disk_state;
    uint32_t    raid_type;
    uint32_t    disk_count;
    uint64_t    total_sectors;
    uint64_t    generation;
    uint32_t    checksum;
    uint32_t    flags;
    uint8_t     reserved[44];
} __attribute__((packed));

/* Disk states */
static const char *disk_state_str(uint32_t state)
{
    switch (state) {
    case 0: return "HEALTHY";
    case 1: return "DEGRADED";
    case 2: return "FAILED";
    case 3: return "REBUILDING";
    default: return "UNKNOWN";
    }
}

/* RAID types */
static const char *raid_type_str(uint32_t type)
{
    switch (type) {
    case 0: return "SINGLE";
    case 1: return "MIRROR";
    case 2: return "RAID5";
    case 3: return "RAID6";
    case 4: return "SHR";
    case 5: return "SHR2";
    default: return "UNKNOWN";
    }
}

/* CRC32c - Castagnoli */
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

    while (len--) {
        crc ^= *buf++;
        crc = (crc >> 8) ^ table[crc & 0xFF];
        crc = (crc >> 8) ^ table[crc & 0xFF];
        crc = (crc >> 8) ^ table[crc & 0xFF];
        crc = (crc >> 8) ^ table[crc & 0xFF];
    }

    return crc ^ 0xFFFFFFFF;
}

static void hexdump(uint8_t *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 32 == 0)
            printf("\n");
        else if ((i + 1) % 8 == 0)
            printf(" ");
    }
    if (len % 32 != 0)
        printf("\n");
}

static int read_superblock(int fd, off_t offset, struct lhsr_sb *sb, int verify)
{
    ssize_t ret;
    uint32_t stored_csum, calc_csum;

    ret = pread(fd, sb, LHSR_SB_SIZE, offset);
    if (ret != LHSR_SB_SIZE) {
        fprintf(stderr, "Failed to read superblock at offset %ld: %s\n",
                (long)offset, strerror(errno));
        return -1;
    }

    /* Verify magic */
    if (memcmp(sb->magic, LHSR_SB_MAGIC, LHSR_SB_MAGIC_LEN) != 0) {
        return -1;  /* No LHSR superblock */
    }

    if (!verify)
        return 0;

    /* Verify checksum */
    stored_csum = sb->checksum;
    sb->checksum = 0;
    calc_csum = crc32c((uint8_t *)sb, LHSR_SB_SIZE);

    if (stored_csum != calc_csum) {
        fprintf(stderr, "Checksum mismatch: expected 0x%08x, got 0x%08x\n",
                stored_csum, calc_csum);
        return -1;
    }

    return 0;
}

static int scan_disk(const char *device, int verbose, int do_recovery)
{
    int fd;
    struct lhsr_sb sb_primary, sb_backup;
    off_t primary_off = LHSR_SB_PRIMARY_OFF;
    off_t backup_off;
    struct stat st;
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

    backup_off = LHSR_SB_BACKUP_OFF(st.st_size);

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
        printf("Created: %s", ctime((time_t *)&sb_primary.creation_time));
        printf("Updated: %s", ctime((time_t *)&sb_primary.last_update));
        printf("Checksum: 0x%08x [VALID]\n", sb_primary.checksum);
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
            printf("Auto-recovery: enabled (not yet implemented)\n");
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

    return 0;
}