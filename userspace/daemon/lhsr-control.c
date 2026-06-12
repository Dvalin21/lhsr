/*
 * LHSR Daemon - Control socket (Unix domain socket)
 *
 * Listens on /run/lhsrd.sock for JSON commands.
 * Thread-safe: accesses daemon state under the state lock.
 *
 * Supported commands (JSON over one line, null-terminated):
 *   {"cmd":"ping"}         -> {"status":"pong"}
 *   {"cmd":"status"}       -> full daemon + array + disk status as JSON
 *   {"cmd":"trends"}       -> trend data for all disks as JSON
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <pthread.h>
#include <syslog.h>
#include <time.h>
#include <errno.h>

#include "lhsrd.h"
#include "lhsr-trend.h"
#include "lhsr-control.h"
#include "../../lib/raid_engine.h"

/* Maximum JSON response size */
#define RESPONSE_MAX (64 * 1024)

/* Maximum command line length */
#define CMD_MAX 4096

/* Maximum clients to queue */
#define LISTEN_BACKLOG 5

/* ---- Internal helpers ---- */

/*
 * Build a JSON string for one array state entry.
 * Returns pointer past the written data.
 */
static char *json_array(char *p, char *end, int idx, struct array_state *a)
{
	p += snprintf(p, end - p,
		"    {\n"
		"      \"index\": %d,\n"
		"      \"name\": \"%s\",\n"
		"      \"uuid\": \"%s\",\n"
		"      \"num_disks\": %d,\n"
		"      \"num_working\": %d\n"
		"    }",
		idx, a->dm_name, a->dm_uuid,
		a->num_disks, a->num_working);
	return p;
}

/*
 * Build a JSON string for one disk health entry.
 * Includes trend warning if available.
 */
static char *json_disk(char *p, char *end, int idx, struct disk_health *d)
{
	char warn[256] = "";
	lhsr_trend_warning(d->device_path, warn, sizeof(warn));

	p += snprintf(p, end - p,
		"    {\n"
		"      \"index\": %d,\n"
		"      \"device\": \"%s\",\n"
		"      \"health\": %d,\n"
		"      \"temperature\": %d,\n"
		"      \"reallocated\": %d,\n"
		"      \"pending\": %d,\n"
		"      \"uncorrectable\": %d,\n"
		"      \"failed\": %d,\n"
		"      \"trend_warning\": \"%s\"\n"
		"    }",
		idx, d->device_path, d->health, d->temperature,
		d->smart_reallocated, d->smart_pending,
		d->smart_uncorrectable, d->failed, warn);
	return p;
}

/*
 * Build a JSON string for one trend entry.
 */
static char *json_trend(char *p, char *end, const char *device, struct lhsr_trend *t)
{
	p += snprintf(p, end - p,
		"    {\n"
		"      \"device\": \"%s\",\n"
		"      \"data_points\": %d,\n"
		"      \"reallocated_slope\": %.2f,\n"
		"      \"pending_slope\": %.2f,\n"
		"      \"uncorrectable_slope\": %.2f,\n"
		"      \"temperature_slope\": %.2f,\n"
		"      \"reallocated_warn\": %d,\n"
		"      \"pending_warn\": %d,\n"
		"      \"uncorrectable_warn\": %d,\n"
		"      \"temperature_warn\": %d\n"
		"    }",
		device,
		t->data_points,
		t->reallocated_slope,
		t->pending_slope,
		t->uncorrectable_slope,
		t->temperature_slope,
		t->reallocated_warn,
		t->pending_warn,
		t->uncorrectable_warn,
		t->temperature_warn);
	return p;
}

/* ---- Public API ---- */

char *lhsr_control_build_status(struct daemon_state *st)
{
	char *resp = malloc(RESPONSE_MAX);
	if (!resp)
		return NULL;

	char *p = resp;
	char *end = resp + RESPONSE_MAX;
	time_t now = time(NULL);
	time_t uptime = st->start_time ? (now - st->start_time) : 0;

	pthread_mutex_lock(&st->lock);

	p += snprintf(p, end - p,
		"{\n"
		"  \"status\": \"ok\",\n"
		"  \"version\": \"%d.%d.%d\",\n"
		"  \"timestamp\": %ld,\n"
		"  \"uptime\": %ld,\n"
		"  \"running\": %d,\n"
		"  \"num_arrays\": %d,\n"
		"  \"num_disks\": %d,\n"
		"  \"arrays\": [\n",
		LHSR_VERSION_MAJOR, LHSR_VERSION_MINOR, LHSR_VERSION_PATCH,
		(long)now, (long)uptime,
		st->running ? 1 : 0,
		st->num_arrays, st->num_disks);

	for (int i = 0; i < st->num_arrays; i++) {
		if (i > 0) {
			p += snprintf(p, end - p, ",\n");
		}
		p = json_array(p, end, i, &st->arrays[i]);
	}

	p += snprintf(p, end - p,
		"\n"
		"  ],\n"
		"  \"disks\": [\n");

	for (int i = 0; i < st->num_disks; i++) {
		if (i > 0) {
			p += snprintf(p, end - p, ",\n");
		}
		p = json_disk(p, end, i, &st->disks[i]);
	}

	p += snprintf(p, end - p,
		"\n"
		"  ]\n"
		"}\n");

	pthread_mutex_unlock(&st->lock);

	return resp;
}

char *lhsr_control_build_trends(struct daemon_state *st)
{
	char *resp = malloc(RESPONSE_MAX);
	if (!resp)
		return NULL;

	char *p = resp;
	char *end = resp + RESPONSE_MAX;

	pthread_mutex_lock(&st->lock);

	p += snprintf(p, end - p,
		"{\n"
		"  \"status\": \"ok\",\n"
		"  \"num_disks\": %d,\n"
		"  \"trends\": [\n",
		st->num_disks);

	int first = 1;
	for (int i = 0; i < st->num_disks; i++) {
		struct lhsr_trend trend;
		if (lhsr_trend_query(st->disks[i].device_path, &trend) == 0) {
			if (!first) {
				p += snprintf(p, end - p, ",\n");
			}
			first = 0;
			p = json_trend(p, end, st->disks[i].device_path, &trend);
		}
	}

	p += snprintf(p, end - p,
		"\n"
		"  ]\n"
		"}\n");

	pthread_mutex_unlock(&st->lock);

	return resp;
}

/*
 * Handle a single client connection.
 * Reads one line, dispatches command, sends response.
 */
static void handle_client(int fd, struct daemon_state *st)
{
	char buf[CMD_MAX];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		close(fd);
		return;
	}
	buf[n] = '\0';

	/* Trim trailing newlines */
	while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
		buf[--n] = '\0';

	char *resp = NULL;

	/* Simple JSON command dispatch (no json-c dependency) */
	if (strstr(buf, "\"ping\"") || strstr(buf, "'ping'")) {
		resp = strdup("{\"status\":\"pong\"}\n");
	} else if (strstr(buf, "\"trends\"") || strstr(buf, "'trends'")) {
		resp = lhsr_control_build_trends(st);
	} else {
		/* Default: return full status */
		resp = lhsr_control_build_status(st);
	}

	if (resp) {
		write(fd, resp, strlen(resp));
		free(resp);
	}

	close(fd);
}

/*
 * Control socket listener thread.
 */
static void *control_thread(void *arg)
{
	struct daemon_state *st = (struct daemon_state *)arg;
	int sock;

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		syslog(LOG_ERR, "control socket: socket() failed: %m");
		return NULL;
	}

	/* Remove stale socket file */
	unlink(LHSRD_SOCKET_PATH);

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, LHSRD_SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		syslog(LOG_ERR, "control socket: bind(%s) failed: %m",
		       LHSRD_SOCKET_PATH);
		close(sock);
		return NULL;
	}

	/* Set permissions so lhsrctl can connect */
	chmod(LHSRD_SOCKET_PATH, 0660);

	if (listen(sock, LISTEN_BACKLOG) < 0) {
		syslog(LOG_ERR, "control socket: listen() failed: %m");
		close(sock);
		unlink(LHSRD_SOCKET_PATH);
		return NULL;
	}

	syslog(LOG_INFO, "control socket listening on %s", LHSRD_SOCKET_PATH);

	while (st->running) {
		struct sockaddr_un client_addr;
		socklen_t addr_len = sizeof(client_addr);

		int client = accept4(sock, (struct sockaddr *)&client_addr,
				     &addr_len, SOCK_CLOEXEC);
		if (client < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			if (!st->running)
				break;
			syslog(LOG_WARNING, "control socket: accept() failed: %m");
			continue;
		}

		handle_client(client, st);
	}

	close(sock);
	unlink(LHSRD_SOCKET_PATH);
	syslog(LOG_INFO, "control socket stopped");
	return NULL;
}

int lhsr_control_start(struct daemon_state *st)
{
	pthread_t tid;
	int ret = pthread_create(&tid, NULL, control_thread, st);
	if (ret != 0) {
		syslog(LOG_ERR, "control thread create failed: %d", ret);
		return -1;
	}
	/* Detach — the thread cleans up when st->running is cleared */
	pthread_detach(tid);
	return 0;
}

void lhsr_control_stop(struct daemon_state *st)
{
	(void)st; /* unused — state accessed via socket path constant */

	/* Trigger accept() to return by connecting briefly */
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd >= 0) {
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, LHSRD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
		connect(fd, (struct sockaddr *)&addr, sizeof(addr));
		close(fd);
	}
	unlink(LHSRD_SOCKET_PATH);
}
