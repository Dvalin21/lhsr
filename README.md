# LHSR — Linux Hybrid Self-Healing RAID

**Version:** 1.3.0 | **License:** GPLv3 | **Kernel:** 5.15+ (tested on 6.12.x)

---

LHSR is a **Linux Device Mapper target** that provides RAID5/6 with CRC32c
scrubbing and read-side self-healing.  It is implemented as a kernel module
(`dm-lhsr.ko`) with userspace tools for administration and recovery.

## What it does

- **RAID5 and RAID6** — XOR and Reed-Solomon GF(2^8) parity with correct RMW
  state machine.  RAID0 (single) and RAID1 (mirror) also supported.
- **CRC32c background scrub** — rate-limited, configurable block size,
  corruption detection and logging.
- **Read-side reconstruction** — transparent parity-read recovery when a disk
  fails.  RAID5 single-failure and RAID6 dual-failure tolerant.
- **Write-intent bitmap (WIB)** — tracks modified regions so rebuilds only copy
  dirty data.  On-disk format with CRC32c protected pages.
- **Write-hole protected metadata** — backup-first superblock writes with
  REQ_FUA, so array metadata always lands on stable media before bitmap clears.
- **dm-integrity stacking** — stack on dm-integrity devices for end-to-end
  per-block CRC32c verification.
- **Module parameter tuning** — `rmw_max_active` controls RMW concurrency
  (default 32, tunable for high-queue-depth devices).
- **Boot-time autodiscovery** — initramfs hook scans for LHSR superblocks and
  assembles arrays before root mount.

## Quick start

```bash
# Build and install
make all
sudo make install

# Load the module
sudo modprobe dm_lhsr

# Quick test with loopback devices
dd if=/dev/zero of=/tmp/d1 bs=1M count=64
dd if=/dev/zero of=/tmp/d2 bs=1M count=64
dd if=/dev/zero of=/tmp/d3 bs=1M count=64
sudo losetup /dev/loop0 /tmp/d1
sudo losetup /dev/loop1 /tmp/d2
sudo losetup /dev/loop2 /tmp/d3

# Create a RAID5 array via the deploy script
sudo deploy/lhsr-create.sh raid5 /dev/loop0 /dev/loop1 /dev/loop2

# Or use dmsetup directly
# sudo echo "0 130448 lhsr raid5 8 1 8 /dev/loop0 0 /dev/loop1 0 /dev/loop2 0" \
#   | sudo dmsetup create lhsr-test

# Write some data and verify
sudo dd if=/dev/urandom of=/dev/mapper/lhsr-* bs=4k count=100

# Check status
sudo dmsetup status lhsr-*

# Clean up
sudo dmsetup remove lhsr-*
sudo rmmod dm_lhsr
sudo losetup -d /dev/loop0 /dev/loop1 /dev/loop2
rm /tmp/d1 /tmp/d2 /tmp/d3
```

For complete installation instructions, see [INSTALL.md](INSTALL.md).

## Architecture

```
Filesystem (ext4, xfs, btrfs, etc.)
    ↓
dm-lhsr (kernel DM target — RAID5/6, scrub, rebuild)
    ↓
Block devices (sdX, nvmeXnY, or dm-integrity devices)
```

The daemon (`lhsrd`) runs in userspace and handles:
- Disk health monitoring (SMART polling, composite health score 0-100)
- Trend tracking (SQLite DB, linear regression for predictive failure)
- Scrubbing orchestration (via dmsetup message)
- Rebuild orchestration
- Prometheus metrics (compatible with node_exporter textfile collector)

## What it does NOT do (yet)

- **SHR flexible disk sizing** — Synology SHR is mdraid + LVM; LHSR doesn't do
  this.  SHR partition data structures exist but are not populated.
- **Online reshape (writes accepted)** — Reshape currently rejects writes
  during migration (read-only filesystem required).  An online reshape with
  bio deferral is planned.  RAID5→RAID6 in-place reshape is implemented via
  [`deploy/lhsr-reshape.sh`](deploy/lhsr-reshape.sh) (adds Q parity disk,
  crash-safe, preserves data).  Offline migration is also supported via
  [`deploy/lhsr-migrate.sh`](deploy/lhsr-migrate.sh) (backup → recreate → restore).
- **Persistent checksum cache** — the CRC32c checksum cache is ephemeral
  (xarray, lost on module unload).  For persistent anti-bit-rot,
  [stack LHSR on dm-integrity](INSTALL.md#dm-integrity-stacking).

## Components

| Component | Path | Purpose |
|-----------|------|---------|
| Kernel module | `kernel/dm-lhsr/dm-lhsr.ko` | DM target (~5600 lines C) |
| Daemon | `userspace/daemon/lhsrd` | SMART, health, trends, Prometheus, HTTP API |
| CLI | `userspace/cli/lhsrctl` | Status, predict, recover, reconstruct, disk-fail |
| Scanner | `userspace/recovery/lhsr-scan` | Superblock discovery (`--deep`, `--json`) |
| Service | `userspace/systemd/lhsrd.service` | systemd unit |
| Initramfs hook | `deploy/initramfs-hook/lhsr` | Boot-time autodiscovery |
| Assembly | `deploy/lhsr-assemble.sh` | Scan + dmsetup create for all LHSR arrays |
| Create | `deploy/lhsr-create.sh` | Validate + initialize + create new arrays |
| Migrate | `deploy/lhsr-migrate.sh` | Offline RAID level migration (safe path, any type) |
| Reshape | `deploy/lhsr-reshape.sh` | In-place RAID5→RAID6 (add Q disk, preserves data) |

## Test suite

| Test | Scope | Run time |
|------|-------|----------|
| `tests/crash-point-injection.sh` | 5 crash scenarios (rmmod, disk failure, WIB) | ~2 min |
| `tests/test-degraded-dmintegrity.sh` | degraded I/O, dm-integrity stacking, bit-flip | ~3 min |
| `tests/test-error-paths.sh` | message fuzzing, double disk failure, dtr+I/O race | ~3 min |
| `tests/test-benchmark.sh` | max_active/chunk-size IOPS sweeps | 5-30 min |
| `tests/smoke-test-raid5.sh` | Basic RAID5 write/read/verify | ~1 min |
| `tests/stress-test-1-concurrent-rmw.sh` | Concurrent RMW stress | ~5 min |
| `tests/*.sh` | 15+ test scripts covering RAID5, RAID6, bitmap, rebuild | varies |

Run all tests:
```bash
sudo make test-all
```

## Requirements

- Linux 5.15+
- Device Mapper (`CONFIG_DM`)
- Kernel headers for building
- `libdevmapper-dev`, `libsqlite3-dev`, `uuid-dev` for userspace tools

## Documentation

| Document | Contents |
|----------|----------|
| [INSTALL.md](INSTALL.md) | Full installation and deployment guide |
| [RECOVERY.md](RECOVERY.md) | Step-by-step disk failure recovery (5 scenarios) |
| [PRODUCTION_READINESS.md](PRODUCTION_READINESS.md) | Honest gap analysis |
| [CHANGELOG.md](CHANGELOG.md) | Version history |
| [ROADMAP.md](ROADMAP.md) | Phased implementation plan |
| [TEST_HARDWARE.md](TEST_HARDWARE.md) | VM test environment specs |
| [KERNEL_NOTES.md](KERNEL_NOTES.md) | Kernel API notes |

## Related projects

LHSR integrates with or builds alongside:

- **mdadm**: Linux software RAID
- **dm-integrity**: Per-block checksum storage (stack below LHSR for anti-bit-rot)
- **LVM**: Volume management
- **dm-crypt**: Block-level encryption
- **Prometheus + node_exporter**: Daemon metrics via textfile collector

## License

GPLv3 — See [LICENSE](LICENSE).
