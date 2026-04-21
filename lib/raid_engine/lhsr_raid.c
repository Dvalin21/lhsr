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

#include "../lib/raid_engine.h"

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
				crc = (crc >> 1) ^ 0xEDB88320;
			else
				crc >>= 1;
		}
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

	return (ret == len) ? 0 : -EIO;
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

	return (ret == len) ? 0 : -EIO;
}

/*
 * Read SMART data from disk
 */
int lhsr_disk_get_smart(struct lhsr_disk *disk, struct lhsr_smart_data *smart)
{
	char cmd[256];
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
				      enum lhsr_raid_type raid_type,
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
	snprintf(arr->uuid, sizeof(arr->uuid), "%08x-%04x-%04x-%04x-%08x%04x",
		(unsigned int)time(NULL), getpid() & 0xFFFF,
		(uuid_counter++) & 0xFFFF, 0,
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
	strncpy(arr->name, name, sizeof(arr->name) - 1);

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
const char *lhsr_raid_name(enum lhsr_raid_type type)
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