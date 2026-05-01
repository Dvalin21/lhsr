# Toshiba MG04ACA400N Firmware Update Research

## Drive Information
- **Model**: TOSHIBA MG04ACA400N (4TB Enterprise HDD)
- **Serial**: 69PQKE4XFSYC
- **Current Firmware**: FJ5A
- **SATA Speed**: 6.0 Gb/s
- **Form Factor**: 3.5 inch
- **Sector Size**: 512 bytes (512n - native)
- **RPM**: 7200
- **Date**: Manufactured ~2018 era

## Firmware Versions Available

### 1. FJ5A (Current)
- Installed on your drive
- Found on 1TB variant (MG04ACA100N) per smarthdd.com
- Status: Unknown changelog

### 2. FJ3D (Dell Official - April 2018)
- **URL**: https://www.dell.com/support/home/en-au/drivers/driversdetails?driverid=9kx6f
- **Release Date**: 24 Apr 2018
- **Fixes**: 
  - Fix for **Offline Issue caused by LBA Counter Register read sequence error**
  - FW changes to affect Toshiba Tomcat-R SATA HDD
- **Download Options**:
  - Windows 64-bit: `Serial-ATA_Firmware_9KX6F_WN64_FJ3D_A00_01.EXE`
  - Windows 32-bit: `Serial-ATA_Firmware_9KX6F_WN32_FJ3D_A00_01.EXE`
  - **Linux (Red Hat)**: `Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN` ← **Use this for Linux**
  - Signature: `Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN.sign`

### 3. FJ1D (Older)
- Release Date: 10 Feb 2016
- Dell SKU: T0FMV

### 4. FK5D (For NY variant)
- For MG04ACA100NY, MG04ACA200NY, MG04ACA400NY (with sanitize feature)
- Not applicable to your drive (yours is not NY variant)

## Critical Finding: FJ3D Fixes Your Exact Issue

The FJ3D changelog explicitly states:
> "Fix for Offline Issue caused by LBA Counter Register read sequence error"

Your symptoms:
1. Drive causes system hangs
2. dmsetup operations hang
3. Password prompt on boot (ATA security frozen state)
4. Drive becomes unresponsive during certain operations

**These are consistent with LBA Counter Register read errors.**

## Firmware Update Methods

### Method 1: Dell Linux Update Package (Recommended)
The Dell-provided `.BIN` file for Red Hat Linux should work on Debian/Ubuntu:

```bash
# Download the firmware (you'll need to visit Dell site manually)
# Save to /tmp/Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN

# Make executable
chmod +x /tmp/Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN

# Run the update (as root)
sudo /tmp/Serial-ATA_Firmware_9KX6F_LN_FJ3D_A00.BIN --force
```

**WARNING**: This method may require Dell-specific drivers/utilities.

### Method 2: hdparm --fwdownload (Advanced)
Based on research from syscall.eu blog post "Upgrading a Toshiba NAS HDD firmware on Linux":

```bash
# Check if drive supports firmware download
sudo hdparm -I /dev/sdd | grep -i "firmware download"

# If supported, download mode 3 can be used:
sudo hdparm --fwdownload-mode3 firmware_file.ftd \
    --yes-i-know-what-i-am-doing \
    --please-destroy-my-drive /dev/sdd
```

**WARNING**: This requires the correct `.ftd` firmware file, which is not publicly available for your drive model.

### Method 3: Windows + Dell Utility (Most Reliable)
1. Download Windows version from Dell: `Serial-ATA_Firmware_9KX6F_WN64_FJ3D_A00_01.EXE`
2. Create Windows PE bootable USB or use existing Windows installation
3. Run the Dell firmware update utility
4. Reboot and verify with `smartctl -i /dev/sdd`

## Password Prompt Issue (ATA Security)

The password prompt is likely caused by **ATA Security** being enabled or the drive being in **frozen** state.

### Check ATA Security Status
```bash
sudo hdparm -I /dev/sdd | grep -A 20 "Security:"
```

### Possible States:
1. **Security: enabled** - Drive is locked with password
2. **Security: frozen** - Drive won't accept security commands (common after boot)
3. **Security: locked** - Drive is locked and requires password

### Solution for Frozen State:
```bash
# Method 1: Suspend/resume to unfreeze
echo -n mem > /sys/power/state
# Wake up system, then check again

# Method 2: Power cycle the drive
# Hot-unplug and re-plug SATA cable (if hot-plug supported)
# Or reboot with drive disconnected, then connect after boot
```

### Solution for Locked Drive:
If the drive has a password set:
```bash
# Try to unlock with empty password (sometimes works)
sudo hdparm --security-unlock "" /dev/sdd

# Or if you know the password:
sudo hdparm --security-unlock "password" /dev/sdd

# Disable security (WARNING: this may require erasing the drive)
sudo hdparm --security-disable "password" /dev/sdd
```

## Recommended Action Plan

### Step 1: Document Current State
```bash
sudo smartctl -x /dev/sdd > /tmp/sdd-smart-before.txt
sudo hdparm -I /dev/sdd > /tmp/sdd-hdparm-before.txt
```

### Step 2: Attempt Firmware Update to FJ3D
1. Download Dell Linux BIN file
2. Run firmware update
3. Reboot
4. Verify: `smartctl -i /dev/sdd | grep Firmware`

### Step 3: Clear ATA Security (if needed)
```bash
sudo hdparm -I /dev/sdd | grep -A 20 "Security:"
# Follow solutions above based on output
```

### Step 4: Test Without Drive
1. Disconnect Toshiba drive
2. Run LHSR tests
3. Verify if hangs persist without Toshiba drive connected

### Step 5: Test With Updated Firmware
1. Reconnect Toshiba drive
2. Run LHSR tests
3. Compare results

## Video Resources
Research indicates there are YouTube videos on Toshiba firmware updates, but none were found specifically for MG04ACA400N FJ5A→FJ3D.

Search terms for future reference:
- "Toshiba MG04ACA400N firmware update"
- "Dell firmware update Linux hdparm"
- "ATA security frozen fix"

## References
1. Dell FJ3D Driver Details: https://www.dell.com/support/home/en-au/drivers/driversdetails?driverid=9kx6f
2. syscall.eu Blog: https://syscall.eu/blog/2024/08/28/toshiba_hdd_firmware/
3. SmartHDD Database: https://smarthdd.com/database/TOSHIBA-MG04ACA100N/FJ5A/
4. Linux Firmware Update Video: https://www.youtube.com/watch?v=TC9viVZzB0g

## Notes
- Your drive's FJ5A firmware may be a Dell OEM-specific version
- The FJ3D update is the latest "official" non-OEM version
- Consider contacting Toshiba support for firmware files if Dell's doesn't work
- **BACKUP ANY DATA before attempting firmware update**
- Firmware update can fail and brick the drive - proceed with caution
