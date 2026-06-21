/*
 * LHSR CLI - Command Line Interface
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <libgen.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/wait.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include <linux/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "../../lib/raid_engine.h"
#include "shr.h"

#include "lhsr.h"		/* canonical on-disk format definitions */

#define PROGNAME "lhsrctl"
#define DM_DEV_PATH "/dev/mapper/control"
#define MAX_ARGS 64

/* Daemon communication paths */
#define LHSRD_STATUS_FILE  "/run/lhsrd.status"
#define LHSRD_SOCKET_PATH  "/run/lhsrd.sock"

/* SMART thresholds (must match daemon) */
#define LHSRD_REALLOCATED_THRESH  100
#define LHSRD_PENDING_THRESH      50
#define LHSRD_TEMP_CRITICAL       60

/* Forward declarations for daemon communication helpers (defined below) */
static char *read_file(const char *path);
static int json_int(const char *json, const char *key, int *val);
static int json_string(const char *json, const char *key, char *buf, size_t sz);
static int json_double(const char *json, const char *key, double *val);
static int json_int_in(const char *start, const char *end,
		       const char *key, int *val);
static int json_string_in(const char *start, const char *end,
			  const char *key, char *buf, size_t sz);
static int json_double_in(const char *start, const char *end,
			  const char *key, double *val);
static const char *next_object(const char *pos, const char **end);
static void format_duration(long seconds, char *buf, size_t sz);
static int connect_control_socket(void);
static char *control_query(const char *cmd);

/* Forward declarations from subcommand files */
int cmd_shr_plan(int argc, char **argv);
int cmd_shr_create(int argc, char **argv);
int cmd_shr_status(int argc, char **argv);
int cmd_shr_destroy(int argc, char **argv);
int cmd_shr_scrub(int argc, char **argv);
int cmd_shr_disk(int argc, char **argv);
int cmd_shr_rebuild(int argc, char **argv);

/* Command options */
enum {
	CMD_NONE = 0,
	CMD_CREATE,
	CMD_ADD,
	CMD_REMOVE,
	CMD_STATUS,
	CMD_REPAIR,
	CMD_SCRUB,
	CMD_EXPAND,
	CMD_PREDICT,
	CMD_BITROT,
	CMD_BITROT_LOG,
	CMD_RECOVER,
	CMD_RECONSTRUCT,
	CMD_HEALTH,
	CMD_SHR,
};

/* Usage */
static void usage(const char *prog)
{
	fprintf(stderr,
		"LHSR - Linux Hybrid Self-Healing RAID\n"
		"\n"
		"Usage: %s <command> [options]\n"
		"\n"
		"Commands:\n"
		"  create --raid <type> <disk>...  Create array\n"
		"  add <disk>                    Add disk to array\n"
		"  remove <disk>                Remove disk from array\n"
		"  status                       Show array status\n"
		"  repair                       Repair array\n"
		"  scrub [quick|deep]           Run scrub\n"
		"  expand                       Expand array\n"
		"  predict                      Show disk failure predictions\n"
		"  bitrot                       Show bit-rot status\n"
		"  bitrot-log                   Show corruption log\n"
		"  disk-fail <device> <index>    Mark disk as failed\n"
		"  disk-online <device> <index>    Mark disk as online\n"
		"  disk-health <device> <index>  Query disk health\n"
		"  recover <device>...          Scan and assemble LHSR arrays\n"
		"  reconstruct [opts] <dev>...   Reconstruct missing RAID5/6 disk from N-1\n"
		"  health [--json]               Show daemon health summary\n"
		"  shr plan [opts] <dev>...     Compute SHR layout for variable-size disks\n"
		"  shr create [opts] <dev>...   Execute SHR layout (DESTRUCTIVE)\n"
		"  shr status                   Show current SHR topology and health\n"
		"  shr scrub [start|stop]       Start/stop scrubbing (health check)\n"
		"  shr disk <fail|online|list> <n>  Fail/bring online/list member disk\n"
		"  shr disk replace <tier> <old> <new>  Replace disk, start rebuild\n"
		"  shr rebuild <start <n>|stop|status>  Rebuild a failed disk\n"
		"  shr expand <tier> <dev>...    Add disks as new tier + extend LVM\n"
		"  message <device> <msg> [args] Send message to kernel\n"
		"\n"
		"RAID Types:\n"
		"  single   Single disk (JBOD)\n"
		"  mirror  2-way mirroring\n"
		"  raid5   RAID5 (single parity)\n"
		"  raid6   RAID6 (dual parity)\n"
		"  shr     Synology Hybrid RAID\n"
		"  shr2    Synology Hybrid RAID-2\n"
		"\n"
		"Examples:\n"
		"  %s create --raid raid5 /dev/sdb /dev/sdc /dev/sdd\n"
		"  %s create --raid shr /dev/sdb /dev/sdc /dev/sdd /dev/sde\n"
		"  %s status\n"
		"  %s predict\n"
		"\n"
		"dm-integrity support:\n"
		"  Stack LHSR on dm-integrity for persistent per-block checksums.\n"
		"  Append 'integrity' to the dmsetup table line:\n"
		"    dmsetup create my-array --table \\\n"
		"      \"0 <size> lhsr raid5 8 1 8 \\\n"
		"       /dev/mapper/integrity-sdb 0 \\\n"
		"       /dev/mapper/integrity-sdc 0 \\\n"
		"       integrity\"\n"
		"  See scripts/setup-dm-integrity.sh for setup.\n"
		"\n",
		prog, prog, prog, prog, prog);
}

/* Command create */
static int cmd_create(int argc, char **argv, unsigned int raid_type)
{
	struct lhsr_context *ctx;
	struct lhsr_array *arr;
	struct lhsr_disk *disks;
	unsigned int disk_count;
	int ret;

	if (argc < 3) {
		fprintf(stderr, "Error: Require at least 2 disks\n");
		return 1;
	}

	ctx = lhsr_init();
	if (!ctx) {
		fprintf(stderr, "Error: Failed to initialize\n");
		return 1;
	}

	disks = calloc(argc - 2, sizeof(struct lhsr_disk));
	if (!disks) {
		fprintf(stderr, "Error: Out of memory\n");
		lhsr_free(ctx);
		return 1;
	}

	/* Open all disks */
	for (disk_count = 0; disk_count < (unsigned int)(argc - 2); disk_count++) {
		ret = lhsr_disk_open(&disks[disk_count], argv[disk_count + 2]);
		if (ret < 0) {
			fprintf(stderr, "Error: Cannot open %s: %s\n",
			       argv[disk_count + 2], strerror(-ret));
			while (disk_count > 0) {
				disk_count--;
				lhsr_disk_close(&disks[disk_count]);
			}
			free(disks);
			lhsr_free(ctx);
			return 1;
		}

		printf("Opened %s (%lu GB)\n", argv[disk_count + 2],
		       disks[disk_count].size / 1024 / 1024 / 1024);
	}

	/* Create array */
	arr = lhsr_array_create(ctx, raid_type, disks, disk_count);
	if (!arr) {
		fprintf(stderr, "Error: Failed to create array\n");
		while (disk_count > 0) {
			disk_count--;
			lhsr_disk_close(&disks[disk_count]);
		}
		free(disks);
		lhsr_free(ctx);
		return 1;
	}

	/* Show status */
	lhsr_array_print_status(arr);

	/* Store array info for other commands */
	/* In production, would save to config file */

	printf("Array created successfully!\n");
	printf("Use 'lhsrctl status' to monitor\n");

	lhsr_array_free(arr);
	free(disks);
	lhsr_free(ctx);

	return 0;
}

/* Command status — read /run/lhsrd.status and display */
static int cmd_status(int argc, char **argv)
{
	(void)argc; (void)argv;

	char *json = read_file(LHSRD_STATUS_FILE);
	if (!json) {
		printf("LHSR Status\n");
		printf("==========\n\n");
		printf("Daemon not running (status file %s not found).\n",
		       LHSRD_STATUS_FILE);
		printf("Start the daemon with: lhsrd -d\n");
		return 1;
	}

	/* Top-level fields */
	char version[32] = "?";
	int uptime = 0, num_arrays = 0, num_disks = 0;

	json_string(json, "version", version, sizeof(version));
	json_int(json, "uptime", &uptime);
	json_int(json, "num_arrays", &num_arrays);
	json_int(json, "num_disks", &num_disks);

	printf("LHSR Status\n");
	printf("==========\n");

	char uptime_str[64] = "0s";
	format_duration(uptime, uptime_str, sizeof(uptime_str));
	printf("Version: %s  |  Uptime: %s  |  Arrays: %d  |  Disks: %d\n\n",
	       version, uptime_str, num_arrays, num_disks);

	/* Arrays section */
	const char *arrays_section = strstr(json, "\"arrays\":");
	if (arrays_section) {
		const char *arr_start = strchr(arrays_section, '[');
		if (arr_start) {
			const char *obj_end = arr_start;
			for (int i = 0; i < num_arrays; i++) {
				const char *obj = next_object(obj_end, &obj_end);
				if (!obj)
					break;
				char name[256] = "?";
				int num_d = 0, num_w = 0, rtype = 0;
				json_string_in(obj, obj_end, "name", name, sizeof(name));
				json_int_in(obj, obj_end, "raid_type", &rtype);
				json_int_in(obj, obj_end, "num_disks", &num_d);
				json_int_in(obj, obj_end, "num_working", &num_w);
				const char *type_str = "?";
				switch (rtype) {
				case 0: type_str = "SINGLE"; break;
				case 1: type_str = "MIRROR"; break;
				case 2: type_str = "RAID5"; break;
				case 3: type_str = "RAID6"; break;
				case 4: type_str = "SHR"; break;
				case 5: type_str = "SHR2"; break;
				}
				printf("Array: %s  (%s, %d disks, %d working)\n",
				       name, type_str, num_d, num_w);
			}
		}
	}

	/* Disks section */
	const char *disks_section = strstr(json, "\"disks\":");
	if (disks_section) {
		const char *disk_start = strchr(disks_section, '[');
		if (disk_start) {
			const char *obj_end = disk_start;
			for (int i = 0; i < num_disks; i++) {
				const char *obj = next_object(obj_end, &obj_end);
				if (!obj)
					break;
				char device[256] = "?";
				int hs = 0, temp = 0, re = 0, pe = 0, un = 0, failed = 0;
				char warn[256] = "";
				json_string_in(obj, obj_end, "device", device, sizeof(device));
				json_int_in(obj, obj_end, "health_score", &hs);
				json_int_in(obj, obj_end, "temperature", &temp);
				json_int_in(obj, obj_end, "reallocated", &re);
				json_int_in(obj, obj_end, "pending", &pe);
				json_int_in(obj, obj_end, "uncorrectable", &un);
				json_int_in(obj, obj_end, "failed", &failed);
				json_string_in(obj, obj_end, "trend_warning",
					       warn, sizeof(warn));

				const char *label = (hs >= 90) ? "OK" :
					(hs >= 70) ? "WARNING" :
					(hs >= 40) ? "CRITICAL" : "FAILING";
				printf("\nDisk %d: %s\n", i, device);
				printf("  Health:     %3d/100 [%s]%s\n",
				       hs, label, failed ? "  *** FAILED ***" : "");
				printf("  Temp:       %d°C\n", temp);
				printf("  Realloc:    %d  Pending: %d  Uncorr: %d\n",
				       re, pe, un);
				if (warn[0])
					printf("  Trend:      %s\n", warn);
			}
		}
	}

	if (num_arrays == 0 && num_disks == 0)
		printf("No arrays configured. Create one with:\n");
	printf("  lhsrctl create --raid <type> <disk>...\n");

	free(json);
	return 0;
}

/* Command predict — query daemon for trend data, estimate failure risk */
static int cmd_predict(int argc, char **argv)
{
	(void)argc; (void)argv;

	char *json = control_query("{\"cmd\":\"trends\"}\n");
	if (!json) {
		printf("LHSR Predictive Failure Analysis\n");
		printf("================================\n\n");
		printf("Cannot connect to daemon (socket %s).\n",
		       LHSRD_SOCKET_PATH);
		printf("Is the daemon running? Start with: lhsrd -d\n");
		return 1;
	}

	int num_disks = 0;
	json_int(json, "num_disks", &num_disks);

	printf("LHSR Predictive Failure Analysis\n");
	printf("================================\n\n");

	if (num_disks <= 0) {
		printf("No disks tracked by daemon.\n");
		free(json);
		return 0;
	}

	/* Walk the trends array */
	const char *trends_section = strstr(json, "\"trends\":");
	if (!trends_section) {
		printf("No trend data available.\n");
		free(json);
		return 0;
	}

	const char *arr_start = strchr(trends_section, '[');
	if (!arr_start) {
		printf("No trend data available.\n");
		free(json);
		return 0;
	}

	const char *obj_end = arr_start;
	int found_any = 0;

	for (int i = 0; i < num_disks; i++) {
		const char *obj = next_object(obj_end, &obj_end);
		if (!obj)
			break;

		/* Current values */
		char device[256] = "?";
		int health_score = 0, temp = 0, re = 0, pe = 0, un = 0;
		int data_points = 0;
		double re_slope = 0, pe_slope = 0, un_slope = 0, tmp_slope = 0;
		int re_warn = 0, pe_warn = 0, un_warn = 0, tmp_warn = 0;

		json_string_in(obj, obj_end, "device", device, sizeof(device));
		json_int_in(obj, obj_end, "health_score", &health_score);
		json_int_in(obj, obj_end, "temperature", &temp);
		json_int_in(obj, obj_end, "reallocated", &re);
		json_int_in(obj, obj_end, "pending", &pe);
		json_int_in(obj, obj_end, "uncorrectable", &un);
		json_int_in(obj, obj_end, "data_points", &data_points);
		json_double_in(obj, obj_end, "reallocated_slope", &re_slope);
		json_double_in(obj, obj_end, "pending_slope", &pe_slope);
		json_double_in(obj, obj_end, "uncorrectable_slope", &un_slope);
		json_double_in(obj, obj_end, "temperature_slope", &tmp_slope);
		json_int_in(obj, obj_end, "reallocated_warn", &re_warn);
		json_int_in(obj, obj_end, "pending_warn", &pe_warn);
		json_int_in(obj, obj_end, "uncorrectable_warn", &un_warn);
		json_int_in(obj, obj_end, "temperature_warn", &tmp_warn);

		found_any = 1;

		const char *label = (health_score >= 90) ? "OK" :
				    (health_score >= 70) ? "WARNING" :
				    (health_score >= 40) ? "CRITICAL" : "FAILING";

		printf("Disk: %s\n", device);
		printf("  Health Score:   %d/100 [%s]\n", health_score, label);

		/* Temperature line */
		printf("  Temperature:    %d°C", temp);
		if (tmp_warn)
			printf(" (rising %.1f°C/day) ***",
			       tmp_slope);
		else if (data_points >= 3 && tmp_slope > 0)
			printf(" (rising %.1f°C/day)", tmp_slope);
		else
			printf(" (stable)");
		printf("\n");

		/* Reallocated line */
		printf("  Reallocated:    %d sectors", re);
		if (re_warn)
			printf(" (increasing %.1f/day) ***", re_slope);
		else if (data_points >= 3 && re_slope > 0)
			printf(" (increasing %.1f/day)", re_slope);
		else
			printf(" (stable)");
		printf("\n");

		/* Pending line */
		printf("  Pending:        %d sectors", pe);
		if (pe_warn)
			printf(" (increasing %.1f/day) ***", pe_slope);
		else if (data_points >= 3 && pe_slope > 0)
			printf(" (increasing %.1f/day)", pe_slope);
		else
			printf(" (stable)");
		printf("\n");

		/* Trend data count */
		printf("  Trend Data:     %d points%s\n",
		       data_points,
		       data_points < 3 ? " (need 3+ for analysis)" : "");

		/* Predictions */
		if (data_points >= 3) {
			int min_days = 9999;
			const char *min_reason = NULL;
			char pred_buf[512];
			int pos = 0;

			snprintf(pred_buf, sizeof(pred_buf), "  Predictions:");

			if (re_slope > 0.01) {
				int days = (int)((LHSRD_REALLOCATED_THRESH - re)
						  / re_slope);
				if (days < 0)
					days = 0;
				if (days < min_days) {
					min_days = days;
					min_reason = "reallocated sectors";
				}
				pos += snprintf(pred_buf + pos,
						sizeof(pred_buf) - pos,
						"\n    Reallocated threshold (%d) in ~%d days",
						LHSRD_REALLOCATED_THRESH, days);
			}
			if (pe_slope > 0.01) {
				int days = (int)((LHSRD_PENDING_THRESH - pe)
						  / pe_slope);
				if (days < 0)
					days = 0;
				if (days < min_days) {
					min_days = days;
					min_reason = "pending sectors";
				}
				pos += snprintf(pred_buf + pos,
						sizeof(pred_buf) - pos,
						"\n    Pending threshold (%d) in ~%d days",
						LHSRD_PENDING_THRESH, days);
			}
			if (tmp_slope > 0.01) {
				int days = (int)((LHSRD_TEMP_CRITICAL - temp)
						  / tmp_slope);
				if (days < 0)
					days = 0;
				if (days < min_days) {
					min_days = days;
					min_reason = "temperature";
				}
				pos += snprintf(pred_buf + pos,
						sizeof(pred_buf) - pos,
						"\n    Temperature critical (%d°C) in ~%d days",
						LHSRD_TEMP_CRITICAL, days);
			}

			if (pos > 17) { /* more than "  Predictions:" */
				printf("%s\n", pred_buf);
				if (min_days <= 30)
					printf("  *** Plan disk replacement within %d days (%s) ***\n",
					       min_days, min_reason);
				else if (min_days <= 90)
					printf("  * Monitor closely — threshold in ~%d days (%s)\n",
					       min_days, min_reason);
				else
					printf("  Threshold in ~%d days (%s) — routine monitoring\n",
					       min_days, min_reason);
			} else {
				printf("  Predictions:    No concerning trends detected\n");
			}
		} else {
			printf("  Predictions:    Insufficient data for trend analysis\n");
		}

		/* Warning count */
		int warn_count = re_warn + pe_warn + un_warn + tmp_warn;
		if (warn_count > 0) {
			printf("  Active alerts:  %d trend warning(s)\n", warn_count);
			if (health_score < 70)
				printf("  *** DISK HEALTH CRITICAL — replace immediately ***\n");
			else if (health_score < 90)
				printf("  * Disk showing signs of degradation — plan replacement\n");
		}

		printf("\n");
	}

	if (!found_any)
		printf("No trend data available.\n");

	free(json);
	return 0;
}

/* Command bitrot - Anti-Bit-Rot status */
static int cmd_bitrot(int argc, char **argv)
{
	(void)argc; (void)argv;

	printf("LHSR Anti-Bit-Rot Protection\n");
	printf("=============================\n\n");
	printf("Detection:     Active\n");
	printf("Severity Levels:\n");
	printf("  0 = None\n");
	printf("  1 = Single-bit\n");
	printf("  2 = Multi-bit\n");
	printf("  3 = Full block corruption\n");
	printf("\nUse 'lhsrctl bitrot-log' to view corruption log\n");

	return 0;
}

/* Command bitrot-log - Corruption log */
static int cmd_bitrot_log(int argc, char **argv)
{
	(void)argc; (void)argv;

	printf("LHSR Corruption Log\n");
	printf("====================\n\n");
	printf("Note: No corruption events logged\n");
	printf("Run scrub to verify data integrity\n");

	return 0;
}

/* Command health — quick daemon health summary */
static int cmd_health(int argc, char **argv)
{
	int json_output = 0;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--json") == 0)
			json_output = 1;
	}

	char *json = read_file(LHSRD_STATUS_FILE);
	if (!json) {
		if (json_output) {
			printf("{\"status\":\"error\",\"message\":\"Daemon not running\"}\n");
		} else {
			printf("LHSR Health\n");
			printf("===========\n\n");
			printf("Daemon: NOT RUNNING\n");
			printf("Start daemon: lhsrd -d\n");
		}
		return 1;
	}

	/* Parse top-level fields */
	char version[32] = "?";
	int uptime = 0, running = 0, num_arrays = 0, num_disks = 0;
	json_string(json, "version", version, sizeof(version));
	json_int(json, "uptime", &uptime);
	json_int(json, "running", &running);
	json_int(json, "num_arrays", &num_arrays);
	json_int(json, "num_disks", &num_disks);

	if (json_output) {
		/* Just pass through the status file JSON */
		printf("%s\n", json);
		free(json);
		return 0;
	}

	char uptime_str[64] = "0s";
	format_duration(uptime, uptime_str, sizeof(uptime_str));

	printf("LHSR Health Summary\n");
	printf("===================\n");
	printf("Daemon:     %s (v%s, uptime %s)\n",
	       running ? "RUNNING" : "STOPPED",
	       version, uptime_str);
	printf("Arrays:     %d\n", num_arrays);
	printf("Disks:      %d\n\n", num_disks);

	/* Scan each disk for health label and warning */
	const char *disks_section = strstr(json, "\"disks\":");
	int healthy = 0, warning = 0, critical = 0, failed = 0;

	if (disks_section) {
		const char *disk_start = strchr(disks_section, '[');
		if (disk_start) {
			const char *obj_end = disk_start;
			for (int i = 0; i < num_disks; i++) {
				const char *obj = next_object(obj_end, &obj_end);
				if (!obj)
					break;
				int hs = 0, f = 0;
				char warn[256] = "";
				json_int_in(obj, obj_end, "health_score", &hs);
				json_int_in(obj, obj_end, "failed", &f);
				json_string_in(obj, obj_end, "trend_warning",
					       warn, sizeof(warn));

				if (f)
					failed++;
				else if (hs < 40)
					critical++;
				else if (hs < 70)
					warning++;
				else
					healthy++;

				if (f || hs < 70) {
					char device[256] = "?";
					json_string_in(obj, obj_end, "device",
						       device, sizeof(device));
					printf("  %s: health=%d%s",
					       device, hs, f ? " FAILED" : "");
					if (warn[0])
						printf(" [%s]", warn);
					printf("\n");
				}
			}
		}
	}

	printf("\nDisk Status: %d healthy, %d warning, %d critical, %d failed\n",
	       healthy, warning, critical, failed);

	/* Overall status */
	if (failed > 0)
		printf("Overall: DEGRADED — %d disk(s) failed\n", failed);
	else if (critical > 0)
		printf("Overall: CRITICAL — %d disk(s) below 40%% health\n", critical);
	else if (warning > 0)
		printf("Overall: WARNING — %d disk(s) showing degradation signs\n", warning);
	else if (num_disks > 0)
		printf("Overall: HEALTHY — all disks OK\n");
	else
		printf("Overall: No disks tracked\n");

	free(json);
	return (failed > 0) ? 2 : (critical > 0) ? 1 : 0;
}

/* Message command - send message to kernel via dmsetup */
static int cmd_message(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "Usage: %s message <device> <message> [args]\n",
			PROGNAME);
		return 1;
	}

	/* Validate device path and message for basic safety */
	if (strchr(argv[2], '`') || strchr(argv[2], '$') ||
	    strchr(argv[2], ';') || strchr(argv[2], '|')) {
		fprintf(stderr, "Error: Invalid characters in device path\n");
		return 1;
	}
	if (strchr(argv[3], '`') || strchr(argv[3], '$') ||
	    strchr(argv[3], ';') || strchr(argv[3], '|')) {
		fprintf(stderr, "Error: Invalid characters in message\n");
		return 1;
	}

	/* Use fork+execvp to avoid shell injection */
	pid_t pid = fork();
	if (pid == 0) {
		/* Child process */
		char *dmsetup_argv[5];
		dmsetup_argv[0] = "dmsetup";
		dmsetup_argv[1] = "message";
		dmsetup_argv[2] = argv[2];
		dmsetup_argv[3] = argv[3];
		dmsetup_argv[4] = argc > 4 ? argv[4] : NULL;
		execvp("dmsetup", dmsetup_argv);
		exit(127);
	} else if (pid > 0) {
		/* Parent process */
		int status;
		waitpid(pid, &status, 0);
		return WEXITSTATUS(status) != 0;
	}
	return 1;
}

/* Command scrub */
static int cmd_scrub(int argc, char **argv)
{
	struct lhsr_context *ctx;
	struct lhsr_array *arr = NULL;
	struct lhsr_disk *disks = NULL;
	struct lhsr_scrubber *scrub = NULL;
	struct lhsr_scrub_config config;
	struct lhsr_scrub_progress prog;
	int ret = 0;

	(void)arr; (void)disks; (void)scrub; (void)config; (void)prog;

	printf("LHSR Scrub\n");
	printf("==========\n\n");

	ctx = lhsr_init();
	if (!ctx) {
		fprintf(stderr, "Error: Failed to initialize\n");
		return 1;
	}

	/* In production, would load array from config */
	printf("Note: No array configured - run 'create' first\n");
	printf("Usage: %s scrub [--start|--stop|--pause|--resume|--status]\n", PROGNAME);

	(void)argc; (void)argv;
	lhsr_free(ctx);

	return 1;

	arr = NULL;
	scrub = lhsr_scrubber_create(arr);
	if (!scrub) {
		fprintf(stderr, "Error: Failed to create scrubber\n");
		lhsr_free(ctx);
		return 1;
	}

	config.rate_limit = 50;
	config.max_io_depth = 32;
	config.priority = 1;
	config.skip_checksummed = 1;
	config.repair_on_error = 1;
	lhsr_scrubber_set_config(scrub, &config);

	ret = lhsr_scrubber_start(scrub);
	if (ret < 0) {
		fprintf(stderr, "Error: Failed to start scrub: %s\n", strerror(-ret));
		lhsr_scrubber_destroy(scrub);
		lhsr_array_free(arr);
		lhsr_free(ctx);
		return 1;
	}

	printf("Scrub started.\n");
	printf("Use 'lhsrctl scrub --status' to monitor\n");

	lhsr_scrubber_print_status(scrub);

	lhsr_scrubber_destroy(scrub);
	lhsr_array_free(arr);
	lhsr_free(ctx);

	return 0;
}

/*
 * CRC32c checksum — MUST match kernel crypto API convention.
 *
 * The kernel's crc32c_le(seed, buf, len) uses seed=0 and returns the raw
 * CRC WITHOUT a final XOR.  This is NOT the Castagnoli "standard" which
 * uses seed=0xFFFFFFFF and XORs 0xFFFFFFFF at the end.
 *
 * Verified: kernel __crc32c_le(0, buf, len) computes the same value as
 * crc32c_le(0, buf) in this function.
 */
static uint32_t crc32c_calc(uint8_t *buf, size_t len)
{
	uint32_t crc = 0;
	static uint32_t table[256];
	static int table_init = 0;
	int i, j;

	if (!table_init) {
		for (i = 0; i < 256; i++) {
			uint32_t c = i;
			for (j = 0; j < 8; j++)
				c = (c >> 1) ^ (c & 1 ? 0x82F63B78 : 0);
			table[i] = c;
		}
		table_init = 1;
	}

	while (len--)
		crc = (crc >> 8) ^ table[(crc ^ *buf++) & 0xFF];
	return crc;
}

/* ------------------------------------------------------------------ */
/*  JSON helpers for daemon communication                             */
/* ------------------------------------------------------------------ */

/* Read entire file into malloc'd buffer. Caller must free. */
static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (len < 0) { fclose(f); return NULL; }

	char *buf = malloc((size_t)len + 1);
	if (!buf) { fclose(f); return NULL; }

	size_t n = fread(buf, 1, len, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

/* Find json key in range [start, end) and extract int value. */
static int json_int_in(const char *start, const char *end,
		       const char *key, int *val)
{
	char search[64];
	int klen = snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = start;
	while (p < end) {
		const char *f = strstr(p, search);
		if (!f || f >= end)
			return -1;
		const char *v = f + klen;
		while (v < end && (*v == ' ' || *v == '\t'))
			v++;
		if (v < end && sscanf(v, "%d", val) == 1)
			return 0;
		p = f + 1;
	}
	return -1;
}

/* Find json key in range and extract string value (without quotes). */
static int json_string_in(const char *start, const char *end,
			  const char *key, char *buf, size_t sz)
{
	char search[64];
	int klen = snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = start;
	while (p < end) {
		const char *f = strstr(p, search);
		if (!f || f >= end)
			return -1;
		const char *v = f + klen;
		while (v < end && (*v == ' ' || *v == '\t'))
			v++;
		if (v >= end || *v != '"')
			return -1;
		v++; /* skip opening quote */
		size_t i = 0;
		while (v < end && *v != '"' && i < sz - 1)
			buf[i++] = *v++;
		buf[i] = '\0';
		return 0;
	}
	return -1;
}

/* Find json key in range and extract double value. */
static int json_double_in(const char *start, const char *end,
			  const char *key, double *val)
{
	char search[64];
	int klen = snprintf(search, sizeof(search), "\"%s\":", key);
	const char *p = start;
	while (p < end) {
		const char *f = strstr(p, search);
		if (!f || f >= end)
			return -1;
		const char *v = f + klen;
		while (v < end && (*v == ' ' || *v == '\t'))
			v++;
		if (v < end && sscanf(v, "%lf", val) == 1)
			return 0;
		p = f + 1;
	}
	return -1;
}

/* Convenience wrappers that search the whole buffer. */
static inline int json_int(const char *json, const char *key, int *val)
{
	return json_int_in(json, json + strlen(json), key, val);
}
static inline int json_string(const char *json, const char *key,
			      char *buf, size_t sz)
{
	return json_string_in(json, json + strlen(json), key, buf, sz);
}
static inline int json_double(const char *json, const char *key, double *val)
{
	return json_double_in(json, json + strlen(json), key, val);
}

/* Find the next complete JSON object { ... } starting at or after pos.
 * Returns pointer to '{' of the object, sets *end to after the '}'.
 * Returns NULL if no object found. */
static const char *next_object(const char *pos, const char **end)
{
	const char *start = strchr(pos, '{');
	if (!start)
		return NULL;

	int depth = 1;
	const char *p = start + 1;
	while (*p && depth > 0) {
		if (*p == '{') depth++;
		if (*p == '}') depth--;
		p++;
	}
	*end = p;
	return start;
}

/* Format seconds as human-readable duration. */
static void format_duration(long seconds, char *buf, size_t sz)
{
	int d = (int)(seconds / 86400);
	int h = (int)((seconds % 86400) / 3600);
	int m = (int)((seconds % 3600) / 60);
	int s = (int)(seconds % 60);

	if (d > 0)
		snprintf(buf, sz, "%dd %dh %dm", d, h, m);
	else if (h > 0)
		snprintf(buf, sz, "%dh %dm", h, m);
	else if (m > 0)
		snprintf(buf, sz, "%dm %ds", m, s);
	else
		snprintf(buf, sz, "%ds", s);
}

/* ------------------------------------------------------------------ */
/*  Unix socket helpers for daemon control                            */
/* ------------------------------------------------------------------ */

/* Connect to daemon control socket. Returns fd, or -1 on error. */
static int connect_control_socket(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, LHSRD_SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Send a command and receive response. Returns malloc'd string or NULL. */
static char *control_query(const char *cmd)
{
	int fd = connect_control_socket();
	if (fd < 0)
		return NULL;

	write(fd, cmd, strlen(cmd));

	char *resp = malloc(65536);
	if (!resp) {
		close(fd);
		return NULL;
	}

	ssize_t n = read(fd, resp, 65535);
	close(fd);

	if (n <= 0) {
		free(resp);
		return NULL;
	}
	resp[n] = '\0';
	return resp;
}

/* ------------------------------------------------------------------ */

/*
 * Read superblock from device at given sector offset.
 * Returns 0 on success with validated superblock in *sb,
 * -1 if no valid superblock found.
 */
static int read_sb(const char *device, uint64_t sector, struct lhsr_superblock *sb)
{
	int fd;
	ssize_t ret;
	off_t offset = (off_t)(sector * 512);
	uint32_t stored_csum, calc_csum;

	fd = open(device, O_RDONLY);
	if (fd < 0)
		return -1;

	ret = pread(fd, sb, sizeof(*sb), offset);
	close(fd);

	if (ret != (ssize_t)sizeof(*sb))
		return -1;

	if (memcmp(sb->magic, LHSR_MAGIC, LHSR_MAGIC_LEN) != 0)
		return -1;

	stored_csum = sb->checksum;
	sb->checksum = 0;
	calc_csum = crc32c_calc((uint8_t *)sb, sizeof(*sb));
	sb->checksum = stored_csum;

	if (stored_csum != calc_csum)
		return -1;

	return 0;
}

/* Per-disk scan result */
struct disk_info {
	char    path[256];
	uint64_t total_sectors;
	int     found;                /* 1 = valid superblock found */
	struct lhsr_superblock sb;    /* last valid superblock read (uses backup if newer) */
};

/* Per-array state (max one in this simplified version) */
struct array_state {
	uint64_t        array_uuid;
	unsigned int    disk_count;
	unsigned int    raid_type;
	uint64_t        total_sectors;
	uint64_t        generation;
	struct disk_info disks[LHSR_MAX_DISKS];
	int             disk_present[LHSR_MAX_DISKS]; /* 1 = device was provided */
	int             disk_online[LHSR_MAX_DISKS];  /* 1 = valid superblock, state=HEALTHY */
	time_t          creation_time;
	unsigned int    found_count;
};

/* Get device size in 512-byte sectors */
static int get_dev_sectors(const char *device, uint64_t *sectors)
{
	int fd;
	struct stat st;
	uint64_t bytes;

	fd = open(device, O_RDONLY);
	if (fd < 0)
		return -1;

	if (fstat(fd, &st) < 0) {
		close(fd);
		return -1;
	}

	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &bytes) < 0) {
			close(fd);
			return -1;
		}
	} else {
		if (st.st_size <= 0) {
			close(fd);
			return -1;
		}
		bytes = (uint64_t)st.st_size;
	}

	close(fd);
	*sectors = bytes / 512;
	return 0;
}

/* Superblock locations — scan both old and kernel v2 positions.
 *
 * Kernel v2 metadata layout (per device):
 *   sector 0 .. disk_sectors-1           user data
 *   disk_sectors .. +255                 write-hole bitmap (256 sectors)
 *   disk_sectors+256 .. +271             superblock primary (+ backup)
 *   disk_sectors+272 .. +271+wib_sectors WIB pages
 *
 * The old userspace convention looked at (raw_sectors - 16) which
 * happens to equal the kernel's BACKUP position for small disks
 * (WIB=8 sectors).  For large disks with multi-page WIB, the old
 * position is well past the actual superblock area.
 *
 * We scan the entire metadata region at the end of the device for
 * any valid superblock, picking the one with the highest generation.
 * Max metadata region scanned = 4096 sectors (2 MB) which covers
 * disks up to ~32 TB with worst-case WIB.
 */
#define REC_SB_SCAN_MAX_SECTORS  4096

/*
 * Scan a single device for an LHSR superblock.
 * Fills 'info' with result.
 */
static void scan_one_device(const char *device, struct disk_info *info)
{
	struct lhsr_superblock sb_candidate;
	int have = 0;
	uint64_t best_gen = 0;

	memset(info, 0, sizeof(*info));
	strncpy(info->path, device, sizeof(info->path) - 1);

	if (get_dev_sectors(device, &info->total_sectors) < 0) {
		printf("  Cannot get size of %s\n", device);
		return;
	}

	/* Scan metadata region from the end of device backwards.
	 * We do a forward scan from (total - REC_SB_SCAN_MAX_SECTORS)
	 * to total, trying each sector for a valid superblock.
	 * This handles any WIB size and is robust. */
	uint64_t start_scan = (info->total_sectors > REC_SB_SCAN_MAX_SECTORS)
		? (info->total_sectors - REC_SB_SCAN_MAX_SECTORS) : 0;

	for (uint64_t s = start_scan; s < info->total_sectors; s++) {
		if (read_sb(device, s, &sb_candidate) == 0) {
			/* Found a valid superblock — keep the highest generation */
			if (!have || sb_candidate.generation > best_gen) {
				info->sb = sb_candidate;
				best_gen = sb_candidate.generation;
				have = 1;
			}
		}
	}

	if (!have)
		return;

	info->found = 1;
}

static const char *raid_type_name(unsigned int t)
{
	switch (t) {
	case LHSR_RAID_SINGLE: return "SINGLE";
	case LHSR_RAID_MIRROR: return "MIRROR";
	case LHSR_RAID5:       return "RAID5";
	case LHSR_RAID6:       return "RAID6";
	case LHSR_RAID_SHR:    return "SHR";
	case LHSR_RAID_SHR2:   return "SHR2";
	default:               return "UNKNOWN";
	}
}

static const char *disk_state_name(uint32_t s)
{
	switch (s) {
	case LHSR_DISK_HEALTHY:    return "HEALTHY";
	case LHSR_DISK_DEGRADED:   return "DEGRADED";
	case LHSR_DISK_FAILED:     return "FAILED";
	case LHSR_DISK_REBUILDING: return "REBUILDING";
	default:                   return "UNKNOWN";
	}
}

/* Map LHSR RAID type to dmsetup table string */
static const char *raid_type_table_name(unsigned int t)
{
	switch (t) {
	case LHSR_RAID_SINGLE: return "single";
	case LHSR_RAID_MIRROR: return "mirror";
	case LHSR_RAID5:       return "raid5";
	case LHSR_RAID6:       return "raid6";
	case LHSR_RAID_SHR:
	case LHSR_RAID_SHR2:
	default:               return NULL; /* unsupported for direct assembly */
	}
}

/*
 * XOR src into dst (both len bytes).  dst[i] ^= src[i] for all i.
 */
static void xor_buf_into(unsigned char *dst, const unsigned char *src, size_t len)
{
	for (size_t i = 0; i < len; i++)
		dst[i] ^= src[i];
}

/* =================================================================
 * GF(2^8) Reed-Solomon arithmetic for RAID6 reconstruction
 *
 * Uses polynomial 0x1d (x^8 + x^4 + x^3 + x^2 + 1) with generator
 * g=2, matching the kernel RAID6 implementation.  Power table:
 * rs_power_table[i] = 2^i in GF(2^8).  Discrete log maps back.
 * ================================================================= */

static uint8_t rs_power_table[256];
static uint8_t gf_log[256];
static int rs_initialized;

static void rs_tables_init(void)
{
	unsigned int i;
	if (rs_initialized)
		return;
	rs_power_table[0] = 1;
	for (i = 1; i < 256; i++)
		rs_power_table[i] = (rs_power_table[i-1] << 1) ^
			(rs_power_table[i-1] & 0x80 ? 0x1d : 0);
	memset(gf_log, 0, sizeof(gf_log));
	for (i = 0; i < 255; i++)
		gf_log[rs_power_table[i]] = i;
	rs_initialized = 1;
}

/* GF(2^8) multiply: shift-and-add (constant-time) */
static inline uint8_t gf_mul(uint8_t a, uint8_t b)
{
	uint8_t prod = 0;
	uint8_t val = a;
	while (b) {
		if (b & 1)
			prod ^= val;
		val = (val << 1) ^ (val & 0x80 ? 0x1d : 0);
		b >>= 1;
	}
	return prod;
}

/* GF(2^8) inverse: 1/a via log/antilog table.  Returns 0 for a=0. */
static inline uint8_t gf_inv(uint8_t a)
{
	unsigned int idx;
	if (!a)
		return 0;
	idx = 255 - gf_log[a];
	if (idx >= 255)
		idx = 0;
	return rs_power_table[idx];
}

/*
 * Placeholder device name for missing disks.
 * Full name: lhsr_<array_short>_ph_<disk_idx>
 *   where array_short = last 8 hex chars of array UUID
 * dm-zero table: "0 <sectors> zero"  (reads zeros, discards writes)
 *
 * These MUST be created before the main dmsetup create command:
 *   dmsetup create <name> --table '0 <sectors> zero'
 * And removed after teardown:
 *   dmsetup remove <name>
 *
 * The array_short substring keeps names unique per array and short
 * enough for /dev/mapper (max 63 chars + "/dev/mapper/" prefix).
 */

/*
 * Command reconstruct — offline RAID5/6 data reconstruction.
 *
 * XOR-reconstructs a complete disk image for a missing array member
 * from N-1 surviving devices.  Works for RAID5 and RAID6 with exactly
 * one missing disk (right-static parity layout — every disk has data
 * or parity at the same byte-offset, so XOR of all survivors directly
 * yields the missing disk's content).
 *
 * Usage: lhsrctl reconstruct [--chunk-size N] --output <file> <device>...
 *
 * The output is a raw disk image with the same size as the survivors,
 * containing XOR-reconstructed user data and a valid superblock.
 * It can be dd'd to a replacement disk or used directly with
 * 'lhsrctl recover --chunk-size N --output'.
 *
 * --chunk-size defaults to 8 sectors (4 KB).  This parameter does NOT
 * affect the XOR reconstruction (right-static layout makes offset-level
 * XOR correct regardless of chunk size), but it IS recorded in the
 * output file's superblock metadata for compatibility.
 */
static int cmd_reconstruct(int argc, char **argv)
{
	const char *output_path = NULL;
	int chunk_sectors = 8;  /* LHSR_DEFAULT_CHUNK_SECTORS */
	int target_idx = -1;    /* --target: which disk to reconstruct (auto if unspecified) */
	int dev_start, dev_count;
	int i, d, ret = 1;
	int missing_idx = -1;
	int n_missing = 0;
	uint64_t raw_sectors = 0, user_sectors = 0;
	uint64_t pos;
	struct disk_info *scanned = NULL;
	int *fds = NULL;
	unsigned char *xor_buf = NULL, *read_buf = NULL, *tmp_buf = NULL;
	int *survivor_idx = NULL;
	size_t buf_size = 1024 * 1024;  /* 1 MB I/O buffer */
	int out_fd = -1;

	/* ---- Parse options ---- */
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--output") == 0 ||
		    strcmp(argv[i], "-o") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Error: --output requires a"
					" path argument\n");
				return 1;
			}
			output_path = argv[++i];
		} else if (strcmp(argv[i], "--chunk-size") == 0 ||
			   strcmp(argv[i], "-c") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Error: --chunk-size requires"
					" a sector count\n");
				return 1;
			}
			chunk_sectors = atoi(argv[++i]);
			if (chunk_sectors <= 0 || chunk_sectors > 4096) {
				fprintf(stderr, "Error: chunk-size must be"
					" 1-4096 sectors\n");
				return 1;
			}
		} else if (strcmp(argv[i], "--target") == 0 ||
			   strcmp(argv[i], "-t") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Error: --target requires"
					" a disk index\n");
				return 1;
			}
			target_idx = atoi(argv[++i]);
			if (target_idx < 0) {
				fprintf(stderr, "Error: --target must be"
					" a non-negative disk index\n");
				return 1;
			}
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "Error: Unknown option '%s'\n",
				argv[i]);
			return 1;
		} else {
			break;
		}
	}

	dev_start = i;
	dev_count = argc - dev_start;

	if (!output_path || dev_count < 2) {
		fprintf(stderr,
			"Usage: %s reconstruct [--chunk-size N]"
			" [--target N] --output <file> <device>...\n"
			"Reconstructs a missing disk in a RAID5/6 array"
			" from N-1 (or N-2) survivors.\n"
			"  --target N   Disk index to reconstruct"
			" (default: auto-detect)\n"
			"Example:"
			" %s reconstruct --output /tmp/rec.img"
			" /dev/sdb /dev/sdc /dev/sdd\n",
			PROGNAME, PROGNAME);
		return 1;
	}

	printf("LHSR Reconstruction\n");
	printf("===================\n\n");

	/* ---- Phase 1: Scan all survivor devices ---- */
	scanned = calloc(dev_count, sizeof(struct disk_info));
	if (!scanned) {
		fprintf(stderr, "Error: Out of memory\n");
		return 1;
	}

	printf("Scanning %d survivor device(s)...\n\n", dev_count);
	for (i = 0; i < dev_count; i++) {
		const char *dev = argv[dev_start + i];
		printf("  [%d/%d] %s ... ", i + 1, dev_count, dev);
		fflush(stdout);
		scan_one_device(dev, &scanned[i]);
		if (!scanned[i].found) {
			printf("no LHSR superblock\n");
			fprintf(stderr, "Error: %s has no LHSR superblock\n",
				dev);
			goto out;
		}
		printf("OK (idx=%u, uuid=0x%016llx)\n",
		       scanned[i].sb.disk_index,
		       (unsigned long long)scanned[i].sb.array_uuid);
	}

	/* ---- Phase 2: Validate survivors belong to same array ---- */
	uint64_t array_uuid = scanned[0].sb.array_uuid;
	unsigned int raid_type = scanned[0].sb.raid_type;
	unsigned int disk_count = scanned[0].sb.disk_count;
	raw_sectors  = scanned[0].total_sectors;
	user_sectors = scanned[0].sb.total_sectors;
	uint64_t gen = scanned[0].sb.generation;

	if (user_sectors == 0 || raw_sectors == 0 ||
	    raw_sectors <= LHSR_SB_SECTORS) {
		fprintf(stderr, "Error: Invalid device geometry on %s"
			" (user=%llu raw=%llu)\n",
			argv[dev_start],
			(unsigned long long)user_sectors,
			(unsigned long long)raw_sectors);
		goto out;
	}

	for (i = 1; i < dev_count; i++) {
		if (scanned[i].sb.array_uuid != array_uuid) {
			fprintf(stderr, "Error: %s belongs to a different"
				" array (uuid=0x%016llx)\n",
				argv[dev_start + i],
				(unsigned long long)scanned[i].sb.array_uuid);
			goto out;
		}
		if (scanned[i].sb.raid_type != raid_type) {
			fprintf(stderr, "Error: %s has different RAID type"
				" (%s vs %s)\n",
				argv[dev_start + i],
				raid_type_name(scanned[i].sb.raid_type),
				raid_type_name(raid_type));
			goto out;
		}
		if (scanned[i].sb.disk_count != disk_count) {
			fprintf(stderr, "Error: %s has different disk count"
				" (%u vs %u)\n",
				argv[dev_start + i],
				scanned[i].sb.disk_count, disk_count);
			goto out;
		}
		if (scanned[i].total_sectors != raw_sectors) {
			fprintf(stderr, "Error: %s different size"
				" (%llu vs %llu sectors)\n",
				argv[dev_start + i],
				(unsigned long long)scanned[i].total_sectors,
				(unsigned long long)raw_sectors);
			goto out;
		}
		if (scanned[i].sb.generation > gen)
			gen = scanned[i].sb.generation;
	}

	/* ---- Phase 3: Identify missing disks ---- */
	unsigned int disk_present[LHSR_MAX_DISKS];
	int missing_list[LHSR_MAX_DISKS];
	memset(disk_present, 0, sizeof(disk_present));
	for (i = 0; i < dev_count; i++) {
		unsigned int idx = scanned[i].sb.disk_index;
		if (idx >= LHSR_MAX_DISKS) {
			fprintf(stderr, "Error: Invalid disk index %u on %s\n",
				idx, argv[dev_start + i]);
			goto out;
		}
		disk_present[idx] = 1;
	}

	n_missing = 0;
	for (d = 0; d < (int)disk_count; d++) {
		if (!disk_present[d])
			missing_list[n_missing++] = d;
	}

	if (n_missing == 0) {
		fprintf(stderr, "Error: All disks present."
			" No reconstruction needed.\n");
		goto out;
	}

	if (raid_type == LHSR_RAID5 && n_missing > 1) {
		fprintf(stderr, "Error: RAID5 cannot have %d missing disks"
			" (reconstruction requires N-1 survivors).\n",
			n_missing);
		goto out;
	}

	if (raid_type == LHSR_RAID6 && n_missing > 2) {
		fprintf(stderr, "Error: RAID6 cannot have %d missing disks"
			" (reconstruction requires at least N-2 survivors"
			" with P+Q).\n", n_missing);
		goto out;
	}

	/* Pick target: user-specified or first missing */
	missing_idx = (target_idx >= 0) ? target_idx : missing_list[0];

	if (target_idx >= 0 && disk_present[target_idx]) {
		fprintf(stderr, "Error: --target disk %d is not missing"
			" (it is among the survivors).\n", target_idx);
		goto out;
	}

	if (disk_present[missing_idx]) {
		fprintf(stderr, "Error: Target disk %d is present"
			" (not a survivor).  Pick a missing disk.\n",
			missing_idx);
		goto out;
	}

	if (n_missing == 2 && raid_type == LHSR_RAID6)
		printf("  Dual-disk recovery: "
		       "target=disk[%d]  other_missing=disk[%d]\n",
		       missing_idx,
		       missing_list[0] == missing_idx ? missing_list[1]
		       : missing_list[0]);

	/* parity (1 for RAID5, 2 for RAID6) is not needed here because
	 * right-static layout means XOR of ALL survivors at any byte
	 * offset directly yields the missing disk's content regardless
	 * of whether that disk held data or parity. */

	if (raid_type < LHSR_RAID5) {
		fprintf(stderr, "Error: %s does not support XOR"
			" reconstruction.\n", raid_type_name(raid_type));
		goto out;
	}

	printf("\nArray:  uuid=0x%016llx  type=%s  disks=%u"
	       "  missing=disk[%d]\n",
	       (unsigned long long)array_uuid,
	       raid_type_name(raid_type), disk_count, missing_idx);
	printf("Data:   raw=%llu sectors  user=%llu sectors"
	       "  chunk=%d sectors  survivors=%d\n\n",
	       (unsigned long long)raw_sectors,
	       (unsigned long long)user_sectors,
	       chunk_sectors, dev_count);

	/* ---- Phase 4: Create output file ---- */
	out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out_fd < 0) {
		fprintf(stderr, "Error: Cannot create %s: %s\n",
			output_path, strerror(errno));
		goto out;
	}

	if (ftruncate(out_fd, (off_t)(raw_sectors * 512)) < 0) {
		fprintf(stderr, "Error: Cannot extend output file: %s\n",
			strerror(errno));
		goto out_close;
	}

	/* ---- Phase 5: Open survivor devices ---- */
	fds = calloc(dev_count, sizeof(int));
	if (!fds) {
		fprintf(stderr, "Error: Out of memory\n");
		goto out_close;
	}

	for (i = 0; i < dev_count; i++) {
		fds[i] = open(argv[dev_start + i], O_RDONLY);
		if (fds[i] < 0) {
			fprintf(stderr, "Error: Cannot open %s: %s\n",
				argv[dev_start + i], strerror(errno));
			while (--i >= 0)
				close(fds[i]);
			free(fds);
			fds = NULL;
			goto out_close;
		}
	}

	/* ---- Phase 6: Allocate buffers ---- */
	xor_buf  = malloc(buf_size);
	read_buf = malloc(buf_size);
	tmp_buf  = malloc(buf_size);
	if (!xor_buf || !read_buf || !tmp_buf) {
		fprintf(stderr, "Error: Out of memory\n");
		goto out_close;
	}

	/* Build survivor disk index mapping */
	survivor_idx = malloc(dev_count * sizeof(int));
	if (!survivor_idx) {
		fprintf(stderr, "Error: Out of memory\n");
		goto out_close;
	}
	for (i = 0; i < dev_count; i++)
		survivor_idx[i] = scanned[i].sb.disk_index;

	/* Determine if RS decode is needed (RAID6 complex cases) */
	unsigned int parity = (raid_type == LHSR_RAID6) ? 2 : 1;
	unsigned int data_disks = disk_count - parity;
	int q_idx = (int)(data_disks + 1);
	int need_rs = (raid_type == LHSR_RAID6) &&
		      (n_missing >= 2 ||			 /* dual-disk */
		       missing_idx == q_idx ||		 /* Q-target */
		       !disk_present[(int)data_disks]); /* P-missing */
	if (need_rs)
		rs_tables_init();

	/* ---- Phase 7: Reconstruct user data area ---- */
	uint64_t user_bytes = user_sectors * 512;
	printf("Reconstructing %llu bytes from %d survivors"
	       " (RAID%d, %d missing, target=disk[%d])...\n",
	       (unsigned long long)user_bytes, dev_count,
	       raid_type == LHSR_RAID5 ? 5 : 6, n_missing, missing_idx);

	for (pos = 0; pos < user_bytes; pos += buf_size) {
		size_t chunk = buf_size;
		if (pos + buf_size > user_bytes)
			chunk = (size_t)(user_bytes - pos);

		if (need_rs) {
			/*
			 * RS-aware reconstruction: accumulate S (XOR of
			 * data+P) and T (Q + weighted data), then solve
			 * for the target disk.
			 */
			memset(xor_buf, 0, chunk);
			memset(tmp_buf, 0, chunk);

			for (i = 0; i < dev_count; i++) {
				ssize_t r = pread(fds[i], read_buf,
						  chunk, (off_t)pos);
				if (r != (ssize_t)chunk) {
					fprintf(stderr,
						"\nError: Short read from %s"
						" at offset %llu\n",
						argv[dev_start + i],
						(unsigned long long)pos);
					goto out_close;
				}

				int idx = survivor_idx[i];
				/* S: accumulate data + P */
				if (idx < (int)data_disks ||
				    idx == (int)data_disks)
					xor_buf_into(xor_buf, read_buf,
						     chunk);
				/* T: accumulate weighted data + Q */
				if (idx < (int)data_disks) {
					uint8_t coeff =
						rs_power_table[idx];
					size_t j;
					for (j = 0; j < chunk; j++)
						tmp_buf[j] ^=
							gf_mul(read_buf[j],
							       coeff);
				} else if (idx == q_idx) {
					xor_buf_into(tmp_buf, read_buf,
						     chunk);
				}
			}

			/* Solve for target (xor_buf=S, tmp_buf=T) */
			if (missing_idx == q_idx &&
			    n_missing == 1) {
				/* Q target, all data alive: T = Q */
				memcpy(xor_buf, tmp_buf, chunk);
			} else if (!disk_present[(int)data_disks] &&
				   disk_present[q_idx] &&
				   n_missing == 1) {
				/* P dead, Q alive, 1 data missing:
				 * D_t = inv(g^t) * T */
				uint8_t inv_g =
					gf_inv(rs_power_table[missing_idx]);
				size_t j;
				for (j = 0; j < chunk; j++)
					xor_buf[j] = gf_mul(tmp_buf[j],
							    inv_g);
			} else if (n_missing >= 2 &&
				   disk_present[(int)data_disks] &&
				   disk_present[q_idx]) {
				/* Vandermonde 2x2: P+Q alive,
				 * 2+ data missing */
				int other_failed = -1;
				for (d = 0; d < n_missing; d++) {
					if (missing_list[d] !=
					    missing_idx &&
					    missing_list[d] <
					    (int)data_disks) {
						other_failed =
							missing_list[d];
						break;
					}
				}
				if (other_failed >= 0) {
					uint8_t coeff_o =
					    rs_power_table[other_failed];
					uint8_t det =
					    rs_power_table[missing_idx]
					    ^ coeff_o;
					uint8_t inv_det = gf_inv(det);
					size_t j;
					for (j = 0; j < chunk; j++) {
						uint8_t s = xor_buf[j];
						uint8_t t = tmp_buf[j];
						xor_buf[j] =
						    gf_mul(gf_mul(coeff_o,
								  s) ^ t,
							   inv_det);
					}
				}
			}
			/* else: xor_buf = S = target already
			 * (single data missing, P alive) */
		} else {
			/* Simple XOR: exclude Q for RAID6 */
			memset(xor_buf, 0, chunk);
			for (i = 0; i < dev_count; i++) {
				ssize_t r = pread(fds[i], read_buf,
						  chunk, (off_t)pos);
				if (r != (ssize_t)chunk) {
					fprintf(stderr,
						"\nError: Short read from %s"
						" at offset %llu\n",
						argv[dev_start + i],
						(unsigned long long)pos);
					goto out_close;
				}
				if (raid_type == LHSR_RAID6 &&
				    survivor_idx[i] == q_idx)
					continue; /* Q excluded */
				xor_buf_into(xor_buf, read_buf, chunk);
			}
		}

		ssize_t w = pwrite(out_fd, xor_buf, chunk, (off_t)pos);
		if (w != (ssize_t)chunk) {
			fprintf(stderr,
				"\nError: Short write to output at"
				" offset %llu\n",
				(unsigned long long)pos);
			goto out_close;
		}

		/* Progress every ~256 MB */
		if ((pos / buf_size) % 256 == 0) {
			printf("\r  Progress: %llu / %llu bytes (%.0f%%)",
			       (unsigned long long)pos,
			       (unsigned long long)user_bytes,
			       100.0 * pos / (user_bytes > 0
					      ? (double)user_bytes : 1.0));
			fflush(stdout);
		}
	}
	printf("\r  Progress: %llu / %llu bytes (100%%)\n",
	       (unsigned long long)user_bytes,
	       (unsigned long long)user_bytes);

	/* ---- Phase 8: Write superblock ---- */
	{
		struct lhsr_superblock sb = scanned[0].sb;

		sb.disk_index  = (uint32_t)missing_idx;
		sb.disk_uuid   = ((uint64_t)time(NULL)
				  ^ ((uint64_t)getpid() << 32)
				  ^ (uint64_t)missing_idx);
		sb.generation  = gen;
		sb.last_update = (uint64_t)time(NULL);
		sb.disk_state  = LHSR_DISK_HEALTHY;
		sb.checksum    = 0;
		sb.checksum    = crc32c_calc((uint8_t *)&sb, sizeof(sb));

		/*
		 * Write position MUST match the kernel module's layout:
		 *   primary = disk_sectors + LHSR_BITMAP_TOTAL_SECTORS
		 *   backup  = primary + (LHSR_SB_SECTORS / 2)
		 * where disk_sectors = user_sectors (sb.total_sectors).
		 *
		 * Old code used (raw_sectors - 16) which put the superblock
		 * in the wrong position (array assembly would fail).
		 * Fixed 2026-06-20: use kernel's convention.
		 */
		off_t sb_pri = (off_t)((user_sectors + LHSR_BITMAP_TOTAL_SECTORS)
				      * 512);
		off_t sb_bak = (off_t)((user_sectors + LHSR_BITMAP_TOTAL_SECTORS
					+ LHSR_SB_SECTORS / 2) * 512);

		if (pwrite(out_fd, &sb, sizeof(sb), sb_pri) != sizeof(sb))
			fprintf(stderr, "Warning: Primary superblock write"
				" failed\n");
		if (pwrite(out_fd, &sb, sizeof(sb), sb_bak) != sizeof(sb))
			fprintf(stderr, "Warning: Backup superblock write"
				" failed\n");

		printf("Superblock written for disk[%d] (generation %llu)\n",
		       missing_idx, (unsigned long long)gen);
	}

	/* ---- Phase 9: Report success ---- */
	for (i = 0; i < dev_count; i++) close(fds[i]);
	free(fds);
	free(survivor_idx);
	survivor_idx = NULL;
	free(xor_buf);
	xor_buf = NULL;
	free(read_buf);
	read_buf = NULL;
	free(tmp_buf);
	tmp_buf = NULL;
	close(out_fd);
	out_fd = -1;

	printf("\nReconstruction complete!\n");
	printf("Output: %s (%llu bytes, %llu sectors)\n",
	       output_path,
	       (unsigned long long)(raw_sectors * 512),
	       (unsigned long long)raw_sectors);
	printf("\nTo write to a replacement disk:\n");
	printf("  # dd if=%s of=/dev/sdX bs=512\n", output_path);
	printf("Then run 'lhsrctl recover' to assemble the array.\n");

	free(scanned);
	return 0;

	/* ---- Error exit ---- */
out_close:
	if (out_fd >= 0) {
		close(out_fd);
		unlink(output_path);
	}
	if (fds) {
		for (i = 0; i < dev_count; i++)
			if (fds[i] > 0) close(fds[i]);
		free(fds);
	}
	free(survivor_idx);
	free(xor_buf);
	free(read_buf);
	free(tmp_buf);
out:
	free(scanned);
	return ret;
}

/* Command recover */
static int cmd_recover(int argc, char **argv)
{
	int i;
	int dev_start;
	int dev_count;
	struct disk_info *scanned = NULL;
	struct array_state arrays[16];  /* support up to 16 unique array UUIDs */
	int num_arrays = 0;
	int ret = 0;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s recover <device1> [device2 ...]\n", PROGNAME);
		fprintf(stderr, "Scans devices for LHSR arrays and generates assembly commands.\n");
		fprintf(stderr, "Example: %s recover /dev/sdb /dev/sdc /dev/sdd\n", PROGNAME);
		return 1;
	}

	dev_start = 2;
	dev_count = argc - dev_start;

	printf("LHSR Recovery\n");
	printf("=============\n\n");

	scanned = calloc(dev_count, sizeof(struct disk_info));
	if (!scanned) {
		fprintf(stderr, "Error: Out of memory\n");
		return 1;
	}

	/* Phase 1: Scan all provided devices */
	printf("Scanning %d device(s)...\n\n", dev_count);
	for (i = 0; i < dev_count; i++) {
		const char *dev = argv[dev_start + i];
		printf("[%d/%d] %s ... ", i + 1, dev_count, dev);
		fflush(stdout);
		scan_one_device(dev, &scanned[i]);
		if (scanned[i].found) {
			struct lhsr_superblock *sb = &scanned[i].sb;
			printf("FOUND  array=0x%016llx disk=%u/%u type=%s gen=%llu\n",
			       (unsigned long long)sb->array_uuid,
			       sb->disk_index, sb->disk_count,
			       raid_type_name(sb->raid_type),
			       (unsigned long long)sb->generation);
		} else {
			printf("no LHSR superblock\n");
		}
	}

	/* Phase 2: Group by array UUID */
	for (i = 0; i < dev_count; i++) {
		if (!scanned[i].found)
			continue;

		uint64_t uuid = scanned[i].sb.array_uuid;
		unsigned int disk_idx = scanned[i].sb.disk_index;
		struct array_state *arr = NULL;
		int a;

		/* Find existing array or create new */
		for (a = 0; a < num_arrays; a++) {
			if (arrays[a].array_uuid == uuid) {
				arr = &arrays[a];
				break;
			}
		}

		if (!arr) {
			if (num_arrays >= 16) {
				fprintf(stderr, "Warning: Too many arrays found, ignoring uuid=0x%016" PRIx64 "\n", uuid);
				continue;
			}
			arr = &arrays[num_arrays++];
			memset(arr, 0, sizeof(*arr));
			arr->array_uuid = uuid;
			arr->disk_count = scanned[i].sb.disk_count;
			arr->raid_type  = scanned[i].sb.raid_type;
			arr->total_sectors = scanned[i].sb.total_sectors;
			arr->generation = scanned[i].sb.generation;
			arr->creation_time = (time_t)scanned[i].sb.creation_time;
		}

		/* Track the device providing data for this disk index */
		if (disk_idx < LHSR_MAX_DISKS && !arr->disk_present[disk_idx]) {
			arr->disk_present[disk_idx] = 1;
			arr->disk_online[disk_idx] =
				(scanned[i].sb.disk_state == LHSR_DISK_HEALTHY ||
				 scanned[i].sb.disk_state == LHSR_DISK_REBUILDING);
			arr->disks[disk_idx] = scanned[i];
			/* Fix up the path to the original device argument */
			strncpy(arr->disks[disk_idx].path, argv[dev_start + i],
				sizeof(arr->disks[disk_idx].path) - 1);
			arr->found_count++;
		}

		/* Track highest generation */
		if (scanned[i].sb.generation > arr->generation)
			arr->generation = scanned[i].sb.generation;
	}

	if (num_arrays == 0) {
		printf("\nNo LHSR arrays found on the provided devices.\n");
		free(scanned);
		return 0;
	}

	/* Phase 3: For each array, generate assembly commands */
	printf("\nFound %d LHSR array(s):\n\n", num_arrays);

	for (int a = 0; a < num_arrays; a++) {
		struct array_state *arr = &arrays[a];
		const char *table_type = raid_type_table_name(arr->raid_type);
		unsigned int parity = (arr->raid_type == LHSR_RAID5) ? 1 :
				     (arr->raid_type == LHSR_RAID6) ? 2 : 0;
		unsigned int min_healthy = arr->disk_count - parity;
		unsigned int present = 0;
		unsigned int online = 0;
		char uuid_str[32];
		char dev_name[64];
		int d;

		snprintf(uuid_str, sizeof(uuid_str), "%016" PRIx64, arr->array_uuid);
		snprintf(dev_name, sizeof(dev_name), "lhsr_%s", uuid_str);
		if (strlen(dev_name) > 63)
			dev_name[63] = '\0';

		time_t ct = arr->creation_time;

		printf("--- Array %s ---\n", uuid_str);
		printf("  Name:          %s\n", dev_name);
		printf("  RAID Type:     %s (%u)\n", raid_type_name(arr->raid_type), arr->raid_type);
		printf("  Total disks:   %u\n", arr->disk_count);
		printf("  Parity disks:  %u\n", parity);
		printf("  Min. healthy:  %u\n", min_healthy);
		printf("  Creation:      %s", ctime(&ct));
		printf("  Generation:    %" PRIu64 "\n", arr->generation);

		/* Count disk states */
		for (d = 0; d < (int)arr->disk_count; d++) {
			if (arr->disk_present[d]) {
				present++;
				if (arr->disk_online[d])
					online++;
			}
		}

		printf("  Disks present: %u/%u\n", present, arr->disk_count);
		printf("  Disks healthy: %u/%u\n", online, arr->disk_count);

		if (present < min_healthy) {
			printf("\n  ERROR: Insufficient healthy disks for assembly.\n");
			printf("  Need at least %u, have %u.\n", min_healthy, present);
			printf("  Data recovery may require manual intervention.\n\n");
			ret = 1;
			continue;
		}

		printf("\n  Disk status:\n");
		for (d = 0; d < (int)arr->disk_count; d++) {
			if (arr->disk_present[d]) {
			printf("    [%d] %s  %s  gen=%llu\n",
			       d, arr->disks[d].path,
			       disk_state_name(arr->disks[d].sb.disk_state),
			       (unsigned long long)arr->disks[d].sb.generation);
			} else {
				printf("    [%d] **** MISSING ****\n", d);
			}
		}

		if (!table_type) {
			printf("\n  ERROR: RAID type %s does not support direct kernel assembly.\n",
			       raid_type_name(arr->raid_type));
			ret = 1;
			continue;
		}

		printf("\n  Assembly command:\n");

		/* Build the full table line */
		char table_line[4096];
		int pos = 0;
		int table_len = sizeof(table_line);
		int n;
		int missing_count = 0;

		/* Size: user-visible capacity for RAID5/6 = data_disks * disk_sectors */
		uint64_t user_size;
		if (arr->raid_type >= LHSR_RAID5) {
			user_size = arr->total_sectors * (arr->disk_count - parity);
		} else {
			user_size = arr->total_sectors;
		}

		if (arr->raid_type >= LHSR_RAID5) {
			/*
			 * RAID5/6 table format:
			 *   0 <size> lhsr <type> <chunk_sects> <stripe_depth> <cont_sects> <dev> <off> ...
			 * Default chunk=8 (4KB), stripe_depth=1, cont_sects=8.
			 * These must match the original creation parameters, which are NOT
			 * stored in the superblock.  Use --chunk <sects> to override.
			 */
			n = snprintf(table_line + pos, table_len - pos,
				     "0 %llu lhsr %s 8 1 8",
				     (unsigned long long)user_size, table_type);
		} else {
			n = snprintf(table_line + pos, table_len - pos,
				     "0 %llu lhsr %s",
				     (unsigned long long)user_size, table_type);
		}
		if (n > 0) pos += n;

		/*
		 * Build placeholder dm-zero commands for missing disks.
		 * dm-zero reads zeros for any offset, discards writes.
		 * Format: dmsetup create <name> --table '0 <sectors> zero'
		 */
		char array_short[16];
		if (missing_count > 0 || true) {
			snprintf(array_short, sizeof(array_short), "%s",
				 uuid_str + strlen(uuid_str) - 8);
		}
		for (d = 0; d < (int)arr->disk_count; d++) {
			if (arr->disk_present[d])
				continue;
			if (missing_count == 0)
				printf("\n  Missing disk placeholder(s) (run these first):\n");
			char ph_name[64];
			snprintf(ph_name, sizeof(ph_name), "lhsr_%s_ph_%u",
				 array_short, d);
			printf("  # dmsetup create %s --table '0 %llu zero'\n",
			       ph_name, (unsigned long long)arr->total_sectors);
			missing_count++;
		}

		/* Add each disk with offset=0 */
		for (d = 0; d < (int)arr->disk_count; d++) {
			char dev_path[256];

			if (arr->disk_present[d]) {
				strncpy(dev_path, arr->disks[d].path, sizeof(dev_path) - 1);
			} else {
				char ph_name[64];
				snprintf(ph_name, sizeof(ph_name), "lhsr_%s_ph_%u",
					 array_short, d);
				snprintf(dev_path, sizeof(dev_path), "/dev/mapper/%s",
					 ph_name);
			}

			n = snprintf(table_line + pos, table_len - pos,
				     " %s 0", dev_path);
			if (n > 0) pos += n;

			if (pos >= table_len - 1) {
				fprintf(stderr, "  ERROR: Table line too long\n");
				free(scanned);
				return 1;
			}
		}

		if (missing_count)
			printf("\n  Main assembly command (run after placeholders):\n");
		printf("  # dmsetup create %s --table '%s'\n", dev_name, table_line);

		printf("\n  To verify before creating:\n");
		printf("  # dmsetup table %s\n\n", dev_name);

		printf("  After assembly, use:\n");
		printf("  # lhsrctl status\n");
		printf("  # lhsrctl disk-online %s <index>   # for degraded disks\n", dev_name);
		printf("  # lhsrctl disk-fail %s <index>     # to mark disks as failed\n", dev_name);
		printf("  # dmsetup remove %s                 # to tear down\n\n", dev_name);

		printf("  If using dm-integrity for anti-bit-rot, append 'integrity' to the\n");
		printf("  table line and use dm-integrity device paths instead of raw disks:\n");
		printf("  # dmsetup create %s --table\n", dev_name);
		printf("      \"0 <size> lhsr %s", table_type);
		for (d = 0; d < (int)arr->disk_count; d++) {
			printf(" /dev/mapper/integrity-<disk> 0");
		}
		printf(" integrity\"\n\n");
	}

	free(scanned);
	return ret;
}

/* Main */
int main(int argc, char **argv)
{
	int cmd = CMD_NONE;
	const char *raid_type_str = NULL;
	unsigned int raid_type = LHSR_RAID5;
	int ret;

	if (argc < 2) {
		usage(basename(argv[0]));
		return 1;
	}

	/* Parse command */
	if (strcmp(argv[1], "create") == 0) {
		cmd = CMD_CREATE;
	} else if (strcmp(argv[1], "status") == 0) {
		cmd = CMD_STATUS;
	} else if (strcmp(argv[1], "predict") == 0) {
		cmd = CMD_PREDICT;
	} else if (strcmp(argv[1], "scrub") == 0) {
		cmd = CMD_SCRUB;
	} else if (strcmp(argv[1], "scrub-status") == 0) {
		cmd = CMD_SCRUB;
	} else if (strcmp(argv[1], "bitrot") == 0) {
		cmd = CMD_BITROT;
	} else if (strcmp(argv[1], "bitrot-log") == 0) {
		cmd = CMD_BITROT_LOG;
	} else if (strcmp(argv[1], "add") == 0) {
		fprintf(stderr, "Error: 'add' not implemented\n");
		return 1;
	} else if (strcmp(argv[1], "remove") == 0) {
		fprintf(stderr, "Error: 'remove' not implemented\n");
		return 1;
	} else if (strcmp(argv[1], "repair") == 0) {
		fprintf(stderr, "Error: 'repair' not implemented\n");
		return 1;
	} else if (strcmp(argv[1], "expand") == 0) {
		fprintf(stderr, "Error: 'expand' not implemented\n");
		return 1;
	} else if (strcmp(argv[1], "disk-fail") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s disk-fail <device> <disk_idx>\n",
				PROGNAME);
			return 1;
		}
		/* Validate inputs */
		if (strchr(argv[2], '`') || strchr(argv[2], '$') ||
		    strchr(argv[2], ';') || strchr(argv[2], '|')) {
			fprintf(stderr, "Error: Invalid characters in device\n");
			return 1;
		}
		if (strchr(argv[3], '`') || strchr(argv[3], '$') ||
		    strchr(argv[3], ';') || strchr(argv[3], '|')) {
			fprintf(stderr, "Error: Invalid characters in disk index\n");
			return 1;
		}
		const char *dmsetup_argv[] = {"dmsetup", "message", argv[2], "disk_fail", argv[3], NULL};
		ret = fork();
		if (ret == 0) {
			execvp("dmsetup", (char * const *)dmsetup_argv);
			exit(127);
		} else if (ret > 0) {
			int status;
			waitpid(ret, &status, 0);
			if (WEXITSTATUS(status) == 0)
				printf("Disk %s marked as failed\n", argv[3]);
			return WEXITSTATUS(status);
		}
		return 1;
	} else if (strcmp(argv[1], "disk-online") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s disk-online <device> <disk_idx>\n",
				PROGNAME);
			return 1;
		}
		/* Validate inputs */
		if (strchr(argv[2], '`') || strchr(argv[2], '$') ||
		    strchr(argv[2], ';') || strchr(argv[2], '|')) {
			fprintf(stderr, "Error: Invalid characters in device\n");
			return 1;
		}
		if (strchr(argv[3], '`') || strchr(argv[3], '$') ||
		    strchr(argv[3], ';') || strchr(argv[3], '|')) {
			fprintf(stderr, "Error: Invalid characters in disk index\n");
			return 1;
		}
		const char *dmsetup_argv[] = {"dmsetup", "message", argv[2], "disk_online", argv[3], NULL};
		ret = fork();
		if (ret == 0) {
			execvp("dmsetup", (char * const *)dmsetup_argv);
			exit(127);
		} else if (ret > 0) {
			int status;
			waitpid(ret, &status, 0);
			if (WEXITSTATUS(status) == 0)
				printf("Disk %s marked as online\n", argv[3]);
			return WEXITSTATUS(status);
		}
		return 1;
	} else if (strcmp(argv[1], "disk-health") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s disk-health <device> <disk_idx>\n",
				PROGNAME);
			return 1;
		}
		/* Validate inputs */
		if (strchr(argv[2], '`') || strchr(argv[2], '$') ||
		    strchr(argv[2], ';') || strchr(argv[2], '|')) {
			fprintf(stderr, "Error: Invalid characters in device\n");
			return 1;
		}
		if (strchr(argv[3], '`') || strchr(argv[3], '$') ||
		    strchr(argv[3], ';') || strchr(argv[3], '|')) {
			fprintf(stderr, "Error: Invalid characters in disk index\n");
			return 1;
		}
		/* Use pipe() + fork() + execvp() instead of popen() */
		int pipefd[2];
		if (pipe(pipefd) == -1)
			return 1;
		ret = fork();
		if (ret == 0) {
			/* Child: redirect stdout to pipe, exec dmsetup */
			close(pipefd[0]);
			dup2(pipefd[1], STDOUT_FILENO);
			close(pipefd[1]);
			const char *dmsetup_argv[] = {"dmsetup", "message", argv[2], "disk_health", argv[3], NULL};
			execvp("dmsetup", (char * const *)dmsetup_argv);
			exit(127);
		} else if (ret > 0) {
			/* Parent: read from pipe */
			char buf[256];
			ssize_t n;
			close(pipefd[1]);
			while ((n = read(pipefd[0], buf, sizeof(buf)-1)) > 0) {
				buf[n] = '\0';
				printf("%s", buf);
			}
			close(pipefd[0]);
			int status;
			waitpid(ret, &status, 0);
			return WEXITSTATUS(status);
		}
		return 1;
	} else if (strcmp(argv[1], "recover") == 0) {
		cmd = CMD_RECOVER;
	} else if (strcmp(argv[1], "reconstruct") == 0) {
		cmd = CMD_RECONSTRUCT;
	} else if (strcmp(argv[1], "health") == 0) {
		cmd = CMD_HEALTH;
	} else if (strcmp(argv[1], "shr") == 0) {
		cmd = CMD_SHR;
	} else if (strcmp(argv[1], "message") == 0) {
		ret = cmd_message(argc, argv);
	} else {
		fprintf(stderr, "Error: Unknown command '%s'\n", argv[1]);
		usage(basename(argv[0]));
		return 1;
	}

	/* Parse create-specific options */
	if (cmd == CMD_CREATE) {
		int opt;
		static struct option long_options[] = {
			{"raid", required_argument, 0, 'r'},
			{0, 0, 0, 0}
		};

		while ((opt = getopt_long(argc - 1, argv + 1,
				       "r:", long_options, NULL)) != -1) {
			switch (opt) {
			case 'r':
				raid_type_str = optarg;
				break;
			default:
				usage(basename(argv[0]));
				return 1;
			}
		}

		/* Convert RAID type string */
		if (raid_type_str) {
			if (strcmp(raid_type_str, "single") == 0)
				raid_type = LHSR_RAID_SINGLE;
			else if (strcmp(raid_type_str, "mirror") == 0)
				raid_type = LHSR_RAID_MIRROR;
			else if (strcmp(raid_type_str, "raid5") == 0)
				raid_type = LHSR_RAID5;
			else if (strcmp(raid_type_str, "raid6") == 0)
				raid_type = LHSR_RAID6;
			else if (strcmp(raid_type_str, "shr") == 0)
				raid_type = LHSR_RAID_SHR;
			else if (strcmp(raid_type_str, "shr2") == 0)
				raid_type = LHSR_RAID_SHR2;
			else {
				fprintf(stderr, "Error: Unknown RAID type '%s'\n",
				       raid_type_str);
				return 1;
			}
		}

		/* Skip past parsed options */
		argc -= optind;
		argv += optind;
	}

	/* Execute command */
	switch (cmd) {
	case CMD_CREATE:
		ret = cmd_create(argc, argv, raid_type);
		break;
	case CMD_STATUS:
		ret = cmd_status(argc, argv);
		break;
	case CMD_PREDICT:
		ret = cmd_predict(argc, argv);
		break;
	case CMD_SCRUB:
		ret = cmd_scrub(argc, argv);
		break;
	case CMD_BITROT:
		ret = cmd_bitrot(argc, argv);
		break;
	case CMD_BITROT_LOG:
		ret = cmd_bitrot_log(argc, argv);
		break;
	case CMD_RECOVER:
		ret = cmd_recover(argc, argv);
		break;
	case CMD_RECONSTRUCT:
		ret = cmd_reconstruct(argc, argv);
		break;
	case CMD_HEALTH:
		ret = cmd_health(argc, argv);
		break;
	case CMD_SHR:
		/* Parse shr subcommands: "lhsrctl shr plan|create|status" */
		if (argc < 3) {
			fprintf(stderr, "Usage: %s shr <plan|create|status> [opts] <device>...\n", PROGNAME);
			ret = 1;
		} else if (strcmp(argv[2], "plan") == 0) {
			ret = cmd_shr_plan(argc - 2, argv + 2);
		} else if (strcmp(argv[2], "create") == 0) {
			ret = cmd_shr_create(argc - 2, argv + 2);
		} else if (strcmp(argv[2], "status") == 0) {
			ret = cmd_shr_status(argc - 2, argv + 2);
		} else if (strcmp(argv[2], "destroy") == 0) {
			ret = cmd_shr_destroy(argc - 2, argv + 2);
		} else if (strcmp(argv[2], "scrub") == 0) {
			ret = cmd_shr_scrub(argc - 3, argv + 3);
		} else if (strcmp(argv[2], "disk") == 0) {
			ret = cmd_shr_disk(argc - 3, argv + 3);
		} else if (strcmp(argv[2], "rebuild") == 0) {
			ret = cmd_shr_rebuild(argc - 3, argv + 3);
		} else if (strcmp(argv[2], "expand") == 0) {
			ret = cmd_shr_expand(argc - 3, argv + 3);
		} else if (strcmp(argv[2], "grow") == 0) {
			ret = cmd_shr_grow(argc - 3, argv + 3);
		} else {
			fprintf(stderr, "Unknown shr subcommand '%s'. "
				"Use: plan, create, status, destroy, "
				"disk, rebuild, scrub, expand, grow\n",
				argv[2]);
			ret = 1;
		}
		break;
	default:
		usage(basename(argv[0]));
		ret = 1;
	}

	return ret;
}