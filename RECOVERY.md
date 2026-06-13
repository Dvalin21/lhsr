# LHSR Disk Failure Recovery Guide

**Last Updated:** 2026-06-13
**Applies to:** LHSR v5.x (dm-lhsr kernel module + userspace tools)
**License:** GPLv3

---

## Table of Contents

1. [What This Guide Covers](#1-what-this-guide-covers)
2. [Prerequisites](#2-prerequisites)
3. [Recovery Tools Quick Reference](#3-recovery-tools-quick-reference)
4. [Scenario A: Single Disk Failed, Array Still Intact](#4-scenario-a-single-disk-failed-array-still-intact)
5. [Scenario B: Array Won't Assemble (Too Few Disks)](#5-scenario-b-array-wont-assemble-too-few-disks)
6. [Scenario C: Offline Reconstruction of a Dead Disk](#6-scenario-c-offline-reconstruction-of-a-dead-disk)
7. [Scenario D: Fresh Array with Missing Disks](#7-scenario-d-fresh-array-with-missing-disks)
8. [Scenario E: Multiple Failures / RAID6 Dual Failure](#8-scenario-e-multiple-failures--raid6-dual-failure)
9. [Full Walkthrough: RAID5 Disk Failure](#9-full-walkthrough-raid5-disk-failure)
10. [Appendix: Superblock Layout](#10-appendix-superblock-layout)

---

## 1. What This Guide Covers

This document covers recovery from **physical disk failure** in an LHSR-managed
RAID array. It assumes you understand RAID levels but have NO prior knowledge
of LHSR recovery tools.

**What LHSR CAN do during recovery:**

| Capability | Where | When |
|-----------|-------|------|
| Read side reconstruction | Kernel module, online | Array still assembled, degraded |
| Degraded assembly (missing disks) | Kernel module, online | Array won't assemble, need total_disks=N |
| Parity reconstruction (offline) | `lhsrctl reconstruct` | Disk is dead, need to recreate it |
| Device scanning + recovery plan | `lhsrctl recover` | Don't know what you have |
| Deep superblock scan | `lhsr-scan --deep --json` | Superblocks are corrupt or missing |

**What LHSR CANNOT do (yet):**
- Dual-disk RAID6 recovery (needs Reed-Solomon online rebuild — Phase 8)
- Live reshape (add/remove disks while online)
- SHR variable-size disk repair

If you need any of those, see [ROADMAP.md](ROADMAP.md) for status.

---

## 2. Prerequisites

```bash
# Check module loaded
lsmod | grep dm_lhsr

# Check tools installed
which lhsrctl
which lhsr-scan

# Check kernel messages for disk errors
dmesg | grep -i "I/O error\|failed\|offline" | tail -20

# Check SMART status of suspect disks
sudo smartctl -H /dev/sdX
```

If the module is not loaded:
```bash
sudo insmod /path/to/dm-lhsr.ko
```

---

## 3. Recovery Tools Quick Reference

| Tool | Purpose | Syntax |
|------|---------|--------|
| `lhsr-scan` | Read superblocks from block devices | `lhsr-scan [-v] [--deep] [--json] [device...]` |
| `lhsrctl recover` | Scan disks and generate `dmsetup create` commands | `lhsrctl recover <device>...` |
| `lhsrctl reconstruct` | Offline XOR reconstruction of a missing RAID5/6 disk | `lhsrctl reconstruct --output <file> <device>...` |
| `dmsetup create` | Assemble the kernel target | See `lhsrctl recover` output |

---

## 4. Scenario A: Single Disk Failed, Array Still Intact

**Situation:** The array is still assembled and accessible. One disk has failed
(stuck I/O, read errors, SMART threshold exceeded). The kernel is already
reconstructing reads from parity or the mirror.

**Step 1: Identify the failed disk**

```bash
# Check array status
sudo dmsetup status <device>
# Look for "DEGRADED" in the status line

# Check kernel messages for I/O errors
dmesg | grep -A2 "I/O error" | tail -10
```

The status output for a DEGRADED array looks like:
```
0 261584 lhsr DEGRADED RAID5 2/3 err=0
```

The table output confirms which disk count is missing:
```
0 261584 lhsr UUID=<uuid> RAID=2 DISKS=3 INTEGRITY=0 DEGRADED=1 TYPE=RAID5
```

**Step 2: Confirm reads still work**

```bash
# Read a test area (must succeed via parity reconstruction)
sudo dd if=/dev/mapper/<device> of=/dev/null bs=1M count=100 status=progress
```

If reads fail with I/O errors, you have more than one failure or a corrupted
array. Stop and proceed to Scenario E.

**Step 3: Replace the failed disk**

```bash
# Remove the failed disk from the array
sudo dmsetup message <device> 0 "remove-disk <failed_disk_index>"

# Physically replace the disk (power down, swap, power up)

# Add the new disk — triggers full rebuild
sudo dmsetup message <device> 0 "add-disk <new_device>"
```

The rebuild runs in the background. Monitor with:
```bash
sudo dmsetup status <device>
```

---

## 5. Scenario B: Array Won't Assemble (Too Few Disks)

**Situation:** One or more disks have failed. The array disassembled (kernel
unloaded, system reboot, or you removed it manually). You cannot reassemble
because the `dmsetup create` command requires exactly `disk_count` devices.

**Step 1: Scan surviving disks for superblocks**

```bash
# Auto-detect and scan all block devices
lhsr-scan -v

# Or scan specific devices
lhsr-scan /dev/sdb /dev/sdc /dev/sdd

# For deeper search (orphaned superblocks beyond standard positions)
lhsr-scan --deep --json /dev/sdb /dev/sdc /dev/sdd
```

Example output:
```
/dev/sdb:       FOUND  LHSR v1  uuid=abc123  type=RAID5  disk=0/3
/dev/sdc:       FOUND  LHSR v1  uuid=abc123  type=RAID5  disk=1/3
/dev/sdd:       no LHSR superblock
```

This tells you: array `abc123` is a 3-disk RAID5. Disks 0 and 1 found.
Disk 2 (/dev/sdd) has no readable superblock.

**Step 2: Generate assembly commands**

```bash
lhsrctl recover /dev/sdb /dev/sdc
```

Output:
```
LHSR Recovery
=============

Scanning 2 device(s)...

[1/2] /dev/sdb ... FOUND  array=abc123 disk=0/3 type=RAID5
[2/2] /dev/sdc ... FOUND  array=abc123 disk=1/3 type=RAID5

Summary
=======
UUID: abc123
Type: RAID5
Disks: 3 (2 present, 1 missing)
Parity: 1
Size: 261584 sectors (127 MB)

Assembly
========
sudo dmsetup create lhsr_abc123 --table "0 261584 lhsr raid5 8 1 8 /dev/mapper/lhsr_abc123_ph_0 /dev/sdb 0 /dev/sdc 0 degraded total_disks=5"
```

**NOTE:** The `lhsrctl recover` output may use dm-zero placeholder names
(lhsr_<uuid>_ph_<idx>) — these are auto-created devices that read zeros
and discard writes. If you prefer to provide `total_disks=N` directly,
remove the placeholders and use the `degraded` flag with `total_disks=N`.

In practice, assemble with:
```bash
sudo dmsetup create lhsr_abc123 --table \
  "0 261584 lhsr raid5 8 1 8 /dev/sdb 0 /dev/sdc 0 degraded total_disks=3"
```

**Explanation of the table line:**
| Field | Value | Meaning |
|-------|-------|---------|
| `0` | Start sector | Always 0 |
| `261584` | Length sectors | From superblock |
| `lhsr` | Target type | LHSR DM target |
| `raid5` | RAID type | RAID5 with XOR parity |
| `8` | Chunk sectors | 4 KB chunks |
| `1` | Stripe depth | Sequential parity layout |
| `8` | Consistency sectors | For online consistency check |
| `/dev/sdb 0` | Device + offset | Disk 0 at sector offset 0 |
| `/dev/sdc 0` | Device + offset | Disk 1 at sector offset 0 |
| `degraded` | Flag | Accept N-1 disks |
| `total_disks=3` | Count | Expected total = 3 disks |

**Step 3: Verify the degraded array**

```bash
# Check status
sudo dmsetup status lhsr_abc123
# Expected: "0 261584 lhsr DEGRADED RAID5 2/3 err=0"

# Read data (will reconstruct from parity)
sudo dd if=/dev/mapper/lhsr_abc123 of=/dev/null bs=1M count=100

# Confirm write rejection (degraded array is read-only)
echo "test" | sudo dd of=/dev/mapper/lhsr_abc123 bs=512 count=1
# Expected: dd: error writing: Input/output error
```

**Reads will succeed** as long as at most 1 disk is missing (RAID5) or 2 disks
(RAID6). The kernel reconstructs the missing data from parity on every read.

**Step 4: Replace the failed disk and bring it online**

```bash
# Physical replacement: swap the failed disk and partition it

# Add the new disk to the array (this triggers a full sync)
# This requires reloading the table with all disks present
sudo dmsetup remove lhsr_abc123
sudo dmsetup create lhsr_abc123 --table \
  "0 261584 lhsr raid5 8 1 8 /dev/sdb 0 /dev/sdc 0 /dev/sdd 0"

# If the kernel supports online rebuild:
sudo dmsetup message lhsr_abc123 0 "rebuild"
```

---

## 6. Scenario C: Offline Reconstruction of a Dead Disk

**Situation:** A disk is physically dead — doesn't spin up, controller won't
talk to it. The array was using RAID5 or RAID6 and you need to recreate the
missing disk's contents so you can dd it onto a new disk.

**This is an OFFLINE operation.** The array should NOT be assembled while
you do this. Work from the surviving disks only.

**Step 1: Verify survivors are consistent**

```bash
# Scan survivors for superblock metadata
lhsr-scan /dev/sdb /dev/sdc

# Confirm they belong to the same array (same UUID) and identify
# which disk index is missing.
```

**Step 2: Reconstruct the missing disk**

```bash
# For RAID5: XOR of all survivors produces the missing disk's content
lhsrctl reconstruct --output /tmp/recovered_disk.img /dev/sdb /dev/sdc
```

The tool:
- Scans each survivor for superblocks
- Validates they belong to the same array
- Auto-detects which disk index is missing
- XORs every byte across survivors
- Writes a valid superblock into the output
- Reports progress every 256 MB

**Step 3: Write to replacement disk**

```bash
# dd the reconstructed image onto a new disk of equal or larger size
sudo dd if=/tmp/recovered_disk.img of=/dev/sdd bs=1M status=progress

# Assemble with all 3 disks
lhsrctl recover /dev/sdb /dev/sdc /dev/sdd
# Follow the generated dmsetup commands
```

**What if there's no output path?**

If you want to pipe the reconstruction directly to a disk:

```bash
# On Linux, /dev/sdd would be the replacement
sudo lhsrctl reconstruct --output /dev/sdd /dev/sdb /dev/sdc

# Or pipe through dd
lhsrctl reconstruct --output /dev/stdout /dev/sdb /dev/sdc | \
  sudo dd of=/dev/sdd bs=1M status=progress
```

**Checking the output:**

The reconstructed output has the same size as the survivors. If you want
to verify it contains valid data (not just zeros), check for non-zero
sectors:

```bash
sudo dd if=/tmp/recovered_disk.img bs=1M 2>/dev/null | xxd | head -5
```

---

## 7. Scenario D: Fresh Array with Missing Disks

**Situation:** You want to create an LHSR array but some disks are not yet
available (still shipping, RMA replacement pending). You want to start using
the array now with N-1 disks and add the missing one later.

**Step 1: Create the array with `degraded + total_disks=N`**

```bash
# 2 disks available, but ultimately want 3-disk RAID5
sudo dmsetup create my-array --table \
  "0 200000 lhsr raid5 8 1 8 /dev/sdb 0 /dev/sdc 0 degraded total_disks=3"
```

This creates a 3-disk RAID5 with 2 present disks. The missing disk slot
(position 2) returns zeros on read and discards writes. The array is
read-write on the present disks, but parity is inconsistent for the
missing slot.

**Step 2: Use the array (read-only for parity-safe ops)**

```bash
# Format as filesystem — this WILL write to present disks
# WARNING: Only the present disks get data/parity. The missing disk's
# stripe positions are NOT updated. Eventually full parity requires
# a rebuild.

# For safety, DON'T write to the degraded array unless you plan to
# immediately rebuild when the missing disk arrives.
```

**Step 3: When the missing disk arrives**

```bash
# Remove the degraded array
sudo dmsetup remove my-array

# Create the full array with all disks
sudo dmsetup create my-array --table \
  "0 200000 lhsr raid5 8 1 8 /dev/sdb 0 /dev/sdc 0 /dev/sdd 0"

# Trigger full rebuild to reconstruct parity on the new disk
sudo dmsetup message my-array 0 "rebuild"
```

---

## 8. Scenario E: Multiple Failures / RAID6 Dual Failure

**Situation:** More disks have failed than the RAID level can tolerate
(2 for RAID5, 3 for RAID6). Data is partially or fully lost.

**What LHSR can do:**

1. **RAID5 + 1 failed disk** → Reconstruct from parity (Scenario C)
2. **RAID5 + 2 failed disks** → PARTIAL recovery possible IF only one
   disk is data and the other is parity. XOR works with N-2 survivors
   to recover a single missing data disk IF you know which one is parity.
3. **RAID6 + 1 failed disk** → Reconstruct from parity (Scenario C)
4. **RAID6 + 2 failed disks** → **NOT SUPPORTED** — needs Reed-Solomon
   dual-failure recovery (GF(2^8) matrix inversion). This is Phase 8.

**What to do for unsupported cases:**

```bash
# Scan all survivors — check if any superblocks remain
lhsr-scan --deep /dev/sdb /dev/sdc /dev/sdd /dev/sde

# If any single disk has a clean superblock, try reconstructing each
# missing disk one at a time (you have to guess which disk index is
# missing and adjust the reconstruction order).
```

**Last resort:** Use `ddrescue` to clone survivors before attempting
any recovery. Make disk images first:

```bash
sudo ddrescue /dev/sdb /tmp/sdb.img /tmp/sdb.log
sudo ddrescue /dev/sdc /tmp/sdc.img /tmp/sdc.log
# Then run recovery tools on the images
lhsrctl reconstruct --output /tmp/rec.img /tmp/sdb.img /tmp/sdc.img
```

---

## 9. Full Walkthrough: RAID5 Disk Failure

This is a real-world walkthrough from failure to full recovery. It uses
loopback devices but the procedure is identical for physical drives.

### Initial State

3-disk RAID5: `/dev/sdb` (disk 0), `/dev/sdc` (disk 1), `/dev/sdd` (disk 2).

```bash
sudo dmsetup status my-data
# 0 261584 lhsr OK RAID5 3/3 err=0
```

### Failure Detection

Kernel logs show `/dev/sdd` dropped offline:

```bash
dmesg | tail
# [12345.678] sd 0:0:3:0: device offline
# [12345.679] I/O error on dm-0, sector 123456
# [12345.680] LHSR: Read error on disk 2, reconstructing from parity
```

### Step 1: Scan survivors

```bash
lhsr-scan /dev/sdb /dev/sdc /dev/sdd
# /dev/sdb:  FOUND LHSR v1 uuid=0xdeadbeef type=RAID5 disk=0/3
# /dev/sdc:  FOUND LHSR v1 uuid=0xdeadbeef type=RAID5 disk=1/3
# /dev/sdd:  no LHSR superblock (or device not found)
```

### Step 2: Generate recovery commands

```bash
lhsrctl recover /dev/sdb /dev/sdc
```

This prints the `dmsetup create` command needed.

### Step 3: Assemble degraded

```bash
sudo dmsetup create lhsr_deadbeef --table \
  "0 261584 lhsr raid5 8 1 8 /dev/sdb 0 /dev/sdc 0 degraded total_disks=3"
```

### Step 4: Verify data is readable

```bash
# Mount the degraded array and check filesystem integrity
sudo fsck /dev/mapper/lhsr_deadbeef

# Or just check block-level reads
sudo dd if=/dev/mapper/lhsr_deadbeef of=/dev/null bs=1M count=100
```

### Step 5: Reconstruct the dead disk offline

```bash
lhsrctl reconstruct --output /tmp/disk2_recovered.img /dev/sdb /dev/sdc
```

### Step 6: Replace the physical disk

```bash
# Shut down, swap /dev/sdd with new disk, partition it
sudo dd if=/tmp/disk2_recovered.img of=/dev/sdd bs=1M status=progress
```

### Step 7: Assemble the full array

```bash
# Remove degraded array
sudo dmsetup remove lhsr_deadbeef

# Create full array
sudo dmsetup create lhsr_deadbeef --table \
  "0 261584 lhsr raid5 8 1 8 /dev/sdb 0 /dev/sdc 0 /dev/sdd 0"

# Verify full health
sudo dmsetup status lhsr_deadbeef
# 0 261584 lhsr OK RAID5 3/3 err=0
```

### Recovery Complete

---

## 10. Appendix: Superblock Layout

LHSR stores a 128-byte superblock at two positions on every member disk:

| Position | Sector offset | Purpose |
|----------|---------------|---------|
| Primary | 8 sectors from start | Standard location |
| Backup | Last 8 sectors of device | Recovery if primary is corrupt |

The superblock contains:

```c
struct lhsr_superblock {
    uint64_t    magic;              // 0x4C4853525F524149 ("LHSR_RAI")
    uint32_t    version;            // Superblock version (currently 1)
    uint32_t    flags;              // Feature flags
    uint64_t    array_uuid;         // Unique array identifier
    uint64_t    disk_uuid;          // Unique per-disk identifier
    uint32_t    disk_index;         // Position in array (0..disk_count-1)
    uint32_t    disk_count;         // Total disks in array
    uint32_t    raid_type;          // 1=RAID0, 2=RAID1, 3=RAID5, 4=RAID6
    uint32_t    disk_state;         // HEALTHY=0, DEGRADED=1, FAILED=2, MISSING=3
    uint64_t    total_sectors;      // Total device capacity (in sectors)
    uint64_t    generation;         // Monotonic counter for superblock ordering
    uint64_t    creation_time;      // Unix timestamp of array creation
    uint32_t    chunk_sectors;      // Stripe chunk size in sectors
    uint32_t    stripe_depth;       // Parity layout depth
    uint32_t    consistency_sectors;// Online consistency check granularity
    uint32_t    csum;               // CRC32c of entire superblock
    uint8_t     reserved[36];       // Future use (zeroed)
} __attribute__((packed));
```

**Key fields for recovery:**
- `magic`: Must match `0x4C4853525F524149` — otherwise no LHSR superblock
- `array_uuid`: All disks in the same array share this value
- `disk_index`: Tells you which position this disk holds
- `disk_count`: How many total disks the array expects
- `generation`: Higher = newer superblock (backup may be stale)
- `csum`: CRC32c over the whole superblock — invalid means corruption

The `--deep` flag on `lhsr-scan` scans beyond the two standard positions,
searching the last ~2048 sectors for orphaned superblocks. This catches
superblocks written to wrong locations by bugs or partial writes.

`lhsrctl recover` reads these fields from each provided device, groups
by `array_uuid`, identifies present vs. missing `disk_index` values, and
generates the correct `dmsetup create` table.
