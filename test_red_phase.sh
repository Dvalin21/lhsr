#!/bin/bash
# TDD RED Phase: Test should FAIL (dmsetup create hangs)
# This documents the bug before we fix it

set -e

MODULE_PATH="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"
TEST_DEVICE="/dev/sdb"
RESULT_FILE="/tmp/lhsr_test_result.txt"

echo "=== TDD RED Phase: dmsetup create should NOT hang ==="
echo "This test should FAIL (timeout) before the fix"

# Ensure clean state
sudo rmmod dm_lhsr 2>/dev/null || true
sudo dmsetup remove lhsr_test 2>/dev/null || true
sleep 1

# Load module
echo "Loading module..."
sudo insmod "$MODULE_PATH" 2>&1 || { echo "FAIL: Cannot load module"; exit 2; }

# Verify module loaded
if ! lsmod | grep -q dm_lhsr; then
    echo "FAIL: Module not loaded"
    exit 2
fi

echo "Module loaded. Attempting dmsetup create (should hang before fix)..."

# Run dmsetup with 10 second timeout - should timeout (RED phase)
if timeout 10 sudo dmsetup create lhsr_test --table "0 $(sudo blockdev --getsize $TEST_DEVICE) lhsr single $TEST_DEVICE 0" 2>$RESULT_FILE; then
    echo "UNEXPECTED PASS: dmsetup create succeeded (code under test may already be fixed?)"
    sudo dmsetup remove lhsr_test 2>/dev/null || true
    sudo rmmod dm_lhsr 2>/dev/null || true
    exit 0
else
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 124 ]; then
        echo "EXPECTED RED: dmsetup create TIMED OUT at 10s (this is the bug)"
        echo "RED phase verified - test fails as expected"
        sudo rmmod dm_lhsr 2>/dev/null || true
        exit 1  # RED phase: test fails
    else
        echo "FAIL: dmsetup failed with code $EXIT_CODE"
        cat $RESULT_FILE
        sudo rmmod dm_lhsr 2>/dev/null || true
        exit 2
    fi
fi
