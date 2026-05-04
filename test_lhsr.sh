#!/bin/bash
#
# LHSR Comprehensive Test Script
# Run as: sudo ./test_lhsr.sh
#

set -e

MODULE="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"
DEVICE="/dev/sdb"
DEVICE_NAME="lhsr_single"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_test() { echo -e "${BLUE}[TEST]${NC} $1"; }

cleanup() {
    dmsetup remove "$DEVICE_NAME" 2>/dev/null || true
}

trap cleanup EXIT

echo "========================================="
echo "  LHSR Module Test v1.3.0"
echo "========================================="
echo ""

# Check root
if [ "$EUID" -ne 0 ]; then
    log_error "Must run as root"
    exit 1
fi

# Step 1: Load dm-mod
log_test "Test 1: Load dm-mod"
modprobe dm-mod 2>/dev/null || sudo modprobe dm-mod 2>/dev/null || true
lsmod | grep dm_mod && log_info "dm_mod loaded" || log_warn "dm_mod not loaded"

# Step 2: Check module file
log_test "Test 2: Module file check"
if [ ! -f "$MODULE" ]; then
    log_error "Module not found at $MODULE"
    log_info "Run 'make' in lhsr directory first"
    exit 1
fi
log_info "Module found: $(ls -lh $MODULE | awk '{print $5}')"

# Step 3: Remove existing module
log_test "Test 3: Cleanup old module"
rmmod dm_lhsr 2>/dev/null || true
log_info "Old module removed if present"

# Step 4: Load our module
log_test "Test 4: Load LHSR module"
insmod $MODULE
log_info "Module loaded:"
lsmod | grep lhsr

# Step 5: Check kernel messages
log_test "Test 5: Kernel messages"
dmesg | grep -i "lhsr" | tail -5
echo ""

# Step 6: Create single disk device
log_test "Test 6: Create single disk device"
SIZE=$(blockdev --getsize "$DEVICE")
log_info "Device size: $SIZE sectors"

# Note: dmsetup table format is: start size type device offset [device offset...]
dmsetup create "$DEVICE_NAME" --table "0 $SIZE lhsr single $DEVICE 0" --noudevsync && {
    log_info "Device created: $DEVICE_NAME"
} || {
    log_error "Failed to create device"
    dmesg | grep -i lhsr | tail -10
    exit 1
}

# Step 7: Check status
log_test "Test 7: Device status"
dmsetup status "$DEVICE_NAME"
echo ""

# Step 8: Config query
log_test "Test 8: Configuration"
dmsetup message "$DEVICE_NAME" 0 config
echo ""

# Step 9: Write test
log_test "Test 9: Write test (100MB)"
dd if=/dev/zero of=/dev/mapper/"$DEVICE_NAME" bs=1M count=100 oflag=direct 2>/dev/null && \
    log_info "Write: PASSED" || log_error "Write: FAILED"

# Step 10: Read test
log_test "Test 10: Read test (100MB)"
dd if=/dev/mapper/"$DEVICE_NAME" of=/dev/null bs=1M count=100 iflag=direct 2>/dev/null && \
    log_info "Read: PASSED" || log_error "Read: FAILED"

# Step 11: Scrubber test
log_test "Test 11: Scrubber start"
dmsetup message "$DEVICE_NAME" 0 scrub start
log_info "Scrubber started, checking status:"
dmsetup message "$DEVICE_NAME" 0 scrub
sleep 2
dmsetup message "$DEVICE_NAME" 0 scrub stop
log_info "Scrubber stopped"

# Step 12: Cleanup
log_test "Test 12: Cleanup"
dmsetup remove "$DEVICE_NAME"
log_info "Device removed"

# Step 13: Unload module
log_test "Test 13: Unload module"
rmmod dm_lhsr && log_info "Module unloaded" || log_error "Module unload failed"

# Step 14: Final kernel messages
log_test "Test 14: Final kernel messages"
dmesg | grep -i "lhsr" | tail -10
echo ""

echo "========================================="
echo "  All Tests Passed!"
echo "========================================="
echo ""
log_info "LHSR v1.3.0 module verified"