# Test matrix

What has actually been run, on what. Blanks are the roadmap. Every green
cell names a build ID — results without one don't count.

Harness: fresh Debian 12 cloud VM (6.1), 4×2GB virtio disks, btrfs unless
noted, `fio` 4-job 4K randwrite for stress. See `vm-work/` notes and
`NEXT.md` for the runnable backlog.

## Green (Oct 2026)

| Scope | Build | Result |
|-------|-------|--------|
| RAID5 64KB file IO, 10 min | 20261010-01 | 556K RMW, 0 err |
| RAID6 64KB file IO, 10 min | 20261010-01+ | 452K RMW, 0 err |
| 4KB-chunk regression, 10 min | 20261010-01+ | 964K RMW, 0 err |
| RAID5→RAID6 reshape, 64KB | 20261010-02 | COMPLETE, 200MB sha identical |
| Degraded RAID6 1-disk 200MB read | 20261010-04 | sha identical |
| Disk rebuild to healthy | 20261010-04 | COMPLETE, sha identical |
| btrfs scrub 3.56GB post-stress | — | 0 errors |
| Dirty bitmap recovery | 20261010-01+ | COMPLETE, twice |

## Blank (not run — in priority order)

1. **RAID6 dual-failure decode** — code exists, never executed. Highest value.
2. **Debian 13 runtime** — image staged at `vm-work/debian-13-genericcloud-amd64.qcow2`;
   needs a fresh VM + the standard stress. Watch for 6.12 behavior skew.
3. **Write performance** — `tests/bench.sh` + `tests/test-benchmark.sh` exist;
   no post-fix numbers yet. VM numbers are indicative only.
4. **Daemon/SHR tiers** — `lhsrctl shr create/expand/status/destroy` end to
   end on current `main` (last ran Jun 19, pre-hunt).
5. **Real disks** — blocked on hardware. SMART polling, failover, NCQ depth,
   and the 5–11x gap all need spinning rust/NVMe to mean anything.

## How to run a cell

1. Fresh VM from the base image (never reuse a crashed guest's overlay).
2. Build the module **in the guest** for its kernel; confirm the build ID.
3. Follow [[Recovery Runbook]] / [[Reshape Runbook]] shapes; checksums at
   every step; `rmw_stats` errors must stay 0.
4. Record the row above with the build ID. No ID, didn't happen.
