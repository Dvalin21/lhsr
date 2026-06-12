/*
 * LHSR Daemon - Composite disk health score computation
 *
 * Combines current SMART values, trend slopes, and error history
 * into a single 0-100 health score.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#ifndef LHSR_HEALTH_H
#define LHSR_HEALTH_H

#include "lhsrd.h"

/*
 * Compute composite health score (0-100) for a disk.
 *
 * Scoring breakdown:
 *   Start at 100
 *   Reallocated sectors:   -10 per 10 sectors (max -30)
 *   Pending sectors:       -15 per sector (max -30)
 *   Uncorrectable sectors: -20 per sector (max -40)
 *   Temperature > 50°C:    -10;  >60°C: -15
 *   Trend warnings:        -10 per active warning (realloc, pending, uncorr, temp)
 *   Consecutive errors:    -5 per error (max -15)
 *   Final clamped to 0-100
 *
 * Higher = healthier. 90-100 = OK, 70-89 = WARNING,
 * 40-69 = CRITICAL, 0-39 = FAILING.
 */
int lhsr_compute_health_score(struct disk_health *dh, const char *disk_path);

/*
 * Return human-readable health label for a score.
 * Returns a pointer to a static string:
 *   >= 90 : "OK"
 *   >= 70 : "WARNING"
 *   >= 40 : "CRITICAL"
 *   < 40  : "FAILING"
 */
const char *lhsr_health_label(int score);

#endif /* LHSR_HEALTH_H */
