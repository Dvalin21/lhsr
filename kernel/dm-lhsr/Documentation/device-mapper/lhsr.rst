.. SPDX-License-Identifier: GPL-3.0-only

=============
dm-lhsr
=============

Linux Hybrid Self-Healing RAID

Description
===========

LHSR is a device-mapper target that implements RAID levels 0, 1, 5, and 6
with automatic media error detection and recovery (self-healing). It operates
at the block level, handling I/O in 4KB chunks with per-block CRC32c
checksums for integrity verification.

RAID Levels
===========

Level 0 — Striping (raid_type=0)
    Data is striped across all member devices. No redundancy.
    Capacity = sum of all device capacities.

Level 1 — Mirroring (raid_type=1)
    Data is mirrored across all member devices. Reads are served
    from the first available device; writes go to all members.
    Capacity = smallest device capacity.

Level 5 — Single Parity (raid_type=2)
    Data and P parity are striped across all devices. Supports
    single-disk failure. Uses read-modify-write for small writes.

Level 6 — Dual Parity (raid_type=3)
    Data plus P and Q parity distributed across all devices.
    Supports two-disk failure. Q parity uses GF(2^8) arithmetic
    with Russian peasant multiplication.

Table Parameters
================

::

   <raid_type> <num_devices> <device> <offset> ...

raid_type:
    0 = RAID0 (striping)
    1 = RAID1 (mirroring)
    2 = RAID5
    3 = RAID6

num_devices:
    Number of member devices (2-32).

device offset:
    Repeated for each device: block device path and start sector offset.

Status Output
=============

The ``dmsetup status`` command outputs::

    <raid_type> <num_devices> <chunk_sectors> <disk_info> <state> <failed>
    <generation>

Where:
    raid_type       RAID level identifier
    num_devices     Number of member devices
    chunk_sectors   Stripe chunk size in sectors (default 8 = 4KB)
    disk_info       Per-disk state flags ('A' = alive, 'F' = failed)
    state           Array state (0 = offline, 1 = healthy, 2 = degraded)
    failed          Bitmask of failed disks
    generation      Superblock generation counter

Self-Healing
============

The scrubber periodically reads every block across all devices,
verifies its CRC32c checksum, and reports or repairs corruption.

- Scrubbing runs as a delayed workqueue item.
- Verified checksums are cached to avoid re-scanning on restart.
- Corrupted blocks are logged; repair is attempted from parity.

Write-Hole Mitigation
=====================

RAID5/6 writes use REQ_FUA on parity update bios to ensure parity
data reaches stable storage. This mitigates the classic RAID write-hole
where a crash between data and parity writes produces inconsistent stripes.

A full write-intent bitmap/journal is planned for future releases.

Message Interface
=================

The ``dmsetup message`` interface supports:

``mark_failed <disk_idx>``
    Manually mark a disk as failed.

``mark_online <disk_idx>``
    Restore a disk after replacement or repair.

``write_verify {none|simple|full}``
    Query or set write-verify mode (currently only "none" implemented).

``config``
    Display current array configuration.

``persist``
    Force superblock persistence (writes current state to all disks).

``rebuild_start <disk_idx>``
    Initiate rebuild to the specified disk.

``rebuild_status``
    Query rebuild progress.

``scrub_start``
    Start a full scrub cycle.

``scrub_status``
    Query scrub progress.

Example
=======

Create a 3-disk RAID5 array::

    # dmsetup create myarray --table '0 8388608 lhsr 2 3 /dev/sda 0 /dev/sdb 0 /dev/sdc 0'

