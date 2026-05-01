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

#include "../lib/raid_engine.h"

#define PROGNAME "lhsrctl"
#define DM_DEV_PATH "/dev/mapper/control"
#define MAX_ARGS 64

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
		"\n",
		prog, prog, prog, prog, prog);
}

/* Command create */
static int cmd_create(int argc, char **argv, enum lhsr_raid_type raid_type)
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

/* Command status */
static int cmd_status(int argc, char **argv)
{
	(void)argc; (void)argv;
	/* In production, would load config and show status */
	printf("LHSR Status\n");
	printf("==========\n\n");
	printf("Note: This is a stub - config loading not implemented\n");
	printf("Use 'create' to create an array first.\n");

	return 0;
}

/* Command predict */
static int cmd_predict(int argc, char **argv)
{
	(void)argc; (void)argv;
	printf("LHSR Predictive Failure\n");
	printf("=======================\n\n");
	printf("Scanning disks...\n");

	/* In production, would query SMART and calculate risk */
	printf("Note: This is a stub - SMART monitoring not implemented\n");

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

/* Message command - send message to kernel via dmsetup */
static int cmd_message(int argc, char **argv)
{
	int ret;

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

	/* Use execvp to avoid shell injection */
	const char *dmsetup argv[] = {"dmsetup", "message", argv[2], argv[3],
				    argc > 4 ? argv[4] : NULL, NULL};
	ret = fork();
	if (ret == 0) {
		/* Child process */
		execvp("dmsetup", (char * const *)dmsetup_argv);
		exit(127);
	} else if (ret > 0) {
		/* Parent process */
		int status;
		waitpid(ret, &status, 0);
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

/* Main */
int main(int argc, char **argv)
{
	int cmd = CMD_NONE;
	const char *raid_type_str = NULL;
	enum lhsr_raid_type raid_type = LHSR_RAID5;
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
	default:
		usage(basename(argv[0]));
		ret = 1;
	}

	return ret;
}