# LHSR Roadmap

**Last Updated:** 2026-06-03 (Phase 0 execution — see CHANGELOG)
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

## Phase 0: Honest Foundation (2026-06-03 — In Progress)

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

### 0.4 Test what exists — ⏳ SCRIPT CREATED, NOT YET RUN
- `tests/smoke-test-raid5.sh` created:
  1. Creates N loopback devices (configurable, default 3 × 64MB)
  2. Loads dm-lhsr.ko
  3. Creates RAID5 DM target
  4. Writes known data pattern
  5. Reads back and verifies checksum
  6. Queries array status via `dmsetup message`
  7. Clean teardown (module unload, loopback detach, temp file cleanup)
- **NOTE:** Pre-flight gate requires explicit approval before running.
  See `lhsr-testing-safety` skill.

### Deliverables
- ✅ Unified superblock struct in `include/lhsr.h`
- ✅ `atomic_long_t failed_disks` with accessor functions
- ✅ Accurate README, PRODUCTION_READINESS.md, ROADMAP.md, CHANGELOG.md
- ⏳ Smoke test script (not yet executed — requires approval)

---

## Phase 1: Persistent Checksums (2-4 weeks)

The scrubber computes checksums but doesn't persist them. This makes anti-bit-rot
non-functional across module reload.

### Option A: Stack on dm-integrity (RECOMMENDED)
- dm-integrity (Linux 4.12+) provides per-block checksum storage with journaling,
  multiple hash algorithms, and upstream maintenance.
- LHSR operates on dm-integrity devices instead of raw block devices.
- Slight performance overhead (~3-5%) but eliminates an entire class of bugs.

### Option B: Custom on-disk checksum tree
- Write checksums to a reserved area on each disk.
- Must handle journaling for crash safety (dm-integrity already solved this).
- Must handle trim/discard (dm-integrity already solved this).
- More control, more bugs.

### Decision criteria
- If LHSR primarily targets dm-integrity-capable kernels (4.12+), Option A is
  clearly better.
- Option B only makes sense if LHSR must run on kernels without dm-integrity
  (pre-4.12), which is almost nobody in 2026.

### Deliverables
- Stack LHSR on dm-integrity or implement custom persistent checksum store
- Checksums populated on write (not just scrub)
- Scrubber uses persistent checksums
- Tests: create array, write data, unload module, reload module, scrub detects
  corruption

---

## Phase 2: Incremental Rebuild with Write-Intent Bitmap (2-3 weeks)

Current rebuild copies every block. Production rebuild should only copy changed
blocks.

### Approach: Adopt mdadm bitmap v6 format
- mdadm bitmap major version 6 (lockless, merged 2025) provides write-intent
  tracking. Use the same on-disk format.
- The kernel already has `md-bitmap` infrastructure, but it's tied to md, not
  DM targets. LHSR needs its own bitmap implementation.
- Keep it simple: a flat bitmap in a reserved area on each disk. Bit = 1 means
  "block dirty." No hierarchical metadata.

### Implementation sketch
```c
struct lhsr_bitmap {
    u64  sectors;       /* Number of sectors covered */
    u32  bits_per_chunk; /* Typically 1 bit per 64KB */
    u32  state;         /* CLEAN, DIRTY, RECOVERING */
    u64  sync_count;    /* Updated atomically on each write */
    u8   bitmap[0];     /* Flexible array */
};
```

### Rebuild algorithm
```
for each chunk:
    if bitmap bit is set:
        read from source, write to target
        clear bitmap bit
    else:
        skip (block was never written or hasn't changed)
```

### Deliverables
- Write-intent bitmap format defined in `include/lhsr.h`
- Bitmap set on write, cleared on rebuild completion
- Incremental rebuild in `rebuild_work()`
- Full-disk rebuild fallback for initial sync or when no bitmap exists
- Tests: create array, write to subset of blocks, trigger rebuild, verify only
  written blocks were copied

---

## Phase 3: Userspace Daemon Refactor (2-3 weeks)

The daemon (`lhsrd`) currently uses `fork()` + `exec()` of `smartctl` and
`dmsetup`. This is slow, fragile, and prevents real monitoring.

### 3.1 Replace dmsetup with libdevmapper
- Current: `fork() + exec(dmsetup message <dev> <cmd>)`
- Target: `dm_task_message()` via libdevmapper
- Eliminates fork overhead, eliminates shell injection vectors

### 3.2 Replace smartctl with direct sysfs/SG_IO
- Current: `fork() + exec(smartctl -a /dev/sdX)` and parse output
- Target: Read SMART data from sysfs where available, fall back to SG_IO ioctl
- Faster, no parsing fragility

### 3.3 Add SMART trend tracking
- Store daily SMART snapshots in a simple SQLite database or flat JSON
- Track key attributes (reallocated_sectors, pending_sectors, temperature,
  read_error_rate, write_error_rate)
- Simple linear regression on each attribute to compute trend slope

### Deliverables
- Daemon uses libdevmapper instead of fork+exec dmsetup
- Daemon reads SMART via sysfs/ioctl instead of forking smartctl
- SMART history database with trend analysis
- Warning when trend slope exceeds thresholds

---

## Phase 4: Predictive Failure Integration (2 weeks)

Add a real (if simple) prediction model using the trend data from Phase 3.

### Approach: Threshold + Trend model
- Define thresholds per SMART attribute based on Backblaze failure data:
  - `reallocated_sectors > N_sectors` → WARNING
  - `pending_sectors > 0` → WARNING
  - Temperature deviation > 2σ from disk's own history → WARNING
  - Trend slope exceeding threshold → WARNING
- Combine into a weighted health score (0-100)
- This is what `argus-disk` does, and it's good enough for a v1

### Failure modes
- **False positives**: The 3-10% FDR of vendor thresholds is a known limitation.
  Trend-based models improve this but cannot eliminate it.
- **False negatives**: Sudden failures (electronics, head crash) have no SMART
  warning. The model cannot predict these.
- **Cascading prediction**: Predicting a disk failure on an already-degraded
  array (RAID5 with one disk missing) creates urgency but no new information.

### Output
```
$ lhsrctl status
Disk 2 (WDC WD80EFAX-68KNBN0):
  Health: 73/100 (WARNING)
  Reallocated sectors: 47 (increasing by 2.3/day)
  Pending sectors: 3
  Temperature: 42°C (stable)
  Estimated time to critical: 47 days
```

### Deliverables
- Health score computation in daemon
- Scheduled checks (every 6h)
- Notifications (optional: ntfy, email, syslog)
- `lhsrctl status` displays health scores
- Prometheus metrics endpoint (for Grafana dashboards)

---

## Phase 5: Instant Recovery Tools (2 weeks)

### 5.1 Degraded array assembly
- Allow `dmsetup create` with fewer disks than required for full redundancy
- Missing disks are reconstructed on-the-fly from parity/mirror
- Read-only mount in degraded mode

### 5.2 Metadata reconstruction
- If primary superblock is corrupt, try backup superblock
- If both are corrupt, scan all disks for valid metadata and reconstruct
- Write reconstructed superblock to all disks

### 5.3 Scan and recover
- `lhsr scan --deep`: Scan all connected disks for LHSR superblocks
  and display possible array configurations
- `lhsr recover`: Assemble the most complete array possible from available disks
- `lhsr reconstruct`: Given N-1 disks, reconstruct data for the missing disk
  (RAID5/6 only)

### Deliverables
- Degraded mount (read-only)
- Metadata reconstruction from backup superblock
- Deep scan tool
- Recovery guide updated with new capabilities

---

## Phase 6: SHR Userspace (NOT YET SCOPED)

Synology Hybrid RAID support is a userspace feature that partitions disks and
stacks mdadm arrays. It does NOT belong in the kernel module.

### Approach
1. Write a tool that takes N disks of varying sizes
2. Computes the optimal layout:
   - Partition disks into equal-sized chunks (size = smallest disk or divisor)
   - Create mdadm RAID arrays from same-sized chunks
   - Merge arrays with LVM (or LHSR in spanning mode)
3. Stack LHSR self-healing on top of mdadm RAID

### This is NOT YET SCOPED
- No implementation plan exists
- No resources allocated
- Listed for completeness only
- Requires significant userspace engineering

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
| 1 | Persistent checksums | 2-4 weeks | Phase 0 |
| 2 | Incremental rebuild | 2-3 weeks | Phase 0 |
| 3 | Daemon refactor | 2-3 weeks | Phase 0 |
| 4 | Predictive failure | 2 weeks | Phase 3 |
| 5 | Recovery tools | 2 weeks | Phase 0 |
| 6 | SHR userspace | NOT YET SCOPED | — |
| 7 | Live migration | NOT YET SCOPED | — |

**Estimated total for Phases 0-5:** 12-18 weeks (3-4 months) with one developer.

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
