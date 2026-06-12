/*
 * LHSR Daemon - Composite disk health score computation
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#include "lhsr-health.h"
#include "lhsr-trend.h"

int lhsr_compute_health_score(struct disk_health *dh, const char *disk_path)
{
	int score = 100;

	/* Reallocated sectors penalty: -10 per 10 sectors, max -30 */
	if (dh->smart_reallocated > 0) {
		int p = (dh->smart_reallocated / 10) * 10;
		if (p > 30)
			p = 30;
		score -= p;
	}

	/* Pending sectors penalty: -15 per sector, max -30 */
	if (dh->smart_pending > 0) {
		int p = dh->smart_pending * 15;
		if (p > 30)
			p = 30;
		score -= p;
	}

	/* Uncorrectable sectors penalty: -20 per sector, max -40 */
	if (dh->smart_uncorrectable > 0) {
		int p = dh->smart_uncorrectable * 20;
		if (p > 40)
			p = 40;
		score -= p;
	}

	/* Temperature penalty */
	if (dh->temperature > 60)
		score -= 15;
	else if (dh->temperature > 50)
		score -= 10;

	/* Trend penalty: -10 per active warning */
	struct lhsr_trend trend;
	if (lhsr_trend_query(disk_path, &trend) == 0) {
		if (trend.reallocated_warn)
			score -= 10;
		if (trend.pending_warn)
			score -= 10;
		if (trend.uncorrectable_warn)
			score -= 10;
		if (trend.temperature_warn)
			score -= 10;
	}

	/* Consecutive errors penalty: -5 per error, max -15 */
	if (dh->consecutive_errors > 0) {
		int p = dh->consecutive_errors * 5;
		if (p > 15)
			p = 15;
		score -= p;
	}

	/* Clamp */
	if (score < 0)
		score = 0;
	if (score > 100)
		score = 100;

	return score;
}

const char *lhsr_health_label(int score)
{
	if (score >= 90)
		return "OK";
	if (score >= 70)
		return "WARNING";
	if (score >= 40)
		return "CRITICAL";
	return "FAILING";
}
