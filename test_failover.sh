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
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_test() { echo -e "${BLUE}[TEST]${NC} $1"; }

cleanup() {
    log_info "Cleaning up..."
    dmsetup remove "$DEVICE_NAME" 2>/dev/null || true
}

trap cleanup EXIT

echo "========================================="
echo "  LHSR Disk Fail Detection Test v1.3.0"
echo "========================================="

# Check root
if [ "$EUID" -ne 0 ]; then
    log_error "Must run as root"
    exit 1
fi

# Check disks exist
for disk in $DISK0 $DISK1; do
    if [ ! -b "$disk" ]; then
        log_error "$disk not found"
        exit 1
    fi
done

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

# ============================================================
# TEST 1: Initial status
# ============================================================
log_test "Test 1: Initial Status"
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 2: Query disk health (before failure)
# ============================================================
log_test "Test 2: Disk Health (Healthy)"
echo "Disk 0:"; dmsetup message "$DEVICE_NAME" 0 disk_health 0 || true
echo "Disk 1:"; dmsetup message "$DEVICE_NAME" 0 disk_health 1 || true
echo ""

# ============================================================
# TEST 3: Config query
# ============================================================
log_test "Test 3: Configuration"
dmsetup message "$DEVICE_NAME" 0 config
echo ""

# ============================================================
# TEST 4: Scrubber status
# ============================================================
log_test "Test 4: Scrubber Status"
dmsetup message "$DEVICE_NAME" 0 scrub
echo ""

# ============================================================
# TEST 5: Mark disk 0 as failed
# ============================================================
log_test "Test 5: Disk 0 Fail"
log_info "Marking disk 0 as failed..."
dmsetup message "$DEVICE_NAME" 0 disk_fail 0
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 6: Check rebuild status after disk 0 failed
# ============================================================
log_test "Test 6: Rebuild Status"
dmsetup message "$DEVICE_NAME" 0 rebuild
echo ""

# ============================================================
# TEST 7: Mark disk 1 as failed (full failure)
# ============================================================
log_test "Test 7: Disk 1 Fail (Full Array)"
log_info "Marking disk 1 as failed..."
dmsetup message "$DEVICE_NAME" 0 disk_fail 1
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 8: Bring disk 0 back online
# ============================================================
log_test "Test 8: Disk 0 Online"
log_info "Marking disk 0 as online..."
dmsetup message "$DEVICE_NAME" 0 disk_online 0
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 9: Bring disk 1 back online (full recovery)
# ============================================================
log_test "Test 9: Disk 1 Online"
log_info "Marking disk 1 as online..."
dmsetup message "$DEVICE_NAME" 0 disk_online 1
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 10: Write verification modes
# ============================================================
log_test "Test 10: Write Verification Modes"
log_info "Setting write_verify none..."
dmsetup message "$DEVICE_NAME" 0 write_verify none
dmsetup message "$DEVICE_NAME" 0 write_verify

log_info "Setting write_verify simple..."
dmsetup message "$DEVICE_NAME" 0 write_verify simple
dmsetup message "$DEVICE_NAME" 0 write_verify

log_info "Setting write_verify full..."
dmsetup message "$DEVICE_NAME" 0 write_verify full
dmsetup message "$DEVICE_NAME" 0 write_verify
echo ""

# ============================================================
# TEST 11: Persist state
# ============================================================
log_test "Test 11: State Persistence"
log_info "Persisting state..."
dmsetup message "$DEVICE_NAME" 0 persist
echo ""

# ============================================================
# TEST 12: Kernel messages
# ============================================================
log_test "Test 12: Kernel Messages"
echo "Last 20 lhsr messages:"
dmesg | grep -i "lhsr" | tail -20 || echo "(none)"
echo ""

# ============================================================
# TEST 13: Superblock read via lhsr-scan
# ============================================================
log_test "Test 13: Superblock Read"
if [ -x ./userspace/recovery/lhsr-scan ]; then
    log_info "Running lhsr-scan on disks..."
    ./userspace/recovery/lhsr-scan -v $DISK0 $DISK1 || log_warn "lhsr-scan had warnings"
else
    log_info "lhsr-scan not built, skipping"
fi
echo ""

echo "========================================="
echo "  All Tests Passed!"
echo "========================================="
echo ""
log_info "Cleanup done automatically"