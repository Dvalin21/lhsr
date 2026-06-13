# Phase 6: SHR Userspace — Design & Scoping Document

**Last Updated:** 2026-06-13
**Status:** SCOPING — no implementation started
**Based on:** ROADMAP.md Phase 6 (was "NOT YET SCOPED")

---

## Table of Contents

1. [What This Document Is](#1-what-this-document-is)
2. [What Is SHR?](#2-what-is-shr)
3. [Data Structures](#3-data-structures)
4. [Partitioning Algorithm](#4-partitioning-algorithm)
5. [LVM + mdadm Stacking](#5-lvm--mdadm-stacking)
6. [LHSR Integration](#6-lhsr-integration)
7. [Tool Interface](#7-tool-interface)
8. [Failure Modes](#8-failure-modes)
9. [What Is NOT in Scope](#9-what-is-not-in-scope)
10. [Effort Estimate](#10-effort-estimate)
11. [Risks](#11-risks)
12. [Decision: Build or Not](#12-decision-build-or-not)

---

## 1. What This Document Is

This is the **scoping and design document** for a userspace tool that implements
Synology Hybrid RAID (SHR) compatible mixed-disk-size RAID using standard Linux
building blocks: partitions, `mdadm`, LVM, and LHSR.

**This document answers:**
- What exactly does the tool do?
- What are the data structures?
- How does the partitioning algorithm work?
- What is the output layout?
- What is the effort to build it?
- Should we build it at all?

**This document does NOT:**
- Specify exact command-line flags (those get ironed out during implementation)
- Include kernel changes (SHR is a userspace-only feature)
- Replace the existing LHSR kernel module

---

## 2. What Is SHR?

Synology Hybrid RAID (SHR) is a **layered storage system** that allows mixing
disks of different sizes while maintaining RAID redundancy. It is NOT a new
RAID level. It is a **userspace partition layout** that:

1. Partitions each disk into one or more equal-sized chunks
2. Groups same-sized chunks into RAID5 or RAID6 arrays (one per "tier")
3. Combines all RAID arrays into a single LVM volume group
4. Presents a single filesystem spanning all space

### Example: 3 disks (2 TB + 4 TB + 6 TB)

```
Disk A (2 TB):  [  Tier1  ][          unused          ]
Disk B (4 TB):  [  Tier1  ][       Tier2       ][unused]
Disk C (6 TB):  [  Tier1  ][       Tier2       ][Tier3 ]

Tier1: 2 TB RAID5 on 3 disks = 4 TB usable (3 disks × 2 TB, 1 parity)
Tier2: 2 TB RAID5 on 2 disks = 2 TB usable (2 disks × 2 TB, 1 parity)
Tier3: 2 TB RAID1 on 1 disk.. wait, RAID1 needs 2 disks.

This is why SHR-1 uses single-parity (RAID5-like) tiers and SHR-2 uses
dual-parity (RAID6-like). Each tier must have at least RAID5 minimum
disks (3 for RAID5, 4 for RAID6).
```

The key insight: **every tier is exactly the same size** (size of the smallest
disk, or a divisor of it). Larger disks contribute to more tiers.

### Synology's actual SHR behavior:
- SHR-1: Each tier is RAID5-like (1 parity per N-1 data)
- SHR-2: Each tier is RAID6-like (2 parity per N-2 data)
- Tiers are created greedily: from smallest remaining capacity, one tier at a time
- All tiers combined into a single LVM VG with one PV per md device
- Performance: all disks participate in I/O, tiers are invisible above LVM

---

## 3. Data Structures

> "Bad programmers worry about the code. Good programmers worry about data structures
> and their relationships." — Linus Torvalds

### 3.1 Partition Map (the core data structure)

```c
/* One partition on one disk. Partitions within a tier are all the same size. */
struct shr_partition {
    unsigned int    disk_idx;       /* Index into the disk array */
    uint64_t        offset_sectors; /* Partition start on this disk */
    uint64_t        size_sectors;   /* Partition size (same across a tier) */
    unsigned int    tier;           /* Which tier this partition belongs to */
};

/* One RAID tier. All partitions in a tier have equal size. */
struct shr_tier {
    unsigned int    partition_count;    /* Number of disks contributing to this tier */
    unsigned int    raid_type;          /* LHSR_RAID5 or LHSR_RAID6 */
    uint64_t        partition_size;     /* Size per partition (all equal) */
    uint64_t        usable_sectors;     /* Usable capacity = partition_size * (disks - parity) */
    char            md_device[32];      /* e.g., /dev/md/shr_tier_0 */
    char            lvm_pv_name[64];    /* e.g., /dev/mapper/shr_pv_tier0 */
    bool            has_raid;           /* false = single-disk "tier" (no redundancy) */
};

/* One disk in the SHR layout */
struct shr_disk {
    char            path[256];          /* e.g., /dev/sdb */
    uint64_t        total_sectors;      /* Full device capacity */
    uint64_t        reserved_sectors;   /* Metadata/alignment overhead */
    uint64_t        available_sectors;  /* total - reserved */
    unsigned int    partition_count;    /* How many partitions on this disk */
};

/* The complete SHR layout */
struct shr_layout {
    unsigned int        disk_count;     /* Total physical disks */
    unsigned int        tier_count;     /* Total RAID tiers */
    struct shr_disk     disks[32];      /* Max 32 disks */
    struct shr_tier     tiers[32];      /* Max 32 tiers */
    struct shr_partition partitions[256]; /* Max partitions across all disks */
    unsigned int        parity_per_tier; /* 1 for SHR-1, 2 for SHR-2 */
    uint64_t            total_usable_sectors; /* Sum of all tier usable_sectors */
    char                vg_name[64];    /* LVM volume group name */
};
```

### 3.2 Key Invariants (MANDATORY — enforced by the tool)

1. **Every partition in a tier has exactly the same `size_sectors`.** This is
   non-negotiable. RAID requires equal-sized members.
2. **No disk sector is assigned to more than one partition.** Partitions on a
   single disk are contiguous and non-overlapping.
3. **Partitions on the same disk are ordered by offset.** Disk layout is
   Tier 0, then Tier 1, then Tier 2, ... no interleaving.
4. **Every tier has at least 3 disks for SHR-1 (RAID5) or 4 disks for SHR-2
   (RAID6).** A tier with 2 disks and 1 parity is RAID1 with 50% waste.
   A tier with 1 disk has zero redundancy and is excluded (or flagged).
5. **`total_usable_sectors < sum(available_sectors)`** because each tier
   loses parity capacity. The tool reports both raw and usable.
6. **Tier index = sorted order by disk count, descending.** The tier with the
   most participating disks is Tier 0 (best efficiency). This is NOT required
   for correctness but maximizes redundancy.

### 3.3 Why These Structures

The partition is the **fundamental unit**. It's what `mdadm` creates RAID arrays
from. It's what LVM maps as PVs. It's what a replacement disk must reproduce
to rejoin the array.

The tier is the **RAID grouping**. It tells you the redundancy level, the
usable capacity, and the md device that implements it.

The disk is the **physical reality**. It constrains the maximum partition size
and the number of tiers it can participate in.

The layout is the **complete picture**. It answers: "If any disk fails, which
tiers are affected, and can we still reconstruct?"

---

## 4. Partitioning Algorithm

### 4.1 Input

- N disks with reported sizes (in sectors)
- Redundancy mode: SHR-1 (1 parity) or SHR-2 (2 parity)
- Optional: alignment offset (default 1 MB = 2048 sectors for modern drives)
- Optional: minimum partition size (default 1 GB = 2097152 sectors)

### 4.2 Algorithm (Greedy Tiering)

```
Input: disks[] sorted by size ascending
       parity = 1 (SHR-1) or 2 (SHR-2)
       alignment = 2048 sectors (1 MB)
       min_partition = 2097152 sectors (1 GB)

Algorithm:

1. For each disk i:
     disks[i].available = disks[i].total - alignment
     disks[i].partition_count = 0

2. tier_idx = 0
   remaining_disks = disks (those with available >= min_partition)

3. WHILE count(remaining_disks with available >= min_partition) >= 3:
     a. Find the SMALLEST available sector count among remaining disks:
          min_avail = min(disk.available for disk in remaining_disks)

     b. This tier's partition size = min_avail

     c. Create tiers[tier_idx]:
          partition_size = min_avail
          partition_count = count(remaining_disks)
          md_device = /dev/md/shr_tier_{tier_idx}

     d. For each disk in remaining_disks:
          Create partition: offset = disk.total - disk.available
                            size = partition_size
                            tier = tier_idx
          disk.available -= partition_size
          if disk.available < min_partition:
              Remove from remaining_disks

     e. tier_idx++

4. Report any remaining capacity too small to form a tier as "unused"
```

### 4.3 Example Walkthrough

**Disks:** 2 TB, 4 TB, 6 TB (in sectors: 3907029168, 7814037168, 11721055744)
**Mode:** SHR-1 (1 parity)

```
Iteration 1:
  remaining: [2T, 4T, 6T]
  min_avail = 2 TB (3907029168 sectors)
  Tier 0: partition_size = 2 TB, 3 disks, RAID5
  Disk A: partition at offset 0, size 2 TB, available = 0 → done
  Disk B: partition at offset 0, size 2 TB, available = 2 TB
  Disk C: partition at offset 0, size 2 TB, available = 4 TB
  remaining: [4T(2T left), 6T(4T left)]

Iteration 2:
  remaining: [4T(2T left), 6T(4T left)]
  min_avail = 2 TB
  Tier 1: partition_size = 2 TB, 2 disks...
  
  **BUT**: 2 disks with RAID5 = 1 data + 1 parity = 50% efficiency.
  
  Decision point: Do we create a 2-disk RAID5 tier, or a RAID1 tier?
  
  Answer: With exactly 2 disks, the effective layout is RAID1 (mirror).
  The mdadm command would be: mdadm --create /dev/md1 --level=1 --raid-disks=2
  
  This gives redundancy but wastes 50% of the 2-disk tier space.
```

### 4.4 Special Cases

| Condition | Behavior |
|-----------|----------|
| **3+ disks available** | Full RAID5/6 tier, normal parity overhead |
| **Exactly 2 disks** | RAID1 tier (50% usable) — warn user |
| **Exactly 1 disk** | No redundancy — warn user, exclude from tiering |
| **0 disks** | Error: cannot create any tier |
| **All disks same size** | Single tier, no partitioning needed — pure RAID5/6 |
| **Disk added later** | Requires manual rebalance — see Section 8 |

### 4.5 Efficiency

SHR-1 efficiency with N disks of sizes S1 ≤ S2 ≤ ... ≤ SN:

```
Let T = number of tiers
Let P = parity per tier (= 1 for SHR-1)

For tier i, built from Di disks:
  raw_capacity_i = Di × partition_size
  usable_i = Di × partition_size × (Di - P) / Di
           = partition_size × (Di - P)

Total usable = Σ(partition_size_i × (Di - 1))  for SHR-1
             = Σ(partition_size_i × (Di - 2))  for SHR-2
```

The absolute best case (all disks equal size) = single tier = standard RAID5/6
efficiency. The worst case (wildly different sizes) creates many small tiers
with diminishing returns.

**Rule of thumb:** SHR helps most when disks differ by ≤ 2× in size. Beyond
that, the smallest disk dominates capacity and larger disks are underutilized.

---

## 5. LVM + mdadm Stacking

### 5.1 Stacking Diagram

```
Filesystem (ext4, xfs, btrfs)
     ↓
LVM Logical Volume (lv_thin, lv_linear, etc.)
     ↓
LVM Volume Group (shr_vg)
     ↓
md0 (Tier 0)   md1 (Tier 1)   md2 (Tier 2)   ...
     ↓              ↓              ↓
partitions on disks A,B,C   partitions on B,C   partition on C
```

### 5.2 Implementation Per Tier

For each tier `t`:

```bash
# Step 1: Create the mdadm RAID array from the partitions in this tier
PARTITIONS=""
for each partition disk in tier t:
    PARTITIONS="${PARTITIONS} /dev/${disk}${PART_NUM}"

mdadm --create /dev/md/shr_tier_${t} \
    --level=${RAID_LEVEL} \
    --raid-disks=${PART_COUNT} \
    ${PARTITIONS}

# Step 2: Initialize as LVM PV
pvcreate /dev/md/shr_tier_${t}

# Step 3: Add to volume group (create VG on first tier, extend on rest)
if [ $t -eq 0 ]; then
    vgcreate shr_vg /dev/md/shr_tier_${t}
else
    vgextend shr_vg /dev/md/shr_tier_${t}
fi
```

### 5.3 Partitioning the Disks

The partitions are standard DOS or GPT partitions. The tool uses `sgdisk` or
`parted` to create them:

```bash
# On each disk, create N partitions for N tiers
# Partition i has size = shr_layout.tiers[i].partition_size
# Starting at offset = sum of previous partition sizes

sgdisk --new=1:0:+${TIER0_SIZE}  /dev/sdb
sgdisk --new=2:+${TIER0_SIZE}:+${TIER1_SIZE} /dev/sdb
# ... etc
```

### 5.4 Filesystem on Top

```bash
# Create a single LV spanning the entire VG
lvcreate -l 100%FREE -n shr_vol shr_vg

# Format
mkfs.ext4 /dev/shr_vg/shr_vol

# Mount
mount /dev/shr_vg/shr_vol /mnt/storage
```

### 5.5 Why This Stack (not a single large RAID0)

An alternative would be to create one giant RAID0 or linear concatenation of
all partitions across all tiers. This is WRONG because:
- A RAID0 across non-identical partitions gives no redundancy
- A single mdadm array cannot span partitions of different sizes across
  different disk subsets
- LVM is THE tool for combining arbitrary block devices into a single volume

mdadm handles the per-tier redundancy. LVM handles the cross-tier concatenation.
Each tool does what it's best at. "Don't reimplement the kernel."

---

## 6. LHSR Integration

### 6.1 Where LHSR Fits

LHSR is stacked **on top of** the physical disks and **below** mdadm:

```
Filesystem
     ↓
LVM Volume Group (shr_vg)
     ↓
md0 (Tier 0)   md1 (Tier 1)   md2 (Tier 2)
     ↓              ↓              ↓
LHSR             LHSR            LHSR       ←  NEW: LHSR sits here
     ↓              ↓              ↓
sdX1             sdY2             sdZ3     ←  Raw partitions
```

**Wait — this doesn't work.** LHSR is itself a RAID5/6 implementation. Stacking
LHSR on top of individual partitions and then mdadm on top of those gives
nested RAID with no purpose.

### 6.2 The Correct Stack

If LHSR is used for self-healing (scrub + read reconstruction), it replaces
mdadm entirely for each tier:

```
Filesystem
     ↓
LVM Volume Group (shr_vg)
     ↓
dm target (Tier 0)   dm target (Tier 1)   dm target (Tier 2)  ← LHSR tiers
     ↓                      ↓                      ↓
sdX1  sdY1  sdZ1         sdY2  sdZ2              sdZ3       ← Raw partitions
```

**Each tier becomes a separate LHSR array.** The tool creates `dmsetup` tables
for each tier instead of `mdadm --create`.

```bash
# Tier 0: 3-disk RAID5 on /dev/sdb1, /dev/sdc1, /dev/sdd1
dmsetup create shr_tier_0 --table \
  "0 ${SIZE} lhsr raid5 8 1 8 ${DISK0_PART} 0 ${DISK1_PART} 0 ${DISK2_PART} 0"

# Tier 1: 2-disk RAID1 on /dev/sdc2, /dev/sdd2
dmsetup create shr_tier_1 --table \
  "0 ${SIZE} lhsr mirror 8 1 8 ${DISK1_PART2} 0 ${DISK2_PART2} 0"

# Each tier is a PV in the LVM VG
pvcreate /dev/mapper/shr_tier_0 /dev/mapper/shr_tier_1
vgcreate shr_vg /dev/mapper/shr_tier_0
vgextend shr_vg /dev/mapper/shr_tier_1
lvcreate -l 100%FREE -n shr_vol shr_vg
```

### 6.3 Benefit of LHSR over mdadm

LHSR provides **background scrubbing** and **read-side self-healing** with
CRC32c verification. mdadm does NOT provide per-block checksum scrubbing.
LHSR does (via dm-integrity stacking).

If the user doesn't need self-healing, pure mdadm is simpler and battle-tested.
The tool should support BOTH modes:
- `--mdadm`: Use mdadm for each tier (proven, fast)
- `--lhsr`: Use LHSR for each tier (self-healing, experimental)

### 6.4 What Does NOT Change in the Kernel Module?

**Nothing.** The SHR tool is PURELY userspace. The kernel module sees each tier
as a normal RAID5/6 array. It has no idea about tiers, partitions, or LVM above.
This is by design: "Don't reimplement the kernel."

---

## 7. Tool Interface

### 7.1 Proposed Command

The SHR tool lives in `userspace/shr/` and is invoked as:

```bash
lhsrctl shr create [--lhsr|--mdadm] [--parity 1|2] /dev/sdX /dev/sdY ...
```

Where:
- `--lhsr` (default): Use LHSR DM target for each tier (self-healing)
- `--mdadm`: Use mdadm for each tier (battle-tested)
- `--parity 1` (default): SHR-1, single parity per tier
- `--parity 2`: SHR-2, dual parity per tier (minimum 4 disks)
- Devices: block devices of varying sizes

### 7.2 What the Tool Does

1. **Validate**: All devices are block devices, readable, not in use
2. **Compute layout**: Run the greedy tiering algorithm
3. **Print layout**: Show the partition plan, ask for confirmation
4. **Execute**:
   a. Create GPT partitions on each disk (using `sgdisk` or `parted`)
   b. Create RAID arrays (mdadm or dmsetup create with LHSR)
   c. Initialize LVM PVs on each array
   d. Create VG and LV

### 7.3 What the Tool Does NOT Do

- Format the filesystem (user chooses ext4/xfs/btrfs)
- Mount the filesystem (user edits /etc/fstab)
- Monitor disk health (lhsrd already does this)
- Replace a failed disk (see Section 8)
- Rebalance tiers after disk addition (see Section 8)

### 7.4 Informational Subcommands

```bash
# Show the layout of existing disks without creating anything
lhsrctl shr plan [--parity 1|2] /dev/sdX /dev/sdY ...

# Show the current SHR layout from metadata
lhsrctl shr status

# Export layout as JSON (for backup or documentation)
lhsrctl shr export [--json]
```

### 7.5 Output Example

```
# lhsrctl shr plan /dev/sdb /dev/sdc /dev/sdd
LHSR SHR Layout Plan
====================
Mode: SHR-1 (single parity)

Disk Layout:
  /dev/sdb (2.00 TB): Partition 1 (2.00 TB, Tier 0)
  /dev/sdc (4.00 TB): Partition 1 (2.00 TB, Tier 0), Partition 2 (2.00 TB, Tier 1)
  /dev/sdd (6.00 TB): Partition 1 (2.00 TB, Tier 0), Partition 2 (2.00 TB, Tier 1), Partition 3 (2.00 TB)

Tiers:
  Tier 0: 3-disk RAID5, 2.00 TB partitions, usable = 4.00 TB
  Tier 1: 2-disk RAID1, 2.00 TB partitions, usable = 2.00 TB

Total usable: 6.00 TB
Unused:       2.00 TB (/dev/sdd partition 3)

WARNING: /dev/sdd has 2.00 TB of capacity that cannot form a redundant tier
(only 1 disk remaining). This space will be inaccessible in the SHR layout.

Proceed? [y/N]
```

---

## 8. Failure Modes

### 8.1 Single Disk Failure (Recoverable)

If one disk in the SHR layout fails, **every tier that disk participated in**
is degraded. With SHR-1, at most one tier loses a disk. The other tiers are
unaffected.

```
Before:  Disk A (Tier 0), Disk B (Tier 0 + 1), Disk C (Tier 0 + 1 + 2)
After Disk A fails:
  Tier 0: 2/3 disks → DEGRADED (reads reconstruct from parity)
  Tier 1: 2/2 disks → unaffected
  Tier 2: 1/1 disks → unaffected
 
But Tier 0 is RAID5 with 3→2 disks, which means 1 disk tolerated.
If Tier 0 was RAID6, it would tolerate 2 failures, so Disk A failure
would be fine.
```

**Recovery:** Replace Disk A with an equal-or-larger disk, repartition to
match the original layout (exact partition sizes and positions), and trigger
a rebuild on each affected tier.

The recovery tool (`RECOVERY.md` Scenarios B + C) handles this per-tier. The
SHR tool does not need to be involved in recovery — standard LHSR or mdadm
recovery procedures apply to each tier independently.

### 8.2 Multiple Disk Failure

If two disks fail in an SHR-1 layout:
- A tier that had both disks → data loss in that tier → partially lost VG
- Tiers that had only one failed disk → degraded, reconstructible
- Tiers with no failed disks → unaffected

SHR-2 (dual parity) tolerates 2 failed disks per tier. Two random disk
failures in a 4+ disk SHR-2 layout are likely safe.

### 8.3 Disk Addition (Grow)

Adding a larger disk to an existing SHR layout requires **manual intervention**.
The tool does NOT auto-rebalance tiers. See Section 12 for why.

What happens when you add a 8 TB disk to the 2+4+6 TB example:
- The smallest disk still determines the smallest tier size
- The new disk could form a new tier at the end (2 TB usable)
- Or the layout could be recomputed entirely (requires moving data)

**Recommendation:** Do NOT support auto-rebalance in v1. The user either
adds disks of equal or larger size to existing tiers, or replaces smaller
disks with larger ones.

### 8.4 Disk Replacement

Replace a failed disk with a new one of the SAME size. The recovery procedure
is:
1. Physically replace the disk
2. Partition it to match the original layout
3. For each tier, have mdadm/LHSR rebuild the missing member
4. Done.

The tool's `shr plan` output (saved to a config file) tells you exactly what
partition sizes and positions are needed.

### 8.5 What Happens to LVM if a Tier Dies?

LVM handles PV failures gracefully IF the VG was created with redundancy
across PVs. But in SHR, each tier IS one PV. If that PV disappears:
- Any LV that spans into that PV's extents loses data
- `vgreduce --removemissing` removes the dead PV
- `lvchange --activate n` on the VG, then `lvchange --activate y` with
  partial mode recovers what's left

This is NOT data-safe. The filesystem must handle missing extents (XFS
handles this better than ext4). This is a fundamental limitation of
stacking RAID tiers under LVM.

---

## 9. What Is NOT in Scope

| Feature | Reason |
|---------|--------|
| **Kernel module changes** | SHR is a userspace partitioning problem. Zero kernel changes needed. |
| **Auto-rebalance on disk add** | Requires moving data. Can be added in v2. |
| **Online tier migration** | Moving data between tiers is a LVM pvmove operation. Document it, don't automate it in v1. |
| **Filesystem formatting/mounting** | User choice. Tool documents options. |
| **Monitoring** | Existing `lhsrd` does SMART monitoring. Does not need SHR awareness. |
| **Replacing mdadm entirely** | LHSR is an alternative to mdadm for self-healing, but mdadm is battle-tested. Tool supports both. |
| **GUI** | CLI only. "LHSR is a CLI tool." |
| **Cross-filesystem snapshots** | LVM snapshots exist. Don't reinvent. |

---

## 10. Effort Estimate

### 10.1 Effort by Component

| Component | Lines | Time | Dependencies |
|-----------|-------|------|-------------|
| 1. Data structures + layout computation | ~400 | 2 sessions | None |
| 2. Partitioning (sgdisk wrapper) | ~200 | 1 session | Component 1 |
| 3. mdadm tier creation | ~200 | 1 session | Component 2 |
| 4. LHSR tier creation (dmsetup) | ~200 | 1 session | Component 2 |
| 5. LVM PV/VG/LV setup | ~150 | 1 session | Component 3 or 4 |
| 6. `shr plan` (dry-run preview) | ~200 | 1 session | Component 1 |
| 7. `shr status` and `shr export` | ~150 | 1 session | Component 1 |
| 8. Error handling + rollback | ~200 | 1 session | Components 2-5 |
| 9. Testing (integration) | ~300 | 2 sessions | Components 2-5 |

**Total:** ~2000 lines, ~11 sessions (roughly 2-3 weeks for one developer)

### 10.2 Recommended Order

```
Session 1-2:  Data structures + layout algorithm + unit tests
Session 3:    Partition creation (sgdisk wrapper)
Session 4-5:  mdadm tier creation + LVM setup
Session 6:    LHSR tier creation + LVM setup
Session 7:    shr plan (dry-run preview)
Session 8:    shr status + shr export
Session 9:    Error handling + rollback + user confirmation flow
Session 10-11: Integration tests + edge cases + documentation
```

### 10.3 What Makes This Hard

- **Partition alignment**: Modern drives (4K native, Advanced Format) require
  alignment to 2048-sector boundaries. Misaligned partitions destroy performance.
- **Partition naming**: After creating partitions with sgdisk, the kernel may
  not immediately see them (`partprobe` or `blockdev --rereadpt` required).
  Devices may appear as `/dev/sdb1` or `/dev/disk/by-partuuid/...`.
- **Rollback**: If step 4 of 5 fails, we need to undo steps 1-3. This is
  error-prone and must be handled carefully.
- **Testing**: Proper testing requires real (or loopback) block devices of
  different sizes. Loopback devices work but add complexity.

---

## 11. Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| **mdadm version incompatibility** | Medium | Test against mdadm from Debian stable; document version requirements |
| **LVM metadata corruption after tier failure** | High | Document recovery procedures; recommend XFS for partial-VG tolerance |
| **Partition alignment on 4K drives** | Medium | Default alignment = 2048 sectors; allow user override |
| **sgdisk vs parted compatibility** | Low | Use sgdisk (more reliable for non-interactive use); warn if missing |
| **Kernel doesn't reread partition table** | Medium | Use `blockdev --rereadpt` with retry loop; document fallback to `partprobe` |
| **LHSR scrub doesn't span tiers** | Low | Each tier is a separate LHSR array; each array scrubs independently |
| **User runs create twice** | Medium | Tool checks for existing partitions/superblocks on ALL devices before starting |
| **Cross-architecture GPT endianness** | Low | sgdisk handles this; tool never writes GPT manually |

---

## 12. Decision: Build or Not

### Arguments FOR building SHR

1. **Different-sized disks are the reality** for NAS users. They don't buy 4
   identical drives at once; they replace failed drives with whatever is
   available/affordable.
2. **mdadm + LVM already works** for this use case (Synology proved it). We're
   not inventing new technology, just automating an existing workflow.
3. **The tool is pure userspace** — no kernel risk, no ABI concerns, easy
   to maintain and update independently.
4. **Self-healing on tiers** (LHSR mode) adds value beyond mdadm alone: per-block
   CRC32c scrubbing on every tier.

### Arguments AGAINST building SHR

1. **mdadm already supports --write-mostly and --re-add** for hot sparing.
   The gap SHR fills (varying disk sizes) is narrow.
2. **The greedy tiering algorithm wastes space** when disks are very different
   sizes. A 2 TB + 10 TB + 12 TB set wastes ~4 TB in the smallest tier.
3. **Tier failure takes down LVM.** If one tier is lost, the whole VG is
   affected. This is worse than a single RAID5 failure.
4. **Complexity.** ~2000 lines of new code, 11 sessions, significant testing
   burden, for a feature that mdadm + LVM already supports (just with manual
   partitioning).
5. **Recovery is harder.** A failed disk in a single RAID5 array is simple.
   A failed disk in a 3-tier SHR layout affects one tier, but the other
   tiers need the replacement disk partitioned identically, then rebuilt
   independently.

### Recommendation

**Build it, but keep it simple.** Three constraints:

1. **v1 is planning + printing only.** The `shr plan` command computes the
   layout and prints the sgdisk + mdadm/LHSR commands. The user executes them
   manually (or pipes to bash if they trust it). This avoids the rollback
   problem entirely.
2. **No auto-rebalance.** v1 does not support adding disks to an existing
   layout. The user creates the layout once and lives with it.
3. **LHSR mode is optional.** mdadm mode is the default (battle-tested).
   LHSR mode is `--lhsr` for users who want self-healing.

This reduces the tool to a **layout calculator + command generator**. The
complex parts (rollback, partition management, LVM setup) are left to
existing tools. The value is in the algorithm: determining the optimal
set of tiers for N disks of varying sizes.

**If only `shr plan` is built**, the tool is ~500 lines and 3-4 sessions.
The remaining risk is in the algorithm correctness, not in disk manipulation.

---

## Appendix A: Synology SHR Compatibility

LHSR SHR is NOT binary-compatible with Synology SHR arrays. Synology uses:

- A proprietary partition table format on each disk (not standard GPT)
- mdadm RAID levels under the hood (yes, Synology uses Linux mdadm)
- LVM2 for volume management
- Custom superblock format on mdadm arrays (mdadm --examine --brief reveals this)

LHSR SHR IS architecturally equivalent: partition → mdadm/LHSR → LVM.
The algorithm is the same greedy tiering. But the partition formats differ.

**If you need to read Synology SHR disks on Linux:**
- `mdadm --examine` on each partition
- `lvm vgscan` and `lvm vgchange -ay` to activate the VG
- This works on standard Linux because Synology uses standard mdadm + LVM

**LHSR SHR does NOT need to match Synology formats.** It's a separate
implementation that solves the same problem the same way.
