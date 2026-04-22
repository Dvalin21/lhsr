#!/bin/bash
# LHSR Test Script
# Run as: sudo ./test_lhsr.sh

set -e

MODULE="kernel/dm-lhsr/dm-lhsr.ko"
DEVICE_PREFIX="lhsr_test"

echo "=== LHSR Module Test ==="

# Check available disks
echo "Available disks:"
lsblk -d -o NAME,SIZE,TYPE,MODEL | grep sd

echo ""
echo "Loading dm-mod..."
modprobe dm-mod || true

echo "Loading LHSR module..."
sudo insmod $MODULE

echo "Module loaded:"
lsmod | grep lhsr

echo ""
echo "dmesg output:"
dmesg | tail -5

echo ""
echo "Creating test array with sdb and sdc..."
echo "0 1000000 lhsr mirror /dev/sdb /dev/sdc" | sudo dmsetup create ${DEVICE_PREFIX}

echo "Checking status:"
sudo dmsetup status ${DEVICE_PREFIX}

echo ""
echo "Writing test data..."
sudo dd if=/dev/zero of=/dev/mapper/${DEVICE_PREFIX} bs=1M count=10 oflag=direct

echo "Reading test data..."
sudo dd if=/dev/mapper/${DEVICE_PREFIX} of=/dev/null bs=1M count=10 iflag=direct

echo ""
echo "Cleaning up..."
sudo dmsetup remove ${DEVICE_PREFIX}

echo "=== Test Complete ==="

echo ""
echo "To remove module when done:"
echo "  sudo rmmod dm_lhsr"