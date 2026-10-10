# Recovery runbook (dead disk drill)

The drill is: fail, prove degraded reads are exact, rebuild, prove
healthy reads are exact. Every step ends with a checksum — never trust
a rebuild you haven't read back.

```bash
DEV=lhsr-0

# 1. Record a baseline while healthy
sudo dd if=/dev/mapper/$DEV of=/tmp/base.bin bs=1M skip=1 count=200 iflag=direct
sha256sum /tmp/base.bin | tee /tmp/base.sha

# 2. Fail a disk
sudo dmsetup message $DEV 0 'disk_fail 0'
sudo dmsetup status $DEV     # DEGRADED RAID6 3/4 err=0

# 3. Prove degraded reads are exact (parity reconstruction)
sudo dd if=/dev/mapper/$DEV of=/tmp/degraded.bin bs=1M skip=1 count=200 iflag=direct
sha256sum /tmp/degraded.bin  # must equal base.sha

# 4. Rebuild onto the (replaced) disk; poll to COMPLETE
sudo dmsetup message $DEV 0 'rebuild start 0'
watch -n 60 'sudo dmsetup message $DEV 0 "rebuild status"'

# 5. Prove healthy reads are exact again
sudo dmsetup status $DEV     # OK RAID6 4/4 err=0
sudo dd if=/dev/mapper/$DEV of=/tmp/rebuilt.bin bs=1M skip=1 count=200 iflag=direct
sha256sum /tmp/rebuilt.bin   # must equal base.sha
```

Verified Oct 2026 on RAID6/64KB: degraded 200MB read sha256-identical,
rebuild COMPLETE, post-rebuild read sha256-identical.

Notes:

- Rebuild is slow on small VMs (~1MB/s with sync IO per chunk) and only
  copies dirty regions (WIB). A 2GB test array takes ~40 min.
- If any `sha256sum` mismatches, stop: capture `dmesg` and see the
  [[Failure Catalog]] before running anything else.
- `disk_online` exists to un-fail a disk; prefer real replacement.
