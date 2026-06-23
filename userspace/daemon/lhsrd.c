/*
 * LHSR Daemon - Background service for array monitoring and management
 *
 * Architecture:
 *   Main thread: signal handling, periodic status updates
 *   Monitor thread: array status polling via libdevmapper
 *   Health thread: disk health monitoring via sysfs / SG_IO
 *
 * Uses libdevmapper (dm_task_*) instead of fork+exec dmsetup.
 * Uses sysfs + SG_IO ATA PASS-THROUGH instead of fork+exec smartctl.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <time.h>

#include "../../lib/raid_engine.h"
#include "lhsrd.h"
#include "lhsr-dm.h"
#include "lhsr-smart.h"
#include "lhsr-config.h"
#include "lhsr-trend.h"
#include "lhsr-control.h"
#include "lhsr-http.h"
#include "lhsr-health.h"

/* Global state for signal handler */
struct daemon_state *g_state = NULL;

/* ---------- Signal handling ---------- */

static void signal_handler(int sig)
{
	syslog(LOG_INFO, "Received signal %d, shutting down", sig);
	if (g_state)
		g_state->running = 0;
}

/* ---------- PID file ---------- */

static void write_pid_file(void)
{
	FILE *f = fopen(LHSRD_PID_FILE, "w");
	if (f) {
		fprintf(f, "%d\n", getpid());
		fclose(f);
	}
}

static void remove_pid_file(void)
{
	unlink(LHSRD_PID_FILE);
}

/* ---------- Daemonization ---------- */

static int daemonize(void)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		exit(0);	/* Parent exits */

	umask(0);

	if (setsid() < 0)
		return -1;

	/* Redirect stdio to /dev/null */
	int fd = open("/dev/null", O_RDWR);
	if (fd >= 0) {
		dup2(fd, STDIN_FILENO);
		dup2(fd, STDOUT_FILENO);
		dup2(fd, STDERR_FILENO);
		if (fd > 2)
			close(fd);
	}

	return 0;
}

/* ---------- Disk health check ---------- */

static void check_disk_health(struct disk_health *dh, const char *dm_device)
{
	struct disk_health smart;
	char msg[512];

	if (lhsr_smart_poll(dh->device_path, &smart) == 0) {
		dh->smart_reallocated   = smart.smart_reallocated;
		dh->smart_pending       = smart.smart_pending;
		dh->smart_uncorrectable = smart.smart_uncorrectable;
		dh->temperature         = smart.temperature;
		dh->power_on_hours      = smart.power_on_hours;
		dh->wear_level          = smart.wear_level;
		dh->health              = smart.health;

		/* Compute composite health score */
		dh->health_score = lhsr_compute_health_score(dh, dh->device_path);

		if (g_state && g_state->cfg.verbose) {
			syslog(LOG_DEBUG, "Disk %s: realloc=%d pending=%d uncorr=%d temp=%d health=%d",
			       dh->device_path, smart.smart_reallocated,
			       smart.smart_pending, smart.smart_uncorrectable,
			       smart.temperature, smart.health);
		}

		/* Check thresholds */
		if (dh->smart_reallocated > SMART_REALLOCATED_THRESHOLD ||
		    dh->smart_pending > SMART_PENDING_THRESHOLD) {
			dh->consecutive_errors++;
		} else {
			dh->consecutive_errors = 0;
		}

		if (dh->health < SMART_HEALTH_LOW) {
			syslog(LOG_WARNING, "Disk %s health critical: %d",
			       dh->device_path, dh->health);

			if (g_state && g_state->cfg.notify_on_fail) {
				char alert_subj[320];
				char alert_msg[1024];
				snprintf(alert_subj, sizeof(alert_subj),
					 "LHSR: Disk %s health critical",
					 dh->device_path);
				snprintf(alert_msg, sizeof(alert_msg),
					 "Disk:        %s\n"
					 "Health:      %d/100\n"
					 "Rellocated:  %d\n"
					 "Pending:     %d\n"
					 "Uncorrectable: %d\n"
					 "Temperature: %d°C\n"
					 "Power-on:    %d hours\n"
					 "Wear level:  %d%%\n",
					 dh->device_path, dh->health,
					 dh->smart_reallocated,
					 dh->smart_pending,
					 dh->smart_uncorrectable,
					 dh->temperature,
					 dh->power_on_hours,
					 dh->wear_level);
				lhsr_notify_alert(g_state, alert_subj, alert_msg);
			}

			if (g_state && g_state->cfg.auto_failover && dm_device) {
				snprintf(msg, sizeof(msg), "disk_fail %d", dh->disk_index);
				syslog(LOG_INFO, "Auto-failover: sending '%s' to %s",
				       msg, dm_device);
				lhsr_dm_message(dm_device, 0, msg);
			}
			dh->failed = 1;
		}
	} else {
		dh->consecutive_errors++;
		syslog(LOG_WARNING, "Failed to poll SMART from %s (error %d/%d)",
		       dh->device_path, dh->consecutive_errors,
		       g_state ? g_state->cfg.error_threshold : 3);

		if (g_state && dh->consecutive_errors >= g_state->cfg.error_threshold) {
			syslog(LOG_WARNING, "Disk %s: too many SMART poll failures, marking failed",
			       dh->device_path);
			if (g_state->cfg.notify_on_fail) {
				char alert_subj[320];
				char alert_msg[512];
				snprintf(alert_subj, sizeof(alert_subj),
					 "LHSR: Disk %s SMART poll failure",
					 dh->device_path);
				snprintf(alert_msg, sizeof(alert_msg),
					 "Disk: %s\n"
					 "Consecutive SMART poll failures: %d\n"
					 "Error threshold: %d\n",
					 dh->device_path,
					 dh->consecutive_errors,
					 g_state->cfg.error_threshold);
				lhsr_notify_alert(g_state, alert_subj, alert_msg);
			}
			if (g_state->cfg.auto_failover && dm_device) {
				snprintf(msg, sizeof(msg), "disk_fail %d", dh->disk_index);
				lhsr_dm_message(dm_device, 0, msg);
			}
			dh->failed = 1;
		}
	}

	dh->last_check = time(NULL);
}

/* ---------- Monitor thread ---------- */

static void cleanup_mutex_unlock(void *arg)
{
	pthread_mutex_unlock((pthread_mutex_t *)arg);
}

static void *monitor_thread(void *arg)
{
	struct daemon_state *st = arg;

	syslog(LOG_INFO, "Monitor thread started");

	while (st->running) {
		sleep(MONITOR_INTERVAL);

		if (!st->running)
			break;

		pthread_mutex_lock(&st->lock);
		pthread_cleanup_push(cleanup_mutex_unlock, &st->lock);

		/* Discover arrays via libdevmapper */
		int n = lhsr_dm_list_arrays(st->arrays, LHSRD_MAX_ARRAYS);
		if (n >= 0)
			st->num_arrays = n;

		/* Update array statuses */
		for (int i = 0; i < st->num_arrays; i++) {
			char *status = lhsr_dm_status(st->arrays[i].dm_name);
			if (status) {
				st->arrays[i].last_status = time(NULL);
				free(status);
			}
		}

		/* Initialize num_working to num_disks (optimistic: all healthy
		 * until health thread proves otherwise) */
		for (int i = 0; i < st->num_arrays; i++)
			st->arrays[i].num_working = st->arrays[i].num_disks;

		/* Discover component disks of each array */
		st->num_disks = 0;
		for (int i = 0; i < st->num_arrays; i++) {
			char devices[LHSRD_MAX_DISKS_PER_ARRAY][256];
			int nd = lhsr_dm_get_devices(st->arrays[i].dm_name,
						     devices,
						     LHSRD_MAX_DISKS_PER_ARRAY);
			for (int d = 0; d < nd && st->num_disks < LHSRD_MAX_DISKS; d++) {
				char devpath[256];
				if (lhsr_dm_resolve_device(devices[d], devpath,
							   sizeof(devpath)) < 0)
					continue;
				/* Avoid duplicates (same disk may appear once) */
				int found = 0;
				for (int j = 0; j < st->num_disks; j++) {
					if (strcmp(st->disks[j].device_path,
						   devpath) == 0) {
						found = 1;
						break;
					}
				}
				if (!found) {
					struct disk_health *dh;
					dh = &st->disks[st->num_disks];
					memset(dh, 0, sizeof(*dh));
					snprintf(dh->device_path,
						 sizeof(dh->device_path), "%s",
						 devpath);
					dh->disk_index = d;	/* index within this array */
					dh->array_idx = i;	/* which array owns this disk */
					dh->health = 100;
					dh->health_score = 100;
					st->num_disks++;
				}
			}
		}

		pthread_cleanup_pop(0);
		pthread_mutex_unlock(&st->lock);

		if (st->cfg.verbose)
			syslog(LOG_DEBUG, "Monitor: %d array(s) found", st->num_arrays);
	}

	syslog(LOG_INFO, "Monitor thread stopping");
	return NULL;
}

/* ---------- Health monitor thread ---------- */

static void *health_monitor_thread(void *arg)
{
	struct daemon_state *st = arg;

	syslog(LOG_INFO, "Health monitor thread started (interval: %ds)",
	       st->cfg.smart_poll_interval);

	while (st->running) {
		sleep(st->cfg.smart_poll_interval);

		if (!st->running)
			break;

		pthread_mutex_lock(&st->lock);
		pthread_cleanup_push(cleanup_mutex_unlock, &st->lock);

		for (int i = 0; i < st->num_disks; i++) {
			/* Find which array this disk belongs to */
			const char *dm_name = NULL;
			if (st->disks[i].array_idx >= 0 &&
			    st->disks[i].array_idx < st->num_arrays) {
				dm_name = st->arrays[st->disks[i].array_idx].dm_name;
			}
			check_disk_health(&st->disks[i], dm_name);
			/* Record trend snapshot if enabled */
			if (st->cfg.trend_enabled) {
				lhsr_trend_record(st->disks[i].device_path,
						  &st->disks[i]);
			}
		}

		/* Recompute num_working for each array */
		for (int i = 0; i < st->num_arrays; i++)
			st->arrays[i].num_working = 0;
		for (int i = 0; i < st->num_disks; i++) {
			int aidx = st->disks[i].array_idx;
			if (aidx >= 0 && aidx < st->num_arrays &&
			    !st->disks[i].failed &&
			    st->disks[i].health_score >= 50)
				st->arrays[aidx].num_working++;
		}

		pthread_cleanup_pop(0);
		pthread_mutex_unlock(&st->lock);
	}

	syslog(LOG_INFO, "Health monitor thread stopping");
	return NULL;
}

/* ---------- Status file ---------- */

static void write_status_file(struct daemon_state *st)
{
	FILE *f = fopen(LHSRD_STATUS_FILE, "w");
	if (!f)
		return;

	time_t now = time(NULL);
	time_t uptime = st->start_time ? (now - st->start_time) : 0;

	pthread_mutex_lock(&st->lock);

	fprintf(f, "{\n");
	fprintf(f, "  \"version\": \"%d.%d.%d\",\n",
		LHSR_VERSION_MAJOR, LHSR_VERSION_MINOR, LHSR_VERSION_PATCH);
	fprintf(f, "  \"timestamp\": %ld,\n", (long)now);
	fprintf(f, "  \"uptime\": %ld,\n", (long)uptime);
	fprintf(f, "  \"running\": %d,\n", st->running ? 1 : 0);
	fprintf(f, "  \"num_arrays\": %d,\n", st->num_arrays);
	fprintf(f, "  \"num_disks\": %d,\n", st->num_disks);
	fprintf(f, "  \"arrays\": [\n");

	for (int i = 0; i < st->num_arrays; i++) {
		char warn[256] = "";
		fprintf(f, "    {\n");
		fprintf(f, "      \"index\": %d,\n", i);
		fprintf(f, "      \"name\": \"%s\",\n", st->arrays[i].dm_name);
		fprintf(f, "      \"uuid\": \"%s\",\n", st->arrays[i].dm_uuid);
		fprintf(f, "      \"raid_type\": %d,\n", st->arrays[i].raid_type);
		fprintf(f, "      \"num_disks\": %d,\n", st->arrays[i].num_disks);
		fprintf(f, "      \"num_working\": %d\n", st->arrays[i].num_working);
		fprintf(f, "    }%s\n", (i + 1 < st->num_arrays) ? "," : "");
		(void)warn;
	}

	fprintf(f, "  ],\n");
	fprintf(f, "  \"disks\": [\n");

	for (int i = 0; i < st->num_disks; i++) {
		char warn[256] = "";
		if (st->cfg.trend_enabled)
			lhsr_trend_warning(st->disks[i].device_path, warn, sizeof(warn));

		fprintf(f, "    {\n");
		fprintf(f, "      \"index\": %d,\n", i);
		fprintf(f, "      \"device\": \"%s\",\n", st->disks[i].device_path);
		fprintf(f, "      \"health\": %d,\n", st->disks[i].health);
		fprintf(f, "      \"health_score\": %d,\n", st->disks[i].health_score);
		fprintf(f, "      \"health_label\": \"%s\",\n",
			lhsr_health_label(st->disks[i].health_score));
		fprintf(f, "      \"temperature\": %d,\n", st->disks[i].temperature);
		fprintf(f, "      \"reallocated\": %d,\n", st->disks[i].smart_reallocated);
		fprintf(f, "      \"pending\": %d,\n", st->disks[i].smart_pending);
		fprintf(f, "      \"uncorrectable\": %d,\n", st->disks[i].smart_uncorrectable);
		fprintf(f, "      \"failed\": %d,\n", st->disks[i].failed);
		fprintf(f, "      \"trend_warning\": \"%s\"\n", warn);
		fprintf(f, "    }%s\n", (i + 1 < st->num_disks) ? "," : "");
	}

	fprintf(f, "  ]\n");
	fprintf(f, "}\n");

	pthread_mutex_unlock(&st->lock);

	fclose(f);
}

/* ---------- Prometheus metrics file ---------- */

static void write_metrics_file(struct daemon_state *st)
{
	FILE *f = fopen(LHSRD_METRICS_FILE, "w");
	if (!f)
		return;

	time_t now = time(NULL);
	time_t uptime = st->start_time ? (now - st->start_time) : 0;

	pthread_mutex_lock(&st->lock);

	/* Daemon info */
	fprintf(f, "# HELP lhsr_daemon_info Daemon metadata (version, health)\n");
	fprintf(f, "# TYPE lhsr_daemon_info gauge\n");
	fprintf(f, "lhsr_daemon_info{version=\"%d.%d.%d\"} 1\n\n",
		LHSR_VERSION_MAJOR, LHSR_VERSION_MINOR, LHSR_VERSION_PATCH);

	fprintf(f, "# HELP lhsr_uptime_seconds Daemon uptime in seconds\n");
	fprintf(f, "# TYPE lhsr_uptime_seconds gauge\n");
	fprintf(f, "lhsr_uptime_seconds %ld\n\n", (long)uptime);

	/* Array-level metrics */
	fprintf(f, "# HELP lhsr_array_info Array metadata (name, raid_type, uuid)\n");
	fprintf(f, "# TYPE lhsr_array_info gauge\n");
	for (int i = 0; i < st->num_arrays; i++) {
		int degraded = st->arrays[i].num_disks - st->arrays[i].num_working;
		fprintf(f, "lhsr_array_info{name=\"%s\",raid_type=\"%d\","
			"uuid=\"%s\"} 1\n",
			st->arrays[i].dm_name,
			st->arrays[i].raid_type,
			st->arrays[i].dm_uuid);
		fprintf(f, "lhsr_array_degraded_disks{name=\"%s\"} %d\n",
			st->arrays[i].dm_name, degraded);
	}
	fprintf(f, "\n");

	/* Disk health gauges */
	fprintf(f, "# HELP lhsr_disk_health Composite health score (0-100, higher=better)\n");
	fprintf(f, "# TYPE lhsr_disk_health gauge\n");
	for (int i = 0; i < st->num_disks; i++) {
		char warn[256] = "";
		if (st->cfg.trend_enabled)
			lhsr_trend_warning(st->disks[i].device_path, warn, sizeof(warn));
		fprintf(f, "lhsr_disk_health{device=\"%s\"} %d\n",
			st->disks[i].device_path, st->disks[i].health_score);
		/* Also emit warning as info metric for alertmanager */
		if (warn[0]) {
			fprintf(f, "lhsr_disk_warning{device=\"%s\",warning=\"%s\"} 1\n",
				st->disks[i].device_path, warn);
		}
	}

	fprintf(f, "\n# HELP lhsr_disk_temperature Disk temperature in Celsius\n");
	fprintf(f, "# TYPE lhsr_disk_temperature gauge\n");
	for (int i = 0; i < st->num_disks; i++) {
		fprintf(f, "lhsr_disk_temperature{device=\"%s\"} %d\n",
			st->disks[i].device_path, st->disks[i].temperature);
	}

	fprintf(f, "\n# HELP lhsr_reallocated_sectors Reallocated sector count\n");
	fprintf(f, "# TYPE lhsr_reallocated_sectors gauge\n");
	for (int i = 0; i < st->num_disks; i++) {
		fprintf(f, "lhsr_reallocated_sectors{device=\"%s\"} %d\n",
			st->disks[i].device_path, st->disks[i].smart_reallocated);
	}

	fprintf(f, "\n# HELP lhsr_pending_sectors Pending sector count\n");
	fprintf(f, "# TYPE lhsr_pending_sectors gauge\n");
	for (int i = 0; i < st->num_disks; i++) {
		fprintf(f, "lhsr_pending_sectors{device=\"%s\"} %d\n",
			st->disks[i].device_path, st->disks[i].smart_pending);
	}

	fprintf(f, "\n# HELP lhsr_uncorrectable_sectors Uncorrectable sector count\n");
	fprintf(f, "# TYPE lhsr_uncorrectable_sectors gauge\n");
	for (int i = 0; i < st->num_disks; i++) {
		fprintf(f, "lhsr_uncorrectable_sectors{device=\"%s\"} %d\n",
			st->disks[i].device_path, st->disks[i].smart_uncorrectable);
	}

	fprintf(f, "\n# HELP lhsr_power_on_hours Total power-on hours\n");
	fprintf(f, "# TYPE lhsr_power_on_hours gauge\n");
	for (int i = 0; i < st->num_disks; i++) {
		fprintf(f, "lhsr_power_on_hours{device=\"%s\"} %d\n",
			st->disks[i].device_path, st->disks[i].power_on_hours);
	}

	fprintf(f, "\n# HELP lhsr_wear_level Normalized wear level (0-100, 100=new, 0=worn)\n");
	fprintf(f, "# TYPE lhsr_wear_level gauge\n");
	for (int i = 0; i < st->num_disks; i++) {
		fprintf(f, "lhsr_wear_level{device=\"%s\"} %d\n",
			st->disks[i].device_path, st->disks[i].wear_level);
	}

	/* Trend slope metrics (if enabled) */
	if (st->cfg.trend_enabled) {
		fprintf(f, "\n# HELP lhsr_trend_reallocated_slope Reallocated sector trend (sectors/day)\n");
		fprintf(f, "# TYPE lhsr_trend_reallocated_slope gauge\n");
		for (int i = 0; i < st->num_disks; i++) {
			struct lhsr_trend trend;
			if (lhsr_trend_query(st->disks[i].device_path, &trend) == 0) {
				fprintf(f, "lhsr_trend_reallocated_slope{device=\"%s\"} %.2f\n",
					st->disks[i].device_path, trend.reallocated_slope);
				fprintf(f, "lhsr_trend_pending_slope{device=\"%s\"} %.2f\n",
					st->disks[i].device_path, trend.pending_slope);
				fprintf(f, "lhsr_trend_uncorrectable_slope{device=\"%s\"} %.2f\n",
					st->disks[i].device_path, trend.uncorrectable_slope);
				fprintf(f, "lhsr_trend_temperature_slope{device=\"%s\"} %.2f\n",
					st->disks[i].device_path, trend.temperature_slope);
			}
		}
	}

	pthread_mutex_unlock(&st->lock);
	fclose(f);
}

/* ---------- Usage ---------- */

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("Options:\n");
	printf("  -d        Run as daemon\n");
	printf("  -v        Verbose output\n");
	printf("  -h        Show this help\n");
	printf("\n");
	printf("Files:\n");
	printf("  PID file:  %s\n", LHSRD_PID_FILE);
	printf("  Config:    %s\n", LHSRD_CONF_FILE);
	printf("  Status:    %s\n", LHSRD_STATUS_FILE);
}

/* ---------- Main ---------- */

int main(int argc, char **argv)
{
	int opt;
	pthread_t monitor_tid, health_tid;
	struct daemon_state state;
	int daemon_mode = 0;
	int ret;

	memset(&state, 0, sizeof(state));
	state.start_time = time(NULL);

	while ((opt = getopt(argc, argv, "dvh")) != -1) {
		switch (opt) {
		case 'd':
			daemon_mode = 1;
			break;
		case 'v':
			state.cfg.verbose = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	openlog("lhsrd", LOG_PID | LOG_PERROR, LOG_DAEMON);

	/* Load configuration */
	lhsr_config_load(&state.cfg, LHSRD_CONF_FILE);

	/* Override verbose from config if not set on command line */
	if (!state.cfg.verbose && optind == argc)
		state.cfg.verbose = 0;

	state.running = 1;
	g_state = &state;
	pthread_mutex_init(&state.lock, NULL);

	if (daemon_mode) {
		if (daemonize() < 0) {
			syslog(LOG_ERR, "Failed to daemonize");
			fprintf(stderr, "Failed to daemonize\n");
			return 1;
		}
	}

	write_pid_file();

	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGHUP, signal_handler);

	syslog(LOG_INFO, "LHSR daemon starting v%d.%d.%d",
	       LHSR_VERSION_MAJOR, LHSR_VERSION_MINOR, LHSR_VERSION_PATCH);

	/* Initialize libdevmapper logging */
	lhsr_dm_init(state.cfg.verbose);

	/* Initialize trend database if enabled */
	if (state.cfg.trend_enabled) {
		if (lhsr_trend_init(state.cfg.trend_db_path) < 0) {
			syslog(LOG_WARNING, "trend DB init failed, disabling trend tracking");
			state.cfg.trend_enabled = 0;
		}
	}

	/* Start control socket (best-effort, not fatal if fails) */
	lhsr_control_start(&state);

	/* Start HTTP metrics server (best-effort, not fatal if fails) */
	lhsr_http_start(&state);

	/* Create monitor thread */
	ret = pthread_create(&monitor_tid, NULL, monitor_thread, &state);
	if (ret != 0) {
		syslog(LOG_ERR, "Failed to create monitor thread: %d", ret);
		remove_pid_file();
		return 1;
	}

	/* Create health monitor thread */
	ret = pthread_create(&health_tid, NULL, health_monitor_thread, &state);
	if (ret != 0) {
		syslog(LOG_ERR, "Failed to create health monitor thread: %d", ret);
		/* Not fatal — we still have the monitor thread */
		health_tid = 0;
	}

	syslog(LOG_INFO, "LHSR daemon started (PID: %d)", getpid());

	/* Create /var/lib/lhsrd if needed (for metrics file) */
	mkdir("/var/lib/lhsrd", 0755);

	/* Main loop: write status + metrics periodically, wait for shutdown */
	while (state.running) {
		sleep(15);
		if (state.running) {
			write_status_file(&state);
			write_metrics_file(&state);
		}
	}

	syslog(LOG_INFO, "LHSR daemon stopping");

	/* Stop control socket, HTTP server, cancel and join threads */
	lhsr_control_stop(&state);
	lhsr_http_stop();
	pthread_cancel(monitor_tid);
	pthread_join(monitor_tid, NULL);
	if (health_tid) {
		pthread_cancel(health_tid);
		pthread_join(health_tid, NULL);
	}

	/* Close trend database */
	if (state.cfg.trend_enabled)
		lhsr_trend_close();

	pthread_mutex_destroy(&state.lock);
	remove_pid_file();
	closelog();

	return 0;
}
