# Changelog

All notable changes to LHSR are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/).

---

## [Unreleased]

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