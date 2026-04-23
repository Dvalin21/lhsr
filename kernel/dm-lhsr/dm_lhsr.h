/*
 * LHSR Kernel Module Header
 *
 * Copyright (C) 2026 LHSR Team
 * License: GPLv3
 */

#ifndef DM_LHSR_H
#define DM_LHSR_H

#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/device-mapper.h>
#include <linux/list.h>

/* RAID states */
#define LHSR_STATE_OFFLINE    0
#define LHSR_STATE_ONLINE     1
#define LHSR_STATE_HEALTHY   2
#define LHSR_STATE_DEGRADED  3

#endif /* DM_LHSR_H */