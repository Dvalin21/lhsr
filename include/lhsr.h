/*
 * LHSR - Linux Hybrid Self-Healing RAID
 * Device Mapper Kernel Module
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#ifndef LHSR_H
#define LHSR_H

#include <linux/types.h>

/* Magic identifier for superblock */
#define LHSR_MAGIC "LHSRDISK"
#define LHSR_MAGIC_LEN 8

/* Version */
#define LHSR_VERSION 1

/* Disk roles */
#define LHSR_ROLE_DATA        0
#define LHSR_ROLE_PARITY     1
#define LHSR_ROLE_METADATA  2
#define LHSR_ROLE_SPARE     3
#define LHSR_ROLE_CACHE    4
#define LHSR_ROLE_JOURNAL  5

/* Disk states */
#define LHSR_STATE_HEALTHY    0
#define LHSR_STATE_DEGRADED   1
#define LHSR_STATE_REBUILDING 2
#define LHSR_STATE_FAILED   3
#define LHSR_STATE_READONLY  4
#define LHSR_STATE_EVACUATING 5

/* RAID types */
#define LHSR_RAID_SINGLE   0
#define LHSR_RAID_MIRROR  1
#define LHSR_RAID5       2
#define LHSR_RAID6       3
#define LHSR_RAID_SHR    4
#define LHSR_RAID_SHR2    5

/* Default block size */
#define LHSR_DEFAULT_BLOCK_SIZE  (128 * 1024)

/* Superblock locations */
#define LHSR_SUPERBLOCK_PRIMARY_OFFSET  (4 * 1024 * 1024)  /* 4MB */
#define LHSR_SUPERBLOCK_BACKUP_OFFSET   (0 - (4 * 1024 * 1024)) /* End - 4MB */

/* Checksum algorithms */
#define LHSR_CHECKSUM_CRC32C  0
#define LHSR_CHECKSUM_BLAKE3  1
#define LHSR_CHECKSUM_XXHASH64 2

/* Self-healing modes */
#define LHSR_HEAL_PASSIVE  0
#define LHSR_HEAL_SCHEDULED 1
#define LHSR_HEAL_AGGRESSIVE 2

/* Superblock structure - on-disk format */
struct lhsr_superblock {
    __u8  magic[8];           /* "LHSRDISK" */
    __u32 version;
    __u64 array_uuid;
    __u64 disk_uuid;
    __u64 creation_time;
    __u64 last_update;
    __u32 disk_role;
    __u32 disk_state;
    __u64 total_blocks;
    __u64 block_size;
    __u64 metadata_offset;
    __u64 data_offset;
    __u64 journal_offset;
    __u32 checksum_algorithm;
    __u32 flags;
    __u8  reserved[20];
    __u64 checksum;
} __attribute__((packed));

/* Metadata header */
struct lhsr_metadata_header {
    __u64 generation;
    __u64 last_update;
    __u64 block_count;
    __u64 raid_level;
    __u64 disk_count;
    __u64 checksum;
} __attribute__((packed));

/* Segment layout for SHR */
struct lhsr_segment {
    __u64 start_block;
    __u64 length;
    __u32 raid_type;
    __u32 disk_count;
    __u64 disk_ids[16];
} __attribute__((packed));

/* Block entry */
struct lhsr_block_entry {
    __u64 block_id;
    __u32 checksum;
    __u32 flags;
} __attribute__((packed));

/* Block flags */
#define LHSR_BLOCK_COMPRESSED  0x01
#define LHSR_BLOCK_ENCRYPTED 0x02
#define LHSR_BLOCK_RELOCATED 0x04
#define LHSR_BLOCK_SCRUBBED  0x08
#define LHSR_BLOCK_DIRTY     0x10

/* IOCTL commands */
#define LHSR_IOCTL_CREATE      0x100
#define LHSR_IOCTL_ADD       0x101
#define LHSR_IOCTL_REMOVE    0x102
#define LHSR_IOCTL_REBUILD  0x103
#define LHSR_IOCTL_SCRUB    0x104
#define LHSR_IOCTL_STATUS   0x105
#define LHSR_IOCTL_REPAIR  0x106

/* Device numbers */
#define LHSR_MAJOR          253
#define LHSR_CTRL_MINOR     0
#define LHSR_DISK_MINOR     1

#endif /* LHSR_H */