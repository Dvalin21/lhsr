#!/bin/bash
# Test script for RAID6 write path - RED phase: should FAIL because Q parity not computed

echo "=== RAID6 Write Path Test (RED Phase) ==="
echo "Test should FAIL (exit 1) before integration because lhsr_rs_parity() is not called."

MODULE_PATH="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"

# Check if module is loaded
if lsmod | grep -q dm_lhsr
then
    echo "LHSR module already loaded"
else
    echo "Loading LHSR module from $MODULE_PATH..."
    sudo insmod "$MODULE_PATH"
    if [ $? -ne 0 ]
    then
        echo "Failed to load module"
        exit 2
    fi
fi

# Create test directory
TEST_DIR="/tmp/lhsr_test_$$"
mkdir -p "$TEST_DIR"
echo "Test directory: $TEST_DIR"

# Create 4 x 100MB test files
for i in 1 2 3 4
do
    dd if=/dev/zero of=$TEST_DIR/disk$i.img bs=1M count=100 2>/dev/null
done

# Attach loop devices
LOOP1=$(sudo losetup --find --show $TEST_DIR/disk1.img)
LOOP2=$(sudo losetup --find --show $TEST_DIR/disk2.img)
LOOP3=$(sudo losetup --find --show $TEST_DIR/disk3.img)
LOOP4=$(sudo losetup --find --show $TEST_DIR/disk4.img)

echo "Loop devices: $LOOP1 $LOOP2 $LOOP3 $LOOP4"

# Create RAID6 array: 2 data + 2 parity disks
TABLE="0 204800 lhsr raid6 $LOOP1 0 $LOOP2 0 $LOOP3 0 $LOOP4 0"
echo "Creating RAID6 device with table: $TABLE"
sudo dmsetup create lhsr_raid6_test --table "$TABLE"

# Write non-zero data pattern
echo "Writing test data to RAID6 device..."
dd if=/dev/urandom of=/dev/mapper/lhsr_raid6_test bs=4096 count=10 2>/dev/null

# Read Q parity disk directly (4th disk = LOOP4) and check if non-zero
echo "Checking if Q parity was written to $LOOP4..."
# Use od instead of xxd (more portable)
Q_DATA=$(sudo dd if=$LOOP4 bs=4096 count=1 2>/dev/null | od -A n -t x1 | tr -d ' \n')

echo "Q parity data (first 64 chars): ${Q_DATA:0:64}"

# Check if all zeros
if echo "$Q_DATA" | grep -qE '^0+$' || [ -z "$Q_DATA" ]
then
    echo ""
    echo "RED PHASE CONFIRMED: Q parity disk is all zeros - lhsr_rs_parity() NOT called!"
    echo "Test FAILS as expected (exit 1)."
    TEST_RESULT=1
else
    echo ""
    echo "GREEN PHASE: Q parity disk has data - lhsr_rs_parity() IS being called!"
    echo "Test PASSES (exit 0)."
    TEST_RESULT=0
fi

# Cleanup
echo "Cleaning up..."
sudo dmsetup remove lhsr_raid6_test 2>/dev/null || true
sudo losetup -d $LOOP1 2>/dev/null || true
sudo losetup -d $LOOP2 2>/dev/null || true
sudo losetup -d $LOOP3 2>/dev/null || true
sudo losetup -d $LOOP4 2>/dev/null || true
rm -rf "$TEST_DIR"

echo "=== Test Complete ==="
exit $TEST_RESULT
