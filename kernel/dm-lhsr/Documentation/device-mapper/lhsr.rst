.. SPDX-License-Identifier: GPL-3.0-only

=============
dm-lhsr
=============

Linux Hybrid Self-Healing RAID

Description
===========

LHSR is a device-mapper target that implements RAID types ``single``
(striping), ``mirror`` (RAID1), ``raid5``, and ``raid6`` with automatic
media error detection and recovery (self-healing). It operates at the
block level, handling I/O in 4 KB chunks.  Per-block CRC32c checksums
are planned for a future phase.

RAID Levels
===========

Single — Striping (``single``)
    Data is striped across all member devices. No redundancy.
    Capacity = sum of all device capacities.
    Alias: ``RAID0`` in legacy contexts.

Mirror — Mirroring (``mirror``)
    Data is mirrored across 2 member devices. Reads are served
    from the first available device; writes go to all members.
    Capacity = smallest device capacity.
    Alias: ``RAID1`` in legacy contexts.

RAID5 — Single Parity (``raid5``)
    Data and P parity are striped across all devices. Supports
    single-disk failure. Uses read-modify-write for small writes.

RAID6 — Dual Parity (``raid6``)
    Data plus P and Q parity distributed across all devices.
    Supports two-disk failure. Q parity uses GF(2^8) arithmetic
    with Russian peasant multiplication.

Table Parameters
================

The device-mapper table format is::

   <raid_type> [<device> <offset>] ...

``raid_type`` is one of the string names above: ``single``, ``mirror``,
``raid5``, ``raid6``.  For ``single``, exactly one device follows.
For ``mirror``, exactly two devices follow.  For ``raid5`` / ``raid6``,
at least three devices follow.

Each device pair is:

``device``
    Full path to the block device (e.g. ``/dev/loop0``).

``offset``
    Start sector offset on that device (typically ``0``).

Example::

    # dmsetup create myarray --table '0 8388608 lhsr raid5 /dev/loop0 0 /dev/loop1 0 /dev/loop2 0'

Status Output
=============

``dmsetup status`` (``STATUSTYPE_INFO``) output depends on the RAID type:

**Single (striping)**::

    OK <disks>/<sectors>

**Mirror (RAID1)**::

    <state> <healthy>/<total> err=<io_errors> fail=<failovers>

``state`` is ``OK`` or ``DEGRADED``.  ``healthy`` is the number of
non-failed devices.

**RAID5 / RAID6**::

    <state> <disks>/<total> <chunk> <failed_bitmask>

If the scrubber is active, ``scrub=<state>-<verified>`` is appended
to all types, where ``state`` is ``RUN``, ``PAUSED``, or ``DONE``.

``dmsetup table`` (``STATUSTYPE_TABLE``)::

    UUID=<uuid> RAID=<type_num> DISKS=<count> [TYPE=<raid5|raid6>]

``TYPE`` is shown only for RAID5 and RAID6.

Self-Healing — Planned (Phase 1+)
==================================

Per-block CRC32c checksum verification is designed but not yet
implemented.  The current module handles media errors by failing
the affected I/O and marking the disk as failed when the error
threshold is exceeded.  True self-healing (checksum verification
and parity-based reconstruction) will follow in Phase 1.

Write-Hole Mitigation
=====================

RAID5/6 writes use REQ_FUA on parity update bios to ensure parity
data reaches stable storage. This mitigates the classic RAID write-hole
where a crash between data and parity writes produces inconsistent stripes.

A full write-intent bitmap/journal is planned for a future phase.

Message Interface
=================

The ``dmsetup message`` interface supports:

``mark_failed <disk_idx>``
    Manually mark a disk as failed.  The disk index is 0-based.

``mark_online <disk_idx>``
    Restore a disk after replacement or repair.

``config``
    Display current array configuration.

``persist``
    Force superblock persistence (writes current state to all disks).

``rebuild_start <disk_idx>``
    Initiate rebuild to the specified disk (not yet implemented —
    currently a no-op placeholder).

``rebuild_status``
    Query rebuild progress (not yet implemented).

``scrub_start``
    Start a full scrub cycle (not yet implemented).

``scrub_status``
    Query scrub status (not yet implemented).

Example
=======

Create a 3-disk RAID5 array with 4 GB of usable space::

    # dmsetup create myarray --table '0 8388608 lhsr raid5 /dev/loop0 0 /dev/loop1 0 /dev/loop2 0'

Create a single-disk (non-RAID) array::

    # dmsetup create myarray --table '0 8388608 lhsr single /dev/loop0 0'

Create a 2-disk mirror::

    # dmsetup create myarray --table '0 8388608 lhsr mirror /dev/loop0 0 /dev/loop1 0'

