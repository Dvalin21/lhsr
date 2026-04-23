# Changelog

All notable changes to LHSR are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/).

---

## [Unreleased]

### Added
- **Scrubber integration** (v1.1.0): Background integrity verification
  - CRC32c per-block checksumming during scrub
  - Rate-limited background scrub with configurable block size (128KB)
  - Corruption detection tracking (scrub_corrupted counter)
  - `scrub start/stop` dmsetup message handlers
  - Scrub progress in status output

- **Rebuild tracking** (v1.1.0): Disk replacement state machine
  - Rebuild states: NONE, PENDING, RUNNING, COMPLETE
  - `rebuild start <disk>` and `rebuild status` message handlers
  - Superblock state updates during rebuild

- **Userspace recovery tool** (v1.1.0): lhsr-scan superblock reader
  - Read and validate superblocks from raw disks
  - Displays array UUID, disk UUID, generation, timestamps
  - Auto-detects primary vs backup superblock locations
  - Reports generation conflicts between primary/backup

- **Superblock persistence** (v1.0.4): On-disk metadata at 4MB primary + backup at end-8MB
  - CRC32c checksum validation with automatic backup recovery
  - Generation counter for crash recovery ordering
  - Array UUID derived from disk name
  - State persisted on: disk_fail, disk_online, I/O errors, periodic check, module unload

### Planned
- Core RAID engine (Phase 1)
- Self-healing engine (Phase 2)
- Anti-bit-rot protection (Phase 3)
- Predictive failure engine (Phase 4)
- Recovery tools (Phase 5)
- Performance optimization (Phase 6)
- Production features (Phase 7)

---

## [0.1.0] — 2026-04-21

### Added

**Core Infrastructure**
- Directory structure created
- Technical specification v1.0
- Kernel module Makefile
- Userspace CLI infrastructure
- Header files (lhsr.h)
- README with feature overview

**On-Disk Format**
- Superblock structure (primary + backup)
- Metadata header
- Segment layout for SHR
- Block entry format
- Distributed metadata design

**CLI Commands (planned)**
- create, add, remove
- status, repair
- scrub, expand

**RAID Types (planned)**
- Single, Mirror
- RAID5, RAID6
- SHR, SHR-2

---

## Roadmap

| Phase | Feature | Status |
|-------|---------|--------|
| 1 | Core RAID | Planned |
| 2 | Self-Healing | Planned |
| 3 | Anti-Bit-Rot | Planned |
| 4 | Predictive | Planned |
| 5 | Recovery | Planned |
| 6 | Performance | Planned |
| 7 | Production | Planned |

---

*Version 0.1.0 — Initial release*