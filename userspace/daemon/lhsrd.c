/*
 * LHSR Daemon - Background service for array monitoring and management
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
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <time.h>

#include "../../lib/raid_engine.h"

#define LHSRD_PID_FILE "/run/lhsrd.pid"
#define LHSRD_CONFIG_DIR "/etc/lhsr"
#define LHSRD_STATUS_FILE "/run/lhsrd.status"
#define MONITOR_INTERVAL 60

static volatile int running = 1;
static int daemon_mode = 0;
static int verbose = 0;

static void signal_handler(int sig)
{
    syslog(LOG_INFO, "Received signal %d, shutting down", sig);
    running = 0;
}

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

static int daemonize(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        exit(0);

    umask(0);

    if (setsid() < 0)
        return -1;

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    return 0;
}

static void log_status(const char *msg)
{
    if (verbose)
        syslog(LOG_DEBUG, "%s", msg);
}

static void monitor_array(struct lhsr_array *arr)
{
    struct lhsr_status status;
    struct lhsr_smart_data smart;
    unsigned int i;
    int ret;

    ret = lhsr_array_status(arr, &status);
    if (ret < 0) {
        syslog(LOG_WARNING, "Failed to get status for array %s", arr->uuid);
        return;
    }

    if (status.state != LHSR_STATE_HEALTHY) {
        syslog(LOG_WARNING, "Array %s in degraded state: %u",
              arr->uuid, status.state);
    }

    for (i = 0; i < status.disk_count; i++) {
        if (status.disk_health[i] < 50) {
            syslog(LOG_WARNING, "Disk %u in array %s has low health: %u",
                  i, arr->uuid, status.disk_health[i]);
        }

        if (arr->disks[i]) {
            ret = lhsr_disk_get_smart(arr->disks[i], &smart);
            if (ret == 0) {
                if (smart.reallocated > 100)
                    syslog(LOG_WARNING, "Disk %u has high reallocated sectors: %d",
                          i, smart.reallocated);
                if (smart.temperature > 50)
                    syslog(LOG_WARNING, "Disk %u temperature high: %d",
                          i, smart.temperature);
            }
        }
    }
}

static void *monitor_thread(void *arg)
{
    struct lhsr_context *ctx = arg;

    (void)ctx;

    syslog(LOG_INFO, "Monitor thread started");

    while (running) {
        sleep(MONITOR_INTERVAL);

        if (!running)
            break;

        if (verbose)
            syslog(LOG_DEBUG, "Checking arrays...");

        if (verbose) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            syslog(LOG_DEBUG, "Monitor tick at %ld.%ld",
                  (long)ts.tv_sec, (long)ts.tv_nsec);
        }
    }

    syslog(LOG_INFO, "Monitor thread stopping");
    return NULL;
}

static void write_status_file(struct lhsr_context *ctx)
{
    FILE *f;
    (void)ctx;

    f = fopen(LHSRD_STATUS_FILE, "w");
    if (!f)
        return;

    fprintf(f, "# LHSR Daemon Status\n");
    fprintf(f, "version: %d.%d.%d\n",
            LHSR_VERSION_MAJOR, LHSR_VERSION_MINOR, LHSR_VERSION_PATCH);
    fprintf(f, "timestamp: %ld\n", (long)time(NULL));
    fprintf(f, "running: %d\n", running ? 1 : 0);

    fclose(f);
}

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
    printf("  Config:    %s/*.conf\n", LHSRD_CONFIG_DIR);
    printf("  Status:    %s\n", LHSRD_STATUS_FILE);
}

int main(int argc, char **argv)
{
    int opt;
    pthread_t monitor;
    struct lhsr_context *ctx = NULL;
    int ret;

    while ((opt = getopt(argc, argv, "dvh")) != -1) {
        switch (opt) {
        case 'd':
            daemon_mode = 1;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    openlog("lhsrd", LOG_PID | LOG_PERROR, LOG_DAEMON);

    if (daemon_mode) {
        if (daemonize() < 0) {
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

    ctx = lhsr_init();
    if (!ctx) {
        syslog(LOG_ERR, "Failed to initialize LHSR context");
        remove_pid_file();
        return 1;
    }

    ret = pthread_create(&monitor, NULL, monitor_thread, ctx);
    if (ret != 0) {
        syslog(LOG_ERR, "Failed to create monitor thread: %d", ret);
        lhsr_free(ctx);
        remove_pid_file();
        return 1;
    }

    syslog(LOG_INFO, "LHSR daemon started (PID: %d)", getpid());

    write_status_file(ctx);

    while (running) {
        sleep(1);
    }

    syslog(LOG_INFO, "LHSR daemon stopping");

    pthread_join(monitor, NULL);

    lhsr_free(ctx);
    remove_pid_file();

    closelog();
    return 0;
}