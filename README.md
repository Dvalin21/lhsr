# LHSR — Linux Hybrid Self-Healing RAID

**Version:** 1.4.0 | **License:** GPLv3 | **Kernel:** 5.15+ (verified on 6.1 Debian 12; builds on 6.12 Debian 13)

---

LHSR is a **Linux Device Mapper target** that provides RAID5/6 with CRC32c
scrubbing and read-side self-healing.  It is implemented as a kernel module
(`dm-lhsr.ko`) with userspace tools for administration and recovery.

> **Status:** qualified production. RAID5/6 survives sustained file IO
> (≈2.5M RMW cycles, reshape, rebuild, scrub — all clean on VM, Oct 2026).
> Verified envelope: 4KB/64KB chunks, btrfs, single-disk degraded, kernel 6.1.
> Not yet verified: RAID6 dual-failure decode, Debian 13 runtime, real disks.
> See [PRODUCTION_READINESS.md](PRODUCTION_READINESS.md) for the honest ledger.

## What it does

- **RAID5 and RAID6** — XOR and Reed-Solomon GF(2^8) parity with correct RMW
  state machine.  RAID0 (single) and RAID1 (mirror) also supported.
- **CRC32c background scrub** — rate-limited, configurable block size,
  corruption detection and logging.
- **Read-side reconstruction** — transparent parity-read recovery when a disk
  fails.  RAID5 single-failure and RAID6 single-failure verified with
  sha256-identical data; RAID6 dual-failure decode is implemented but
  untested on current code paths.
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
- **SHR-like tiered arrays** — `lhsrctl shr` partitions mixed-size disks into
  equal chunks, builds one LHSR target per tier, merges with LVM
  (LHSR-native, no mdadm).  Plan/create/status/destroy/expand/replace.

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

## Operations

Three things operators need.  Every module build carries a build ID —
refuse to test a box that reports the wrong one:

```bash
# What build is loaded? (must match the commit under test)
sudo dmesg | grep 'Module loaded'
# RMW accounting: works must equal done; errors must stay 0
sudo dmsetup message <dev> 0 rmw_stats
# Stripe locks: buckets with age growing past ~60s are wedged
sudo dmsetup message <dev> 0 rmw_locks
```

A single transient EIO on a single-device filesystem (btrfs included)
forces it read-only — that is filesystem behavior, not an LHSR bug.
Monitor `rmw_stats` errors and dmesg; the recovery runbook is in
[RECOVERY.md](RECOVERY.md), the full ledger in
[PRODUCTION_READINESS.md](PRODUCTION_READINESS.md).

## What it does NOT do (yet)

- **Online reshape with writes accepted** — RAID5→RAID6 in-place reshape is
  implemented (kernel-based, crash-safe, sha256-verified data preservation)
  via [`deploy/lhsr-reshape.sh`](deploy/lhsr-reshape.sh), supports chunks up
  to 64KB, but **writes are rejected** during migration — the filesystem must
  be quiet.  A fully online reshape with bio deferral is deferred.
  Offline migration (backup → recreate → restore) is also supported via
  [`deploy/lhsr-migrate.sh`](deploy/lhsr-migrate.sh).
- **Persistent checksum cache** — the CRC32c checksum cache is ephemeral
  (xarray, lost on module unload).  For persistent anti-bit-rot,
  [stack LHSR on dm-integrity](INSTALL.md#dm-integrity-stacking).
- **RAID6 dual-failure recovery in production** — the Vandermonde 2x2 decode
  exists but has not been executed against current code.  Do not rely on it
  until it is tested.
- **Debian 13 runtime + real disks** — verified on 6.1 (Debian 12 VM);
  6.12 builds clean but version skew has bitten before (standalone FLUSH
  bios).  Real-disk SMART/failover behavior is untested.
- **mdadm-class write performance** — ~5-11x slower on VM (4 sync IOs per
  4KB write; measured 3-5MB/s btrfs file IO).  Unmeasured after the
  October fixes; real hardware numbers needed.

## Components

| Component | Path | Purpose |
|-----------|------|---------|
| Kernel module | `kernel/dm-lhsr/dm-lhsr.ko` | DM target (~6200 lines C) |
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

## Verification status (Oct 2026)

Fresh-VM runs, kernel 6.1, btrfs, sustained 4K file IO unless noted:

| Scope | Result |
|-------|--------|
| RAID5 64KB stress, release build | 556K RMW cycles, 0 errors |
| RAID6 64KB stress (Q path) | 452K RMW cycles, 0 errors |
| 4KB-chunk regression | 964K RMW cycles, 0 errors |
| In-place RAID5→RAID6 reshape | Complete, 200MB sha256 identical |
| Degraded RAID6 single-disk read | 200MB sha256 identical |
| Disk rebuild to healthy | Complete, sha256 identical |
| btrfs scrub 3.56GB after stress | 0 errors found |

Same campaign also fixed: orphaned stripe-lock GPF, flush-barrier failures,
recovery/reshape/degraded-read heap overflows (see CHANGELOG).  Every module
under test is identified by build ID (`dmesg | grep 'Module loaded'`).

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
