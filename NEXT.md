# NEXT — LHSR session pickup + runnable backlog

Last updated: 2026-10-10. `main` is green and pushed (run `git log --oneline -5`
to confirm; HEAD should be at or after `3a3b5f1`).

## Picking up a session

1. Read this file, then `PRODUCTION_READINESS.md` (tail first).
2. Memory keys (octopoda, agent `keith`): `lhsr_gambit_20261010`,
   `lhsr_verdict_20261010`, `lhsr_promoted_20261010`,
   `lhsr_main_confirm_20261010`, `lhsr_recovery_memset_panic`.
3. Debug branch `debug/bio-shape-20261009` holds throwaway probes — never
   merge it. `main` carries fixes only.
4. VM rule: never test on bare metal. Fresh overlay per campaign, build the
   module **in the guest**, confirm the build ID before trusting anything.

## VM inventory (`~/vm-work/`)

- `lhsr-gpf/` — working test VM (Debian 12, port 2225, key `~/.ssh/id_ed25519`).
  May be running or stopped; check `ps` + SSH before assuming.
- `debian-12-genericcloud-amd64.qcow2` — pristine base. Never boot it directly;
  use `-b` overlays.
- `debian-13-genericcloud-amd64.qcow2` — staged for item 2, untouched.
- `debian-12-test.qcow2`, `vm.qcow2`, `*_bench*` — prior sessions, treat as
  haunted. The two bench VMs share one disk image; do not trust their state.

## Runnable backlog (from the production report's outside list)

### 1. RAID6 dual-failure decode — highest value, runnable now
Needs: any healthy RAID6 array (create per Recovery-Runbook shape if none).
```bash
# write baseline + sha, then fail TWO data disks
sudo dmsetup message <dev> 0 'disk_fail 0'
sudo dmsetup message <dev> 0 'disk_fail 1'
sudo dd if=/dev/mapper/<dev> of=/tmp/dual.bin bs=1M skip=1 count=200 iflag=direct
sha256sum /tmp/dual.bin   # must equal baseline sha
```
Scripted version exists: `tests/test-raid6-double-failure.sh` (verify it
matches current message syntax before trusting it). Pass = Vandermonde path
executes on current code for the first time. Record in `docs/wiki/Test-Matrix.md`.

### 2. Debian 13 runtime — image staged, needs a fresh VM
```bash
cd ~/vm-work
# seed with your key, boot on a new port (2225 may be taken)
cloud-localds d13-seed.iso user-data-13 meta-data-13   # debian user, your pubkey
qemu-img create -f qcow2 -b debian-13-genericcloud-amd64.qcow2 -F qcow2 d13-root.qcow2
# boot like lhsr-gpf but 6.12 guest: install headers, build module IN guest,
# confirm build ID, then run the standard stress (item: btrfs + fio below)
```
Then: create array, mkfs, mount, 10-min fio, watch `rmw_stats`/`rmw_locks`.
Kernel skew has bitten once (standalone FLUSH) — any dmesg warning stops the run.

### 4. Write performance — harnesses exist, no post-fix numbers
```bash
sudo ./tests/perf/bench.sh both 3     # mdadm vs lhsr, seqwrite + randwrite
sudo ./tests/test-benchmark.sh        # max_active/chunk-size sweeps
```
Record in `docs/wiki/Performance-Notes.md` with hardware + build ID.
VM numbers are indicative only; real numbers need item 3's hardware.

### 5. Daemon + SHR tiers end to end
```bash
make -C userspace/cli && make -C userspace/daemon
sudo ./userspace/cli/lhsrctl shr create ...    # see lhsrctl --help
sudo ./userspace/cli/lhsrctl shr expand ...
sudo ./userspace/cli/lhsrctl shr status ...
# daemon: build lhsrd, run against the test array, check /run/lhsrd.status
```
Last ran Jun 19 (pre-hunt). Re-run on current `main`, record in Test-Matrix.

### 3. Real disks — BLOCKED on hardware
Needs: physical SATA/NVMe + host with 6.12. Then items 1/2/4 rerun there,
plus SMART polling and failover behavior. Nothing to stage until it lands.

## Standard stress (used for every cell above)

```bash
# in guest: btrfs on the array at /mnt/lhsr, then
fio --directory=/mnt/lhsr --ioengine=sync --direct=0 --bs=4k \
    --rw=randwrite --numjobs=4 --size=512m --runtime=600 --time_based
# watch from host: rmw_stats (works==done, errors==0), rmw_locks (no aging),
# dmesg (no oops), mount stays rw. Then: btrfs scrub start -B /mnt/lhsr.
```

## Standing rules

- One fault, one fix. No drive-by refactors in kernel code.
- Zero new compiler warnings, every build, both kernels.
- Fresh build ID per binary. No ID, didn't happen.
- `git push` only with explicit say-so. Debug branches never merge.
