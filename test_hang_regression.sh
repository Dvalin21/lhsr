#!/bin/bash
# RED phase: This test should FAIL (hang) before the fix
# Tests that dmsetup create doesn't hang due to blocking submit_bio_wait()

set -e

MODULE_PATH="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"
TEST_DEVICE="/dev/sdb"

echo "=== RED Phase Test: dmsetup create should not hang ==="

# Ensure clean state
sudo rmmod dm_lhsr 2>/dev/null || true
sudo dmsetup remove lhsr_test 2>/dev/null || true

# Load module
echo "Loading module..."
sudo insmod "$MODULE_PATH" || { echo "FAIL: Cannot load module"; exit 2; }

# Try dmsetup create with 10 second timeout
# This should HANG before fix (because ctr() calls submit_bio_wait() which blocks)
echo "Creating test device (should complete in <5s)..."
if timeout 10 sudo dmsetup create lhsr_test --table "0 $(sudo blockdev --getsize $TEST_DEVICE) lhsr single $TEST_DEVICE 0" 2>&1; then
    echo "PASS: dmsetup create completed (no hang)"
    sudo dmsetup remove lhsr_test 2>/dev/null || true
    sudo rmmod dm_lhsr 2>/dev/null || true
    exit 0
else
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 124 ]; then
        echo "FAIL (RED): dmsetup create TIMED OUT at 10s - this is the bug"
    else
        echo "FAIL: dmsetup create failed with code $EXIT_CODE"
    fi
    sudo rmmod dm_lhsr 2>/dev/null || true
    exit 1
fi
