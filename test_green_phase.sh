#!/bin/bash
# TDD GREEN Phase: Test should NOW PASS (no hang)
# Verifies the fix works

set -e

MODULE_PATH="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"
TEST_DEVICE="/dev/sdb"

echo "=== TDD GREEN Phase: dmsetup create should NOT hang ==="

# Ensure clean state
sudo rmmod dm_lhsr 2>/dev/null || true
sudo dmsetup remove lhsr_test 2>/dev/null || true
sleep 1

# Load module
echo "Loading module..."
sudo insmod "$MODULE_PATH" || { echo "FAIL: Cannot load module"; exit 2; }

echo "Module loaded. Creating test device (should complete in <5s)..."

# This should NOW succeed (GREEN phase)
START=$(date +%s)
sudo dmsetup create lhsr_test --table "0 $(sudo blockdev --getsize $TEST_DEVICE) lhsr single $TEST_DEVICE 0" 2>&1
END=$(date +%s)
ELAPSED=$((END - START))

# Verify device created
if sudo dmsetup ls | grep -q lhsr_test; then
    echo "SUCCESS: dmsetup create completed in ${ELAPSED}s (no hang!)"
    sudo dmsetup remove lhsr_test 2>/dev/null || true
    sudo rmmod dm_lhsr 2>/dev/null || true
    echo "=== GREEN Phase PASSED ==="
    exit 0
else
    echo "FAIL: Device not created"
    sudo rmmod dm_lhsr 2>/dev/null || true
    exit 1
fi
