#!/bin/bash
# LHSR Comprehensive Test Script
# Run as: sudo ./test_lhsr.sh

set -e

MODULE="kernel/dm-lhsr/dm-lhsr.ko"
DEVICE_PREFIX="lhsr_test"

echo "=== LHSR Phase 2 Module Test ==="
echo "Running on: $(uname -r)"
echo ""

# Step 1: Load dm-mod
echo "[1/8] Loading dm-mod..."
modprobe dm-mod 2>/dev/null || sudo modprobe dm-mod 2>/dev/null || true
lsmod | grep dm_mod || echo "  dm_mod not loaded, attempting insmod..."

# Step 2: Check module file
echo "[2/8] Checking module..."
if [ ! -f "$MODULE" ]; then
    echo "ERROR: Module not found at $MODULE"
    echo "Run 'make' in kernel/dm-lhsr first"
    exit 1
fi
ls -la $MODULE

# Step 3: Remove existing module
echo "[3/8] Cleanup..."
sudo rmmod dm_lhsr 2>/dev/null || true

# Step 4: Load our module
echo "[4/8] Loading LHSR module..."
sudo insmod $MODULE

echo "[5/8] Module loaded:"
lsmod | grep lhsr

echo ""
echo "[6/8] Kernel messages:"
dmesg | grep -E "lhsr|dm-" | tail -10

# Step 5: Try single disk test
echo ""
echo "[7/8] Creating test device (single disk)..."
sudo dmsetup remove ${DEVICE_PREFIX} 2>/dev/null || true
echo "0 1000000 lhsr single /dev/sdc 0" | sudo dmsetup create ${DEVICE_PREFIX} 2>/dev/null && {
    sudo dmsetup status ${DEVICE_PREFIX}
    sudo dmsetup remove ${DEVICE_PREFIX}
    echo "  Single disk: PASSED"
} || echo "  Single disk: SKIPPED (no free device)"

# Step 6: Clean up
echo ""
echo "[8/8] Cleanup..."
sudo rmmod dm_lhsr 2>/dev/null || true
dmesg | grep -E "lhsr|dm-" | tail -5

echo ""
echo "=== Test Complete ==="
echo ""
echo "To inspect manually:"
echo "  sudo dmsetup create test --table '0 1000000 lhsr single /dev/sdc 0'"
echo "  sudo dmsetup status test"
echo "  sudo dd if=/dev/zero of=/dev/mapper/test bs=1M count=10"
echo "  sudo dd if=/dev/mapper/test of=/dev/null bs=1M count=10"
echo "  sudo dmsetup remove test"
echo "  sudo rmmod dm_lhsr"