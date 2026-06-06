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
		dh->health              = smart.health;

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

static void *monitor_thread(void *arg)
{
	struct daemon_state *st = arg;

	syslog(LOG_INFO, "Monitor thread started");

	while (st->running) {
		sleep(MONITOR_INTERVAL);

		if (!st->running)
			break;

		pthread_mutex_lock(&st->lock);

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

		for (int i = 0; i < st->num_disks; i++) {
			/* Find which array this disk belongs to */
			const char *dm_name = NULL;
			for (int j = 0; j < st->num_arrays; j++) {
				if (i < st->arrays[j].num_disks) {
					/* Rough mapping: assume disk i belongs to this array */
					dm_name = st->arrays[j].dm_name;
					break;
				}
			}
			check_disk_health(&st->disks[i], dm_name);
		}

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

	fprintf(f, "# LHSR Daemon Status\n");
	fprintf(f, "version: %d.%d.%d\n",
		LHSR_VERSION_MAJOR, LHSR_VERSION_MINOR, LHSR_VERSION_PATCH);
	fprintf(f, "timestamp: %ld\n", (long)time(NULL));
	fprintf(f, "running: %d\n", st->running ? 1 : 0);
	fprintf(f, "num_arrays: %d\n", st->num_arrays);
	fprintf(f, "num_disks: %d\n", st->num_disks);

	pthread_mutex_lock(&st->lock);

	for (int i = 0; i < st->num_arrays; i++) {
		fprintf(f, "array.%d.name: %s\n", i, st->arrays[i].dm_name);
		fprintf(f, "array.%d.type: raid%d\n", i, st->arrays[i].raid_type);
		fprintf(f, "array.%d.disks: %d\n", i, st->arrays[i].num_disks);
	}

	for (int i = 0; i < st->num_disks; i++) {
		fprintf(f, "disk.%d.device: %s\n", i, st->disks[i].device_path);
		fprintf(f, "disk.%d.health: %d\n", i, st->disks[i].health);
		fprintf(f, "disk.%d.temp: %d\n", i, st->disks[i].temperature);
		fprintf(f, "disk.%d.failed: %d\n", i, st->disks[i].failed);
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

	/* Main loop: write status file periodically, wait for shutdown */
	while (state.running) {
		sleep(15);
		if (state.running)
			write_status_file(&state);
	}

	syslog(LOG_INFO, "LHSR daemon stopping");

	/* Join threads */
	pthread_join(monitor_tid, NULL);
	if (health_tid)
		pthread_join(health_tid, NULL);

	pthread_mutex_destroy(&state.lock);
	remove_pid_file();
	closelog();

	return 0;
}
