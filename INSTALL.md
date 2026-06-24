# LHSR Installation & Deployment Guide

**Version:** 1.3.0 | **License:** GPLv3 | **Kernel:** 5.15+ (tested on 6.12.x)

---

## Table of Contents

1. [Overview](#overview)
2. [Prerequisites](#prerequisites)
3. [Building](#building)
4. [Installation](#installation)
5. [Initramfs Integration](#initramfs-integration)
6. [Creating Arrays](#creating-arrays)
7. [Management](#management)
8. [Array Migration](#array-migration)
9. [Monitoring](#monitoring)
10. [Recovery](#recovery)
11. [Troubleshooting](#troubleshooting)

---

## Overview

LHSR provides RAID5/6 with CRC32c scrubbing and read-side self-healing as a
Linux Device Mapper target (`dm-lhsr`).  It comprises four components:

| Component | Path | Purpose |
|-----------|------|---------|
| **dm-lhsr.ko** | `kernel/dm-lhsr/` | Kernel DM target — RAID5/6 XOR/RS, scrub, rebuild |
| **lhsrd** | `userspace/daemon/` | Monitoring daemon — SMART polling, health scoring, Prometheus metrics |
| **lhsrctl** | `userspace/cli/` | CLI — status, predict, recover, reconstruct, disk-fail, scrub |
| **lhsr-scan** | `userspace/recovery/` | Superblock scanner — deep scan, JSON output, auto-detect |

---

## Prerequisites

### Required packages (Debian/Ubuntu)

```bash
sudo apt install build-essential linux-headers-$(uname -r) \
    libdevmapper-dev libsqlite3-dev uuid-dev
```

### Kernel config

Verify these are enabled (`/boot/config-$(uname -r)`):

```
CONFIG_DM=y
CONFIG_DM_UEVENT=y               # recommended
CONFIG_CRC32C=y                  # required for superblock + scrub
CONFIG_DM_INTEGRITY=y            # optional, for dm-integrity stacking
```

### Hardware

- Any block device supported by Linux (SATA, NVMe, SAS, loopback for testing)
- Minimum 2 disks (mirror), 3 disks (RAID5), or 4 disks (RAID6)
- No special hardware requirements beyond standard block I/O

---

## Building

### Quick build (kernel module + userspace)

```bash
# From the project root:
make all               # Build everything
make modules           # Just the kernel module
make userspace         # Just userspace tools
make clean
```

### Build artifacts

| Target | Output | Notes |
|--------|--------|-------|
| Kernel module | `kernel/dm-lhsr/dm-lhsr.ko` | Load with `insmod` or `modprobe` |
| Daemon | `userspace/daemon/lhsrd` | Monitoring daemon |
| CLI | `userspace/cli/lhsrctl` | Administration tool |
| Scanner | `userspace/recovery/lhsr-scan` | Superblock recovery tool |

### Cross-compilation

Set `CROSS_COMPILE` and `KDIR` for non-native builds:

```bash
make CROSS_COMPILE=aarch64-linux-gnu- KDIR=/path/to/kernel
```

---

## Installation

### Quick install

```bash
make install              # installs module + daemon + CLI
# Requires root or sudo
```

### Manual install

```bash
# 1. Kernel module
sudo cp kernel/dm-lhsr/dm-lhsr.ko /lib/modules/$(uname -r)/kernel/drivers/md/
sudo depmod -a

# 2. Daemon + CLI
sudo cp userspace/daemon/lhsrd /usr/local/sbin/
sudo cp userspace/cli/lhsrctl /usr/local/bin/
sudo cp userspace/recovery/lhsr-scan /usr/local/bin/

# 3. Service file
sudo cp userspace/systemd/lhsrd.service /usr/lib/systemd/system/
sudo systemctl daemon-reload

# 4. Config
sudo mkdir -p /etc/lhsr
sudo cp userspace/config/lhsrd.conf.example /etc/lhsr/lhsrd.conf

# 5. Deployment scripts
sudo cp deploy/lhsr-create.sh /usr/lib/lhsr/
sudo cp deploy/lhsr-assemble.sh /usr/lib/lhsr/
sudo cp deploy/lhsr-migrate.sh /usr/lib/lhsr/
sudo cp deploy/lhsr-reshape.sh /usr/lib/lhsr/
```

### Loading the module

```bash
# Load with default settings (32 RMW workers)
sudo modprobe dm_lhsr

# Or load with custom max_active (RMW concurrency)
sudo modprobe dm_lhsr rmw_max_active=64

# Verify it loaded
lsmod | grep dm_lhsr
```

### Starting the daemon

```bash
sudo systemctl enable --now lhsrd
journalctl -u lhsrd -f     # watch daemon logs
```

### Verifying the installation

```bash
# Check module is loaded
sudo dmsetup targets | grep lhsr

# Check daemon status
sudo lhsrctl status

# Quick smoke test with loopback devices
dd if=/dev/zero of=/tmp/lhsr-test1.img bs=1M count=64
dd if=/dev/zero of=/tmp/lhsr-test2.img bs=1M count=64
dd if=/dev/zero of=/tmp/lhsr-test3.img bs=1M count=64
sudo losetup /dev/loop0 /tmp/lhsr-test1.img
sudo losetup /dev/loop1 /tmp/lhsr-test2.img
sudo losetup /dev/loop2 /tmp/lhsr-test3.img
sudo lhsr-create.sh raid5 /dev/loop0 /dev/loop1 /dev/loop2
# → /dev/mapper/lhsr-<uuid>
sudo dd if=/dev/urandom of=/dev/mapper/lhsr-<uuid> bs=4k count=100
sudo dmsetup remove lhsr-<uuid>
sudo losetup -d /dev/loop0 /dev/loop1 /dev/loop2
rm /tmp/lhsr-test*.img
```

---

## Initramfs Integration

For systems where LHSR arrays contain the root filesystem, or need to be
available at boot time.

### Step 1: Install the hook

```bash
# Copy the initramfs-tools hook
sudo cp deploy/initramfs-hook/lhsr /usr/share/initramfs-tools/hooks/
sudo chmod +x /usr/share/initramfs-tools/hooks/lhsr

# The hook requires dm_lhsr.ko to be in /lib/modules/
# (already done in the installation step above)
```

### Step 2: Install the assembly script

```bash
sudo cp deploy/lhsr-assemble.sh /usr/lib/lhsr/lhsr-assemble.sh
```

### Step 3: Update initramfs

```bash
sudo update-initramfs -u
```

### Step 4: Boot parameters (if root is on LHSR)

If your root filesystem is on an LHSR array, add these kernel parameters:

```
rd.dm=1 rd.auto=1
```

Or for more control, specify the exact array in `/etc/initramfs-tools/conf.d/lhsr`:

```
# /etc/initramfs-tools/conf.d/lhsr
LHSR_ROOT_WAIT=5
```

The initramfs hook:
- Loads `dm_lhsr.ko` before root filesystem mount
- Runs `lhsr-assemble.sh` to discover and assemble all arrays
- Creates device mapper nodes at `/dev/mapper/lhsr-*`

---

## Creating Arrays

### Using `lhsr-create.sh` (recommended)

The `lhsr-create.sh` script validates devices, computes metadata overhead,
builds the correct DM table, and creates the array:

```bash
# RAID5 with 4 disks (3 data + 1 parity)
sudo lhsr-create.sh raid5 /dev/sdb /dev/sdc /dev/sdd /dev/sde
# → /dev/mapper/lhsr-2a1f3e8b

# RAID6 with 5 disks (3 data + 2 parity)
sudo lhsr-create.sh raid6 /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf

# Mirror with 2 disks
sudo lhsr-create.sh mirror /dev/sdb /dev/sdc

# Custom chunk size (16 sectors = 8 KB)
sudo lhsr-create.sh raid5 --chunk-size 16 /dev/sdb /dev/sdc /dev/sdd /dev/sde

# Custom name
sudo lhsr-create.sh raid5 --name myarray /dev/sdb /dev/sdc /dev/sdd /dev/sde
```

### Using `dmsetup` directly

For scripting or advanced use, the raw DM table format is:

```bash
# RAID5: start size lhsr raid5 chunk_sects stripes_per_cont cont_sects dev1 off1 dev2 off2 ...
sudo echo "0 $(( $(blockdev --getsz /dev/sdb) - 272 )) lhsr raid5 8 1 8 \
    /dev/sdb 0 /dev/sdc 0 /dev/sdd 0 /dev/sde 0" | \
    sudo dmsetup create myarray

# Mirror: start size lhsr mirror dev1 off1 dev2 off2
sudo echo "0 $(( $(blockdev --getsz /dev/sdb) - 272 )) lhsr mirror \
    /dev/sdb 0 /dev/sdc 0" | \
    sudo dmsetup create mymirror

# Single: start size lhsr single dev1 off1
sudo echo "0 $(( $(blockdev --getsz /dev/sdb) - 272 )) lhsr single \
    /dev/sdb 0" | \
    sudo dmsetup create mysingle
```

### Filesystem on LHSR

```bash
# Create a filesystem
sudo mkfs.ext4 /dev/mapper/lhsr-2a1f3e8b

# Mount
sudo mount /dev/mapper/lhsr-2a1f3e8b /mnt/data
```

### dm-integrity stacking

For full end-to-end checksums (not just scrub-time CRC32c):

```bash
# Create dm-integrity devices below LHSR
for dev in /dev/sdb /dev/sdc /dev/sdd /dev/sde; do
    sudo integritysetup format "$dev"
    sudo integritysetup open "$dev" "$(basename $dev)-int"
done

# Stack LHSR on top with 'integrity' flag
sudo lhsr-create.sh raid5 --name secured /dev/mapper/sdb-int /dev/mapper/sdc-int \
    /dev/mapper/sdd-int /dev/mapper/sde-int
```

When dm-integrity is detected below, LHSR skips its own checksum cache
and trusts dm-integrity's per-block CRC32c verification.

---

## Management

### Status queries

```bash
# Overall array status
sudo dmsetup status myarray

# Detailed config
sudo dmsetup message myarray 0 "config"

# WIB status
sudo dmsetup message myarray 0 "wib_status"

# Scrub status
sudo dmsetup message myarray 0 "scrub-status"

# Daemon status (requires running lhsrd)
sudo lhsrctl status
```

### Scrubbing

```bash
# Start scrub
sudo dmsetup message myarray 0 "scrub start"

# Start scrub with custom block size (default 128 KB)
sudo dmsetup message myarray 0 "scrub start 256"

# Stop scrub
sudo dmsetup message myarray 0 "scrub stop"

# Check progress
sudo dmsetup message myarray 0 "scrub-status"
```

### Disk management

```bash
# Mark a disk as failed (triggers read-side reconstruction)
sudo lhsrctl disk-fail /dev/mapper/myarray 0

# Bring a disk back online (after replacement)
sudo lhsrctl disk-online /dev/mapper/myarray 0

# Check per-disk health
sudo lhsrctl disk-health /dev/mapper/myarray

# Rebuild a replaced disk
sudo dmsetup message myarray 0 "rebuild start 0"
sudo dmsetup message myarray 0 "rebuild status"
sudo dmsetup message myarray 0 "rebuild stop"
```

### Module parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `rmw_max_active` | 32 | Max concurrent RMW workers (1-128). Higher values improve throughput on multi-core systems with deep NCQ queues. |

```bash
# Set at module load
sudo modprobe dm_lhsr rmw_max_active=64

# Or at insmod
sudo insmod /lib/modules/$(uname -r)/kernel/drivers/md/dm-lhsr.ko rmw_max_active=64
```

---

## Array Migration

> **Important**: LHSR does not yet support in-place RAID level migration
> (kernel reshape). The migration script handles the **offline** path:
> backup → destroy old → create new → restore.

### Supported paths

| From | To | Minimum disks |
|------|----|---------------|
| single | raid5 | 3 |
| single | mirror | 2 |
| raid5 | raid6 | 4 |

### Procedure

```bash
# 1. Backup the filesystem
sudo lhsr-migrate.sh /dev/mapper/oldarray raid6 \
    /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf \
    --backup /backup/lhsr-migration

# 2. Or with existing backup
sudo lhsr-migrate.sh /dev/mapper/oldarray raid5 \
    /dev/sdb /dev/sdc /dev/sdd /dev/sde \
    --no-backup

# The script:
#   a) Validates source array health
#   b) Creates backup (optional)
#   c) Removes source array
#   d) Creates new array with target layout
#   e) Prints restore instructions

# 3. Restore from backup (if --backup was used)
sudo dd if=/backup/lhsr-migration/lhsr-backup.img \
    of=/dev/mapper/lhsr-<uuid> bs=1M status=progress
sudo fsck /dev/mapper/lhsr-<uuid>
```

---

## Monitoring

### Daemon health checks

The `lhsrd` daemon provides:

- **SMART polling**: Reads reallocated/pending/uncorrectable sectors, temperature
- **Health score**: Composite 0-100 score per disk (OK/WARNING/CRITICAL/FAILING)
- **Trend tracking**: SQLite database tracks metric slopes for predictive failure
- **Prometheus metrics**: `/var/lib/lhsrd/metrics.prom` — compatible with
  `node_exporter` textfile collector

```bash
# Quick health check
sudo lhsrctl status

# Predictive failure analysis
sudo lhsrctl predict

# Watch Prometheus metrics
watch cat /var/lib/lhsrd/metrics.prom
```

### Kernel-level monitoring

```bash
# Watch dm-lhsr logs
sudo dmesg -w | grep lhsr

# IO stats for the DM device
sudo iostat -x /dev/mapper/myarray 5

# Per-disk latency
sudo iostat -x -p sdb,sdc,sdd,sde 5
```

### Performance benchmarking

```bash
# Run the benchmark suite
sudo tests/test-benchmark.sh --quick

# Full benchmark (takes ~30 minutes)
sudo tests/test-benchmark.sh --full
```

The benchmark sweeps `rmw_max_active` (1-128) and chunk size (4-256 sectors)
to find optimal settings for your hardware.

---

## Recovery

See [RECOVERY.md](RECOVERY.md) for detailed recovery procedures covering:

- **Scenario A**: Single disk failed, array still intact — online replacement
- **Scenario B**: Array won't assemble (too few disks) — `lhsrctl recover` + `degraded` mode
- **Scenario C**: Offline reconstruction — `lhsrctl reconstruct` for dead disks
- **Scenario D**: Fresh array with missing disks — `degraded + total_disks=N`
- **Scenario E**: Multiple failures / RAID6 dual failure — limitations

### Quick recovery commands

```bash
# Scan devices for LHSR superblocks
sudo lhsr-scan /dev/sdb /dev/sdc /dev/sdd

# Generate assembly commands for a degraded array
sudo lhsrctl recover /dev/sdb /dev/sdc

# Reconstruct a missing RAID5 disk offline
sudo lhsrctl reconstruct -o /tmp/recovered.img /dev/sdb /dev/sdc /dev/sdd
```

---

## Troubleshooting

### "Device or resource busy" when removing array

```bash
# Check if the device is still mounted
mount | grep /dev/mapper/myarray

# Check if anything has the device open
sudo lsof /dev/mapper/myarray

# Force remove (use with caution)
sudo dmsetup remove -f myarray
```

### "No superblock found" when creating array

The kernel module initializes superblocks on `dmsetup create`.  If this fails:

```bash
# Check dmesg for details
dmesg | tail -20

# Verify devices are large enough (need > 272 sectors for metadata)
sudo blockdev --getsz /dev/sdb

# Verify the module is loaded
lsmod | grep dm_lhsr
```

### "RMW worker stuck" or performance issues

```bash
# Check max_active setting (default 32)
sudo dmsetup message myarray 0 "config" | grep max_active

# Try higher values (e.g., 64) on multi-core systems with deep NCQ
sudo modprobe -r dm_lhsr
sudo modprobe dm_lhsr rmw_max_active=64

# Check for IO errors on underlying devices
dmesg | grep -i "i/o error"
```

### Daemon won't start

```bash
# Check config file
sudo lhsrd -c /etc/lhsr/lhsrd.conf

# Run in foreground
sudo lhsrd -f

# Check journal
journalctl -u lhsrd -n 50 --no-pager
```

---

## Upgrading

```bash
# 1. Pull latest code
cd /path/to/lhsr
git pull

# 2. Build everything
make clean && make all

# 3. Stop daemon and unload old module
sudo systemctl stop lhsrd
sudo rmmod dm_lhsr

# 4. Install new version
sudo make install
sudo cp deploy/lhsr-*.sh /usr/lib/lhsr/

# 5. Update initramfs
sudo update-initramfs -u

# 6. Load new module and start daemon
sudo modprobe dm_lhsr
sudo systemctl start lhsrd

# 7. Verify
sudo dmsetup targets | grep lhsr
sudo lhsrctl status
```

---

## References

- [README.md](README.md) — Project overview
- [RECOVERY.md](RECOVERY.md) — Disk failure recovery guide
- [PRODUCTION_READINESS.md](PRODUCTION_READINESS.md) — Gap analysis
- [CHANGELOG.md](CHANGELOG.md) — Version history
- [docs/technical-specification.md](docs/technical-specification.md) — Original design spec
