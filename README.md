# LHSR — Linux Hybrid Self-Healing RAID

**Status:** Development — v1.3.0
**License:** GPLv3
**Kernel:** 5.15+ (tested on 6.12.x)

---

## What LHSR Actually Is

LHSR is a **Linux Device Mapper target** that provides RAID5/6 with
CRC32c scrubbing and read-side self-healing. It is implemented as a kernel
module (`dm-lhsr`) with userspace tools for administration and recovery.

**What it does well today:**
- RAID0, RAID1, RAID5, RAID6 with correct Reed-Solomon GF(2^8) parity
- Background CRC32c scrubbing with rate limiting
- Read-side reconstruction from parity when a disk fails
- Write-hole protected superblock (backup-first + REQ_FUA)
- Write verification modes (none/simple/full)

**What it does NOT do (yet):**
- SHR flexible disk sizing (Synology SHR is mdraid + LVM — LHSR doesn't do this)
- Persistent anti-bit-rot checksums (checksum cache is ephemeral — lost on module unload)
- Incremental rebuild (full-disk rebuild only; no write-intent bitmap)
- Predictive failure with risk scoring (SMART polling exists, but no model)
- Live block migration (mdadm --grow already does this)
- Firmware failure mitigation (the kernel driver layer already handles quirks)
- Instant recovery with partial array mount (read-side reconstruction works, but no degraded mount)

See [PRODUCTION_READINESS.md](PRODUCTION_READINESS.md) for the full gap analysis.

---

## Quick Start

```bash
# Build
make
sudo make install

# Create a RAID5 array with 3 loopback devices (for testing)
dd if=/dev/zero of=/tmp/disk1 bs=1M count=100
dd if=/dev/zero of=/tmp/disk2 bs=1M count=100
dd if=/dev/zero of=/tmp/disk3 bs=1M count=100
losetup /dev/loop1 /tmp/disk1
losetup /dev/loop2 /tmp/disk2
losetup /dev/loop3 /tmp/disk3

# Load the module and create the target
sudo insmod kernel/dm-lhsr/dm-lhsr.ko
sudo dmsetup create lhsr-test --table "0 204800 lhsr 5 3 /dev/loop1 /dev/loop2 /dev/loop3"

# Status
sudo dmsetup message lhsr-test 0 status

# Clean up
sudo dmsetup remove lhsr-test
sudo rmmod dm-lhsr
```

---

## Feature Matrix

| Feature | Status | Notes |
|---------|--------|-------|
| RAID0 | ✅ Working | Standard striping |
| RAID1 | ✅ Working | Mirror with automatic source selection on rebuild |
| RAID5 | ✅ Working | XOR parity, correct RMW state machine |
| RAID6 | ✅ Working | Reed-Solomon GF(2^8) |
| SHR/SHR2 | ❌ Not implemented | Segment data structures exist but never populated |
| Background scrub | ✅ Working | CRC32c per block, rate-limited |
| Read-side reconstruction | ✅ Working | Parity-read on failed disk I/O |
| Write verification | ✅ Working | none/simple/full modes |
| Superblock persistence | ✅ Working | Primary + backup, CRC32c, generation counter |
| Persistent checksums | ❌ Not implemented | Ephemeral xarray cache only |
| Incremental rebuild | ❌ Not implemented | Full-disk copy only |
| Write-intent bitmap | ❌ Not implemented | Required for incremental rebuild |
| Predictive failure model | ❌ Not implemented | SMART polling exists, no prediction |
| Live block migration | ❌ Not implemented | Use mdadm --grow |
| Firmware mitigation | ❌ Not implemented | Kernel driver quirks handle this |
| Partial-array mount | ❌ Not implemented | Only full assembly supported |

---

## Architecture

```
Filesystem (ext4, xfs, btrfs, etc.)
    ↓
dm-lhsr (kernel DM target)
    ↓
Block devices (sdX, nvmeXnY, or dm-integrity devices)
```

The daemon (`lhsrd`) runs in userspace and handles:
- Disk health monitoring (SMART via smartctl)
- Scrubbing orchestration (via dmsetup message)
- Rebuild orchestration (via dmsetup message)

---

## Requirements

- Linux 5.15+
- Device Mapper (CONFIG_DM)
- kernel headers for building

## Building

```bash
make all              # Full build
make modules          # Just the kernel module
make userspace        # Just userspace tools
make clean
```

## Installation

```bash
sudo make install
# Or manually:
sudo insmod kernel/dm-lhsr/dm-lhsr.ko
sudo cp userspace/cli/lhsrctl /usr/local/bin/
sudo cp userspace/daemon/lhsrd /usr/local/bin/
```

---

## Documentation

| Document | Contents |
|----------|----------|
| [PRODUCTION_READINESS.md](PRODUCTION_READINESS.md) | Honest gap analysis — what works, what doesn't |
| [ROADMAP.md](ROADMAP.md) | Phased implementation plan |
| [docs/technical-specification.md](docs/technical-specification.md) | Original spec (needs update to match reality) |
| [KERNEL_NOTES.md](KERNEL_NOTES.md) | Kernel API notes from development |

---

## Related Projects / Upstream Alternatives

LHSR does not aim to replace these — it integrates with them or builds on top:

- **mdadm**: Linux software RAID (RAID0/1/4/5/6/10, reshape, grow, replace, bitmap v6)
- **dm-integrity**: Per-block checksum storage (Linux 4.12+, use under LHSR for anti-bit-rot)
- **LVM**: Volume management, snapshots, RAID
- **bcache / dm-cache**: Block caching
- **dm-crypt**: Block-level encryption
- **argus-disk**: SMART failure prediction with linear regression
- **Btrfs / ZFS**: Filesystem-level RAID with checksumming

---

## License

GPLv3 — See [LICENSE](LICENSE) file.
