#!/bin/bash
#
# LHSR Disk Fail Detection/Failover Test Script
# Run: sudo ./test_failover.sh
#

set -e

DEVICE_NAME="lhsr_mirror"
DISK0="/dev/sdb"
DISK1="/dev/sdc"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

cleanup() {
    log_info "Cleaning up..."
    dmsetup remove "$DEVICE_NAME" 2>/dev/null || true
}

trap cleanup EXIT

echo "========================================="
echo "  LHSR Disk Fail Detection Test"
echo "========================================="

# Check root
if [ "$EUID" -ne 0 ]; then
    log_error "Must run as root"
    exit 1
fi

# Check disks exist
if [ ! -b "$DISK0" ]; then
    log_error "$DISK0 not found"
    exit 1
fi
if [ ! -b "$DISK1" ]; then
    log_error "$DISK1 not found"
    exit 1
fi

# Clean up any existing device
cleanup

# Get size
SIZE=$(blockdev --getsize "$DISK0")
log_info "Disk size: $SIZE sectors"

# Create mirror device
log_info "Creating mirror device..."
dmsetup create "$DEVICE_NAME" --table "0 $SIZE lhsr mirror $DISK0 $DISK1"

# Verify creation
if ! dmsetup ls | grep -q "$DEVICE_NAME"; then
    log_error "Failed to create device"
    exit 1
fi
log_info "Device created successfully"

# Test 1: Initial status
echo ""
echo "========================================="
echo "  Test 1: Initial Status"
echo "========================================="
dmsetup status "$DEVICE_NAME"
echo ""

# Test 2: Query disk health (before failure)
echo "========================================="
echo "  Test 2: Disk Health (Healthy)"
echo "========================================="
echo "Disk 0:"; dmsetup message "$DEVICE_NAME" 0 disk_health 0 || true
echo "Disk 1:"; dmsetup message "$DEVICE_NAME" 0 disk_health 1 || true
echo ""

# Test 3: Mark disk 0 as failed
echo "========================================="
echo "  Test 3: Disk 0 Fail"
echo "========================================="
log_info "Marking disk 0 as failed..."
dmsetup message "$DEVICE_NAME" 0 disk_fail 0
dmsetup status "$DEVICE_NAME"
echo ""

# Test 4: Mark disk 1 as failed (full failure)
echo "========================================="
echo "  Test 4: Disk 1 Fail (Full Array)"
echo "========================================="
log_info "Marking disk 1 as failed..."
dmsetup message "$DEVICE_NAME" 0 disk_fail 1
dmsetup status "$DEVICE_NAME"
echo ""

# Test 5: Bring disk 0 back online
echo "========================================="
echo "  Test 5: Disk 0 Online"
echo "========================================="
log_info "Marking disk 0 as online..."
dmsetup message "$DEVICE_NAME" 0 disk_online 0
dmsetup status "$DEVICE_NAME"
echo ""

# Test 6: Bring disk 1 back online (full recovery)
echo "========================================="
echo "  Test 6: Disk 1 Online"
echo "========================================="
log_info "Marking disk 1 as online..."
dmsetup message "$DEVICE_NAME" 0 disk_online 1
dmsetup status "$DEVICE_NAME"
echo ""

# Test 7: Check dmesg for kernel messages
echo "========================================="
echo "  Test 7: Kernel Messages"
echo "========================================="
echo "Last 20 lhsr messages:"
dmesg | grep -i "lhsr" | tail -20 || echo "(none)"
echo ""

echo "========================================="
echo "  All Tests Passed!"
echo "========================================="
echo ""
log_info "Cleanup done automatically"