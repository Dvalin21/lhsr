#!/bin/bash
# Test script to verify dmsetup create doesn't hang
# This is the RED phase test that should FAIL (hang) before fix

set -e

MODULE_PATH="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"
TEST_DEVICE="/dev/sdb"

echo "=== Test: dmsetup create should not hang ==="

# Check if module is loaded
if lsmod | grep -q dm_lhsr; then
    echo "ERROR: Module already loaded. Remove with: sudo rmmod dm_lhsr"
    exit 2
fi

# Load module
echo "Loading module..."
sudo insmod "$MODULE_PATH" || { echo "Failed to load module"; exit 2; }

# Check module loaded
lsmod | grep dm_lhsr || { echo "Module not loaded"; exit 2; }

echo "Module loaded. Creating test device with 5 second timeout..."

# Try dmsetup create with timeout
timeout 10 sudo dmsetup create lhsr_test --table "0 $(sudo blockdev --getsize $TEST_DEVICE) lhsr single $TEST_DEVICE 0" 2>&1 || {
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 124 ]; then
        echo "FAIL: dmsetup create TIMED OUT (this is the bug we're fixing)"
    else
        echo "FAIL: dmsetup create failed with code $EXIT_CODE"
    fi
    sudo rmmod dm_lhsr 2>/dev/null || true
    exit 1
}

echo "SUCCESS: dmsetup create completed (did not hang)"

# Cleanup
sudo dmsetup remove lhsr_test 2>/dev/null || true
sudo rmmod dm_lhsr 2>/dev/null || true

echo "=== Test PASSED ==="
exit 0
