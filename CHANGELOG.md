# Changelog

All notable changes to LHSR are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/).

---

## [1.4.0] — 2026-06-03 — Phase 0: Honest Foundation

### Fixed (Critical Bugs)
- **Superblock struct divergence (CRITICAL):** `include/lhsr.h` (96 bytes) and
  `kernel/dm-lhsr/dm_lhsr.h` (128 bytes) defined different superblock layouts.
  Unified into single `struct lhsr_superblock` (128 bytes packed) defined ONLY
  in `include/lhsr.h`. Both kernel and userspace include this header.
  `BUILD_BUG_ON(size == 128)` prevents silent layout drift.
  Affected: `include/lhsr.h`, `kernel/dm-lhsr/dm_lhsr.h` (removed duplicate),
  `userspace/recovery/lhsr-scan.c` (removed duplicate).

- **`failed_disks` bitmask race (HIGH):** `u32 failed_disks` read with
  `READ_ONCE` in hot I/O path and written under `sb_sem`. Converted to
  `atomic_long_t` with inline accessor functions (`lhsr_failed_disks_get/set`).
  All 30+ call sites updated to use accessors. 32-disk limit raised to
  64-disks on 64-bit arches as side effect.
  Affected: `struct lhsr_array` in `dm_lhsr.h`, all `arr->failed_disks`
  references in `dm-lhsr.c`.

### Added
- **Compile-time structural invariant checks:** `BUILD_BUG_ON` for superblock
  size (128 bytes), field offsets documented, `atomic_long_t` width check,
  version string printed on module load.
- **Smoke test script:** `tests/smoke-test-raid5.sh` — creates loopback devices,
  loads module, creates RAID5 target, writes/verifies data, clean teardown.
  Requires pre-flight approval to execute.
- **Type compatibility layer:** `include/lhsr.h` handles `__u8`/`__u32`/`__u64`
  correctly in all three contexts: `__KERNEL__` → `<linux/types.h>`,
  `__linux__` → `<linux/types.h>`, otherwise `<stdint.h>` + typedefs.

### Changed
- `Makefile` include path: `-I$(src)/../include` → `-I$(src)/../../include`
  (was pointing at non-existent `kernel/include/`, now correct at project root
  `include/`).
- `include/lhsr.h` rewritten: single packed `struct lhsr_superblock` (128 bytes)
  matching kernel's real layout. Removed vaporware `total_blocks`/`block_size`
  fields. Preprocessor guards for `LHSR_MAGIC`/`LHSR_SB_MAGIC` backward compat.
- `kernel/dm-lhsr/dm_lhsr.h` rewritten: includes `<lhsr.h>` instead of
  duplicating superblock. `struct lhsr_array` moved here from `.c` file.
- `kernel/dm-lhsr/dm-lhsr.c`: removed `struct lhsr_array` definition (moved to
  `.h`), `enum lhsr_raid_type` → `unsigned int` (conflicted with shared header
  `#define` constants), `lhsr_failed_disks_get`/`set` moved to header as inlines.
- `userspace/recovery/lhsr-scan.c`: removed duplicate `struct lhsr_sb`, now
  includes `../../include/lhsr.h`.

### Documentation
- `PRODUCTION_READINESS.md`: Critical bugs table now has "Status" column with
  Phase 0 fixes marked complete.
- `ROADMAP.md`: Phase 0 items 0.1-0.3 marked complete, 0.4 marked as script
  created (not yet run).
- `CHANGELOG.md`: This entry.
- Skills (`lhsr-dev`, `lhsr-testing-safety`): Updated for Phase 0 state.

---

## [1.3.0] — 2026-06-02 — Rebuild Execution

### Added
- **Rebuild execution** (v1.3.0): Actual mirror resync
  - `rebuild_work` function copies data from good disk to replacement
  - 128KB chunk-based rebuilding with rate limiting
  - Automatic source disk selection for RAID1 mirror
  - Progress tracking (current offset, percentage, sectors copied)
  - `rebuild stop` command to abort in-progress rebuild
  - Superblock state update on rebuild completion

---

## [1.2.0] — 2026-05-31 — Production Hardening

### Added
- **Production hardening** (v1.2.0): Enterprise reliability features
  - Mutex + rwsem locking for concurrent I/O safety
  - Atomic superblock writes (backup-first-then-primary for write-hole protection)
  - Fixed mirror write bug (duplicate writes to disk 1)
  - Write verification modes (none/simple/full)
  - Configuration query via `config` message
  - Mutex initialization in constructor, cleanup in destructor

---

## [1.1.0] — 2026-05-28 — CRC32c Scrubbing & Rebuild Tracking

### Added
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

---

## [1.0.4] — 2026-05-12 — Superblock Persistence

### Added
- **Superblock persistence** (v1.0.4): On-disk metadata at 4MB primary + backup at end-8MB
  - CRC32c checksum validation with automatic backup recovery
  - Generation counter for crash recovery ordering
  - Array UUID derived from disk name
  - State persisted on: disk_fail, disk_online, I/O errors, periodic check, module unload

### Planned (see ROADMAP.md for current phasing)
- Phase 1: Persistent checksums — stack on dm-integrity or custom on-disk store
- Phase 2: Incremental rebuild — write-intent bitmap (mdadm v6 compatible)
- Phase 3: Daemon refactor — libdevmapper, sysfs SMART, trend tracking
- Phase 4: Predictive failure — threshold + trend health scores
- Phase 5: Recovery tools — degraded mount, metadata reconstruction
- Phase 6: SHR userspace — NOT YET SCOPED
- Phase 7: Live migration — NOT YET SCOPED

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
| 0 | Honest foundation | Phase 0 (current) |
| 1 | Persistent checksums | Planned |
| 2 | Incremental rebuild | Planned |
| 3 | Daemon refactor | Planned |
| 4 | Predictive failure | Planned |
| 5 | Recovery tools | Planned |
| 6 | SHR userspace | Not yet scoped |
| 7 | Live migration | Not yet scoped |

---

*Version 0.1.0 — Initial release*