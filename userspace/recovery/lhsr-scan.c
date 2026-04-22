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
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "../../lib/raid_engine.h"

#define PROGNAME "lhsr-scan"

static int verbose = 0;

static void usage(const char *prog)
{
    printf("Usage: %s [options] [device...]\n", prog);
    printf("Options:\n");
    printf("  -v        Verbose output\n");
    printf("  -r        Attempt automatic recovery\n");
    printf("  -h        Show this help\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                  # Scan all available disks\n", prog);
    printf("  %s /dev/sdb /dev/sdc # Scan specific disks\n", prog);
    printf("  %s -r               # Auto-repair found arrays\n", prog);
}

static int scan_disk(const char *device, int do_recovery)
{
    int fd;
    struct lhsr_disk disk;

    (void)do_recovery;

    if (verbose)
        printf("Scanning %s...\n", device);

    fd = open(device, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", device, strerror(errno));
        return -1;
    }

    memset(&disk, 0, sizeof(disk));
    strncpy(disk.path, device, sizeof(disk.path) - 1);

    if (ioctl(fd, BLKGETSIZE64, &disk.size) < 0) {
        close(fd);
        return -1;
    }

    close(fd);

    if (verbose)
        printf("  Size: %lu bytes\n", disk.size);

    return 0;
}

static int scan_devdir(const char *path, int do_recovery)
{
    DIR *dir;
    struct dirent *entry;
    char devpath[512];
    int count = 0;

    (void)do_recovery;

    dir = opendir(path);
    if (!dir)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        snprintf(devpath, sizeof(devpath), "%s/%s", path, entry->d_name);

        if (scan_disk(devpath, do_recovery) == 0)
            count++;
    }

    closedir(dir);
    return count;
}

int main(int argc, char **argv)
{
    int opt;
    int do_recovery = 0;
    int i;
    int found = 0;

    while ((opt = getopt(argc, argv, "vrh")) != -1) {
        switch (opt) {
        case 'v':
            verbose = 1;
            break;
        case 'r':
            do_recovery = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    printf("LHSR Recovery Scanner\n");
    printf("======================\n\n");

    if (optind < argc) {
        for (i = optind; i < argc; i++) {
            if (scan_disk(argv[i], do_recovery) == 0)
                found++;
        }
    } else {
        printf("Scanning /dev/disk/by-id...\n");
        found = scan_devdir("/dev/disk/by-id", do_recovery);

        if (found == 0) {
            printf("Scanning /dev/sd...\n");
            found = scan_devdir("/dev", do_recovery);
        }
    }

    printf("\nScan complete: %d device(s) scanned\n", found);

    return 0;
}