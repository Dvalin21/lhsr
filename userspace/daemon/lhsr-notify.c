/*
 * LHSR Daemon - Notification delivery (email + webhook)
 *
 * Best-effort delivery via fork+exec of sendmail(8) and/or curl(1).
 * No library dependencies — POSIX fork/exec and standard tools.
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>

#include "lhsrd.h"

/*
 * Fork+exec a command with arguments.
 * Returns 0 on success (child exited 0), -1 on failure.
 */
static int run_cmd(const char *path, char *const argv[], const char *stdin_data)
{
	pid_t pid;
	int pipefd[2];

	if (pipe(pipefd) < 0) {
		syslog(LOG_WARNING, "notify: pipe() failed: %m");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		syslog(LOG_WARNING, "notify: fork() failed: %m");
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	if (pid == 0) {
		/* Child */
		close(pipefd[1]);	/* close write end */
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);

		/* Set up stdout/stderr to /dev/null */
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > 2)
				close(fd);
		}

		execvp(path, argv);
		_exit(127);	/* exec failed */
	}

	/* Parent */
	close(pipefd[0]);	/* close read end */
	if (stdin_data)
		write(pipefd[1], stdin_data, strlen(stdin_data));
	close(pipefd[1]);

	int status;
	waitpid(pid, &status, 0);

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;

	syslog(LOG_WARNING, "notify: %s exited with status %d",
	       path, WEXITSTATUS(status));
	return -1;
}

/*
 * Deliver an email alert via sendmail.
 */
static int notify_email(struct daemon_state *st,
			const char *subject, const char *body)
{
	if (!st->cfg.notify_sendmail[0] || !st->cfg.notify_email_to[0])
		return -1;

	/* Build email headers + body to pipe into sendmail */
	char msg[4096];
	time_t now = time(NULL);
	char time_str[64];
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(time_str, sizeof(time_str), "%a, %d %b %Y %H:%M:%S %z", &tm);

	int len = snprintf(msg, sizeof(msg),
		"From: LHSR Daemon <root>\n"
		"To: %s\n"
		"Subject: %s\n"
		"Date: %s\n"
		"\n"
		"%s\n",
		st->cfg.notify_email_to,
		subject,
		time_str,
		body);

	if (len < 0 || (size_t)len >= sizeof(msg)) {
		syslog(LOG_WARNING, "notify: email message too long, truncated");
		msg[sizeof(msg) - 1] = '\0';
	}

	char *argv[] = {
		(char *)st->cfg.notify_sendmail,
		"-i",			/* ignore dots in message */
		"-t",			/* read To: header from message */
		NULL
	};

	int ret = run_cmd(st->cfg.notify_sendmail, argv, msg);
	if (ret == 0)
		syslog(LOG_INFO, "notify: email sent to %s",
		       st->cfg.notify_email_to);
	else
		syslog(LOG_WARNING, "notify: email delivery failed");

	return ret;
}

/*
 * Deliver a webhook alert via curl HTTP POST.
 */
static int notify_webhook(struct daemon_state *st,
			  const char *subject, const char *body)
{
	if (!st->cfg.notify_webhook_url[0])
		return -1;

	/* Build JSON payload */
	char payload[4096];
	time_t now = time(NULL);

	int len = snprintf(payload, sizeof(payload),
		"{\n"
		"  \"subject\": \"%s\",\n"
		"  \"message\": \"%s\",\n"
		"  \"timestamp\": %ld,\n"
		"  \"hostname\": \"%s\"\n"
		"}\n",
		subject, body, (long)now,
		st->cfg.notify_email_to[0] ? st->cfg.notify_email_to : "lhsrd");

	if (len < 0 || (size_t)len >= sizeof(payload)) {
		syslog(LOG_WARNING, "notify: webhook payload too long, truncated");
		payload[sizeof(payload) - 1] = '\0';
	}

	char *argv[] = {
		"curl",
		"-s",			/* silent */
		"-S",			/* show errors on stderr */
		"-o", "/dev/null",	/* discard response body */
		"-X", "POST",
		"-H", "Content-Type: application/json",
		"-d", payload,
		(char *)st->cfg.notify_webhook_url,
		NULL
	};

	syslog(LOG_INFO, "notify: sending webhook to %s",
	       st->cfg.notify_webhook_url);

	int ret = run_cmd("curl", argv, NULL);
	if (ret == 0)
		syslog(LOG_INFO, "notify: webhook sent");
	else
		syslog(LOG_WARNING, "notify: webhook delivery failed (curl exit %d)", ret);

	return ret;
}

void lhsr_notify_alert(struct daemon_state *st,
		       const char *subject, const char *message)
{
	/* Try email */
	if (st->cfg.notify_sendmail[0] && st->cfg.notify_email_to[0])
		notify_email(st, subject, message);

	/* Try webhook */
	if (st->cfg.notify_webhook_url[0])
		notify_webhook(st, subject, message);

	/* If neither is configured, silently ignore */
}
