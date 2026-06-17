# Phase 9: Live Migration — Design & Implementation Plan

**Last Updated:** 2026-06-16
**Status:** SCOPING — implementation ready
**Based on:** ROADMAP.md Phase 9 (was "NOT YET SCOPED")

---

## 1. What This Document Is

This is the scoping and implementation plan for Phase 9 of LHSR: online RAID
reshape ("live migration") for SHR tiers.

**Decision from ROADMAP.md:** Option A — Stack on mdadm. All reshape happens
in mdadm. No kernel changes needed.

---

## 2. What "Live Migration" Means for SHR

The existing `shr expand` (Phase 6.9) adds capacity by creating **new tiers**:

```
Before:  Tier 0 (mdadm RAID5, 3×100MB) = 200MB usable
         LVM VG spans Tier 0
After:   Tier 0 (mdadm RAID5, 3×100MB) = 200MB usable
         Tier 1 (mdadm RAID1, 2×100MB) = 100MB usable  ← NEW
         LVM VG spans both tiers
```

Phase 9 `shr grow` reshapes an **existing tier** to include new disks:

```
Before:  Tier 0 (mdadm RAID5, 3×100MB) = 200MB usable
After:   Tier 0 (mdadm RAID5, 4×100MB) = 300MB usable  ← GROWN
```

Benefits over `shr expand`:
- No wasted parity overhead (one tier instead of two)
- Simpler LVM topology (one PV instead of two)
- Better performance (one RAID stripe set instead of LVM striping across tiers)

---

## 3. Scope (v1)

### 3.1 What Is Built

**`lhsrctl shr grow <tier> <device> [device...]`**

Adds one or more disks to an existing mdadm-mode SHR tier by:
1. Adding the new device(s) to the mdadm RAID array via `mdadm --grow`
2. Waiting for the online reshape to complete
3. Resizing the LVM PV on the md device
4. Extending the LVM LV to use the new space
5. Optionally growing the filesystem (ext4 `resize2fs` or xfs `xfs_growfs`)

### 3.2 What Is NOT in v1

| Feature | Reason |
|---------|--------|
| **LHSR-mode tier grow** | Requires kernel changes to dm-lhsr (reshape infrastructure). Deferred. See Section 7. |
| **RAID level change** | `mdadm --grow --level=N` works but is a separate operation. Not in scope for v1. |
| **Chunk size change** | Rarely needed. Document as separate `mdadm --grow --chunk=` operation. |
| **Disk removal** | `mdadm --grow --raid-devices=N` supports shrinking. Add in v2. |
| **Automatic rebalance** | Adding disks to one tier doesn't rebalance data from other tiers. |
| **Stacked LHSR-on-mdadm create** | Future Phase 9 v2 — requires new `shr create --stacked` mode. |

---

## 4. Data Structures

No new data structures are needed. The existing structures from `shr.h` are
sufficient:

```c
struct md_entry {
    char name[64];           /* e.g., "shr_tier_0" */
    int  md_num;             /* md device number, -1 if unknown */
    char members[N][512];    /* member partition paths */
    int  nmembers;           /* number of members */
};

struct dm_entry {
    char name[64];           /* e.g., "shr_tier_0" */
};
```

Additional per-tier geometry is read from sysfs at runtime:
- `/sys/block/md<N>/md/raid_disks` — current disk count
- `/sys/block/md<N>/md/level` — RAID level string
- `/sys/block/md<N>/md/array_size` — array size in 1K sectors
- `/sys/block/md<N>/size` — block device size in 512-byte sectors
- `/sys/block/md<N>/md/sync_action` — "idle", "reshape", etc.
- `/sys/block/md<N>/md/sync_completed` — reshape progress

---

## 5. Algorithm

### 5.1 Grow Command Flow

```
Input: tier_name, new_devices[], count
Output: reshaped mdadm array, resized LVM PV/LV

1. Parse options (--mdadm, --yes, --no-growfs)
2. Validate all new device paths (block devices, no shell chars)
3. Read sizes of new devices
4. Discover existing tier:
   a. Scan /dev/md/ for shr_tier_* (mdadm mode)
   b. Scan dmsetup for shr_tier_* (LHSR mode)
   c. Match by name
5. If LHSR mode → print error: "Use 'shr expand' instead"
6. If mdadm mode:
   a. Read sysfs for current geometry:
      - raid_disks, level, array_size, sync_action
   b. Verify array is idle (not already reshaping)
   c. Print grow plan with before/after sizes
   d. Get confirmation
   e. Execute:
      mdadm --grow /dev/md/<tier> \
            --raid-devices=<current + count> \
            --add <dev1> [<dev2> ...]
   f. Wait for reshape:
      - Poll /sys/block/md<N>/md/sync_action until "idle"
      - Show progress from sync_completed every 5 seconds
   g. Resize LVM:
      - Discover VG containing the md device
      - pvresize /dev/md/<tier>
      - lvextend -l +100%FREE <vg>/shr_vol
   h. Optionally grow filesystem
7. Report success
```

### 5.2 LHSR Mode: Deferred

For LHSR-mode tiers, the stacked architecture would be:

```
dm-lhsr SINGLE (self-healing wrapper) → mdadm RAID5 → raw partitions
```

This requires changes at **create time** (`shr create --stacked`), not
at grow time. Once a tier is created in stacked mode:
- dm-lhsr passes all I/O through (SINGLE mode)
- mdadm handles RAID math and reshape below
- dm-lhsr provides scrub/health monitoring on reads

v1 of Phase 9 does NOT implement this. LHSR-mode tiers should use
`shr expand` to add capacity via new tiers.

### 5.3 Partition Handling

For mdadm mode in SHR, tier members are partitions on disks (e.g.,
`/dev/sdb1`, `/dev/sdc1`). When the user provides a whole disk for
`shr grow`, we need to partition it to match existing member size.

However, mdadm --grow --add accepts any block device of sufficient size.
The kernel doesn't require exact size matching — the device just needs
to be >= existing members.

For v1:
- If the user passes a partition device (ends with digit): use directly
- If the user passes a whole disk: partition it to match existing
  member partition size, then use the partition
- mdadm handles any size mismatch gracefully

---

## 6. Implementation

### 6.1 Files to Change

| File | Change |
|------|--------|
| `userspace/cli/shr.c` | Add `cmd_shr_grow()` function (~250 lines) |
| `userspace/cli/shr.h` | Add `cmd_shr_grow()` declaration |
| `userspace/cli/lhsrctl.c` | Add `"grow"` dispatch in the SHR subcommand handler |
| `ROADMAP.md` | Mark Phase 9 as COMPLETE after testing |

### 6.2 Build & Test

- Build: `make clean && make` — zero new warnings
- VM test: 3 loopback devices → mdadm RAID5 → LVM → filesystem
  → write 100MB test file + checksum
  → `shr grow` with 4th loopback
  → verify data integrity after reshape
  → verify new capacity visible in filesystem

---

## 7. Future Work (Phase 9 v2)

### 7.1 Stacked LHSR-on-mdadm Create Mode

Add `shr create --stacked` that creates:
```
dm-lhsr SINGLE → mdadm RAID5 → raw disks
```

This requires no kernel changes (lhsr SINGLE mode already exists).
The dm-lhsr wraps the mdadm array for health monitoring + scrubbing,
while mdadm handles all RAID math and reshape.

### 7.2 LHSR-mode Tier Grow

If the tier was created in native LHSR mode (`shr create --lhsr`),
growing it requires either:
a) Kernel reshape infrastructure in dm-lhsr (Option B — multi-month)
b) Offline migration: stop LHSR → create mdadm → restart as stacked

### 7.3 RAID Level Migration

`mdadm --grow --level=6` to convert RAID5 → RAID6.
Add as `shr grow --to-raid6` or similar.

### 7.4 Disk Removal

`mdadm --grow --raid-devices=N-1 --remove <dev>` to shrink a tier.
Needs careful space checking before allowing.
