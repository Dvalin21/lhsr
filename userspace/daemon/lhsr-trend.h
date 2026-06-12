/*
 * LHSR Daemon - SMART trend tracking via SQLite
 *
 * Stores daily SMART snapshots and computes linear regression
 * slopes for trend analysis.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#ifndef LHSR_TREND_H
#define LHSR_TREND_H

#include "lhsrd.h"

/* Trend result for a single disk */
struct lhsr_trend {
	int    data_points;         /* Number of snapshots used */
	double reallocated_slope;   /* Sectors/day (positive = worsening) */
	double pending_slope;
	double uncorrectable_slope;
	double temperature_slope;   /* Degrees C/day */
	int    reallocated_warn;    /* Nonzero if slope exceeds threshold */
	int    pending_warn;
	int    uncorrectable_warn;
	int    temperature_warn;
};

/* Thresholds for trend warnings (slope per day) */
#define LHSR_TREND_REALLOCATED_WARN  1.0  /* >1 sector/day = warning */
#define LHSR_TREND_PENDING_WARN      1.0
#define LHSR_TREND_UNCORRECTABLE_WARN 1.0
#define LHSR_TREND_TEMP_WARN         2.0  /* >2 degrees C/day */

/* Default paths and intervals */
#define LHSR_TREND_DB_PATH    "/var/lib/lhsrd/trends.db"
#define LHSR_TREND_SNAPSHOT_INTERVAL 86400  /* 24 hours */

/* Initialize trend database. Creates tables if needed.
 * Returns 0 on success, -1 on error. */
int lhsr_trend_init(const char *db_path);

/* Record a SMART snapshot for a disk.
 * Skips if a snapshot already exists within the interval.
 * Returns 0 on success, -1 on error. */
int lhsr_trend_record(const char *disk_path, struct disk_health *dh);

/* Query trends for a disk.
 * Returns 0 on success, -1 if insufficient data (<3 points). */
int lhsr_trend_query(const char *disk_path, struct lhsr_trend *trend);

/* Get a human-readable trend warning string.
 * Returns length written (0 = no warning). */
int lhsr_trend_warning(const char *disk_path, char *buf, size_t buf_size);

/* Close trend database. */
void lhsr_trend_close(void);

#endif /* LHSR_TREND_H */
