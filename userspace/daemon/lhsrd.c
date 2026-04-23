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
#include <glob.h>

#include "../../lib/raid_engine.h"

#define LHSRD_PID_FILE "/run/lhsrd.pid"
#define LHSRD_CONFIG_DIR "/etc/lhsr"
#define LHSRD_STATUS_FILE "/run/lhsrd.status"
#define MONITOR_INTERVAL 60

/* SMART thresholds */
#define SMART_REALLOCATED_THRESHOLD 100
#define SMART_PENDING_THRESHOLD 50
#define SMART_TEMP_THRESHOLD 50
#define SMART_HEALTH_LOW 50

/* Config */
struct monitor_config {
    int smart_poll_interval;
    int error_threshold;
    int auto_failover;
    int notify_on_fail;
};

/* Disk health state */
struct disk_health_state {
    char device_path[256];
    int disk_index;
    int smart_reallocated;
    int smart_pending;
    int smart_uncorrectable;
    int temperature;
    int health;
    time_t last_check;
    int consecutive_errors;
    int failed;
};

static volatile int running = 1;
static int daemon_mode = 0;
static int verbose = 0;
static struct monitor_config global_config = {
    .smart_poll_interval = 300,
    .error_threshold = 3,
    .auto_failover = 1,
    .notify_on_fail = 1
};

static struct disk_health_state disk_states[32];
static int disk_state_count = 0;

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

/* Execute command and get output */
static int run_command(const char *cmd, char *output, size_t out_size)
{
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;

    if (output && fgets(output, out_size, p))
        output[strlen(output) - 1] = '\0';

    int ret = pclose(p);
    return WEXITSTATUS(ret);
}

/* Poll SMART data from a disk - supports both SATA and NVMe */
static int poll_smart_data(const char *device, struct lhsr_smart_data *smart)
{
    char cmd[512];
    char line[256];
    FILE *p;
    int ret = -1;

    smart->reallocated = 0;
    smart->pending = 0;
    smart->uncorrectable = 0;
    smart->temperature = 0;
    smart->health = 100;

    /* Check if NVMe device */
    if (strstr(device, "nvme")) {
        snprintf(cmd, sizeof(cmd),
            "smartctl -A %s 2>/dev/null | grep -E 'Reallocated_Sector_Ct|Pending_Sector|Offline_Uncorrectable|Temperature_Celsius'",
            device);
    } else {
        /* SATA device - extract drive letter */
        snprintf(cmd, sizeof(cmd),
            "smartctl -A /dev/sd%c 2>/dev/null | grep -E 'Reallocated_Sector_Ct|Pending_Sector|Offline_Uncorrectable|Temperature_Celsius'",
            device[strlen(device) - 1]);
    }

    p = popen(cmd, "r");
    if (!p)
        return -1;

    while (fgets(line, sizeof(line), p)) {
        if (sscanf(line, "%d %d", &ret, &smart->reallocated) == 2) {
            continue;
        }
        if (strstr(line, "Reallocated")) {
            sscanf(line, "%*s %d", &smart->reallocated);
        } else if (strstr(line, "Pending")) {
            sscanf(line, "%*s %d", &smart->pending);
        } else if (strstr(line, "Uncorrectable")) {
            sscanf(line, "%*s %d", &smart->uncorrectable);
        } else if (strstr(line, "Temperature")) {
            sscanf(line, "%*s %d", &smart->temperature);
        }
    }
    pclose(p);

    if (smart->reallocated > SMART_REALLOCATED_THRESHOLD)
        smart->health -= 30;
    if (smart->pending > SMART_PENDING_THRESHOLD)
        smart->health -= 20;
    if (smart->uncorrectable > 0)
        smart->health -= 40;
    if (smart->temperature > SMART_TEMP_THRESHOLD)
        smart->health -= 10;

    if (smart->health < 0)
        smart->health = 0;

    return 0;
}

/* Check disk health and report failure if needed */
static void check_disk_health(struct disk_health_state *state, const char *dm_device)
{
    struct lhsr_smart_data smart;
    char cmd[512];
    int health_score = 100;

    if (poll_smart_data(state->device_path, &smart) == 0) {
        state->smart_reallocated = smart.reallocated;
        state->smart_pending = smart.pending;
        state->temperature = smart.temperature;
        state->health = smart.health;

        if (verbose) {
            syslog(LOG_DEBUG, "Disk %s: realloc=%d pending=%d temp=%d health=%d",
                  state->device_path, smart.reallocated, smart.pending,
                  smart.temperature, smart.health);
        }

        if (smart.reallocated > SMART_REALLOCATED_THRESHOLD) {
            syslog(LOG_WARNING, "Disk %s has high reallocated sectors: %d",
                  state->device_path, smart.reallocated);
            health_score -= 30;
            state->consecutive_errors++;
        }

        if (smart.pending > SMART_PENDING_THRESHOLD) {
            syslog(LOG_WARNING, "Disk %s has pending sectors: %d",
                  state->device_path, smart.pending);
            health_score -= 20;
            state->consecutive_errors++;
        }

        if (smart.temperature > SMART_TEMP_THRESHOLD) {
            syslog(LOG_WARNING, "Disk %s temperature high: %d",
                  state->device_path, smart.temperature);
            health_score -= 10;
        }

        if (smart.health < SMART_HEALTH_LOW || health_score < SMART_HEALTH_LOW) {
            syslog(LOG_WARNING, "Disk %s health critical: %d",
                  state->device_path, health_score);

            if (global_config.auto_failover && dm_device) {
                snprintf(cmd, sizeof(cmd), "dmsetup message %s disk_fail %d",
                    dm_device, state->disk_index);
                syslog(LOG_INFO, "Notifying kernel: %s", cmd);
                run_command(cmd, NULL, 0);
            }

            state->failed = 1;
            state->consecutive_errors = global_config.error_threshold;
        }
    } else {
        state->consecutive_errors++;
        syslog(LOG_WARNING, "Failed to poll SMART from %s (error %d)",
              state->device_path, state->consecutive_errors);

        if (state->consecutive_errors >= global_config.error_threshold) {
            if (global_config.auto_failover && dm_device) {
                snprintf(cmd, sizeof(cmd), "dmsetup message %s disk_fail %d",
                    dm_device, state->disk_index);
                syslog(LOG_INFO, "Notifying kernel of disk failure: %s", cmd);
                run_command(cmd, NULL, 0);
            }
            state->failed = 1;
        }
    }

    state->last_check = time(NULL);
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

/* Health monitor with SMART polling */
static void *health_monitor_thread(void *arg)
{
    struct lhsr_context *ctx = arg;
    (void)ctx;

    syslog(LOG_INFO, "Health monitor thread started (interval: %ds)",
          global_config.smart_poll_interval);

    while (running) {
        sleep(global_config.smart_poll_interval);

        if (!running)
            break;

        for (int i = 0; i < disk_state_count; i++) {
            if (verbose)
                syslog(LOG_DEBUG, "Checking disk %s...", disk_states[i].device_path);

            check_disk_health(&disk_states[i], NULL);
        }
    }

    syslog(LOG_INFO, "Health monitor thread stopping");
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
    pthread_t health_monitor;
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

    ret = pthread_create(&health_monitor, NULL, health_monitor_thread, ctx);
    if (ret != 0) {
        syslog(LOG_ERR, "Failed to create health monitor thread: %d", ret);
    }

    syslog(LOG_INFO, "LHSR daemon started (PID: %d)", getpid());

    write_status_file(ctx);

    while (running) {
        sleep(1);
    }

    syslog(LOG_INFO, "LHSR daemon stopping");

    pthread_join(monitor, NULL);
    pthread_join(health_monitor, NULL);

    lhsr_free(ctx);
    remove_pid_file();

    closelog();
    return 0;
}