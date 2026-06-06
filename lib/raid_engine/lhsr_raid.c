/*
 * LHSR RAID Engine Library
 * Userspace library for RAID operations
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/random.h>
#include <time.h>

#include "../raid_engine.h"

/* Default values */
#define LHSR_DEFAULT_BLOCK_SIZE  (128 * 1024)
#define LHSR_MAX_DISK_PATH      256

/*
 * Initialize RAID engine context
 */
struct lhsr_context *lhsr_init(void)
{
	struct lhsr_context *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->block_size = LHSR_DEFAULT_BLOCK_SIZE;
	ctx->checksum_algo = LHSR_CHECKSUM_CRC32C;
	ctx->heal_mode = LHSR_HEAL_SCHEDULED;
	ctx->scrub_interval = 86400;  /* 24 hours */

	pthread_mutex_init(&ctx->lock, NULL);

	return ctx;
}

/*
 * Free context
 */
void lhsr_free(struct lhsr_context *ctx)
{
	if (!ctx)
		return;

	pthread_mutex_destroy(&ctx->lock);
	free(ctx);
}

/*
 * Calculate CRC32C checksum
 */
uint32_t lhsr_checksum_crc32c(const void *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFF;
	const uint8_t *p = data;

	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0x82F63B78;  /* CRC32C (Castagnoli) - matches kernel */
			else
				crc >>= 1;
		}
	}

	return ~crc;
}
/* CRC32C table lookup for faster computation */
static uint32_t crc32c_table[256];
static int crc32c_table_initialized = 0;

static void init_crc32c_table(void)
{
	if (crc32c_table_initialized)
		return;

	for (int i = 0; i < 256; i++) {
		uint32_t crc = i;
		for (int j = 0; j < 8; j++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0x82F63B78;
			else
				crc >>= 1;
		}
		crc32c_table[i] = crc;
	}
	crc32c_table_initialized = 1;
}

uint32_t lhsr_checksum_crc32c_fast(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t crc = 0xFFFFFFFF;

	init_crc32c_table();

	while (len--) {
		crc = crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	}

	return ~crc;
}

/*
 * Verify block checksum
 */
int lhsr_verify_block(struct lhsr_block *block)
{
	uint32_t calc_checksum;

	if (!block || !block->data)
		return -EINVAL;

	switch (block->checksum_algo) {
	case LHSR_CHECKSUM_CRC32C:
		calc_checksum = lhsr_checksum_crc32c(block->data, block->size);
		break;
	default:
		return -ENOTSUP;
	}

	if (calc_checksum != block->checksum) {
		block->corrupted = 1;
		return -EILSEQ;
	}

	block->corrupted = 0;
	return 0;
}

/*
 * Calculate parity for RAID5
 */
void lhsr_raid5_calc_parity(void *data, void **disks, unsigned int count, size_t block_size)
{
	memset(data, 0, block_size);

	for (unsigned int i = 0; i < count - 1; i++) {
		uint32_t *src = disks[i];
		uint32_t *dst = data;
		for (size_t j = 0; j < block_size / 4; j++)
			dst[j] ^= src[j];
	}
}

/*
 * Reconstruct block from RAID5
 */
int lhsr_raid5_reconstruct(void *result, void **disks, unsigned int count,
			    unsigned int failed_disk, size_t block_size)
{
	if (failed_disk >= count)
		return -EINVAL;

	/* XOR all remaining disks to reconstruct */
	memset(result, 0, block_size);

	for (unsigned int i = 0; i < count; i++) {
		if (i == failed_disk)
			continue;

		uint32_t *src = disks[i];
		uint32_t *dst = result;
		for (size_t j = 0; j < block_size / 4; j++)
			dst[j] ^= src[j];
	}

	return 0;
}

/*
 * Reconstruct block from RAID6 (dual parity)
 */
int lhsr_raid6_reconstruct(void *result, void *p, void *q,
			    void **disks, unsigned int count,
			    unsigned int failed1, unsigned int failed2, size_t block_size)
{
	(void)p;
	(void)q;
	(void)failed1;
	/* Simplified reconstruction - in production would use proper GF math */
	return lhsr_raid5_reconstruct(result, disks, count - 2, failed2, block_size);
}

/*
 * Open disk device
 */
int lhsr_disk_open(struct lhsr_disk *disk, const char *path)
{
	if (!disk || !path)
		return -EINVAL;

	disk->fd = open(path, O_RDWR);
	if (disk->fd < 0) {
		disk->fd = open(path, O_RDONLY);
		if (disk->fd < 0)
			return -errno;
		disk->readonly = 1;
	}

	/* Get device size */
	if (ioctl(disk->fd, BLKGETSIZE64, &disk->size) < 0)
		disk->size = 0;

	/* Get disk UUID from /dev/disk/by-id if available */
	strncpy(disk->path, path, LHSR_MAX_DISK_PATH - 1);

	return 0;
}

/*
 * Close disk
 */
void lhsr_disk_close(struct lhsr_disk *disk)
{
	if (disk && disk->fd >= 0) {
		close(disk->fd);
		disk->fd = -1;
	}
}

/*
 * Read block from disk
 */
int lhsr_disk_read(struct lhsr_disk *disk, uint64_t offset, void *buf, size_t len)
{
	ssize_t ret;

	if (!disk || !buf || disk->fd < 0)
		return -EINVAL;

	ret = pread(disk->fd, buf, len, offset);
	if (ret < 0)
		return -errno;

	return (ret == (ssize_t)len) ? 0 : -EIO;
}

/*
 * Write block to disk
 */
int lhsr_disk_write(struct lhsr_disk *disk, uint64_t offset, const void *buf, size_t len)
{
	ssize_t ret;

	if (!disk || !buf || disk->fd < 0 || disk->readonly)
		return -EINVAL;

	ret = pwrite(disk->fd, buf, len, offset);
	if (ret < 0)
		return -errno;

	return (ret == (ssize_t)len) ? 0 : -EIO;
}

/*
 * Read SMART data from disk
 */
int lhsr_disk_get_smart(struct lhsr_disk *disk, struct lhsr_smart_data *smart)
{
	char cmd[512];
	FILE *fp;

	if (!disk || !smart)
		return -EINVAL;

	/* Use smartctl to get SMART data */
	snprintf(cmd, sizeof(cmd), "smartctl -A %s 2>/dev/null", disk->path);

	fp = popen(cmd, "r");
	if (!fp)
		return -EIO;

	/* Parse smartctl output */
	memset(smart, 0, sizeof(*smart));

	char line[512];
	while (fgets(line, sizeof(line), fp)) {
		unsigned int id;
		if (sscanf(line, " %u ", &id) == 1) {
			/* Parse specific SMART attributes */
			switch (id) {
			case 5:  /* Reallocated_Sector_Ct */
				sscanf(line, "%*u %*u %u", &smart->reallocated);
				break;
			case 197: /* Current_Pending_Sector */
				sscanf(line, "%*u %*u %u", &smart->pending);
				break;
			case 198: /* Offline_Uncorrectable */
				sscanf(line, "%*u %*u %u", &smart->uncorrectable);
				break;
			case 194: /* Temperature_Celsius */
				sscanf(line, "%*u %*u %u", &smart->temperature);
				break;
			}
		}
	}

	pclose(fp);

	/* Calculate health score */
	smart->health = 100;
	if (smart->reallocated > 100)
		smart->health -= 30;
	if (smart->pending > 10)
		smart->health -= 20;
	if (smart->uncorrectable > 0)
		smart->health -= 40;

	return 0;
}

/*
 * Create new RAID array
 */
struct lhsr_array *lhsr_array_create(struct lhsr_context *ctx,
				      unsigned int raid_type,
				      struct lhsr_disk *disks,
				      unsigned int disk_count)
{
	struct lhsr_array *arr;
	char name[64];

	if (!ctx || !disks || disk_count < 2)
		return NULL;

	arr = calloc(1, sizeof(*arr));
	if (!arr)
		return NULL;

	/* Generate UUID */
	static unsigned int uuid_counter = 0;
	unsigned int seq = uuid_counter++;
	snprintf(arr->uuid, sizeof(arr->uuid), "%08x-%04x-%04x-%04x-%08x%04x",
		(unsigned int)time(NULL), getpid() & 0xFFFF,
		seq & 0xFFFF, 0,
		(unsigned int)time(NULL), uuid_counter);

	/* Set configuration */
	arr->raid_type = raid_type;
	arr->disk_count = disk_count;
	arr->block_size = ctx->block_size;
	arr->checksum_algo = ctx->checksum_algo;

	/* Copy disk references */
	arr->disks = calloc(disk_count, sizeof(struct lhsr_disk *));
	if (!arr->disks) {
		free(arr);
		return NULL;
	}

	for (unsigned int i = 0; i < disk_count; i++) {
		arr->disks[i] = &disks[i];
		disks[i].array = arr;
		disks[i].index = i;
	}

	/* Calculate capacity based on RAID type */
	switch (raid_type) {
	case LHSR_RAID_SINGLE:
		arr->total_capacity = disks[0].size;
		arr->data_disks = 1;
		arr->parity_disks = 0;
		break;
	case LHSR_RAID_MIRROR:
		arr->total_capacity = disks[0].size * (disk_count / 2);
		arr->data_disks = disk_count / 2;
		arr->parity_disks = disk_count / 2;
		break;
	case LHSR_RAID5:
		arr->total_capacity = disks[0].size * (disk_count - 1);
		arr->data_disks = disk_count - 1;
		arr->parity_disks = 1;
		break;
	case LHSR_RAID6:
		arr->total_capacity = disks[0].size * (disk_count - 2);
		arr->data_disks = disk_count - 2;
		arr->parity_disks = 2;
		break;
	case LHSR_RAID_SHR:
	case LHSR_RAID_SHR2:
		/* SHR calculates optimal layout */
		lhsr_shr_calculate_layout(arr, disks, disk_count);
		break;
	default:
		free(arr->disks);
		free(arr);
		return NULL;
	}

	/* Generate device name */
	snprintf(name, sizeof(name), "lhsr-%s", arr->uuid + 24);
	strncpy(arr->name, name, sizeof(arr->name));
	arr->name[sizeof(arr->name) - 1] = '\0';

	printf("Created %s array: %u disks, capacity %lu sectors\n",
	       lhsr_raid_name(raid_type), disk_count, arr->total_capacity);

	return arr;
}

/*
 * Calculate SHR layout (simplified)
 */
void lhsr_shr_calculate_layout(struct lhsr_array *arr,
				struct lhsr_disk *disks,
				unsigned int count)
{
	/* Simplified SHR calculation */
	/* In production, this would calculate optimal segment layout */

	arr->segment_count = 1;
	arr->segments = calloc(1, sizeof(struct lhsr_segment));
	if (!arr->segments)
		return;

	/* Calculate usable space (smallest disk * (count - 1) for RAID5 equivalent) */
	uint64_t min_size = disks[0].size;
	for (unsigned int i = 1; i < count; i++) {
		if (disks[i].size < min_size)
			min_size = disks[i].size;
	}

	arr->segments[0].start = 0;
	arr->segments[0].size = min_size * (count - 1);
	arr->segments[0].raid_type = LHSR_RAID5;
	arr->total_capacity = arr->segments[0].size;

	printf("SHR layout: %lu sectors across %u disks\n",
	       arr->total_capacity, count);
}

/*
 * Get RAID type name
 */
const char *lhsr_raid_name(unsigned int type)
{
	switch (type) {
	case LHSR_RAID_SINGLE: return "Single";
	case LHSR_RAID_MIRROR: return "Mirror";
	case LHSR_RAID5: return "RAID5";
	case LHSR_RAID6: return "RAID6";
	case LHSR_RAID_SHR: return "SHR";
	case LHSR_RAID_SHR2: return "SHR-2";
	default: return "Unknown";
	}
}

/*
 * Get array status
 */
int lhsr_array_status(struct lhsr_array *arr, struct lhsr_status *status)
{
	if (!arr || !status)
		return -EINVAL;

	memset(status, 0, sizeof(*status));

	status->state = arr->state;
	status->raid_type = arr->raid_type;
	status->disk_count = arr->disk_count;
	status->healthy_disks = arr->disk_count;

	/* Check each disk */
	for (unsigned int i = 0; i < arr->disk_count; i++) {
		struct lhsr_disk *disk = arr->disks[i];

		/* Get SMART data */
		struct lhsr_smart_data smart;
		lhsr_disk_get_smart(disk, &smart);

		if (smart.health < 50)
			status->healthy_disks--;

		status->disk_health[i] = smart.health;
		status->disk_temp[i] = smart.temperature;
	}

	/* Calculate capacity */
	status->total_capacity = arr->total_capacity;
	status->used_capacity = 0;  /* Would track actual usage */
	status->free_capacity = arr->total_capacity - status->used_capacity;

	return 0;
}

/*
 * Print array status
 */
void lhsr_array_print_status(struct lhsr_array *arr)
{
	struct lhsr_status status;

	if (lhsr_array_status(arr, &status) < 0) {
		fprintf(stderr, "Failed to get array status\n");
		return;
	}

	printf("\n=== LHSR Array Status ===\n");
	printf("Name:     %s\n", arr->name);
	printf("UUID:     %s\n", arr->uuid);
	printf("RAID:     %s\n", lhsr_raid_name(arr->raid_type));
	printf("State:    %s\n",
	       status.state == LHSR_STATE_HEALTHY ? "Healthy" :
	       status.state == LHSR_STATE_DEGRADED ? "Degraded" : "Failed");
	printf("Disks:    %u/%u healthy\n",
	       status.healthy_disks, status.disk_count);
	printf("Capacity: %lu GB used / %lu GB total\n",
	       status.used_capacity / 2 / 1024 / 1024,
	       status.total_capacity / 2 / 1024 / 1024);

	printf("\nDisk Status:\n");
	printf("%-8s %-10s %-10s %-8s\n", "Disk", "Health", "Temp", "State");
	printf("%-8s %-10s %-10s %-8s\n", "----", "------", "----", "-----");

	for (unsigned int i = 0; i < arr->disk_count; i++) {
		printf("/dev/sd%c  %3u%%      %3u°C    %s\n",
		       'a' + i,
		       status.disk_health[i],
		       status.disk_temp[i],
		       status.disk_health[i] >= 50 ? "OK" : "WARNING");
	}

	printf("\n");
}

/*
 * Free array
 */
void lhsr_array_free(struct lhsr_array *arr)
{
	if (!arr)
		return;

	if (arr->disks) {
		free(arr->disks);
	}

	if (arr->segments) {
		free(arr->segments);
	}

	free(arr);
}

/*
 * Create scrubber for array
 */
struct lhsr_scrubber *lhsr_scrubber_create(struct lhsr_array *arr)
{
	struct lhsr_scrubber *scrub;

	if (!arr)
		return NULL;

	scrub = calloc(1, sizeof(*scrub));
	if (!scrub)
		return NULL;

	scrub->array = arr;
	scrub->should_stop = 0;
	scrub->should_pause = 0;
	scrub->is_running = 0;
	scrub->request_rescan = 0;

	pthread_mutex_init(&scrub->lock, NULL);
	pthread_cond_init(&scrub->wake_cond, NULL);

	scrub->config.rate_limit = 50;
	scrub->config.max_io_depth = 32;
	scrub->config.priority = 1;
	scrub->config.skip_checksummed = 1;
	scrub->config.repair_on_error = 1;
	scrub->config.pause_on_error = 0;
	scrub->config.interruptible = 1;

	return scrub;
}

/*
 * Destroy scrubber
 */
void lhsr_scrubber_destroy(struct lhsr_scrubber *scrub)
{
	if (!scrub)
		return;

	lhsr_scrubber_stop(scrub);

	pthread_mutex_destroy(&scrub->lock);
	pthread_cond_destroy(&scrub->wake_cond);

	free(scrub);
}

/*
 * Verify a single block across all disks
 */
static int scrub_verify_block(struct lhsr_scrubber *scrub,
			     uint64_t offset, void *buffer, size_t block_size)
{
	(void)buffer;
	struct lhsr_array *arr = scrub->array;
	void *disk_buffers[LHSR_MAX_DISKS];
	void *parity_buffer = NULL;
	int has_error = 0;
	unsigned int i;

	memset(disk_buffers, 0, sizeof(disk_buffers));

	if (arr->raid_type >= LHSR_RAID5) {
		parity_buffer = malloc(block_size);
		if (!parity_buffer)
			return -ENOMEM;
	}

	for (i = 0; i < arr->disk_count; i++) {
		disk_buffers[i] = malloc(block_size);
		if (!disk_buffers[i])
			continue;

		ssize_t ret = pread(arr->disks[i]->fd, disk_buffers[i],
				    block_size, offset);
		if (ret != (ssize_t)block_size) {
			scrub->progress.corrupted_blocks++;
			has_error = 1;
			free(disk_buffers[i]);
			disk_buffers[i] = NULL;
		}
	}

	scrub->progress.verified_blocks++;

	if (parity_buffer && disk_buffers[0]) {
		void *disks_for_calc[LHSR_MAX_DISKS];
		unsigned int disk_count = 0;
	for (i = 0; i < arr->disk_count; i++) {
		free(disk_buffers[i]);
	}
		if (disk_count > 0)
			lhsr_raid5_calc_parity(parity_buffer, disks_for_calc,
					       disk_count, block_size);
	}

	for (i = 0; i < arr->disk_count; i++) {
		if (disk_buffers[i])
			free(disk_buffers[i]);
	}
	if (parity_buffer)
		free(parity_buffer);

	return has_error ? -EILSEQ : 0;
}

/*
 * Repair block by reconstructing from other disks
 */
int lhsr_scrubber_repair_block(struct lhsr_scrubber *scrub,
			       uint64_t offset, unsigned int disk_idx)
{
	struct lhsr_array *arr = scrub->array;
	void *reconstructed;
	void *disk_buffers[LHSR_MAX_DISKS];
	size_t block_size = arr->block_size;
	unsigned int i;
	int ret = 0;

	if (!scrub || disk_idx >= arr->disk_count)
		return -EINVAL;

	reconstructed = malloc(block_size);
	if (!reconstructed)
		return -ENOMEM;

	memset(disk_buffers, 0, sizeof(disk_buffers));

	for (i = 0; i < arr->disk_count; i++) {
		if (i == disk_idx)
			continue;

		disk_buffers[i] = malloc(block_size);
		if (!disk_buffers[i])
			continue;

		ssize_t r = pread(arr->disks[i]->fd, disk_buffers[i],
				  block_size, offset);
		if (r != (ssize_t)block_size) {
			free(disk_buffers[i]);
			disk_buffers[i] = NULL;
		}
	}

	if (arr->raid_type == LHSR_RAID5) {
		ret = lhsr_raid5_reconstruct(reconstructed, disk_buffers,
					      arr->disk_count, disk_idx,
					      block_size);
	} else if (arr->raid_type == LHSR_RAID6) {
		void *p = malloc(block_size);
		void *q = malloc(block_size);
		if (p && q) {
			ret = lhsr_raid6_reconstruct(reconstructed, p, q,
						      disk_buffers, arr->disk_count,
						      disk_idx, (disk_idx + 1) % arr->disk_count,
						      block_size);
			free(p);
			free(q);
		} else {
			ret = -ENOMEM;
		}
	}

	if (ret == 0) {
		ret = pwrite(arr->disks[disk_idx]->fd, reconstructed,
			     block_size, offset);
		if (ret == 0) {
			scrub->progress.repaired_blocks++;
			printf("Repaired block at offset %lu on disk %u\n",
			       offset, disk_idx);
		}
	}

	for (i = 0; i < arr->disk_count; i++) {
		if (disk_buffers[i])
			free(disk_buffers[i]);
	}
	free(reconstructed);

	return ret;
}

/*
 * Scrubber worker thread
 */
static void *scrub_worker(void *arg)
{
	struct lhsr_scrubber *scrub = arg;
	struct lhsr_array *arr = scrub->array;
	size_t block_size = arr->block_size;
	uint64_t offset;
	int ret;

	pthread_mutex_lock(&scrub->lock);

	scrub->progress.state = LHSR_SCRUB_RUNNING;
	scrub->progress.start_time = time(NULL);

	printf("Scrubber started for array %s\n", arr->uuid);

	while (!scrub->should_stop) {

		while (scrub->should_pause && !scrub->should_stop) {
			scrub->progress.state = LHSR_SCRUB_PAUSED;
			pthread_cond_wait(&scrub->wake_cond, &scrub->lock);
		}

		if (scrub->should_stop)
			break;

		scrub->progress.state = LHSR_SCRUB_RUNNING;

		offset = scrub->progress.current_offset;

		if (offset >= arr->total_capacity) {
			scrub->progress.state = LHSR_SCRUB_COMPLETED;
			break;
		}

		pthread_mutex_unlock(&scrub->lock);

		void *buffer = malloc(block_size);
		if (buffer) {
			ret = scrub_verify_block(scrub, offset, buffer, block_size);
			if (ret == -EILSEQ && scrub->config.repair_on_error) {
				for (unsigned int d = 0; d < arr->disk_count; d++) {
					lhsr_scrubber_repair_block(scrub, offset, d);
				}
			}
			free(buffer);
		}

		pthread_mutex_lock(&scrub->lock);

		scrub->progress.processed_blocks++;
		scrub->progress.current_offset += block_size;
		scrub->progress.last_update = time(NULL);

		if (scrub->request_rescan) {
			scrub->progress.current_offset = 0;
			scrub->progress.processed_blocks = 0;
			scrub->request_rescan = 0;
		}

		if (scrub->config.rate_limit > 0) {
			usleep((block_size * 1000000) / (scrub->config.rate_limit * 1024 * 1024));
		}
	}

	scrub->is_running = 0;
	scrub->progress.state = scrub->should_stop ? LHSR_SCRUB_IDLE : LHSR_SCRUB_COMPLETED;

	printf("Scrubber stopped for array %s\n", arr->uuid);

	pthread_mutex_unlock(&scrub->lock);

	return NULL;
}

/*
 * Start scrubber
 */
int lhsr_scrubber_start(struct lhsr_scrubber *scrub)
{
	int ret;

	if (!scrub)
		return -EINVAL;

	pthread_mutex_lock(&scrub->lock);

	if (scrub->is_running) {
		pthread_mutex_unlock(&scrub->lock);
		return -EBUSY;
	}

	scrub->should_stop = 0;
	scrub->should_pause = 0;
	scrub->progress.current_offset = 0;
	scrub->progress.processed_blocks = 0;
	scrub->progress.verified_blocks = 0;
	scrub->progress.corrupted_blocks = 0;
	scrub->progress.repaired_blocks = 0;
	scrub->progress.failed_blocks = 0;
	scrub->progress.total_capacity = scrub->array->total_capacity;

	ret = pthread_create(&scrub->thread, NULL, scrub_worker, scrub);
	if (ret == 0)
		scrub->is_running = 1;

	pthread_mutex_unlock(&scrub->lock);

	return ret;
}

/*
 * Stop scrubber
 */
int lhsr_scrubber_stop(struct lhsr_scrubber *scrub)
{
	if (!scrub)
		return -EINVAL;

	pthread_mutex_lock(&scrub->lock);

	if (!scrub->is_running) {
		pthread_mutex_unlock(&scrub->lock);
		return 0;
	}

	scrub->should_stop = 1;
	scrub->should_pause = 0;
	pthread_cond_signal(&scrub->wake_cond);

	pthread_mutex_unlock(&scrub->lock);

	pthread_join(scrub->thread, NULL);

	return 0;
}

/*
 * Pause scrubber
 */
int lhsr_scrubber_pause(struct lhsr_scrubber *scrub)
{
	if (!scrub)
		return -EINVAL;

	pthread_mutex_lock(&scrub->lock);
	scrub->should_pause = 1;
	pthread_mutex_unlock(&scrub->lock);

	return 0;
}

/*
 * Resume scrubber
 */
int lhsr_scrubber_resume(struct lhsr_scrubber *scrub)
{
	if (!scrub)
		return -EINVAL;

	pthread_mutex_lock(&scrub->lock);
	scrub->should_pause = 0;
	pthread_cond_signal(&scrub->wake_cond);
	pthread_mutex_unlock(&scrub->lock);

	return 0;
}

/*
 * Request rescan
 */
int lhsr_scrubber_rescan(struct lhsr_scrubber *scrub)
{
	if (!scrub)
		return -EINVAL;

	pthread_mutex_lock(&scrub->lock);
	scrub->request_rescan = 1;
	pthread_mutex_unlock(&scrub->lock);

	return 0;
}

/*
 * Get scrub progress
 */
int lhsr_scrubber_get_progress(struct lhsr_scrubber *scrub,
			       struct lhsr_scrub_progress *prog)
{
	if (!scrub || !prog)
		return -EINVAL;

	pthread_mutex_lock(&scrub->lock);
	*prog = scrub->progress;

	if (scrub->progress.state == LHSR_SCRUB_RUNNING &&
	    scrub->progress.processed_blocks > 0) {
		time_t elapsed = time(NULL) - scrub->progress.start_time;
		if (elapsed > 0) {
			uint64_t rate = scrub->progress.processed_blocks * scrub->array->block_size / elapsed;
			uint64_t remaining = scrub->array->total_capacity - scrub->progress.current_offset;
			scrub->progress.estimated_complete = time(NULL) + (remaining / (rate ?: 1));
		}
	}

	pthread_mutex_unlock(&scrub->lock);

	return 0;
}

/*
 * Set scrubber configuration
 */
void lhsr_scrubber_set_config(struct lhsr_scrubber *scrub,
			      struct lhsr_scrub_config *cfg)
{
	if (!scrub || !cfg)
		return;

	pthread_mutex_lock(&scrub->lock);
	scrub->config = *cfg;
	pthread_mutex_unlock(&scrub->lock);
}

/*
 * Verify block range
 */
int lhsr_scrubber_verify_block_range(struct lhsr_scrubber *scrub,
				      uint64_t start, uint64_t end)
{
	struct lhsr_array *arr;
	size_t block_size;
	uint64_t offset;
	void *buffer;
	int errors = 0;

	if (!scrub || !scrub->array)
		return -EINVAL;

	arr = scrub->array;
	block_size = arr->block_size;

	if (start >= arr->total_capacity || end > arr->total_capacity || start >= end)
		return -EINVAL;

	buffer = malloc(block_size);
	if (!buffer)
		return -ENOMEM;

	for (offset = start; offset < end; offset += block_size) {
		int ret = scrub_verify_block(scrub, offset, buffer, block_size);
		if (ret)
			errors++;
	}

	free(buffer);

	return errors;
}

/*
 * Print scrub status
 */
void lhsr_scrubber_print_status(struct lhsr_scrubber *scrub)
{
	struct lhsr_scrub_progress prog;

	if (!scrub)
		return;

	lhsr_scrubber_get_progress(scrub, &prog);

	printf("\n=== LHSR Scrub Status ===\n");
	printf("State:      %s\n",
	       prog.state == LHSR_SCRUB_IDLE ? "Idle" :
	       prog.state == LHSR_SCRUB_RUNNING ? "Running" :
	       prog.state == LHSR_SCRUB_PAUSED ? "Paused" :
	       prog.state == LHSR_SCRUB_COMPLETED ? "Completed" :
	       prog.state == LHSR_SCRUB_FAILED ? "Failed" : "Unknown");
	printf("Progress:   %lu / %lu blocks (%.1f%%)\n",
	       prog.processed_blocks, prog.total_capacity,
	       prog.total_capacity ? (double)prog.processed_blocks * 100 / prog.total_capacity : 0);
	printf("Verified:   %lu blocks\n", prog.verified_blocks);
	printf("Corrupted:  %lu blocks\n", prog.corrupted_blocks);
	printf("Repaired:   %lu blocks\n", prog.repaired_blocks);
	printf("Failed:     %lu blocks\n", prog.failed_blocks);

	if (prog.estimated_complete > 0) {
		char time_buf[64];
		struct tm *tm = localtime(&prog.estimated_complete);
		strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
		printf("ETA:        %s\n", time_buf);
	}

	printf("\n");
}

/*
 * Assess corruption severity based on checksum difference
 */
uint32_t lhsr_bitrotd_assess_severity(uint32_t expected, uint32_t actual)
{
	if (expected == actual)
		return LHSR_CORRUPT_NONE;

	uint32_t diff = expected ^ actual;

	if ((diff & (diff - 1)) == 0)
		return LHSR_CORRUPT_SINGLE_BIT;

	if ((diff & 0xFFFF) == 0)
		return LHSR_CORRUPT_MULTI_BIT;

	return LHSR_CORRUPT_FULL_BLOCK;
}

/*
 * Create bit-rot detector
 */
struct lhsr_bitrotd *lhsr_bitrotd_create(struct lhsr_array *arr)
{
	struct lhsr_bitrotd *det;

	if (!arr)
		return NULL;

	det = calloc(1, sizeof(*det));
	if (!det)
		return NULL;

	det->array = arr;
	det->is_running = 0;
	det->should_stop = 0;
	det->should_pause = 0;
	det->request_rescan = 0;

	pthread_mutex_init(&det->log.lock, NULL);

	det->log.max_entries = 1024;
	det->log.current_count = 0;
	det->log.write_offset = 0;
	det->log.entries = calloc(det->log.max_entries,
				sizeof(struct lhsr_corruption_entry));
	if (!det->log.entries) {
		pthread_mutex_destroy(&det->log.lock);
		free(det);
		return NULL;
	}

	det->verify_interval = 86400;
	det->log_retention_days = 30;
	det->alert_threshold = 10;
	det->auto_repair = 1;

	return det;
}

/*
 * Destroy bit-rot detector
 */
void lhsr_bitrotd_destroy(struct lhsr_bitrotd *det)
{
	if (!det)
		return;

	lhsr_bitrotd_stop(det);

	pthread_mutex_destroy(&det->log.lock);

	if (det->log.entries)
		free(det->log.entries);

	free(det);
}

/*
 * Log corruption event
 */
int lhsr_bitrotd_log_corruption(struct lhsr_bitrotd *det,
				struct lhsr_corruption_entry *entry)
{
	if (!det || !entry)
		return -EINVAL;

	pthread_mutex_lock(&det->log.lock);

	entry->detected_time = time(NULL);
	entry->resolved = 0;

	uint64_t idx = det->log.write_offset % det->log.max_entries;
	det->log.entries[idx] = *entry;

	det->log.write_offset++;
	det->log.current_count++;

	if (det->log.current_count > det->log.max_entries)
		det->log.current_count = det->log.max_entries;

	pthread_mutex_unlock(&det->log.lock);

	printf("ALERT: Bit-rot detected at block %lu disk %u severity %u\n",
	       entry->block_offset, entry->disk_index, entry->severity);

	return 0;
}

/*
 * Get corruption count
 */
int lhsr_bitrotd_get_corruption_count(struct lhsr_bitrotd *det)
{
	if (!det)
		return -EINVAL;

	pthread_mutex_lock(&det->log.lock);
	uint64_t count = det->log.current_count;
	pthread_mutex_unlock(&det->log.lock);

	return (int)count;
}

/*
 * Get corruption entries
 */
int lhsr_bitrotd_get_corruptions(struct lhsr_bitrotd *det,
				struct lhsr_corruption_entry *entries,
				uint64_t max_entries)
{
	if (!det || !entries)
		return -EINVAL;

	pthread_mutex_lock(&det->log.lock);

	uint64_t count = det->log.current_count;
	if (count > max_entries)
		count = max_entries;

	uint64_t start = det->log.write_offset - det->log.current_count;
	for (uint64_t i = 0; i < count; i++) {
		uint64_t idx = (start + i) % det->log.max_entries;
		entries[i] = det->log.entries[idx];
	}

	pthread_mutex_unlock(&det->log.lock);

	return (int)count;
}

/*
 * Verify single block for bit-rot
 */
int lhsr_bitrotd_verify_block(struct lhsr_bitrotd *det,
				uint64_t offset, unsigned int disk_idx,
				struct lhsr_integrity_result *result)
{
	struct lhsr_array *arr;
	struct lhsr_block block;
	uint32_t calc_checksum;
	void *buffer;
	int ret;

	if (!det || !result)
		return -EINVAL;

	arr = det->array;
	if (!arr || disk_idx >= arr->disk_count)
		return -EINVAL;

	result->block_offset = offset;
	result->disk_index = disk_idx;
	result->checksum_match = 1;
	result->severity = LHSR_CORRUPT_NONE;

	buffer = malloc(arr->block_size);
	if (!buffer)
		return -ENOMEM;

	ret = pread(arr->disks[disk_idx]->fd, buffer,
		    arr->block_size, offset);
	if (ret != (ssize_t)arr->block_size) {
		result->checksum_match = 0;
		result->severity = LHSR_CORRUPT_FULL_BLOCK;
		free(buffer);
		return -EIO;
	}

	calc_checksum = lhsr_checksum_crc32c(buffer, arr->block_size);
	free(buffer);

	block.checksum = calc_checksum;
	block.data = NULL;
	block.size = arr->block_size;

	ret = lhsr_verify_block(&block);
	if (ret != 0) {
		result->checksum_match = 0;
		result->severity = lhsr_bitrotd_assess_severity(
			block.checksum, block.checksum);
	}

	return 0;
}

/*
 * Print corruption log
 */
void lhsr_bitrotd_print_log(struct lhsr_bitrotd *det)
{
	struct lhsr_corruption_entry entry;
	struct tm *tm;
	char time_buf[64];
	uint64_t unresolved = 0;
	uint64_t resolved = 0;

	if (!det)
		return;

	printf("\n=== LHSR Anti-Bit-Rot Corruption Log ===\n");

	pthread_mutex_lock(&det->log.lock);

	for (uint64_t i = 0; i < det->log.current_count; i++) {
		entry = det->log.entries[i];
		if (entry.resolved)
			resolved++;
		else
			unresolved++;
	}

	printf("Total Events: %lu\n", det->log.current_count);
	printf("Unresolved:  %lu\n", unresolved);
	printf("Resolved:    %lu\n", resolved);

	if (det->log.current_count > 0) {
		printf("\nLast 10 Events:\n");
		printf("%-12s %-8s %-8s %-10s %s\n",
		       "Time", "Disk", "Offset", "Severity", "Status");
		printf("%-12s %-8s %-8s %-10s %s\n",
		       "----", "----", "------", "--------", "------");

		uint64_t start = det->log.write_offset > 10 ?
			       det->log.write_offset - 10 : 0;

		for (uint64_t i = start; i < det->log.write_offset; i++) {
			uint64_t idx = i % det->log.max_entries;
			entry = det->log.entries[idx];

			tm = localtime((time_t *)&entry.detected_time);
			strftime(time_buf, sizeof(time_buf),
			       "%Y-%m-%d %H:%M", tm);

			printf("/dev/sd%c  0x%06lx  %-10u  %s\n",
			       'a' + entry.disk_index,
			       entry.block_offset,
			       entry.severity,
			       entry.resolved ? "Resolved" : "UNRESOLVED");
		}
	}

	pthread_mutex_unlock(&det->log.lock);

	printf("\n");
}

/*
 * Start bit-rot detector daemon
 */
int lhsr_bitrotd_start(struct lhsr_bitrotd *det)
{
	if (!det)
		return -EINVAL;

	pthread_mutex_lock(&det->log.lock);

	if (det->is_running) {
		pthread_mutex_unlock(&det->log.lock);
		return -EBUSY;
	}

	det->should_stop = 0;
	det->should_pause = 0;
	det->is_running = 1;

	pthread_mutex_unlock(&det->log.lock);

	printf("Bit-rot detector started for array %s\n", det->array->uuid);

	return 0;
}

/*
 * Stop bit-rot detector
 */
int lhsr_bitrotd_stop(struct lhsr_bitrotd *det)
{
	if (!det)
		return -EINVAL;

	pthread_mutex_lock(&det->log.lock);

	if (!det->is_running) {
		pthread_mutex_unlock(&det->log.lock);
		return 0;
	}

	det->should_stop = 1;
	det->is_running = 0;

	pthread_mutex_unlock(&det->log.lock);

	printf("Bit-rot detector stopped\n");

	return 0;
}