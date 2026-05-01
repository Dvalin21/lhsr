# Firmware Update Guide - All Drives (NOT NVMe)

## Priority Order:
1. **sdd (Toshiba)** - MATCHES SYMPTOMS, fix LBA Counter error
2. **sda (HGST)** - System drive, update for stability
3. **sdc (Western Digital)** - Update for reliability

---

## 1. sdd - TOSHIBA MG04ACA400N (4TB) - PRIORITY #1

### Current State:
- Firmware: FJ5A
- Serial: 69PQKE4XFSYC
- Issue: Password prompt, dmsetup hangs, LBA Counter errors

### Update Available:
- **Version**: FJ3D (April 2018)
- **Fixes**: LBA Counter Register read sequence error (EXACT match to symptoms!)
- **Source**: Dell (official)

### Download Options:

**Option A: Linux BIN file (Recommended)**
```bash
# Download from Dell (manual download required):
# URL: https://www.dell.com/support/home/en-au/drivers/driversdetails?driverid=9kx6f

# File: Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN
# Make executable:
chmod +x Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN

# Run update (Dell tool):
sudo ./Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN --force
```

**Option B: Windows EXE (Most Reliable)**
1. Download: `Serial-ATA_Firmware_9KX6F_WN64_FJ3D_A00_01.EXE`
2. Use Windows PC or create Windows PE bootable USB
3. Run Dell firmware utility
4. Reboot and verify with `smartctl -i /dev/sdd | grep Firmware`

**Option C: hdparm method (Advanced)**
```bash
# Check if drive supports firmware download:
sudo hdparm -I /dev/sdd | grep -i "firmware download"

# Requires .ftd firmware file (not publicly available for this model)
# See: https://syscall.eu/blog/2024/08/28/toshiba_hdd_firmware/
```

### Verification:
```bash
sudo smartctl -i /dev/sdd | grep -E "Firmware|Serial"
# Should show: Firmware Version: FJ3D
```

---

## 2. sda - HGST HUS724030ALA640 (3TB)

### Current State:
- Firmware: MF8OAC50
- Serial: PN1281P9GVK57X
- Latest Stable: MF8OAA70 (47.826% prevalence)

### Update Available:
- **Latest Version**: MF8OAA70
- **Source**: HDD Guru / Hitachi firmware databases
- **Download**: http://files.hddguru.com/download/Firmware%20updates/Hitachi/

### Files Available (from hddguru):
- `MF0PBC50.zip` - Firmware C50 for HUS724040ALA640, HUS724030ALA640, HUS724020ALA640
- `MJ07B580.zip` - Firmware 580 for HUS724040ALE640, HUS724030ALE640, HUS724020ALE640

### Update Method:
```bash
# Download firmware pack from hddguru
# Extract the correct .ftd or BIN file for MF8OAA70

# Use hdparm method:
sudo hdparm --fwdownload-mode3 firmware_file.ftd --yes-i-know-what-i-am-doing --please-destroy-my-drive /dev/sda
```

**Note**: HGST was acquired by Western Digital - check WD support site as well.

---

## 3. sdc - WL3000GSA6472E0 (3TB)

### Current State:
- Firmware: MF8OAAZ0
- Serial: P9HHPAPX
- Model: Western Digital WL3000GSA6472E0 (equivalent to WD30EZRX)

### Update Available:
- **Latest Found**: MJ8OA5F0 (from smarthdd.com)
- **Source**: Western Digital support / hddguru.com

### Research Links:
- http://files.hddguru.com/download/Firmware%20updates/Western%20Digital/
- https://support.wdc.com/

### Update Method:
```bash
# Western Digital drives typically use:
# 1. Western Digital Dashboard (Windows)
# 2. hdparm with .ftd file

# Check current firmware details:
sudo hdparm -I /dev/sdc | grep -E "Firmware|Model"
```

---

## 4. NVMe - DO NOT TOUCH

### nvme0n1 - SPCC M.2 PCIe SSD (1.8TB)
- **This is the system drive**
- **NO firmware updates recommended**
- Risk of bricking system if update fails

---

## Common Update Methods:

### Method 1: Dell/Manufacturer BIN/EXE (Easiest)
- Download manufacturer-provided tool
- Run as root (Linux) or in Windows
- Reboot to apply

### Method 2: hdparm --fwdownload (Advanced)
```bash
# Check support:
sudo hdparm -I /dev/sdX | grep -i "firmware download"

# Download firmware file (.ftd format)
# Apply:
sudo hdparm --fwdownload-mode3 firmware.ftd \
  --yes-i-know-what-i-am-doing \
  --please-destroy-my-drive /dev/sdX
```

### Method 3: Windows + Manufacturer Tool (Most Reliable)
- Create Windows PE bootable USB
- Download manufacturer's firmware update utility
- Run in Windows environment
- Reboot and verify

---

## Verification After All Updates:

```bash
# Check all drive firmware versions:
for drive in sda sdb sdc sdd; do
  echo "=== /dev/$drive ==="
  sudo smartctl -i /dev/$drive | grep -E "Model|Firmware|Serial"
done

# Check for errors:
sudo smartctl -l error /dev/sdX

# Test LHSR without hangs:
sudo dmsetup create lhsr_test --table "0 $(sudo blockdev --getsize /dev/sdb) lhsr single /dev/sdb" --noudevsync
sudo dmsetup remove lhsr_test
```

---

## References:

1. **Toshiba FJ3D**: https://www.dell.com/support/home/en-au/drivers/driversdetails?driverid=9kx6f
2. **HGST Firmware**: http://files.hddguru.com/download/Firmware%20updates/Hitachi/
3. **WD Firmware**: http://files.hddguru.com/download/Firmware%20updates/Western%20Digital/
4. **Toshiba Research**: https://syscall.eu/blog/2024/08/28/toshiba_hdd_firmware/
5. **Smart HDD Database**: https://smarthdd.com/database/

---

## Next Steps:

1. ⚠️ **PRIORITY**: Update sdd (Toshiba) to FJ3D - fixes LBA Counter error
2. Download and apply HGST MF8OAA70 to sda
3. Research WD firmware for sdc (WL3000GSA6472E0)
4. Wipe all drives: `sudo /home/keith/lhsr/scripts/wipe-drives-for-testing.sh`
5. Test LHSR with --noudevsync flag
