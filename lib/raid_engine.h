/*
 * LHSR RAID Engine Header
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#ifndef LHSR_RAID_H
#define LHSR_RAID_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Version */
#define LHSR_VERSION_MAJOR  1
#define LHSR_VERSION_MINOR   0
#define LHSR_VERSION_PATCH  0

/* RAID types */
enum lhsr_raid_type {
	LHSR_RAID_SINGLE = 0,
	LHSR_RAID_MIRROR = 1,
	LHSR_RAID5    = 2,
	LHSR_RAID6    = 3,
	LHSR_RAID_SHR = 4,
	LHSR_RAID_SHR2 = 5,
};

/* States */
enum lhsr_state {
	LHSR_STATE_OFFLINE    = 0,
	LHSR_STATE_ONLINE     = 1,
	LHSR_STATE_HEALTHY   = 2,
	LHSR_STATE_DEGRADED = 3,
	LHSR_STATE_REBUILDING = 4,
	LHSR_STATE_FAILED   = 5,
};

/* Checksum algorithms */
enum lhsr_checksum {
	LHSR_CHECKSUM_NONE   = 0,
	LHSR_CHECKSUM_CRC32C = 1,
	LHSR_CHECKSUM_BLAKE3 = 2,
};

/* Self-healing modes */
enum lhsr_heal_mode {
	LHSR_HEAL_DISABLED  = 0,
	LHSR_HEAL_PASSIVE  = 1,
	LHSR_HEAL_SCHEDULED = 2,
	LHSR_HEAL_AGGRESSIVE = 3,
};

/* Maximums */
#define LHSR_MAX_DISKS  32
#define LHSR_MAX_DISK_PATH 256

/* Context */
struct lhsr_context {
	uint32_t block_size;
	uint32_t checksum_algo;
	uint32_t heal_mode;
	uint32_t scrub_interval;

	pthread_mutex_t lock;
};

/* Disk information */
struct lhsr_disk {
	char path[LHSR_MAX_DISK_PATH];
	int fd;
	bool readonly;
	uint64_t size;  /* in bytes */
	int index;

	/* SMART data */
	int smart_reallocated;
	int smart_pending;
	int smart_uncorrectable;
	int smart_temperature;
	int health;

	/* Link to array */
	struct lhsr_array *array;
};

/* Array structure */
struct lhsr_array {
	char uuid[37];        /* UUID string */
	unsigned char uuid_bin[16];  /* UUID binary */

	char name[64];
	enum lhsr_raid_type raid_type;
	unsigned int disk_count;

	uint64_t total_capacity;
	uint64_t used_capacity;

	uint32_t block_size;
	uint32_t state;
	uint32_t checksum_algo;

	/* Disks */
	struct lhsr_disk **disks;
	unsigned int data_disks;
	unsigned int parity_disks;

	/* SHR segments */
	unsigned int segment_count;
	struct lhsr_segment *segments;
};

/* Segment (for SHR) */
struct lhsr_segment {
	uint64_t start;
	uint64_t size;
	enum lhsr_raid_type raid_type;
};

/* SMART data */
struct lhsr_smart_data {
	int reallocated;
	int pending;
	int uncorrectable;
	int temperature;
	int health;
};

/* Status */
struct lhsr_status {
	uint32_t state;
	uint32_t raid_type;
	unsigned int disk_count;
	unsigned int healthy_disks;
	uint64_t total_capacity;
	uint64_t used_capacity;
	uint64_t free_capacity;
	uint32_t disk_health[LHSR_MAX_DISKS];
	uint32_t disk_temp[LHSR_MAX_DISKS];
};

/* Block */
struct lhsr_block {
	void *data;
	size_t size;
	uint32_t checksum;
	uint32_t checksum_algo;
	int corrupted;
};

/* Context functions */
struct lhsr_context *lhsr_init(void);
void lhsr_free(struct lhsr_context *ctx);

/* Array functions */
struct lhsr_array *lhsr_array_create(struct lhsr_context *ctx,
					enum lhsr_raid_type raid_type,
					struct lhsr_disk *disks,
					unsigned int disk_count);
void lhsr_array_free(struct lhsr_array *arr);
int lhsr_array_status(struct lhsr_array *arr, struct lhsr_status *status);
void lhsr_array_print_status(struct lhsr_array *arr);

/* Disk functions */
int lhsr_disk_open(struct lhsr_disk *disk, const char *path);
void lhsr_disk_close(struct lhsr_disk *disk);
int lhsr_disk_read(struct lhsr_disk *disk, uint64_t offset, void *buf, size_t len);
int lhsr_disk_write(struct lhsr_disk *disk, uint64_t offset, const void *buf, size_t len);
int lhsr_disk_get_smart(struct lhsr_disk *disk, struct lhsr_smart_data *smart);

/* RAID operations */
int lhsr_verify_block(struct lhsr_block *block);
void lhsr_raid5_calc_parity(void *data, void **disks, unsigned int count, size_t block_len);
int lhsr_raid5_reconstruct(void *result, void **disks, unsigned int count,
			    unsigned int failed_disk, size_t block_len);
int lhsr_raid6_reconstruct(void *result, void *p, void *q,
			    void **disks, unsigned int count,
			    unsigned int failed1, unsigned int failed2, size_t block_len);

/* SHR layout */
void lhsr_shr_calculate_layout(struct lhsr_array *arr,
				struct lhsr_disk *disks,
				unsigned int count);

/* Utility */
const char *lhsr_raid_name(enum lhsr_raid_type type);

/* Checksum */
uint32_t lhsr_checksum_crc32c(const void *data, size_t len);

#endif /* LHSR_RAID_H */