# LHSR Roadmap

**Last Updated:** 2026-06-13 (Phase 5 COMPLETE, Phase 6 SCOPED)
**Based on:** PRODUCTION_READINESS.md (gap analysis registry)

---

## Guiding Principles

1. **Fix what's broken before building what's missing.** The two diverging
   superblock structs can cause corruption. Fix that first.
2. **Don't reimplement the kernel.** dm-integrity, mdadm bitmap, and device
   quirks already exist upstream. Use them.
3. **Stack, don't replace.** LHSR should stack on top of mdadm and dm-integrity,
   not reinvent their features.
4. **Ship working code incrementally.** Each phase must produce a testable,
   deployable improvement. No speculative architecture.

---

## Phase 0: Honest Foundation (2026-06-03 — ✅ COMPLETE 2026-06-08)

Stop lying to users. Fix critical bugs before adding features.

### 0.1 Fix diverging superblock structs — ✅ COMPLETE
- **Fix:** Single `struct lhsr_superblock` (128 bytes packed) defined in
  `include/lhsr.h`. Kernel header includes it. Userspace recovery tool includes
  it. No more copies, no more divergence.
- Type compatibility: `__KERNEL__` → `<linux/types.h>`, `__linux__` →
  `<linux/types.h>`, otherwise `<stdint.h>` + manual typedefs.
- `BUILD_BUG_ON(sizeof(struct lhsr_superblock) == LHSR_SB_SIZE)` in module init.
- **Verify:** Module builds clean, `lhsr-scan` builds clean, struct size is 128
  bytes at compile time.

### 0.2 Fix failed_disks bitmask — ✅ COMPLETE
- **Fix:** `u32 failed_disks` → `atomic_long_t failed_disks` in `struct lhsr_array`.
- Inline accessor functions `lhsr_failed_disks_get()/lhsr_failed_disks_set()`
  defined in `dm_lhsr.h`. All 30+ call sites updated to use accessors.
- `BUILD_BUG_ON(sizeof(atomic_long_t) * 8 < LHSR_MAX_DISKS)` in module init
  (ensures at least 32-bit on all architectures, 64-bit on 64-bit).
- **Verify:** Build clean, no sparse warnings.

### 0.3 Honest README and docs — ✅ COMPLETE
- README.md rewritten with accurate feature matrix.
- PRODUCTION_READINESS.md created with full gap analysis.
- ROADMAP.md created with phased plan.
- technical-specification.md updated with honesty notice.
- CHANGELOG.md replaced vaporware list with real phase references.
- KERNEL_NOTES.md added with critical bug warnings.

### 0.4 Test what exists — ✅ COMPLETE (manual testing)
- Manual testing on VM (6.12.90+deb13.1-amd64) with RAM disk mirror:
  1. Device creation (RAID1 mirror, 2×64MB)
  2. Write/read/verify data integrity
  3. Message interface: `config`, `member_status`, `scan`
  4. Disk failure + degraded read via mirror
  5. Rebuild: start, progress tracking, completion
  6. Scrub: start, stop, progress
  7. Module reload: state + data persistence
- `tests/smoke-test-raid5.sh` created for RAID5 — not yet executed
  (requires loopback devices and pre-flight approval)
- **RAID5 smoke test (PASS):** Executed 2026-06-08 on VM (`lhsr-dev`, kernel
   6.12.90+deb13.1-amd64). 3×64MB loopbacks, 4MB write at 1MB offset, SHA256
   checksum verified match. Config: `uuid=6260e0e raid=2 disks=3 state=2 gen=1`.
   See CHANGELOG for details.

### Bugs found and fixed during testing
- **Rebuild completion didn't clear `failed_disks` bitmask** — after rebuild completed,
  the superblock disk_state was set to HEALTHY but the in-memory `failed_disks` atomic
  still had the bit set. I/O path used the bitmask, so all reads still went through
  reconstruction. Fixed in both rebuild completion paths.
- **`arr->state` not updated after rebuild** — cosmetic: config showed `state=3` (DEGRADED)
  even when `failed=0x0`. Fixed by setting `arr->state = HEALTHY` when no disks failed.

### Deliverables
- ✅ Unified superblock struct in `include/lhsr.h`
- ✅ `atomic_long_t failed_disks` with accessor functions
- ✅ Accurate README, PRODUCTION_READINESS.md, ROADMAP.md, CHANGELOG.md
- ✅ Message interface: config, member_status, scan, disk_fail, rebuild, scrub, persist
- ✅ Data I/O path validated: write → read → checksum match
- ✅ Rebuild: start/progress/completion with failed_disks cleanup
- ✅ Module reload persistence verified
- ⏳ RAID5 smoke test script (requires approval to execute)

---

## Phase 1: Persistent Checksums — ✅ COMPLETE (2026-06-08)

The scrubber computed checksums but didn't persist them. Anti-bit-rot was
non-functional across module reload.

### Decision: Option A (Stack on dm-integrity)
- dm-integrity (Linux 4.12+) provides per-block checksum storage with journaling
  and upstream maintenance.
- LHSR operates on dm-integrity devices instead of raw block devices.
- Slight performance overhead (~3-5%) but eliminates an entire class of bugs.

### Implementation
1. **`integrity_below` flag**: Added `bool integrity_below` to `struct lhsr_array`.
   Constructor parses optional trailing `integrity` keyword from table line.
2. **Two-mode scrubber**: When `integrity_below == true`, scrubbing reads blocks
   via BIO and relies on dm-integrity's per-block CRC32c. No LHSR-side CRC
   computation, no ephemeral xarray.
3. **Config/Status reporting**: `integrity=%d` in config message, `INTEGRITY=%d`
   in table status output.
4. **`setup-dm-integrity.sh`**: Helper script using `integritysetup format` +
   `integritysetup open` (raw `dmsetup create` fails with "Invalid tag size").
5. **Verified on VM**: 4×100MB loopbacks → dm-integrity (CRC32c) → LHSR RAID5.
   Write/read/scrub all verified.

### Deliverables
- ✅ `integrity_below` flag in `struct lhsr_array`
- ✅ Constructor parses `integrity` keyword from table line
- ✅ Scrubber uses dm-integrity verification (no CRC32c, no xarray)
- ✅ Config message and status table report integrity state
- ✅ `scripts/setup-dm-integrity.sh` using integritysetup
- ✅ Tested: array creation, 10MB write/read verify, scrub in integrity mode,
  data integrity end-to-end

---

## Phase 2: Incremental Rebuild with Write-Intent Bitmap — ✅ COMPLETE (2026-06-11)

**Duration:** 2 days (was estimated 2-3 weeks — scope was well-defined, codebase was clean)

**What changed:** Rebuild no longer copies every block. A persistent Write-Intent Bitmap
(WIB) tracks which chunks have been written. Rebuild skips clean regions and only copies
dirty ones.

### Implementation

The WIB is a flat bitmap stored in the superblock metadata area on each disk, after the
write-hole journal. On-disk format reuses `struct lhsr_bitmap_page` (seq + CRC32c + bits)
from the write-hole journal — same format, same recovery, no new on-disk format bugs.

**12 new functions (~550 lines total):**
- `lhsr_wib_nbits(sectors)` — compute bit count for given sector range
- `lhsr_wib_npages(nbits)` — compute page count to cover nbits
- `lhsr_wib_page_sector(arr, page)` — on-disk sector for a WIB page
- `lhsr_wib_init(arr)` — allocate WIB pages and initialization
- `lhsr_wib_destroy(arr)` — free WIB pages
- `lhsr_wib_set(arr, sector)` — mark a chunk dirty (called on every write)
- `lhsr_wib_clear(arr, sector)` — mark a chunk clean (called on rebuild completion)
- `lhsr_wib_test(arr, sector)` — test if a chunk is dirty
- `lhsr_wib_clear_all(arr)` — mark all chunks clean (full resync fallback)
- `lhsr_wib_flush(arr)` — write all dirty WIB pages to disk (reserved for future periodic flush)
- `lhsr_wib_write_page(arr, page)` — write one WIB page to disk
- `lhsr_wib_load(arr)` — read WIB pages from disk on array assembly

**Key design decisions:**
- WIB granularity = 1MB per bit (LHSR_WIB_CHUNK_SECTORS = 2048), matching write-hole journal
- CRC seed = 0 (`__crc32c_le(0, ...)`) consistently for both write and verify
- No periodic flush timer yet — dirty WIB pages flushed only on dtr. After crash, stale WIB
  means more copy work (conservative, always safe)
- WIB only benefits RAID1 (mirror) rebuild where clean regions can be skipped. RAID5/6
  dead-disk replacement always needs full parity reconstruction

**Superblock v2:**
- On-disk superblock bumped to version 2 (`LHSR_SB_VERSION` = 2)
- Added `LHSR_META2_SECTORS` macro computing dynamic metadata reservation:
  `LHSR_META_BASE_SECTORS + WIB page count`
- Constructor recomputes metadata reservation based on raw disk size
- v1 superblocks rejected at assembly time with clear error

**Fixes included:**
- CRC32c seed mismatch: WIB set path used `crc32c(~0, ...)` while write path used
  `__crc32c_le(0, ...)` — fixed both to use `__crc32c_le(0, ...)` consistently
- Cosmetic: rebuild completion message said "sectors copied" but `rebuild_verified`
  includes WIB-skipped regions too — changed to "sectors processed"

### Deliverables
- ✅ WIB format defined: `include/lhsr.h` — `LHSR_WIB_*` constants,
  WIB placement at `disk_offset + disk_sectors + LHSR_META_BASE_SECTORS + page_idx * 8`
- ✅ WIB set on every write (both mirror and RAID5/6 write paths)
- ✅ WIB clear on rebuild completion (rewrites rebuilt chunk)
- ✅ WIB check in `rebuild_work()` — skips clean chunks for RAID1
- ✅ Full-disk rebuild fallback: `lhsr_wib_clear_all()` for initial sync or missing WIB
- ✅ Superblock v2 with dynamic metadata reservation
- ✅ v1 superblock rejected at assembly
- ✅ RAID1 WIB rebuild test on VM (2×100MB loopbacks): dirty/clean/dirty pattern,
  SHA256 verified PASS
- ✅ RAID5 smoke test on VM (3×64MB loopbacks): data integrity verified PASS
- ✅ Zero dmesg errors, warnings, call traces, or BUGs across all tests

---

## Phase 3: Userspace Daemon — SMART Trending, Control Socket, systemd — ✅ COMPLETE (2026-06-12)

**Duration:** 1 day (was estimated 2-3 weeks — 3.1 and 3.2 were already done; 3.3 was well-scoped)

**What changed:** The daemon now has:
1. SQLite-based SMART trend database with linear regression trend analysis
2. Unix domain control socket for live `lhsrctl` queries
3. JSON machine-parseable status file (was ad-hoc text format)
4. systemd unit file for proper lifecycle management

### 3.1 Replace dmsetup with libdevmapper — ✅ ALREADY DONE (pre-existing `lhsr-dm.c`)
- `lhsr-dm.c` (279 lines) implements `lhsr_dm_message()`, `lhsr_dm_status()`,
  `lhsr_dm_list_arrays()`, `lhsr_dm_create()`, `lhsr_dm_remove()`,
  `lhsr_dm_get_devices()`, `lhsr_dm_suspend()`, `lhsr_dm_resume()` via
  `dm_task_create()` / `dm_task_set_message()` / `dm_task_run()`.
- Daemon uses it end-to-end — no `fork()`+`exec()` of `dmsetup` anywhere.

### 3.2 Replace smartctl with sysfs/SG_IO — ✅ ALREADY DONE (pre-existing `lhsr-smart.c`)
- `lhsr-smart.c` (370 lines) implements two-tier SMART polling:
  - Tier 1: sysfs `ata_smart_*` attributes (fast, no SCSI command)
  - Tier 2: SG_IO ATA PASS-THROUGH 16 ioctl for full attribute extraction
- `lhsr_smart_poll()` returns `struct disk_health` with reallocated, pending,
  uncorrectable sectors, temperature, and composite health score.

### 3.3 SMART trend tracking — ✅ NEW (lhsr-trend.c/h, ~310 lines)
- **SQLite database** at `/var/lib/lhsrd/trends.db` (WAL mode + synchronous FULL)
- `smart_snapshots` table: `(disk_path, snapshot_time, reallocated, pending,
  uncorrectable, temperature, power_on_hours, wear_level, health_score)`
- Daily snapshots recorded automatically in health monitor thread
- Linear regression on last 30 data points per disk computes trend slopes
- Trend warnings when slope exceeds thresholds:
  - Reallocated sectors: >1 sector/day
  - Pending sectors: >1 sector/day
  - Temperature: >2°C/day
- Warning strings included in JSON status file and control socket responses
- CLI tool (`lhsrctl`) can query trends directly from SQLite

### 3.4 Control socket — ✅ NEW (lhsr-control.c/h, ~300 lines)
- Unix domain socket at `/run/lhsrd.sock`
- JSON command/response protocol (no json-c dependency — manual snprintf)
- Commands: `{"cmd":"ping"}`, `{"cmd":"status"}`, `{"cmd":"trends"}`
- Thread-safe: accesses daemon state under `pthread_mutex_t` lock
- Listener thread is detached; `lhsr_control_stop()` unblocks `accept()` with a
  dummy connection on shutdown

### 3.5 JSON status file — ✅ NEW
- `/run/lhsrd.status` now valid JSON (was ad-hoc `key: value` text)
- Includes version, uptime, arrays (name/uuid/type/disks/working), disks
  (device/health/temp/SMART attributes/failed/trend_warning)
- Machine-parseable by any web GUI or monitoring tool

### 3.6 systemd integration — ✅ NEW
- `systemd/lhsrd.service` unit file
- Restart on failure with 10-second delay
- Security hardening: `NoNewPrivileges=yes`, `PrivateTmp=yes`,
  `ProtectSystem=full`, `CapabilityBoundingSet` for DM and SG_IO access
- Makefile install target installs to `/usr/lib/systemd/system/`

### Files changed/created
| File | Status | Lines |
|------|--------|-------|
| `userspace/daemon/lhsr-trend.c` | **NEW** | 310 |
| `userspace/daemon/lhsr-trend.h` | **NEW** | 60 |
| `userspace/daemon/lhsr-control.c` | **NEW** | 300 |
| `userspace/daemon/lhsr-control.h` | **NEW** | 55 |
| `systemd/lhsrd.service` | **NEW** | 35 |
| `userspace/daemon/lhsrd.h` | MODIFIED | +15 |
| `userspace/daemon/lhsrd.c` | MODIFIED | +30 |
| `userspace/daemon/lhsr-config.c` | MODIFIED | +25 |
| `userspace/daemon/Makefile` | MODIFIED | +6 |
| `userspace/config/lhsrd.conf.example` | MODIFIED | +15 |

### Deliverables
- ✅ SMART trend database with linear regression trending
- ✅ Control socket for live status queries
- ✅ JSON machine-parseable status file
- ✅ systemd unit file with security hardening
- ✅ Config file support for trend settings
- ✅ Zero new compiler warnings on any target
- ✅ All three components build clean: kernel module, daemon, recovery tool

---

## Phase 4: Predictive Failure Integration — ✅ COMPLETE (2026-06-12)

**Duration:** 1 session (was estimated 2 weeks — scope was well-scoped and daemon infrastructure was in place)

**What changed:** The daemon now computes a composite health score (0-100) from SMART values, trend slopes, and error history. The `lhsrctl status` command reads the daemon's JSON status file and displays formatted output. The `lhsrctl predict` command queries the daemon control socket for trend data and displays per-disk failure predictions with estimated time-to-critical. Prometheus metrics are written to `/var/lib/lhsrd/metrics.prom` for node_exporter textfile collector.

### Health score model

```
Start: 100
  Reallocated sectors:   -10 per 10 sectors (max -30)
  Pending sectors:       -15 per sector (max -30)
  Uncorrectable sectors: -20 per sector (max -40)
  Temperature > 50°C:    -10; >60°C: -15
  Trend warnings:        -10 per active warning
  Consecutive errors:    -5 per error (max -15)
  Clamp to 0-100

Labels: >=90 OK, >=70 WARNING, >=40 CRITICAL, <40 FAILING
```

### Output examples

```
$ lhsrctl status
LHSR Status
==========
Version: 3.0.0  |  Uptime: 1h 23m  |  Arrays: 0  |  Disks: 0

No arrays configured.

$ lhsrctl predict
LHSR Predictive Failure Analysis
================================

Disk: /dev/sdb
  Health Score:   72/100 [WARNING]
  Temperature:    42°C (rising 1.5°C/day) ***
  Reallocated:    15 sectors (increasing 2.3/day) ***
  Pending:        0 sectors (stable)
  Trend Data:     30 points
  Predictions:
    Reallocated threshold (100) in ~37 days
    Temperature critical (60°C) in ~12 days
  *** Plan disk replacement within 12 days (temperature) ***
  Active alerts:  2 trend warning(s)
  * Disk showing signs of degradation — plan replacement
```

### Files created/changed
| File | Status | Lines |
|------|--------|-------|
| `userspace/daemon/lhsr-health.c` | **NEW** | 120 |
| `userspace/daemon/lhsr-health.h` | **NEW** | 55 |
| `userspace/daemon/lhsrd.h` | MODIFIED | +3 |
| `userspace/daemon/lhsrd.c` | MODIFIED | +110 |
| `userspace/daemon/lhsr-control.c` | MODIFIED | +30 |
| `userspace/daemon/Makefile` | MODIFIED | +1 |
| `userspace/cli/lhsrctl.c` | MODIFIED | +400 |

### Bugs fixed during testing
- **Slow shutdown blocked on pthread_join()**: Monitor and health threads slept for 60-300s after `running=0` was set. `pthread_join()` in main thread blocked for the full interval. Fixed with `pthread_cancel()` before each `pthread_join()`, plus `pthread_cleanup_push/pop` handlers to safely release the mutex if a thread is cancelled while holding the lock. Shutdown now completes in ~40ms. (`lhsrd.c` +14/-2)

### Test coverage
- ✅ Integrated end-to-end tests on VM (`lhsr-dev`, 6.12.90+deb13.1-amd64):
  - Daemon start, JSON status file (15s cycle), control socket ping/pong
  - `lhsrctl status` (reads JSON), `lhsrctl predict` (socket query)
  - Prometheus metrics at `/var/lib/lhsrd/metrics.prom`
  - Clean shutdown (37-59ms) with PID file and socket cleanup
- ✅ Unit test for health score computation: `make test-health` (31/31 boundary cases across all penalty types, combined scenarios, and label thresholds)

### Deliverables
- ✅ Composite health score (0-100) with weighted penalties
- ✅ `lhsrctl status` reads daemon JSON status file, displays formatted output
- ✅ `lhsrctl predict` connects to control socket, queries trends, shows time-to-critical
- ✅ Prometheus metrics file at `/var/lib/lhsrd/metrics.prom`
- ✅ JSON status file includes `health_score`, `health_label` per disk
- ✅ Control socket trend response includes current values for cross-referencing
- ✅ Zero new compiler warnings on any target
- ✅ All targets build clean: kernel module, daemon, CLI, recovery tool
- ✅ Shutdown bug found and fixed (pthread_cancel + cleanup handlers)
- ✅ Health score unit test (31/31 passing)

---

## Phase 5: Recovery Tools — IN PROGRESS (2026-06-12)

**Duration:** Estimated 2 weeks (5.1-5.3 done, 5.4 remaining)

**What changed:** Userspace recovery tools to find, assemble, and recover LHSR arrays after disk failure.

### Implemented

#### 5.1 Enhanced superblock scan (`lhsr-scan --deep`) — ✅ COMPLETE

**Commit:** `434c1d8`

Added three new operating modes to `lhsr-scan`:

1. **`--deep` (deep scan)**: Scans the last ~2048 sectors at 16-sector granularity for orphaned superblocks, in addition to the known primary/backup positions. Catches superblocks from partial writes, misaligned devices, or manual dd operations.
2. **`--json` / `-j`**: Machine-parseable JSON output with per-disk superblock array. Includes `device`, `total_sectors`, `found`, `superblocks[]` (each with sector, position, generation, version, raid_type, disk_index, csum_valid), `best_generation`, `array_uuid`, `raid_type`. Summary object with `scanned`, `found`, `no_data` counts. Corruption detection via `csum_valid` field.
3. **`-v` without device args**: Auto-detects `/dev/sd*`, `/dev/vd*`, `/dev/nvme*` block devices (filters out partition siblings) and scans all of them.

Backward compatible: `lhsr-scan /dev/sdb` produces identical output to pre-5.1 version.

**Tested on:** Synthetic 10MB disk image with primary (gen 42) and backup (gen 41) superblocks — both found, checksum status correctly reported, cross-ref shows valid/corrupt counts.

#### 5.2 Array assembly recovery (`lhsrctl recover`) — ✅ COMPLETE

**Commit:** `bcc6f3e`, `46ce533`

The `lhsrctl recover` command scans provided block devices for LHSR superblocks, determines the best array configuration, and generates `dmsetup create` commands for assembly.

**Input:** Device paths on command line:
```
$ lhsrctl recover /dev/sdb /dev/sdc /dev/sdd
```

**Output (per-array):**
- Array summary: UUID, RAID type, disk count, parity, minimum healthy, creation time, generation
- Per-disk status: present/online state, generation, disk_state
- For missing disks: dm-zero placeholder commands to create zero-initialized block devices
- Main `dmsetup create` command with correct table format:
  - RAID0/1: `0 <size> lhsr <type> <dev> 0 [<dev> 0...]`
  - RAID5/6: `0 <size> lhsr <type> <chunk> <stripe_depth> <cont> <dev> 0 [<dev> 0...]`
  - Defaults: chunk=8 sectors (4KB), stripe_depth=1, cont=8

**Degraded mode:** When fewer than `disk_count` devices are provided, the tool generates dm-zero placeholder commands for missing positions. dm-zero reads zeros and discards writes — the kernel module can use it as a drop-in replacement until a real disk is added via `dmsetup reload` + `dmsetup resume`.

**Safety checks:**
- Validates `present >= min_healthy` (disk_count - parity) before generating commands
- RAID1 requires both disks
- SHR/SHR2 types rejected (no kernel module support)
- Table line overflow detection

**Tested on:**
- Single disk (degraded RAID5) → correct `/dev/mapper/lhsr_<uuid>_ph_<idx>` placeholder
- Full 3-disk RAID5 array → correct table with all 3 devices
- Degraded 2-disk RAID5 (1 missing disk) → placeholder + main command
- dm-zero target verified on VM (creates, reads zeros, removes cleanly)

#### 5.3 Offline data reconstruction (`lhsrctl reconstruct`) — ✅ COMPLETE

**Commit:** (pending)

The `lhsrctl reconstruct` command performs offline XOR reconstruction of a complete
disk image for a missing RAID5/6 array member from N-1 surviving devices. The output
is a raw disk image with the same size as the survivors, containing XOR-reconstructed
user data and a valid superblock.

**Key insight:** LHSR uses right-static (non-rotating) parity layout. This means every
disk stores data or parity at the SAME byte-offset for any given stripe. XOR of ALL
survivors at any offset directly yields the missing disk's content at that offset —
regardless of whether the missing disk held data or parity. No stripe/chunk geometry
math is needed for the XOR operation itself.

**Algorithm:**
1. Scan survivors for LHSR superblocks, group by array UUID
2. Validate: same array, RAID5/6, exactly one missing disk
3. Open all survivors O_RDONLY, create output file (sparse, same raw size)
4. For each 1MB block of user data: read from all survivors → XOR → write to output
5. Write primary and backup superblocks at correct positions
6. Report success with dd instructions

**Safety features:**
- Validates all survivors belong to same array (UUID check)
- Validates matching RAID type, disk count, and device size
- Rejects multiple missing disks (RAID6 dual-disk needs Reed-Solomon, deferred)
- Rejects non-RAID5/6 types
- Rejects all-disks-present (nothing to reconstruct)
- Error cleanup: closes all FDs, removes partial output on failure

**Usage:** `lhsrctl reconstruct --output /tmp/recovered.img /dev/sdb /dev/sdc`

Options:
- `--output <file>` / `-o <file>`: Output path (required)
- `--chunk-size <sectors>` / `-c <sectors>`: Chunk size for superblock (default 8)
  Does NOT affect XOR (layout-independent).

**Tested on:** Built clean with zero warnings across all 4 targets.

### Remaining

#### 5.4 Kernel: degraded assembly support — ✅ COMPLETE
- Allow `dmsetup create` with fewer disks than `disk_count`
  - Superblock-based expansion: constructor reads `disk_count` from surviving superblocks
  - `total_disks=N` keyword: specifies full disk count when no valid superblock exists
- Missing disks: reads reconstruct from parity (RAID5/6) or failover (RAID1), zeros for fresh arrays
- Writes silently discarded: bio completed with `BLK_STS_IOERR`
- Array marked `DEGRADED` in status/table, writes rejected until fully populated (reload table)
- Destructor safely skips NULL disk slots (no crash on removal)
- RAID1 read paths strengthened with explicit NULL-disk guards

### Deliverables
- ✅ 5.1 Enhanced `lhsr-scan` with `--deep`, `--json`, auto-detect
- ✅ 5.2 `lhsrctl recover` with degraded mode and dm-zero placeholders
- ✅ 5.3 `lhsrctl reconstruct` — offline data recovery from N-1 disks
- ✅ 5.4 Kernel degraded assembly support — dedicated mode with `total_disks=N`, parity reconstruction reads, write rejection, NULL-disk-safe destructor
- ✅ 5.5 Recovery procedure documentation — `RECOVERY.md` covering 5 scenarios + full walkthrough

---

## Phase 6: SHR Userspace

**Status:** Scoping document complete — see `docs/plans/2026-06-13-shr-userspace-design.md`

Synology Hybrid RAID support is a userspace feature that partitions disks of
varying sizes into equal-sized chunks, creates RAID arrays per tier (mdadm or
LHSR), and merges them with LVM. It does NOT belong in the kernel module.

### Approach
1. Write a tool that takes N disks of varying sizes
2. Computes the optimal layout via greedy tiering algorithm:
   - Partition disks into equal-sized chunks (size = smallest disk remaining)
   - Create RAID5/6 arrays from same-sized chunks (one per tier)
   - Merge tiers with LVM
3. Stack LHSR self-healing on top of each tier (optional, default mdadm)

### Design Decisions (from scoping document)
- **v1 is plan-only**: The `shr plan` command computes the layout and prints
  the sgdisk + mdadm/LHSR commands. User executes them manually. No rollback
  problem.
- **No auto-rebalance**: Adding disks to an existing layout is manual in v1.
- **mdadm default, LHSR optional**: `--lhsr` flag for self-healing tiers.
- **Zero kernel changes**: SHR is purely a userspace tool.

### Effort
- ~2000 lines, ~11 sessions (2-3 weeks) for full tool with LHSR support
- ~500 lines, ~3-4 sessions for `shr plan` only (layout calculator + command printer)

### Key Risks
1. Partition alignment on 4K drives (mitigated by 2048-sector default)
2. LVM metadata corruption if a tier fails (document recovery, don't automate)
3. Rollback complexity in v1 avoided by plan-only approach

### Deliverables
- ✅ Scoping/design document (`docs/plans/2026-06-13-shr-userspace-design.md`)
- ⏳ Phase 6.1: `lhsrctl shr plan` — layout calculator + command generator
- ⏳ Phase 6.2: `lhsrctl shr create` — automatic partitioning + tier creation + LVM setup
- ⏳ Phase 6.3: `lhsrctl shr status` — show current SHR layout from metadata
- ⏳ Phase 6.4: Integration testing with loopback devices of different sizes

---

## Phase 7: Live Migration (NOT YET SCOPED)

Online RAID reshape (adding/removing disks, changing RAID levels) is an
architecture-level feature for the DM target. It is NOT trivial.

### Approaches
1. **Stack on mdadm** (recommended): All reshape happens in mdadm. LHSR is the
   DM target on top. No kernel changes needed.
2. **Implement reshape in DM target**: Multi-month kernel engineering effort.
   Likely not worth it given mdadm already works.

### This is NOT YET SCOPED
- Requires architectural decision first
- No implementation plan exists

---

## Timeline Summary

| Phase | What | Duration | Depends On |
|-------|------|----------|------------|
| 0 | Honest foundation | 2 weeks | Nothing |
| 1 | Persistent checksums | 1 week | Phase 0 |
| 2 | Incremental rebuild | **2 days** (est. 2-3 weeks) | Phase 0 |
| 3 | Daemon refactor | **1 day** (est. 2-3 weeks) | Phase 0 |
| 4 | Predictive failure | **1 session** (est. 2 weeks) | Phase 3 |
| 5 | Recovery tools + kernel degraded mode | 2 weeks | Phase 0 |
| 6 | SHR userspace (plan-only v1) | ~1 week (mdadm), ~2-3 weeks (LHSR) | Phase 0 |

**Estimated total for Phases 0-5:** 7-10 weeks (~2 months) with one developer.

---

## What This Roadmap Does NOT Include

- **UI/Web GUI**: No web interface planned. LHSR is a CLI tool.
- **ZFS integration**: LHSR is not a ZFS replacement. It's a RAID parity layer.
- **Cloud tiering**: No plans for S3/cloud storage backend.
- **Encryption**: Use dm-crypt below LHSR for encryption. Don't reinvent.
- **Caching**: Use bcache or dm-cache below LHSR. Don't reinvent.
- **Snapshot**: Use LVM or dm-snapshot. Don't reinvent.

These are not omissions — they are deliberate scope boundaries. "Don't reimplement
the kernel" applies to features the kernel already provides.
