/*
 * LHSR Daemon - libdevmapper wrapper
 *
 * Replaces fork+exec of dmsetup(8) with direct libdevmapper (dm_task_*) calls.
 *
 * libdevmapper API reference:
 *   struct dm_task      — an ioctl wrapper
 *   dm_task_create(DM_DEVICE_*) — create a task of a given type
 *   dm_task_set_*()     — set task parameters (name, uuid, message, etc.)
 *   dm_task_run()       — execute the ioctl
 *   dm_task_get_*()     — retrieve results
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <sys/sysmacros.h>
#include <libdevmapper.h>

#include "lhsrd.h"

/* ---------- initialization ---------- */

void lhsr_dm_init(int verbose)
{
	dm_log_init_verbose(verbose ? 4 : 0);
}

/* ---------- helpers ---------- */

static int dm_simple_task(const char *dm_name, int task_type)
{
	struct dm_task *dmt;
	int r;

	dmt = dm_task_create(task_type);
	if (!dmt) {
		syslog(LOG_ERR, "dm_task_create(%d) failed for %s", task_type, dm_name);
		return -1;
	}

	if (dm_name)
		dm_task_set_name(dmt, dm_name);

	r = dm_task_run(dmt);
	if (!r) {
		syslog(LOG_ERR, "dm_task_run(%d) failed for %s: %s",
		       task_type, dm_name, strerror(errno));
	}
	dm_task_destroy(dmt);
	return r ? 0 : -1;
}

/* ---------- public API ---------- */

int lhsr_dm_message(const char *dm_name, int sector, const char *msg)
{
	struct dm_task *dmt;
	int r;

	dmt = dm_task_create(DM_DEVICE_TARGET_MSG);
	if (!dmt) {
		syslog(LOG_ERR, "dm_task_create(DM_DEVICE_TARGET_MSG) failed");
		return -1;
	}

	dm_task_set_name(dmt, dm_name);
	dm_task_set_sector(dmt, (uint64_t)sector);
	dm_task_set_message(dmt, msg);

	r = dm_task_run(dmt);
	if (!r)
		syslog(LOG_ERR, "lhsr_dm_message(%s, %d, %s) failed: %s",
		       dm_name, sector, msg, strerror(errno));

	dm_task_destroy(dmt);
	return r ? 0 : -1;
}

char *lhsr_dm_status(const char *dm_name)
{
	struct dm_task *dmt;
	struct dm_info info;
	const char *status;
	char *result = NULL;

	dmt = dm_task_create(DM_DEVICE_STATUS);
	if (!dmt)
		return NULL;

	dm_task_set_name(dmt, dm_name);
	if (!dm_task_run(dmt))
		goto out;

	if (!dm_task_get_info(dmt, &info) || !info.exists)
		goto out;

	status = dm_task_get_message_response(dmt);
	if (status)
		result = strdup(status);

out:
	dm_task_destroy(dmt);
	return result;
}

/*
 * List all LHSR DM devices by iterating devmaps.
 */
int lhsr_dm_list_arrays(struct array_state *arrays, int max_arrays)
{
	struct dm_task *dmt;
	struct dm_names *names;
	unsigned int next = 0;
	int count = 0;

	dmt = dm_task_create(DM_DEVICE_LIST);
	if (!dmt)
		return -1;

	if (!dm_task_run(dmt)) {
		dm_task_destroy(dmt);
		return -1;
	}

	names = dm_task_get_names(dmt);
	if (!names || !names->dev) {
		dm_task_destroy(dmt);
		return 0;
	}

	do {
		struct dm_task *infot;

		names = (struct dm_names *)((char *)names + next);
		if (count >= max_arrays)
			break;

		/* Check if this is an LHSR target by querying its table type */
		infot = dm_task_create(DM_DEVICE_TABLE);
		if (!infot)
			continue;

		dm_task_set_name(infot, names->name);
		if (dm_task_run(infot)) {
			void *next_ptr = NULL;
			uint64_t start, length;
			char *target_type = NULL;
			char *params = NULL;

			dm_get_next_target(infot, &next_ptr, &start, &length,
					   &target_type, &params);

			if (target_type && strcmp(target_type, "lhsr") == 0) {
				struct array_state *as = &arrays[count];
				memset(as, 0, sizeof(*as));
				strncpy(as->dm_name, names->name, sizeof(as->dm_name) - 1);
				/* Parse raid_type from params if present */
				if (params) {
					char *tok = strtok(params, " ");
					if (tok) {
						char type_name[32];
						strncpy(type_name, tok, sizeof(type_name) - 1);
						if (strcmp(type_name, "raid0") == 0)
							as->raid_type = 0;
						else if (strcmp(type_name, "raid1") == 0)
							as->raid_type = 1;
						else if (strcmp(type_name, "raid5") == 0)
							as->raid_type = 5;
						else if (strcmp(type_name, "raid10") == 0)
							as->raid_type = 10;
						/* count remaining args (device offset pairs) */
						int pairs = 0;
						while (tok) {
							tok = strtok(NULL, " ");
							pairs++;
						}
						as->num_disks = pairs / 2;
					}
				}
				as->last_status = time(NULL);
				count++;
			}
		}

		dm_task_destroy(infot);
		next = names->next;
	} while (next);

	dm_task_destroy(dmt);
	return count;
}

int lhsr_dm_create(const char *name, const char *table)
{
	struct dm_task *dmt;
	int r;

	dmt = dm_task_create(DM_DEVICE_CREATE);
	if (!dmt)
		return -1;

	dm_task_set_name(dmt, name);

	/* Parse table line: start length target_type target_args */
	{
		unsigned long long start, length;
		char target[64];
		char args[1024];
		int n = sscanf(table, "%llu %llu %63s %1023[^\n]",
			      &start, &length, target, args);
		if (n < 4) {
			syslog(LOG_ERR, "lhsr_dm_create: invalid table line: %s", table);
			dm_task_destroy(dmt);
			return -1;
		}
		dm_task_add_target(dmt, start, length, target, args);
	}

	r = dm_task_run(dmt);
	if (!r)
		syslog(LOG_ERR, "lhsr_dm_create(%s) failed: %s",
		       name, strerror(errno));

	dm_task_destroy(dmt);
	return r ? 0 : -1;
}

int lhsr_dm_remove(const char *name)
{
	return dm_simple_task(name, DM_DEVICE_REMOVE);
}

int lhsr_dm_get_devices(const char *dm_name, char devices[][256], int max_devices)
{
	struct dm_task *dmt;
	struct dm_deps *deps;
	int count = 0;

	dmt = dm_task_create(DM_DEVICE_DEPS);
	if (!dmt)
		return -1;

	dm_task_set_name(dmt, dm_name);
	if (!dm_task_run(dmt)) {
		dm_task_destroy(dmt);
		return -1;
	}

	deps = dm_task_get_deps(dmt);
	if (!deps || !deps->count) {
		dm_task_destroy(dmt);
		return 0;
	}

	for (uint32_t i = 0; i < deps->count && i < (uint32_t)max_devices; i++) {
		snprintf(devices[i], 256, "%d:%d",
			 major(deps->device[i]), minor(deps->device[i]));
		count++;
	}

	dm_task_destroy(dmt);
	return count;
}

int lhsr_dm_suspend(const char *name)
{
	return dm_simple_task(name, DM_DEVICE_SUSPEND);
}

int lhsr_dm_resume(const char *name)
{
	return dm_simple_task(name, DM_DEVICE_RESUME);
}
