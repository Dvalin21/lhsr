# LHSR dmsetup Hang Fix & Documentation

## Problem: dmsetup create Hangs

### Root Cause (from testing 2026-04-30):
- `udev-worker (pid 3608) hung in D state` when running `dmsetup create`
- This is a **udev issue**, NOT a kernel module bug
- udev waits for device events that never complete properly

### Solution: Use --noudevsync Flag

**WRONG (hangs)**:
```bash
sudo dmsetup create lhsr_test --table "0 $(sudo blockdev --getsize /dev/sdb) lhsr single /dev/sdb"
```

**CORRECT (bypasses udev)**:
```bash
sudo dmsetup create lhsr_test --table "0 $(sudo blockdev --getsize /dev/sdb) lhsr single /dev/sdb" --noudevsync
```

## Timeout Mechanism Added (LHSR v1.3.0)

Added `lhsr_submit_bio_timeout()` function (lines 32-66 in dm-lhsr.c):
- 5-second timeout for all BIO operations
- Returns `-ETIMEDOUT` on timeout
- Prevents kernel module from hanging indefinitely

## Complete Test Sequence (Fixed)

```bash
# 1. Load dm-mod
sudo modprobe dm-mod

# 2. Load LHSR module
sudo insmod /home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko

# 3. Verify module loaded
lsmod | grep lhsr
dmesg | grep -i lhsr | tail -10

# 4. Create test device (WITH --noudevsync!)
sudo dmsetup create lhsr_test --table "0 $(sudo blockdev --getsize /dev/sdb) lhsr single /dev/sdb" --noudevsync

# 5. Verify device created
sudo dmsetup status lhsr_test
ls -la /dev/mapper/lhsr_test

# 6. Cleanup
sudo dmsetup remove lhsr_test
sudo rmmod dm_lhsr
```

## Drive Wipe Script

Created: `/home/keith/lhsr/scripts/wipe-drives-for-testing.sh`

**WARNING**: This destroys all data on sda, sdb, sdc, sdd!

```bash
# Review before running:
cat /home/keith/lhsr/scripts/wipe-drives-for-testing.sh

# Run with sudo:
sudo /home/keith/lhsr/scripts/wipe-drives-for-testing.sh
```

## Toshiba Firmware Issue

The Toshiba MG04ACA400N drive (firmware FJ5A) has known issues:
- **FJ3D firmware** (April 2018) fixes "LBA Counter Register read sequence error"
- This matches symptoms: drive hangs, dmsetup issues, password prompt on boot

### Firmware Update (when ready):
```bash
# Download from Dell (Linux BIN file):
# https://www.dell.com/support/home/en-au/drivers/driversdetails?driverid=9kx6f
# File: Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN

chmod +x Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN
sudo ./Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN --force
```

## References

1. **LHSR Test Plan**: `octopoda_recall('keith', 'lhsr_test_plan')`
2. **Test Results**: `octopoda_search('keith', 'lhsr_test')`
3. **Toshiba Research**: `/home/keith/lhsr/docs/toshiba-firmware-research.md`
4. **Previous Session**: `octopoda_recall('keith', 'session_state_2026-04-30')`
