# LHSR — Linux Hybrid Self-Healing RAID

## Technical Specification v1.0

> **⚠️ HONESTY NOTICE**: This document was written as a vision document before
> most features were implemented. For the current implementation status of every
> claimed feature, see [PRODUCTION_READINESS.md](../PRODUCTION_READINESS.md).
>
> Key facts that contradict this document:
> - Only RAID0/1/5/6 are implemented. SHR/SHR2 data structures exist but are
>   never populated (zero implementation).
> - Scrubbing with CRC32c works, but checksums are **not persisted** (lost on
>   module unload). Anti-bit-rot requires persistent checksums.
> - No prediction model exists. The `predict` command is a stub.
> - Full-disk rebuild only. No incremental rebuild implementation.
> - Live block migration, firmware mitigation not implemented.
> - Two superblock structs exist (`include/lhsr.h` vs `kernel/dm-lhsr/dm_lhsr.h`)
>   and they differ in size and fields — this is a latent corruption bug.

**Target Environment:** Linux production servers, Proxmox VE, enterprise storage arrays

---

## 1. Core Design Goals (STATUS SUMMARY)

| # | Goal | Status | Notes |
|---|------|--------|-------|
| 1 | SHR-Style Flexible Disk Sizes | ❌ Vaporware | Segment data structures exist but never populated |
| 2 | Self-Healing Engine | ⚠️ Partial | Scrub + read-repair work, but checksum cache is ephemeral |
| 3 | Anti-Bit-Rot Protection | ❌ Vaporware | dm-integrity exists upstream; ephemeral xarray is not production |
| 4 | Predictive Failure Engine | ⚠️ Basic | SMART polling exists, no prediction model |
| 5 | Live Block Migration | ❌ Vaporware | mdadm --grow already does this |
| 6 | Instant RAID Recovery | ⚠️ Basic | Read-side reconstruction works; no partial mount |
| 7 | Incremental Rebuild | ❌ Vaporware | Full-disk rebuild only; no write-intent bitmap |
| 8 | Firmware-Aware Protection | ❌ Vaporware | Kernel driver quirks already handle this |

---

## 2. Architecture Overview

```
+----------------------------------+
| LHSR Management Layer (CLI)      |
+----------------------------------+
| LHSR Virtual RAID Engine           |
+----------------------------------+
| Block Mapping Layer              |
+----------------------------------+
| Disk Abstraction Layer           |
+----------------------------------+
| Raw Disks / NVMe / SSD / HDD    |
+----------------------------------+
```

### Hybrid Architecture

- **Kernel Layer (dm-lhsr)** — Device Mapper for performance-critical I/O
- **Userspace Daemon (lhsrd)** — Background monitoring, scrubbing, rebuild
- **CLI Tool (lhsrctl)** — Administrative interface
- **Recovery Tools** — Scan, reconstruct, repair utilities

---

## 3. RAID Types Supported

| Type | Description | Min Disks | Use Case |
|------|--------------|----------|----------|
| Single | 1 disk | 1 | JBOD |
| Mirror | 2-way mirroring | 2 | Performance |
| RAID5 | XOR parity | 3+ | Standard |
| RAID6 | Dual parity | 4+ | High availability |
| SHR | Synology Hybrid RAID | 2+ | Mixed disks |
| SHR-2 | Dual parity SHR | 4+ | Mixed disks + parity |

---

## 4. Core Features

### 4.1 SHR-Like Flexible Disk Sizes

Traditional RAID wastes space with mixed disk sizes. LHSR optimizes layout:

**Example Layout:**
```
Disk1: 12TB
Disk2: 8TB  
Disk3: 4TB
Disk4: 4TB

Layout:
├── 4TB RAID5 across all disks
├── 4TB RAID1 across Disk1 + Disk2
└── 4TB leftover on Disk1
```

### 4.2 Self-Healing Engine

**Workflow:**
1. Read block
2. Verify checksum
3. If mismatch → Pull parity/replica
4. Repair block
5. Rewrite block
6. Log event

**Modes:**
| Mode | Behavior |
|------|----------|
| Passive | Verify on read |
| Scheduled | Nightly scrub |
| Aggressive | Continuous verify |

### 4.3 Anti-Bit-Rot Protection

**Detection:** Block-level checksums with verification

**Repair:**
1. Detect corruption
2. Compare replicas
3. Find valid block
4. Repair automatically

**Multi-Disk Detection:**
```
RAID5: Disk1(corrupted) + Disk2(good) + Disk3(parity)
    → Detect → Repair → Rewrite
```

### 4.4 Predictive Failure Engine

**Monitors:**
- SMART data
- I/O latency
- Sector retry count
- Thermal data

**Output:**
```
$ lhsr status
Disk 2: 82% failure risk
Recommendation: migrate data
```

### 4.5 Live Block Migration

Detects SMART errors → Migrates data live → Marks disk degraded proactively

### 4.6 Instant RAID Recovery

```
$ lhsr scan --reconstruct
Scans all disks → Rebuilds RAID map → Reconstructs volume
```

### 4.7 Incremental Rebuild

vs traditional rebuild:
- Traditional: Rebuild entire disk
- LHSR: Rebuild only used blocks (e.g., 3TB instead of 10TB)

### 4.8 Firmware Mitigation

**Features:**
- Drive fingerprinting
- Known bad firmware database
- Staggered drive usage
- Write verification mode
- Firmware crash detection

---

## 5. On-Disk Format

### Disk Layout

```
+----------------------+
| Superblock (Primary) | 4MB
+----------------------+
| Metadata Area        |
+----------------------+
| Allocation Map      |
+----------------------+
| Data Blocks         |
+----------------------+
| Journal             |
+----------------------+
| Superblock (Backup) | End-4MB
+----------------------+
```

### Superblock Structure

```c
struct lhsr_superblock {
    char     magic[8];          // "LHSRDISK"
    uint32   version;
    uint64   array_uuid;
    uint64   disk_uuid;
    uint64   creation_time;
    uint64   last_update;
    uint32   disk_role;        // 0=data,1=parity,2=metadata,3=spare
    uint32   disk_state;        // 0=healthy,1=degraded,2=rebuilding
    uint64   total_blocks;
    uint64   block_size;
    uint64   checksum;
};
```

### Metadata Design

- Distributed metadata: Every disk contains full metadata copy
- No single point of failure
- Corruption-resistant

---

## 6. CLI Commands

| Command | Description |
|---------|-------------|
| `lhsrctl create` | Create new array |
| `lhsrctl add` | Add disk to array |
| `lhsrctl remove` | Remove disk |
| `lhsrctl status` | Show array status |
| `lhsrctl repair` | Repair array |
| `lhsrctl scrub` | Run scrub |
| `lhsrctl expand` | Expand array |
| `lhsrctl rebalance` | Rebalance layout |
| `lhsrctl predict` | Show disk failure predictions |
| `lhsrctl snapshot` | Create/restore snapshot |
| `lhsrctl recover` | Instant recovery mount |

---

## 7. Performance Targets

| Metric | Target |
|--------|--------|
| Sequential throughput | ≥ mdadm |
| Random IO latency | ≤ +5–10% overhead |
| CPU overhead | < 10% |
| Rebuild speed | Faster than mdadm |
| Scrub impact | Near-zero production impact |

---

## 8. Filesystem Support

- ext4
- XFS
- Btrfs
- ZFS (block backend mode)

---

## 9. Comparison

| Feature | mdadm | ZFS | SHR | LHSR |
|---------|-------|-----|-----|------|
| Mixed disk sizes | No | Limited | Yes | Yes |
| Self healing | No | Yes | Partial | Yes |
| Anti bit-rot | No | Yes | No | Yes |
| Predictive migration | No | No | No | Yes |
| Instant recovery | No | Yes | No | Yes |
| Incremental rebuild | No | No | No | Yes |
| Firmware protection | No | No | No | Yes |
| Linux native | Yes | Yes | No | Yes |

---

## 10. Implementation Phases

### Phase 1 — MVP
- Basic RAID engine
- SHR-like disk sizing
- Create/mount

### Phase 2 — Self Healing
- Checksums
- Block validation
- Auto repair

### Phase 3 — Anti Bit-Rot
- Scrub engine
- Checksum tree
- Background integrity

### Phase 4 — Predictive Failure
- SMART monitoring
- Migration

### Phase 5 — Recovery Engine
- Scan
- Reconstruct
- Partial recovery

### Phase 6 — Performance
- Caching
- Parallel IO
- NVMe metadata

### Phase 7 — Production
- Metrics
- Alerting
- Web UI (optional)

---

## 11. Dependencies

### Kernel
- Linux 5.15+
- Device Mapper (dm-mod)

### Userspace
- libblkid
- libuuid
- libconfig
- libpthread

### Build
- make
- gcc/clang
- rustc (optional)

---

## 12. License

GPLv3 — Enterprise open source

---

## 13. Repository Structure

```
lhsr/
├── kernel/dm-lhsr/       # Device Mapper module
├── userspace/             # CLI, daemon, recovery
├── lib/                   # Shared libraries
├── include/               # Headers
├── tests/                 # Unit/integration tests
├── tools/                 # Benchmark tools
├── docs/                  # Documentation
└── packaging/            # DEB/RPM/Docker
```

---

## Appendix A: Firmware Risk Database

```json
{
  "Samsung 980 Pro": {
    "bad_firmware": ["1B2QGXA7"],
    "risk": "high"
  },
  "Seagate Barracuda": {
    "bad_firmware": ["CC41"],
    "risk": "medium"
  }
}
```

---

## Appendix B: Prometheus Metrics

- `lhsr_disk_health` — Disk health score
- `lhsr_rebuild_progress` — Rebuild percentage
- `lhsr_scrub_errors` — Scrub error count
- `lhsr_io_latency` — I/O latency
- `lhsr_bitrot_detected_total` — Bit-rot events
- `lhsr_bitrot_repaired_total` — Auto-repairs

---

*Specification Version: 1.0*
*Date: 2026-04-21*
*Project: LHSR — Linux Hybrid Self-Healing RAID*