/*
 * LHSR Daemon - SMART trend tracking via SQLite
 *
 * Stores daily SMART snapshots and computes linear regression
 * slopes for trend analysis.  Single daemon, single DB connection.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <syslog.h>

#include "lhsr-trend.h"

/* Single database handle (daemon-wide) */
static sqlite3 *db = NULL;

/* Prepared statements */
static sqlite3_stmt *stmt_insert   = NULL;
static sqlite3_stmt *stmt_latest   = NULL;
static sqlite3_stmt *stmt_query    = NULL;

/* ------------------------------------------------------------------ */
/*  Internal: execute a one-shot SQL string (no result)               */
/* ------------------------------------------------------------------ */
static int exec_sql(const char *sql)
{
	char *err = NULL;
	int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK) {
		syslog(LOG_ERR, "trend DB: %s: %s", sql, err);
		sqlite3_free(err);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Internal: prepare (or re-prepare) a statement                     */
/* ------------------------------------------------------------------ */
static int prepare_stmt(const char *sql, sqlite3_stmt **stmt)
{
	if (*stmt) {
		sqlite3_finalize(*stmt);
		*stmt = NULL;
	}
	int rc = sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
	if (rc != SQLITE_OK) {
		syslog(LOG_ERR, "trend DB prepare: %s: %s", sql, sqlite3_errmsg(db));
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int lhsr_trend_init(const char *db_path)
{
	if (db) {
		syslog(LOG_WARNING, "trend DB already open, closing first");
		lhsr_trend_close();
	}

	/* Open (or create) the database */
	int rc = sqlite3_open(db_path, &db);
	if (rc != SQLITE_OK) {
		syslog(LOG_ERR, "trend DB open(%s): %s", db_path, sqlite3_errmsg(db));
		db = NULL;
		return -1;
	}

	/* WAL mode for concurrent readers (lhsrctl) */
	exec_sql("PRAGMA journal_mode=WAL");
	/* Synchronous FULL = safe on power loss */
	exec_sql("PRAGMA synchronous=FULL");

	/* Create table if not exists */
	const char *create =
		"CREATE TABLE IF NOT EXISTS smart_snapshots ("
		"  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
		"  disk_path         TEXT    NOT NULL,"
		"  snapshot_time     INTEGER NOT NULL,"
		"  reallocated       INTEGER DEFAULT 0,"
		"  pending           INTEGER DEFAULT 0,"
		"  uncorrectable     INTEGER DEFAULT 0,"
		"  temperature       INTEGER DEFAULT 0,"
		"  power_on_hours    INTEGER DEFAULT 0,"
		"  wear_level        INTEGER DEFAULT 0,"
		"  health_score      INTEGER DEFAULT 100,"
		"  UNIQUE(disk_path, snapshot_time)"
		")";
	if (exec_sql(create) < 0)
		goto fail;

	/* Index for fast lookup by disk+time */
	if (exec_sql(
		"CREATE INDEX IF NOT EXISTS idx_snapshots_disk_time "
		"ON smart_snapshots(disk_path, snapshot_time)") < 0)
		goto fail;

	/* Prepared: insert */
	if (prepare_stmt(
		"INSERT OR IGNORE INTO smart_snapshots "
		"(disk_path, snapshot_time, reallocated, pending, uncorrectable, "
		" temperature, power_on_hours, wear_level, health_score) "
		"VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)", &stmt_insert) < 0)
		goto fail;

	/* Prepared: latest snapshot time for a disk */
	if (prepare_stmt(
		"SELECT MAX(snapshot_time) FROM smart_snapshots "
		"WHERE disk_path = ?1", &stmt_latest) < 0)
		goto fail;

	/* Prepared: linear regression data for last 30 points */
	if (prepare_stmt(
		"SELECT snapshot_time, reallocated, pending, uncorrectable, temperature "
		"FROM smart_snapshots "
		"WHERE disk_path = ?1 "
		"ORDER BY snapshot_time DESC LIMIT 30", &stmt_query) < 0)
		goto fail;

	syslog(LOG_INFO, "trend DB opened: %s", db_path);
	return 0;

fail:
	lhsr_trend_close();
	return -1;
}

int lhsr_trend_record(const char *disk_path, struct disk_health *dh)
{
	if (!db || !stmt_insert || !stmt_latest)
		return -1;

	time_t now = time(NULL);

	/* Check if we already have a recent snapshot */
	sqlite3_reset(stmt_latest);
	sqlite3_bind_text(stmt_latest, 1, disk_path, -1, SQLITE_STATIC);

	int rc = sqlite3_step(stmt_latest);
	if (rc == SQLITE_ROW) {
		time_t last = (time_t)sqlite3_column_int64(stmt_latest, 0);
		/* Skip if snapshot exists within the interval */
		if (last > 0 && (now - last) < LHSR_TREND_SNAPSHOT_INTERVAL)
			return 0;
	}
	sqlite3_reset(stmt_latest);

	/* Insert new snapshot */
	sqlite3_reset(stmt_insert);
	sqlite3_bind_text(stmt_insert, 1, disk_path, -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt_insert, 2, (sqlite3_int64)now);
	sqlite3_bind_int(stmt_insert, 3, dh->smart_reallocated);
	sqlite3_bind_int(stmt_insert, 4, dh->smart_pending);
	sqlite3_bind_int(stmt_insert, 5, dh->smart_uncorrectable);
	sqlite3_bind_int(stmt_insert, 6, dh->temperature);
	sqlite3_bind_int(stmt_insert, 7, 0);		/* power_on_hours (not yet parsed) */
	sqlite3_bind_int(stmt_insert, 8, 0);		/* wear_level (NVMe) */
	sqlite3_bind_int(stmt_insert, 9, dh->health);

	rc = sqlite3_step(stmt_insert);
	if (rc != SQLITE_DONE) {
		syslog(LOG_WARNING, "trend DB insert for %s: %s",
		       disk_path, sqlite3_errmsg(db));
		return -1;
	}

	syslog(LOG_DEBUG, "trend: recorded snapshot for %s (realloc=%d pending=%d temp=%d)",
	       disk_path, dh->smart_reallocated, dh->smart_pending, dh->temperature);

	return 0;
}

/* ------------------------------------------------------------------ */
/*  Linear regression: slope = (n*sum_xy - sum_x*sum_y)              */
/*                          / (n*sum_xx - sum_x*sum_x)               */
/* ------------------------------------------------------------------ */
int lhsr_trend_query(const char *disk_path, struct lhsr_trend *trend)
{
	if (!db || !stmt_query)
		return -1;

	memset(trend, 0, sizeof(*trend));

	sqlite3_reset(stmt_query);
	sqlite3_bind_text(stmt_query, 1, disk_path, -1, SQLITE_STATIC);

	int n = 0;
	double sum_x = 0, sum_y_re = 0, sum_y_pe = 0, sum_y_un = 0, sum_y_tm = 0;
	double sum_xy_re = 0, sum_xy_pe = 0, sum_xy_un = 0, sum_xy_tm = 0;
	double sum_xx = 0;
	double base_x = 0;
	int first = 1;

	while (sqlite3_step(stmt_query) == SQLITE_ROW) {
		sqlite3_int64 t = sqlite3_column_int64(stmt_query, 0);
		int re = sqlite3_column_int(stmt_query, 1);
		int pe = sqlite3_column_int(stmt_query, 2);
		int un = sqlite3_column_int(stmt_query, 3);
		int tm = sqlite3_column_int(stmt_query, 4);

		/* Convert to days from first snapshot */
		if (first) {
			base_x = (double)t;
			first = 0;
		}
		double x = ((double)t - base_x) / 86400.0;
		double y_re = (double)re;
		double y_pe = (double)pe;
		double y_un = (double)un;
		double y_tm = (double)tm;

		n++;
		sum_x     += x;
		sum_y_re  += y_re;
		sum_y_pe  += y_pe;
		sum_y_un  += y_un;
		sum_y_tm  += y_tm;
		sum_xy_re += x * y_re;
		sum_xy_pe += x * y_pe;
		sum_xy_un += x * y_un;
		sum_xy_tm += x * y_tm;
		sum_xx    += x * x;
	}
	sqlite3_reset(stmt_query);

	if (n < 3)
		return -1;  /* Not enough data for meaningful trend */

	trend->data_points = n;

	/* Compute slopes */
	double denom = n * sum_xx - sum_x * sum_x;
	if (denom == 0) {
		/* All x values identical (snapshots at same time) */
		trend->reallocated_slope = 0;
		trend->pending_slope = 0;
		trend->uncorrectable_slope = 0;
		trend->temperature_slope = 0;
	} else {
		trend->reallocated_slope   = (n * sum_xy_re - sum_x * sum_y_re) / denom;
		trend->pending_slope       = (n * sum_xy_pe - sum_x * sum_y_pe) / denom;
		trend->uncorrectable_slope = (n * sum_xy_un - sum_x * sum_y_un) / denom;
		trend->temperature_slope   = (n * sum_xy_tm - sum_x * sum_y_tm) / denom;
	}

	/* Set warning flags based on thresholds */
	trend->reallocated_warn   = (trend->reallocated_slope > LHSR_TREND_REALLOCATED_WARN)   ? 1 : 0;
	trend->pending_warn       = (trend->pending_slope > LHSR_TREND_PENDING_WARN)           ? 1 : 0;
	trend->uncorrectable_warn = (trend->uncorrectable_slope > LHSR_TREND_UNCORRECTABLE_WARN) ? 1 : 0;
	trend->temperature_warn   = (trend->temperature_slope > LHSR_TREND_TEMP_WARN)           ? 1 : 0;

	return 0;
}

int lhsr_trend_warning(const char *disk_path, char *buf, size_t buf_size)
{
	struct lhsr_trend trend;
	char tmp[512];
	int len = 0;

	if (lhsr_trend_query(disk_path, &trend) < 0)
		return 0;

	buf[0] = '\0';

	if (trend.reallocated_warn) {
		len = snprintf(tmp, sizeof(tmp),
			       "reallocated sectors increasing %.1f/day ",
			       trend.reallocated_slope);
		if (len < (int)buf_size) {
			strncat(buf, tmp, buf_size - strlen(buf) - 1);
		}
	}
	if (trend.pending_warn) {
		len = snprintf(tmp, sizeof(tmp),
			       "pending sectors increasing %.1f/day ",
			       trend.pending_slope);
		if (len < (int)buf_size)
			strncat(buf, tmp, buf_size - strlen(buf) - 1);
	}
	if (trend.temperature_warn) {
		len = snprintf(tmp, sizeof(tmp),
			       "temperature rising %.1f°C/day ",
			       trend.temperature_slope);
		if (len < (int)buf_size)
			strncat(buf, tmp, buf_size - strlen(buf) - 1);
	}

	return (int)strlen(buf);
}

void lhsr_trend_close(void)
{
	if (stmt_insert)  { sqlite3_finalize(stmt_insert);  stmt_insert  = NULL; }
	if (stmt_latest)  { sqlite3_finalize(stmt_latest);  stmt_latest  = NULL; }
	if (stmt_query)   { sqlite3_finalize(stmt_query);   stmt_query   = NULL; }
	if (db)           { sqlite3_close(db);              db           = NULL; }
}
