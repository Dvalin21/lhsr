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

/* Scrubber states */
enum lhsr_scrub_state {
	LHSR_SCRUB_IDLE = 0,
	LHSR_SCRUB_RUNNING = 1,
	LHSR_SCRUB_PAUSED = 2,
	LHSR_SCRUB_COMPLETED = 3,
	LHSR_SCRUB_FAILED = 4,
};

/* Scrubber configuration */
struct lhsr_scrub_config {
	uint64_t rate_limit;          /* MB/s limit for scrub */
	uint32_t max_io_depth;        /* Max concurrent I/O operations */
	uint32_t priority;            /* I/O priority (0=low, 3=high) */
	uint32_t skip_checksummed;    /* Skip blocks with valid checksums */
	uint32_t repair_on_error;     /* Auto-repair corrupted blocks */
	uint32_t pause_on_error;      /* Pause on uncorrectable error */
	uint32_t interruptible;       /* Allow scrub to be interrupted */
};

/* Scrubber progress */
struct lhsr_scrub_progress {
	enum lhsr_scrub_state state;
	uint64_t total_blocks;
	uint64_t processed_blocks;
	uint64_t verified_blocks;
	uint64_t corrupted_blocks;
	uint64_t repaired_blocks;
	uint64_t failed_blocks;
	uint64_t current_offset;
	uint64_t total_capacity;
	time_t start_time;
	time_t last_update;
	time_t estimated_complete;
};

/* Scrubber */
struct lhsr_scrubber {
	pthread_t thread;
	pthread_cond_t wake_cond;
	pthread_mutex_t lock;
	int should_stop;
	int should_pause;
	int is_running;
	int request_rescan;

	struct lhsr_array *array;
	struct lhsr_scrub_config config;
	struct lhsr_scrub_progress progress;
};

/* Scrubber functions */
struct lhsr_scrubber *lhsr_scrubber_create(struct lhsr_array *arr);
void lhsr_scrubber_destroy(struct lhsr_scrubber *scrub);
int lhsr_scrubber_start(struct lhsr_scrubber *scrub);
int lhsr_scrubber_stop(struct lhsr_scrubber *scrub);
int lhsr_scrubber_pause(struct lhsr_scrubber *scrub);
int lhsr_scrubber_resume(struct lhsr_scrubber *scrub);
int lhsr_scrubber_rescan(struct lhsr_scrubber *scrub);
int lhsr_scrubber_get_progress(struct lhsr_scrubber *scrub,
			       struct lhsr_scrub_progress *prog);
void lhsr_scrubber_set_config(struct lhsr_scrubber *scrub,
			      struct lhsr_scrub_config *cfg);
int lhsr_scrubber_verify_block_range(struct lhsr_scrubber *scrub,
				      uint64_t start, uint64_t end);
int lhsr_scrubber_repair_block(struct lhsr_scrubber *scrub,
			       uint64_t offset, unsigned int disk_idx);
void lhsr_scrubber_print_status(struct lhsr_scrubber *scrub);

#endif /* LHSR_RAID_H */