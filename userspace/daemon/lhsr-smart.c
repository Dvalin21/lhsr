/*
 * LHSR Daemon - SMART monitoring via sysfs and SG_IO
 *
 * Replaces fork+exec of smartctl(8) with two-tier approach:
 *   Tier 1: sysfs — read /sys/block/<dev>/device/ for basic health
 *   Tier 2: SG_IO — ATA PASS-THROUGH 16 command for full SMART attributes
 *
 * This eliminates the smartctl binary dependency and the fork+exec overhead.
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
#include <errno.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>
#include <scsi/scsi.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <syslog.h>

#include "lhsrd.h"

/* ---------- helpers ---------- */

/*
 * Given a device path like /dev/sda or /dev/nvme0n1,
 * resolve the sysfs path for the device.
 * Returns 0 on success with buf containing the sysfs path.
 */
static int device_to_sysfs(const char *device, char *buf, size_t bufsz)
{
	struct stat st;

	if (stat(device, &st) < 0)
		return -1;
	if (!S_ISBLK(st.st_mode))
		return -1;

	snprintf(buf, bufsz, "/sys/dev/block/%d:%d/device",
		 major(st.st_rdev), minor(st.st_rdev));
	return 0;
}

/*
 * Read a sysfs attribute file (first line only).
 * Returns 0 on success, -1 on error.
 */
static int sysfs_read_attr(const char *path, char *buf, size_t bufsz)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, bufsz, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	/* Strip trailing newline */
	size_t len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';
	return 0;
}

/* ---------- Tier 1: sysfs health ---------- */

int lhsr_smart_sysfs_health(const char *device, int *health)
{
	char sysfs_path[512];
	char attr[128];

	*health = 100;

	if (device_to_sysfs(device, sysfs_path, sizeof(sysfs_path)) < 0)
		return -1;

	/* Try NVMe health first */
	{
		char nvme_path[640];
		snprintf(nvme_path, sizeof(nvme_path),
			 "/sys/block/%s/device/health", strrchr(device, '/') + 1);

		if (sysfs_read_attr(nvme_path, attr, sizeof(attr)) == 0) {
			long val = strtol(attr, NULL, 0);
			if (val > 0 && val < 100)
				*health = (int)val;
			return 0;
		}
	}

	/*
	 * For ATA/SATA devices, try to get a rough health estimate
	 * from the sysfs interface.
	 * Not all attributes are available via sysfs — most are
	 * only accessible through SG_IO ATA PASS-THROUGH.
	 */
	return -1;	/* Fall through to SG_IO */
}

/* ---------- Tier 2: SG_IO ATA PASS-THROUGH ---------- */

/*
 * ATA PASS-THROUGH 16 command (for SCSI/ATA translation).
 * SCSI opcode: 0x85 = ATA PASS-THROUGH (16)
 * Total CDB length: 16 bytes — NO bitfields, plain byte layout.
 */
#define ATA_PT_OPCODE  0x85
#define ATA_PT_LEN     16

/* ATA commands used for SMART */
#define ATA_CMD_SMART_READ_DATA    0xD0
#define ATA_CMD_SMART_READ_LOG     0xD5
#define ATA_CMD_SMART_THRESHOLDS   0xD1
#define ATA_CMD_SMART_STATUS       0xDA
#define ATA_CMD_IDENTIFY_DEVICE    0xEC

/*
 * Execute a SCSI/ATA pass-through command via SG_IO ioctl.
 *
 * fd: open file descriptor to the block device
 * cmd: the ATA command (ATA_CMD_SMART_*)
 * features: ATA features register
 * data: output buffer (or NULL for no data)
 * data_len: size of data buffer
 * dir: SG_DXFER_FROM_DEV (read) or SG_DXFER_TO_DEV (write)
 *
 * Returns 0 on success, -1 on error with errno set.
 */
static int ata_pass_through(int fd, uint8_t cmd, uint8_t features,
			    uint8_t *data, size_t data_len, int dir)
{
	struct sg_io_hdr hdr;
	uint8_t sense[32];
	uint8_t cdb[ATA_PT_LEN];
	int r;

	memset(&cdb, 0, sizeof(cdb));
	memset(&hdr, 0, sizeof(hdr));
	memset(&sense, 0, sizeof(sense));

	/*
	 * ATA PASS-THROUGH (16) CDB layout:
	 *   byte 0: opcode (0x85)
	 *   byte 1: [0:3]=protocol, [4]=multiple, [5]=ext, [6:7]=reserved
	 *   byte 2: [0:1]=t_length, [2]=t_dir(1=from-dev), [3]=byte_block(0=sectors),
	 *            [4]=t_type, [5:7]=reserved
	 *   bytes 3-7: reserved (logical block address for LBA48, but zero for SMART)
	 *   byte 8: features (low)
	 *   byte 9: features (high) — zero for 28-bit
	 *   byte 10: sector count (low)
	 *   byte 11: sector count (high) — zero for 28-bit
	 *   byte 12: LBA low
	 *   byte 13: LBA mid
	 *   byte 14: LBA high
	 *   byte 15: device
	 *   byte 16: command
	 *   byte 17: reserved
	 *   byte 18: control
	 *
	 * Wait — the standard 16-byte CDB only goes up to byte 15.
	 * For SPC-4 / SAT: 16-byte CDB = bytes 0-15.
	 * But some implementations use a 12-byte CDB.
	 *
	 * Let's use the correct 16-byte layout per SAT-4:
	 *   byte 0:  opcode = 0x85
	 *   byte 1:  [0:3]=protocol, [4]=multiple, [5]=ext, [6]=t_dir,
	 *            [7]=byte_block (for 12-byte also)
	 *   byte 2:  [0:1]=t_length, [2]=t_type, [3:7]=reserved
	 *   byte 3:  features (15:8)
	 *   byte 4:  features (7:0)
	 *   byte 5:  sector count (15:8)
	 *   byte 6:  sector count (7:0)
	 *   byte 7:  LBA low (7:0)
	 *   byte 8:  LBA low (15:8)
	 *   byte 9:  LBA mid (7:0)
	 *   byte 10: LBA mid (15:8)
	 *   byte 11: LBA high (7:0)
	 *   byte 12: LBA high (15:8)
	 *   byte 13: device
	 *   byte 14: command
	 *   byte 15: control
	 *
	 * For SMART READ DATA:
	 *   protocol = 4 (PIO Data-In), t_dir = 1 (from device)
	 *   features = 0, sector count = 1
	 *   LBA mid = 0x4F, LBA high = 0xC2 (SMART magic)
	 *   command = 0xD0 (SMART READ DATA)
	 */
	cdb[0]  = ATA_PT_OPCODE;			/* opcode */
	cdb[1]  = (4 << 0) | (1 << 6);			/* protocol=PIO-In, t_dir=from-dev */
	cdb[2]  = 0;					/* t_length=0 (sectors), t_type=0 */
	cdb[3]  = 0;					/* features high = 0 */
	cdb[4]  = features;				/* features low */
	cdb[5]  = 0;					/* sector count high = 0 (28-bit) */
	cdb[6]  = (data_len > 0) ? (data_len / 512) : 0; /* sector count low */
	cdb[7]  = 0;					/* LBA low low */
	cdb[8]  = 0;					/* LBA low high */
	cdb[9]  = 0x4F;					/* LBA mid low = SMART magic */
	cdb[10] = 0;					/* LBA mid high */
	cdb[11] = 0xC2;					/* LBA high low = SMART magic */
	cdb[12] = 0;					/* LBA high high */
	cdb[13] = (1 << 6) | (1 << 4);			/* device = obs|LBA=0|DEV=1|reserved */
	cdb[14] = cmd;					/* ATA command (0xD0 = SMART READ) */
	cdb[15] = 0;					/* control */

	hdr.interface_id = 'S';
	hdr.dxfer_direction = dir;
	hdr.cmd_len = ATA_PT_LEN;
	hdr.mx_sb_len = sizeof(sense);
	hdr.dxferp = data;
	hdr.dxfer_len = data_len;
	hdr.cmdp = cdb;
	hdr.sbp = sense;
	hdr.timeout = 5000;		/* 5 seconds */

	r = ioctl(fd, SG_IO, &hdr);
	if (r < 0)
		return -1;

	/* Check SCSI status */
	if (hdr.status || hdr.host_status || hdr.driver_status) {
		errno = EIO;
		return -1;
	}

	return 0;
}

/*
 * Read the full SMART data log (512 bytes) via ATA PASS-THROUGH.
 * data must be at least 512 bytes.
 * Returns 0 on success, -1 on error.
 */
static int smart_read_data(int fd, uint8_t *data)
{
	return ata_pass_through(fd, ATA_CMD_SMART_READ_DATA, 0,
				data, 512, SG_DXFER_FROM_DEV);
}

/*
 * Extract key SMART attributes from the raw 512-byte SMART data page.
 *
 * SMART attribute format (each entry is 12 bytes):
 *   Byte 0:       Attribute ID
 *   Bytes 1-2:    Status/flags (raw)
 *   Byte 3:       Current value
 *   Byte 4:       Worst value
 *   Byte 5:       Raw data low byte
 *   Bytes 6-10:   Raw data (5 more bytes, vendor-specific)
 *   Byte 11:      Reserved
 *
 * Offsets: attributes start at byte 2 of the SMART data page.
 */
#define SMART_DATA_OFFSET_ATTRIBUTES  2
#define SMART_ATTRIBUTE_SIZE          12
#define SMART_MAX_ATTRIBUTES          30

/* Known attribute IDs */
#define SMART_ATTR_REALLOCATED    0x05
#define SMART_ATTR_PENDING        0xC5
#define SMART_ATTR_UNCORRECTABLE  0xC6
#define SMART_ATTR_TEMPERATURE    0xC2
#define SMART_ATTR_POWER_ON_HOURS 0x09
#define SMART_ATTR_WEAR_LEVEL     0xE8

static void parse_smart_attributes(const uint8_t *data, struct disk_health *dh)
{
	int i;

	for (i = 0; i < SMART_MAX_ATTRIBUTES; i++) {
		const uint8_t *attr = data + SMART_DATA_OFFSET_ATTRIBUTES + i * SMART_ATTRIBUTE_SIZE;
		uint8_t id = attr[0];

		if (id == 0x00)
			break;		/* End of table */

		switch (id) {
		case SMART_ATTR_REALLOCATED: {
			/* Raw data is bytes 5-11 (5 bytes, little-endian) */
			uint64_t raw = (uint64_t)attr[5] | ((uint64_t)attr[6] << 8) |
				       ((uint64_t)attr[7] << 16) | ((uint64_t)attr[8] << 24);
			dh->smart_reallocated = (int)raw;
			break;
		}
		case SMART_ATTR_PENDING: {
			uint64_t raw = (uint64_t)attr[5] | ((uint64_t)attr[6] << 8);
			dh->smart_pending = (int)raw;
			break;
		}
		case SMART_ATTR_UNCORRECTABLE: {
			uint64_t raw = (uint64_t)attr[5] | ((uint64_t)attr[6] << 8);
			dh->smart_uncorrectable = (int)raw;
			break;
		}
		case SMART_ATTR_TEMPERATURE:
			dh->temperature = attr[5];	/* Current temp in Celsius */
			break;
		case SMART_ATTR_POWER_ON_HOURS: {
			/* 4-byte little-endian raw value starting at byte 5 */
			uint64_t raw = (uint64_t)attr[5] | ((uint64_t)attr[6] << 8) |
				       ((uint64_t)attr[7] << 16) | ((uint64_t)attr[8] << 24);
			dh->power_on_hours = (int)raw;
			break;
		}
		case SMART_ATTR_WEAR_LEVEL:
			/* Normalized value is byte 3 (0-100), raw is bytes 5-6 */
			dh->wear_level = attr[3];	/* normalized 0-100 */
			break;
		}
	}
}

/* ---------- public API ---------- */

int lhsr_smart_sg_io(const char *device, struct disk_health *dh)
{
	int fd;
	uint8_t smart_data[512] = {0};
	int r;

	fd = open(device, O_RDWR | O_EXCL);
	if (fd < 0) {
		fd = open(device, O_RDONLY);
		if (fd < 0) {
			syslog(LOG_WARNING, "smart_sg_io: cannot open %s: %s",
			       device, strerror(errno));
			return -1;
		}
	}

	r = smart_read_data(fd, smart_data);
	close(fd);

	if (r < 0) {
		syslog(LOG_DEBUG, "smart_sg_io: ATA PASS-THROUGH failed for %s: %s",
		       device, strerror(errno));
		return -1;
	}

	parse_smart_attributes(smart_data, dh);

	/* Compute health score from parsed attributes */
	dh->health = 100;
	if (dh->smart_reallocated > SMART_REALLOCATED_THRESHOLD)
		dh->health -= 30;
	if (dh->smart_pending > SMART_PENDING_THRESHOLD)
		dh->health -= 20;
	if (dh->smart_uncorrectable > 0)
		dh->health -= 40;
	if (dh->temperature > SMART_TEMP_THRESHOLD)
		dh->health -= 10;
	if (dh->health < 0)
		dh->health = 0;

	dh->last_check = time(NULL);
	return 0;
}

int lhsr_smart_poll(const char *device, struct disk_health *dh)
{
	memset(dh, 0, sizeof(*dh));
	strncpy(dh->device_path, device, sizeof(dh->device_path) - 1);

	/* Tier 1: sysfs (fast, best-effort) */
	if (lhsr_smart_sysfs_health(device, &dh->health) == 0) {
		dh->last_check = time(NULL);
		return 0;
	}

	/* Tier 2: SG_IO ATA PASS-THROUGH */
	if (lhsr_smart_sg_io(device, dh) == 0)
		return 0;

	return -1;	/* Both methods failed */
}
