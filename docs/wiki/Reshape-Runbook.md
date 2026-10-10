# Reshape runbook (RAID5 → RAID6)

Adds a Q parity disk to a RAID5 array, in place, preserving data.
Writes are **rejected** while it runs — quiet the filesystem first
(unmount, or remount read-only). Reads keep working.

Chunks up to 64KB are supported. The array must be healthy (no failed
disks).

## Procedure

```bash
# 1. Baseline: record what is on the array BEFORE (paranoia pays)
sudo dd if=/dev/mapper/<dev> of=/tmp/pre-reshape.bin bs=1M skip=1 count=200 iflag=direct
sha256sum /tmp/pre-reshape.bin | tee /tmp/pre-reshape.sha

# 2. Start: add the new disk as Q parity
sudo dmsetup message <dev> 0 'reshape raid6 add /dev/<newdisk>'

# 3. Poll (takes minutes on gigabytes; ~0.9MB/s on a small VM)
watch -n 30 'sudo dmsetup message <dev> 0 "reshape status"'
# reshape: state=COMPLETE progress=100% (...) target=raid6

# 4. Verify: same bytes, new level
sudo dmsetup status <dev>   # OK RAID6 4/4 err=0
sudo dd if=/dev/mapper/<dev> of=/tmp/post-reshape.bin bs=1M skip=1 count=200 iflag=direct
sha256sum -c /tmp/pre-reshape.sha   # must match (compare against post file)
```

Verified Oct 2026: 3-disk RAID5 (64KB chunks) → 4-disk RAID6, 200MB
sha256-identical, no oops. See `deploy/lhsr-reshape.sh` for the scripted
version (`reshape <device> add <new-device>`, `reshape <device> status`).

## If it fails halfway

Reshape is crash-safe by design: unwritten stripes keep their RAID5
layout, completed stripes have valid Q. Restarting the reshape resumes
(or re-run `add`). If the box itself died mid-reshape, the dirty bitmap
regions replay on next assembly — check `dmesg` for `recovery COMPLETE`
before trusting the array.
