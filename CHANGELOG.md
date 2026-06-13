# Changelog

All notable changes to LHSR are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/).

---

## [5.0.0] — 2026-06-12 — Phase 5: Recovery Tools (5.1-5.3)

### Added

#### Enhanced `lhsr-scan` (5.1)
- **`--deep` flag**: Scans last ~2048 sectors at 16-sector granularity for orphaned
  superblocks (beyond primary/backup positions). Catches superblocks from partial
  writes, misaligned devices, or manual dd.
- **`--json` / `-j` flag**: Machine-parseable JSON output with per-disk superblock
  array including `device`, `total_sectors`, `found`, `superblocks[]` (sector,
  position, generation, version, raid_type, disk_index, csum_valid), summary
  object with counts. Corruption detection via `csum_valid` field.
- **`-v` without device args**: Auto-detects `/dev/sd*`, `/dev/vd*`, `/dev/nvme*`
  block devices (filters partitions) and scans all of them.

#### `lhsrctl recover` (5.2)
- **New `recover` subcommand**: Scans provided block devices for LHSR superblocks,
  cross-references by array UUID, and generates `dmsetup create` commands for
  assembly.
- **Per-array output**: UUID, RAID type, disk count, parity, minimum healthy,
  creation time, generation, per-disk present/online state.
- **Correct table format**: RAID5/6 includes required `<chunk_sects> <stripe_depth>
  <cont_sects>` parameters (defaults: 8 1 8 for 4KB chunks).
- **Degraded mode with dm-zero placeholders**: When fewer than `disk_count`
  devices provided, generates `dmsetup create` commands for dm-zero placeholder
  targets (reads zeros, discards writes) for missing disk positions.
- **Placeholder names**: `lhsr_<array_short>_ph_<disk_idx>` — unique per array and
  short enough for `/dev/mapper/` limits.
- **Safety validation**: Rejects assembly when `present < min_healthy` (e.g., RAID1
  with only 1 disk), rejects unsupported RAID types (SHR/SHR2).

#### `lhsrctl reconstruct` (5.3)
- **New `reconstruct` subcommand**: Offline XOR reconstruction of a missing RAID5/6
  disk from N-1 survivors. Right-static parity layout means XOR of ALL survivors at
  any byte offset directly yields the missing disk's content — no stripe/chunk math
  needed for the XOR operation.
- **`--output <file>` / `-o <file>`**: Writes reconstructed disk image to a file
  (raw format, same size as survivors, sparse)
- **`--chunk-size <sectors>` / `-c <sectors>`**: Override default chunk size (8
  sectors = 4KB) for superblock metadata compatibility. Does NOT affect XOR
  reconstruction (layout-independent).
- **Single-disk reconstruction**: Works for RAID5 (any missing disk, data or parity)
  and RAID6 (single missing disk, data or parity). RAID6 dual-disk needs Reed-Solomon
  (deferred).
- **Auto-detection**: Scans survivors for superblocks, groups by UUID, validates all
  belong to same array, identifies which disk index is missing.
- **Validation**: Rejects mismatched UUID/RAID type/disk count/size, multiple missing
  disks, non-RAID5/6 types, all-disks-present.
- **Superblock**: Writes primary and backup superblocks at correct positions with
  unique disk UUID, correct disk index, highest generation, HEALTHY state, valid CRC32c.
- **Progress**: Reports progress every 256 MB during reconstruction.
- **Usage**: `lhsrctl reconstruct --output /tmp/rec.img /dev/sdb /dev/sdc`
- **Output instructions**: Prints dd command for writing output to replacement disk.

### Changed
- `userspace/recovery/lhsr-scan.c`: +527/-145 lines — deep scan, JSON output,
  auto-detect, corruption detection.
- `userspace/cli/lhsrctl.c`: `cmd_recover()` fully implemented. Removed
  `create_missing_placeholder()` and `cleanup_placeholders()` (bad approach —
  required root+losetup). Replaced with dm-zero placeholders (no root needed,
  proper block devices).

### Build
- Zero new compiler warnings on any target (`-Wall -Wextra -O2 -g`).
- All targets build clean: kernel module (dm-lhsr.ko), daemon (lhsrd), CLI
  (lhsrctl), recovery tool (lhsr-scan).

### Documentation
- `ROADMAP.md`: Phase 5 marked 5.1-5.3 complete, 5.4 in progress.

---

## [4.0.0] — 2026-06-12 — Phase 4: Predictive Failure Health Score, CLI Integration

### Added
- **Composite health score**: New `lhsr-health.c/h` module computes a 0-100
  weighted health score for each disk from SMART values, trend slopes, and error
  history. Scoring: reallocated (-30 max), pending (-30 max), uncorrectable (-40
  max), temperature (>50°C: -10, >60°C: -15), trend warnings (-10 each),
  consecutive errors (-15 max). Labels: >=90 OK, >=70 WARNING, >=40 CRITICAL,
  <40 FAILING.
- **`lhsrctl status` rewritten**: Reads daemon JSON status file (`/run/lhsrd.status`)
  and displays formatted output: version, uptime, arrays, per-disk health score,
  temperature, SMART attributes, and trend warnings. No longer a stub.
- **`lhsrctl predict` rewritten**: Connects to daemon control socket
  (`/run/lhsrd.sock`), queries trend data, and displays per-disk failure
  predictions with estimated time-to-critical for reallocated sectors, pending
  sectors, and temperature. Shows active trend warnings and replacement
  recommendations.
- **Prometheus metrics file**: Daemon writes `/var/lib/lhsrd/metrics.prom` with
  gauge metrics for `lhsr_uptime_seconds`, `lhsr_disk_health`,
  `lhsr_disk_temperature`, `lhsr_reallocated_sectors`, `lhsr_pending_sectors`,
  `lhsr_uncorrectable_sectors`. Compatible with node_exporter textfile collector.
- **Control socket trend response enhanced**: Now includes `health_score`,
  `health_label`, and current SMART values (`temperature`, `reallocated`,
  `pending`, `uncorrectable`) alongside trend slopes — `predict` command is
  self-contained with no need to query multiple sources.

### New files
| File | Description | Lines |
|------|-------------|-------|
| `userspace/daemon/lhsr-health.c` | Composite health score computation | 120 |
| `userspace/daemon/lhsr-health.h` | Health score API header | 55 |

### Changed
- `userspace/daemon/lhsrd.h`: Added `health_score` field to `struct disk_health`.
  Added `LHSRD_METRICS_FILE` define.
- `userspace/daemon/lhsrd.c`: Computes composite health score in
  `check_disk_health()`. Writes Prometheus metrics file every cycle. Creates
  `/var/lib/lhsrd/` directory on startup.
- `userspace/daemon/lhsr-control.c`: `json_disk()` now includes `health_score`
  and `health_label`. `json_trend()` now includes current SMART values.
  `lhsr_control_build_trends()` always emits an entry per disk (even with no
  trend data).
- `userspace/daemon/Makefile`: Added `lhsr-health.o`.
- `userspace/cli/lhsrctl.c`: Added JSON helpers (`json_int`, `json_string`,
  `json_double`, `next_object`), socket helpers (`connect_control_socket`,
  `control_query`), file reader, and duration formatter. `cmd_status()` and
  `cmd_predict()` replaced with real implementations.

### Fixed
- **Slow daemon shutdown blocked on pthread_join()**: Monitor thread (60s sleep)
  and health monitor thread (300s sleep) would block shutdown for the full
  interval after SIGTERM because `pthread_join()` waited for the sleeping thread.
  Fixed: `pthread_cancel()` before each `pthread_join()`, plus `pthread_cleanup_push/pop`
  handlers to safely release mutex if thread is cancelled while holding the lock.
  Shutdown now completes in 37-59ms instead of up to 300s. (`lhsrd.c` +14/-2)

### Test
- **Health score unit test**: `tests/test-health.c` with 31/31 boundary cases
  covering all penalty types (reallocated, pending, uncorrectable, temperature,
  errors), combined penalties, clamped extremes, and all label thresholds
  (OK/WARNING/CRITICAL/FAILING). Run with `make test-health`.

### Build
- Zero new compiler warnings on any target (`-Wall -Wextra -O2 -g`).
- All targets build clean: kernel module (dm-lhsr.ko), daemon (lhsrd), CLI
  (lhsrctl), recovery tool (lhsr-scan).

### Documentation
- `ROADMAP.md`: Phase 4 marked ✅ COMPLETE. Bug fix and test coverage documented.
  Duration updated to 1 session (was estimated 2 weeks). Phase 5 section updated
  with detailed implementation plan.
- `CHANGELOG.md`: This entry.

---

## [3.0.0] — 2026-06-12 — Phase 3: SMART Trend Tracking, Control Socket, systemd

### Added
- **SMART trend database**: SQLite-based daily snapshot storage at
  `/var/lib/lhsrd/trends.db`. Table `smart_snapshots` records reallocated/pending/
  uncorrectable sectors, temperature, power-on hours, wear level, and composite
  health score for each disk. Linear regression on last 30 data points computes
  trend slopes per attribute.
- **Trend warnings**: Automatic warnings when SMART attribute trends exceed
  thresholds (reallocated >1/day, pending >1/day, temperature >2°C/day).
  Warnings appear in JSON status file and control socket responses.
- **Control socket**: Unix domain socket at `/run/lhsrd.sock`. JSON command/
  response protocol. Commands: `ping`, `status`, `trends`. Thread-safe under
  daemon state lock.
- **JSON status file**: `/run/lhsrd.status` converted from ad-hoc text to valid
  JSON. Includes version, uptime, array list (name/uuid/type/disks/working), and
  disk list (device/health/temp/SMART attributes/failed/trend_warning).
- **systemd unit**: `systemd/lhsrd.service` with restart-on-failure, security
  hardening (`NoNewPrivileges`, `PrivateTmp`, `ProtectSystem`,
  `CapabilityBoundingSet`). Installed via Makefile to `/usr/lib/systemd/system/`.
- **Config support**: New config keys: `trend_enabled`, `trend_db_path`,
  `trend_snapshot_interval`. Backward-compatible with existing config files.

### New files
| File | Description | Lines |
|------|-------------|-------|
| `userspace/daemon/lhsr-trend.c` | SQLite trend database (init/record/query/warning/close) | 310 |
| `userspace/daemon/lhsr-trend.h` | Trend API header | 60 |
| `userspace/daemon/lhsr-control.c` | Unix domain socket control interface | 300 |
| `userspace/daemon/lhsr-control.h` | Control socket API header | 55 |
| `systemd/lhsrd.service` | systemd unit file | 35 |

### Changed
- `userspace/daemon/lhsrd.h`: Added `trend_enabled`, `trend_db_path`,
  `trend_snapshot_interval` to `struct daemon_config`. Added `start_time` to
  `struct daemon_state`. Added `LHSRD_SOCKET_FILE` define.
- `userspace/daemon/lhsrd.c`: Integrated trend DB init (startup), trend recording
  (health monitor thread), control socket (parallel thread), JSON status file
  (was text format), and clean shutdown (trend DB close, control socket stop).
- `userspace/daemon/lhsr-config.c`: `lhsr_config_defaults()` sets trend defaults.
  Config parser accepts trend keys. `lhsr_config_save()` writes trend settings.
- `userspace/daemon/Makefile`: Added `lhsr-trend.o`, `lhsr-control.o`, `-lsqlite3`.
  Install target creates `/var/lib/lhsrd/` and installs systemd unit.
- `userspace/config/lhsrd.conf.example`: Updated with trend configuration
  options and defaults.

### Build
- Zero new compiler warnings on any target (`-Wall -Wextra -O2 -g`).
- Dependencies: `libsqlite3-dev` (new), `libdevmapper-dev`, `libpthread`.
- All three components build clean: kernel module (dm-lhsr.ko), userspace daemon
  (lhsrd), recovery tool (lhsr-scan).

### Documentation
- `ROADMAP.md`: Phase 3 marked ✅ COMPLETE. Duration updated to 1 day (was
  estimated 2-3 weeks — 3.1 and 3.2 were already implemented). New Phase 3
  completion section with detailed file summary.
- `CHANGELOG.md`: This entry.

---

### Added
- **Write-Intent Bitmap (WIB)**: Persistent on-disk bitmap tracking written chunks
  (1MB granularity). Stored in superblock metadata area on each disk. On-disk format
  reuses `struct lhsr_bitmap_page` (seq + CRC32c + bits) from write-hole journal.
  New functions: `lhsr_wib_init/destroy/set/clear/test/clear_all/flush/load/write_page`
  plus helpers `lhsr_wib_nbits/npages/page_sector` (~550 lines total).
- **Superblock v2**: On-disk superblock bumped to version 2. `LHSR_META2_SECTORS`
  macro for dynamic metadata reservation (base + WIB pages). v1 superblocks rejected
  at assembly with clear error message.
- **Incremental rebuild**: In `rebuild_work()`, RAID1 rebuild checks WIB and skips
  clean chunks. Only dirty (written) regions are copied. RAID5/6 always reconstruct
  from parity (conservative — WIB bits never cleared in RAID5/6 write path).
- **WIB in write paths**: Both mirror and RAID5/6 write paths call `lhsr_wib_set()`
  on every write to mark the affected chunk dirty.

### Fixed
- **CRC32c seed mismatch**: `lhsr_wib_load()` used `crc32c(~0, ...)` but write path
  used `__crc32c_le(0, ...)` — different seeds produce different CRC values. Fixed
  both to use `__crc32c_le(0, ...)` consistently.
- **Cosmetic rebuild message**: "Rebuild complete: %llu sectors copied" changed to
  "sectors processed" because `rebuild_verified` includes WIB-skipped regions, not
  just actually copied sectors.

### Verified
- **RAID1 WIB rebuild test (VM, 2026-06-11)**: 2×100MB loopback devices, LHSR mirror.
  1. Write dirty/clean/dirty pattern: 2MB at offset 0, skip 2MB, 1MB at offset 4MB
  2. SHA256 baseline recorded: `003545fc...`
  3. Fail disk 0 → array DEGRADED, reads failover to disk 1
  4. Start rebuild → dmesg shows WIB-aware skip messages:
     `"rebuild: skip sector X (clean WIB bit, already handled by live write)"`
  5. Rebuild complete: status `OK 2/2 err=0 fail=0`
  6. SHA256 verify: **PASS** — checksum matches baseline
  7. Zero dmesg errors, warnings, call traces, or BUGs
- **RAID5 smoke test (VM, 2026-06-11)**: 3×64MB loopback devices, 4MB write at 1MB
  offset, SHA256 read-back verify **PASS**.

### Changed
- `ROADMAP.md`: Phase 2 marked ✅ COMPLETE. Timeline updated (Phase 2 took 2 days
  vs estimated 2-3 weeks).
- `PRODUCTION_READINESS.md`: Feature #7 (incremental rebuild) updated from
  ❌ VAPORWARE to ✅ FUNCTIONAL. New Phase 2 testing section.
- Version in module: `dm-lhsr.ko` now prints "1.4.0" on load (was 1.3.0 in Phase 0).

### Documentation
- `include/lhsr.h`: WIB constants (chunk size, page size, sectors per page, etc.)
  and `LHSR_META2_SECTORS` macro documented.
- `kernel/dm-lhsr/dm_lhsr.h`: WIB fields in `struct lhsr_array` documented with
  locking rules.

---

## [1.5.0] — 2026-06-08 — Phase 1: dm-integrity Stacking (Persistent Checksums)

### Added
- **`integrity_below` flag**: `bool integrity_below` in `struct lhsr_array`.
  Constructor parses optional trailing `integrity` keyword from table line.
  Config message shows `integrity=%d`, status table shows `INTEGRITY=%d`.
- **Two-mode scrubber**: When `integrity_below == true`, scrubber reads blocks
  via BIO and relies on dm-integrity per-block CRC32c verification (no LHSR-side
  CRC computation, no ephemeral xarray). Legacy mode unchanged.
- **`scripts/setup-dm-integrity.sh`**: Helper script for creating/removing
  dm-integrity devices using `integritysetup format` + `integritysetup open`
  (raw `dmsetup create` with integrity target fails with "Invalid tag size").

### Verified
- **VM testing (6.12.90+deb13.1-amd64)**: 4 loopback devices → dm-integrity
  (CRC32c, 4K blocks, 4-byte tags) → LHSR RAID5 with `integrity` flag.
  - Write 10MB random data, read back SHA256 — checksum match
  - Scrub in integrity mode — dmesg: `Scrub (integrity): block at offset... verified OK`
  - Config query returns `integrity=1`, status shows `INTEGRITY=1`
  - RAID5 parity reconstruction handles corrupted sectors transparently

### Changed
- `PRODUCTION_READINESS.md`: Features #2 (self-healing) and #3 (anti-bit-rot)
  updated to ✅ FUNCTIONAL via dm-integrity stacking. Ephemeral checksum cache
  marked as Phase 1 FIXED. New Phase 1 testing section.
- `ROADMAP.md`: Phase 1 marked ✅ COMPLETE. Duration updated to 1 week.
- `scripts/setup-dm-integrity.sh`: Rewritten from raw `dmsetup create` to
  `integritysetup format` + `integritysetup open` two-step process.

---

## [1.4.1] — 2026-06-08 — Phase 0 Complete / RAID5 Smoke Test

### Added
- **RAID5 smoke test executed (PASS):** 3×64MB loopback devices, write 4MB at 1MB
  offset, read-back SHA256 checksum verified match, zero dmesg errors.
  Config: `uuid=6260e0e raid=2 disks=3 state=2 gen=1`.
- **LHSR module now prints version 1.4.0** on load (was 1.3.0).

### Fixed
- **Format specifier warning:** `bio->bi_iter.bi_size` is `unsigned int`, not
  `size_t`. Cast to `(size_t)` in DMINFO to suppress `-Wformat` warning.

### Changed
- `PRODUCTION_READINESS.md`, `ROADMAP.md`: Phase 0 marked fully complete,
  RAID5 smoke test result recorded.

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
| 0 | Honest foundation | ✅ Complete |
| 1 | Persistent checksums | ✅ Complete |
| 2 | Incremental rebuild | ✅ Complete |
| 3 | Daemon refactor | ✅ Complete |
| 4 | Predictive failure | Planned |
| 5 | Recovery tools | Planned |
| 6 | SHR userspace | Not yet scoped |
| 7 | Live migration | Not yet scoped |

---

*Version 0.1.0 — Initial release*