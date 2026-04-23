#!/bin/bash
# LHSR Disk Setup - Simple version using fdisk
# Run: sudo ./setup_drives.sh sdc

DRIVE="${1:-sdc}"
DEVICE="/dev/$DRIVE"

echo "=== LHSR Disk Setup ==="
echo "Drive: $DEVICE"

# Check if drive exists
if [ ! -b "$DEVICE" ]; then
    echo "ERROR: $DEVICE not found"
    exit 1
fi

# Warn if mounted
if lsblk $DEVICE -o MOUNTPOINT --noheaders 2>/dev/null | grep -v '^$' | grep -v 'MOUNTPOINT' >/dev/null; then
    echo "ERROR: $DEVICE has mounted filesystems!"
    exit 1
fi

echo "Current:"
lsblk $DEVICE

echo ""
echo "Using fdisk to create partition..."

# Use fdisk to create partition
# n = new, p = primary, 1 = partition number, default = first sector, default = last sector
# w = write
echo -e "n\np\n1\n\n\nw" | fdisk $DEVICE 2>&1 || {
    echo "fdisk failed, trying alternative..."
    # Alternative: use sfdisk
    echo "1 : start=0, size=0, type=83" | sfdisk $DEVICE
}

# Notify kernel
partprobe $DEVICE 2>/dev/null || true

sleep 1

echo ""
echo "Results:"
lsblk $DEVICE
cat /proc/partitions | grep $DRIVE

echo ""
echo "Partition ready: ${DEVICE}1"