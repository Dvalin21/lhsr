/*
 * LHSR Daemon - Shared types and helpers
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#ifndef LHSRD_H
#define LHSRD_H

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#define LHSRD_PID_FILE    "/run/lhsrd.pid"
#define LHSRD_CONFIG_DIR  "/etc/lhsr"
#define LHSRD_STATUS_FILE  "/run/lhsrd.status"
#define LHSRD_METRICS_FILE "/var/lib/lhsrd/metrics.prom"
#define LHSRD_CONF_FILE    LHSRD_CONFIG_DIR "/lhsrd.conf"
#define LHSRD_SOCKET_FILE  "/run/lhsrd.sock"

/* Trend DB defaults */
#define LHSRD_TREND_DB           "/var/lib/lhsrd/trends.db"
#define LHSRD_TREND_SNAPSHOT_INT 86400   /* 24 hours */

#define MONITOR_INTERVAL       60
#define SMART_POLL_INTERVAL   300

/* SMART thresholds */
#define SMART_REALLOCATED_THRESHOLD  100
#define SMART_PENDING_THRESHOLD      50
#define SMART_TEMP_THRESHOLD         50
#define SMART_HEALTH_LOW             50

/* Max arrays and disks */
#define LHSRD_MAX_ARRAYS  16
#define LHSRD_MAX_DISKS   32

/* Disk health state */
struct disk_health {
	char   device_path[256];
	int    disk_index;
	int    smart_reallocated;
	int    smart_pending;
	int    smart_uncorrectable;
	int    temperature;
	int    health;			/* 0-100 raw SMART health */
	int    health_score;		/* 0-100 composite (computed) */
	time_t last_check;
	int    consecutive_errors;
	int    failed;
};

/* Array tracking state */
struct array_state {
	char   dm_name[256];
	char   dm_uuid[256];
	int    raid_type;
	int    num_disks;
	int    num_working;
	uint64_t failed_disks;
	time_t last_status;
};

/* Daemon configuration */
struct daemon_config {
	int    smart_poll_interval;
	int    error_threshold;
	int    auto_failover;
	int    notify_on_fail;
	int    verbose;
	int    trend_enabled;
	char   trend_db_path[256];
	int    trend_snapshot_interval;	/* seconds between snapshots */
};

/* Global daemon state */
struct daemon_state {
	struct daemon_config  cfg;
	struct array_state    arrays[LHSRD_MAX_ARRAYS];
	int                   num_arrays;
	struct disk_health    disks[LHSRD_MAX_DISKS];
	int                   num_disks;
	pthread_mutex_t       lock;
	volatile int          running;
	time_t                start_time;  /* Daemon start time (for uptime) */
};

/* Shared state pointer (for signal handler access) */
extern struct daemon_state *g_state;

/* ---------- libdevmapper wrappers (lhsr-dm.c) ---------- */

/*
 * Initialize libdevmapper logging.
 */
void lhsr_dm_init(int verbose);

/*
 * Send a message to an LHSR DM device.
 * Returns 0 on success, -1 on error.
 */
int lhsr_dm_message(const char *dm_name, int sector, const char *msg);

/*
 * Get status of an LHSR DM device.
 * Returns a nul-terminated string (caller must free), or NULL on error.
 */
char *lhsr_dm_status(const char *dm_name);

/*
 * List all DM devices with the "lhsr" target.
 * Returns number of arrays found, or -1 on error.
 * arrays must hold at least LHSRD_MAX_ARRAYS entries.
 */
int lhsr_dm_list_arrays(struct array_state *arrays, int max_arrays);

/*
 * Create an LHSR DM device.
 * Returns 0 on success, -1 on error.
 */
int lhsr_dm_create(const char *name, const char *table);

/*
 * Remove an LHSR DM device.
 * Returns 0 on success, -1 on error.
 */
int lhsr_dm_remove(const char *name);

/*
 * Get the underlying block devices for an LHSR target.
 * Returns number of devices found, or -1 on error.
 */
int lhsr_dm_get_devices(const char *dm_name, char devices[][256], int max_devices);

/*
 * Suspend / resume an LHSR DM device.
 * Returns 0 on success, -1 on error.
 */
int lhsr_dm_suspend(const char *name);
int lhsr_dm_resume(const char *name);

/* ---------- SMART monitoring via sysfs/SG_IO (lhsr-smart.c) ---------- */

/*
 * Get SMART health via sysfs (fast, no SCSI command).
 * Returns 0 if basic health info is available, -1 otherwise.
 * If available, fills *health (0=bad, 100=good).
 */
int lhsr_smart_sysfs_health(const char *device, int *health);

/*
 * Get detailed SMART data via SG_IO ATA PASS-THROUGH.
 * Returns 0 on success, -1 on error.
 */
int lhsr_smart_sg_io(const char *device, struct disk_health *dh);

/*
 * Combined SMART poll: tries sysfs first, falls back to SG_IO.
 * Returns 0 on success, -1 on both methods failed.
 */
int lhsr_smart_poll(const char *device, struct disk_health *dh);

/* ---------- Config file parsing (lhsr-config.c) ---------- */

/*
 * Load configuration from file.
 * Returns 0 on success, -1 on error (file missing is not an error).
 */
int lhsr_config_load(struct daemon_config *cfg, const char *path);

/*
 * Save configuration to file.
 * Returns 0 on success, -1 on error.
 */
int lhsr_config_save(const struct daemon_config *cfg, const char *path);

#endif /* LHSRD_H */
