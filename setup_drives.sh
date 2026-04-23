#!/bin/bash
# LHSR Disk Setup Script
# Partitions empty drive for LHSR RAID arrays

DRIVE="${1:-sdc}"
DEVICE="/dev/$DRIVE"

echo "=== LHSR Disk Setup ==="
echo "Drive: $DEVICE"
echo ""

# Check if drive exists
if [ ! -b "$DEVICE" ]; then
    echo "ERROR: $DEVICE not found"
    exit 1
fi

# Show current partition layout
echo "Current layout:"
lsblk $DEVICE

# Warn if drive has data
if [ -n "$(lsblk $DEVICE -o MOUNTPOINT --noheaders 2>/dev/null | grep -v '^$')" ]; then
    echo "ERROR: $DEVICE has mounted filesystems!"
    exit 1
fi

echo ""
echo "Creating partition table..."
# Create GPT partition table
parted -s $DEVICE mklabel gpt

# Create single partition using full disk
parted -s $DEVICE mkpart primary 0% 100%

# Set partition to Linux RAID auto-detect
parted -s $DEVICE set 1 raid on

# Notify kernel
partprobe $DEVICE

sleep 1

echo ""
echo "New layout:"
lsblk $DEVICE

echo ""
echo "Partitions created:"
cat /proc/partitions | grep $DRIVE

echo ""
echo "=== Ready for LHSR ==="
echo "Partition: ${DEVICE}1"
echo ""
echo "Now run test:"
echo "  sudo dmsetup create lhsr_test --table '0 1000000 lhsr single ${DEVICE}1 0'"
echo "  sudo dmsetup status lhsr_test"