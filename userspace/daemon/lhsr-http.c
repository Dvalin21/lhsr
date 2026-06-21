/*
 * LHSR Daemon - Prometheus metrics HTTP endpoint
 *
 * Minimal HTTP/1.0 server that serves the metrics file on port 9101
 * for Prometheus scraping.  Single-threaded accept loop.
 *
 * Supported requests:
 *   GET /metrics  → Content-Type: text/plain (Prometheus format)
 *   GET /         → 302 redirect to /metrics
 *   all others    → 404
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
#include <netinet/in.h>
#include <pthread.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "lhsrd.h"

#define HTTP_PORT       9101
#define BACKLOG         5
#define BUF_SIZE        4096
#define METRICS_PATH    "/var/lib/lhsrd/metrics.prom"

static pthread_t http_tid;
static int http_running;
static int listen_fd;

/* ---------- helpers ---------- */

static void send_response(int fd, int status, const char *status_text,
			  const char *content_type,
			  const char *body, size_t body_len)
{
	char header[512];
	int n = snprintf(header, sizeof(header),
			 "HTTP/1.0 %d %s\r\n"
			 "Content-Type: %s\r\n"
			 "Content-Length: %zu\r\n"
			 "Connection: close\r\n"
			 "\r\n",
			 status, status_text,
			 content_type ? content_type : "text/plain",
			 body_len);
	write(fd, header, n);
	if (body && body_len > 0)
		write(fd, body, body_len);
}

static void send_redirect(int fd, const char *location)
{
	char body[256];
	int n = snprintf(body, sizeof(body),
			 "<!DOCTYPE html><html><head>"
			 "<meta http-equiv=\"refresh\" content=\"0;url=%s\">"
			 "</head></html>", location);
	send_response(fd, 302, "Found", "text/html", body, n);
}

static void send_404(int fd)
{
	send_response(fd, 404, "Not Found", "text/plain", "404 Not Found\n", 14);
}

/* ---------- request handler ---------- */

static void handle_client(int client_fd)
{
	char buf[BUF_SIZE];
	ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		close(client_fd);
		return;
	}
	buf[n] = '\0';

	/* Parse first line: "GET /path HTTP/1.x" */
	char method[16], path[256];
	if (sscanf(buf, "%15s %255s", method, path) < 2) {
		send_404(client_fd);
		close(client_fd);
		return;
	}

	/* Only accept GET */
	if (strcmp(method, "GET") != 0) {
		send_response(client_fd, 405, "Method Not Allowed",
			      NULL, "405 Method Not Allowed\n", 23);
		close(client_fd);
		return;
	}

	/* Route */
	if (strcmp(path, "/metrics") == 0 || strcmp(path, "/metrics/") == 0) {
		/* Read and serve the metrics file */
		int fd = open(METRICS_PATH, O_RDONLY);
		if (fd < 0) {
			/* File not ready yet — return empty */
			send_response(client_fd, 200, "OK",
				      "text/plain; charset=utf-8", "", 0);
			close(client_fd);
			return;
		}

		/* Get file size */
		struct stat st;
		fstat(fd, &st);
		size_t fsize = st.st_size;

		/* Read into heap buffer */
		char *body = malloc(fsize + 1);
		if (!body) {
			close(fd);
			send_response(client_fd, 500, "Internal Server Error",
				      NULL, "500 OOM\n", 9);
			close(client_fd);
			return;
		}
		n = read(fd, body, fsize);
		close(fd);
		body[n] = '\0';

		send_response(client_fd, 200, "OK",
			      "text/plain; charset=utf-8", body, n);
		free(body);
	} else if (strcmp(path, "/") == 0) {
		send_redirect(client_fd, "/metrics");
	} else {
		send_404(client_fd);
	}

	close(client_fd);
}

/* ---------- HTTP server thread ---------- */

static void *http_server_thread(void *arg)
{
	(void)arg;

	syslog(LOG_INFO, "HTTP metrics server listening on port %d", HTTP_PORT);

	while (http_running) {
		struct sockaddr_in client_addr;
		socklen_t addrlen = sizeof(client_addr);
		int client_fd = accept(listen_fd,
				       (struct sockaddr *)&client_addr,
				       &addrlen);
		if (client_fd < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			if (!http_running)
				break;
			syslog(LOG_WARNING, "HTTP accept error: %s",
			       strerror(errno));
			continue;
		}

		handle_client(client_fd);
	}

	syslog(LOG_INFO, "HTTP metrics server stopped");
	close(listen_fd);
	listen_fd = -1;
	return NULL;
}

/* ---------- public API ---------- */

int lhsr_http_start(struct daemon_state *st)
{
	(void)st;

	/* Create socket */
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		syslog(LOG_WARNING, "HTTP: socket() failed: %s",
		       strerror(errno));
		return -1;
	}

	/* Allow immediate reuse after restart */
	int opt = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	/* Bind to port 9101 on all interfaces */
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(HTTP_PORT);

	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		syslog(LOG_WARNING, "HTTP: bind(port %d) failed: %s",
		       HTTP_PORT, strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return -1;
	}

	if (listen(listen_fd, BACKLOG) < 0) {
		syslog(LOG_WARNING, "HTTP: listen() failed: %s",
		       strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return -1;
	}

	http_running = 1;
	if (pthread_create(&http_tid, NULL, http_server_thread, NULL) != 0) {
		syslog(LOG_WARNING, "HTTP: pthread_create failed: %s",
		       strerror(errno));
		http_running = 0;
		close(listen_fd);
		listen_fd = -1;
		return -1;
	}

	return 0;
}

void lhsr_http_stop(void)
{
	if (http_running) {
		http_running = 0;
		pthread_cancel(http_tid);
		pthread_join(http_tid, NULL);
	}
	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}
}
