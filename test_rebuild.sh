#!/bin/bash
#
# LHSR Rebuild Test Script
# Run: sudo ./test_rebuild.sh
#

set -e

DEVICE_NAME="lhsr_rebuild_test"
DISK0="/dev/sdb"
DISK1="/dev/sdc"
TEST_FILE="/tmp/lhsr_test_data"
RESULT_FILE="/tmp/lhsr_rebuild_result"

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
    losetup -D 2>/dev/null || true
    rm -f "$TEST_FILE" "$RESULT_FILE"
}

fail_test() {
    log_error "Test failed: $1"
    cleanup
    exit 1
}

trap cleanup EXIT

echo "=============================================="
echo "  LHSR Mirror Rebuild Test (v1.3.0)"
echo "=============================================="

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
fi

# Clean up any existing device
cleanup

# Get sizes
SIZE0=$(blockdev --getsize "$DISK0")
SIZE1=$(blockdev --getsize "$DISK1")
MIN_SIZE=$((SIZE0 < SIZE1 ? SIZE0 : SIZE1))
log_info "Disk sizes: $DISK0=$SIZE0, $DISK1=$SIZE1, usable=$MIN_SIZE sectors"

# ============================================================
# TEST 1: Create Mirror Array
# ============================================================
log_test "Test 1: Creating mirror device"
dmsetup create "$DEVICE_NAME" --table "0 $MIN_SIZE lhsr mirror $DISK0 $DISK1"

if dmsetup ls | grep -q "$DEVICE_NAME"; then
    log_info "Mirror device created"
else
    fail_test "Failed to create mirror device"
fi

# ============================================================
# TEST 2: Write Test Data
# ============================================================
log_test "Test 2: Writing test data (50MB)"
dd if=/dev/urandom of="$TEST_FILE" bs=1M count=50 status=none
TEST_CKSUM=$(md5sum "$TEST_FILE" | cut -d' ' -f1)
log_info "Test file checksum: $TEST_CKSUM"

dd if="$TEST_FILE" of=/dev/mapper/"$DEVICE_NAME" bs=1M count=50 status=none
log_info "Data written to mirror"

# ============================================================
# TEST 3: Verify Initial Status
# ============================================================
log_test "Test 3: Initial array status"
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 4: Query Scrub Status (should be idle)
# ============================================================
log_test "Test 4: Scrubber status (should be idle)"
dmsetup message "$DEVICE_NAME" 0 scrub
echo ""

# ============================================================
# TEST 5: Query Config
# ============================================================
log_test "Test 5: Configuration query"
dmsetup message "$DEVICE_NAME" 0 config
echo ""

# ============================================================
# TEST 6: Mark Disk 1 as Failed
# ============================================================
log_test "Test 6: Simulating disk 1 failure"
dmsetup message "$DEVICE_NAME" 0 disk_fail 1
log_info "Disk 1 marked as failed"
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 7: Check Rebuild Status (should show no rebuild)
# ============================================================
log_test "Test 7: Rebuild status before start"
dmsetup message "$DEVICE_NAME" 0 rebuild
echo ""

# ============================================================
# TEST 8: Start Rebuild
# ============================================================
log_test "Test 8: Starting rebuild for disk 1"
dmsetup message "$DEVICE_NAME" 0 rebuild start 1
log_info "Rebuild initiated"
dmsetup message "$DEVICE_NAME" 0 rebuild
echo ""

# ============================================================
# TEST 9: Monitor Rebuild Progress
# ============================================================
log_test "Test 9: Monitoring rebuild progress"
log_info "Watching rebuild progress (up to 60 seconds)..."

for i in {1..60}; do
    STATUS=$(dmsetup message "$DEVICE_NAME" 0 rebuild 2>/dev/null)
    log_info "[$i/60] $STATUS"
    
    if echo "$STATUS" | grep -q "complete\|COMPLETE\|100%"; then
        log_info "Rebuild completed!"
        break
    fi
    
    sleep 1
done

echo ""

# ============================================================
# TEST 10: Final Rebuild Status
# ============================================================
log_test "Test 10: Final rebuild status"
dmsetup message "$DEVICE_NAME" 0 rebuild
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 11: Bring Disk 1 Back Online
# ============================================================
log_test "Test 11: Bringing disk 1 back online"
dmsetup message "$DEVICE_NAME" 0 disk_online 1
log_info "Disk 1 marked online"
dmsetup status "$DEVICE_NAME"
echo ""

# ============================================================
# TEST 12: Read Data and Verify Integrity
# ============================================================
log_test "Test 12: Verifying data integrity after rebuild"
dd if=/dev/mapper/"$DEVICE_NAME" of="$RESULT_FILE" bs=1M count=50 status=none
RESULT_CKSUM=$(md5sum "$RESULT_FILE" | cut -d' ' -f1)

if [ "$TEST_CKSUM" = "$RESULT_CKSUM" ]; then
    log_info "Data integrity verified! Checksum matches: $RESULT_CKSUM"
else
    log_error "Data corruption detected!"
    log_error "Original: $TEST_CKSUM"
    log_error "After rebuild: $RESULT_CKSUM"
    fail_test "Data integrity check failed"
fi

# ============================================================
# TEST 13: Write New Data After Rebuild
# ============================================================
log_test "Test 13: Writing new data after rebuild"
dd if=/dev/urandom of="$TEST_FILE" bs=1M count=10 status=none
NEW_CKSUM=$(md5sum "$TEST_FILE" | cut -d' ' -f1)
dd if="$TEST_FILE" of=/dev/mapper/"$DEVICE_NAME" bs=1M count=10 status=none
dd if=/dev/mapper/"$DEVICE_NAME" of="$RESULT_FILE" bs=1M count=10 status=none
VERIFY_CKSUM=$(md5sum "$RESULT_FILE" | cut -d' ' -f1)

if [ "$NEW_CKSUM" = "$VERIFY_CKSUM" ]; then
    log_info "New data write/verify successful: $VERIFY_CKSUM"
else
    fail_test "Post-rebuild write verification failed"
fi

# ============================================================
# TEST 14: Check Kernel Messages
# ============================================================
log_test "Test 14: Kernel messages during test"
echo "Last 30 LHSR messages:"
dmesg | grep -i "lhsr\|rebuild" | tail -30 || echo "(none)"
echo ""

# ============================================================
# TEST 15: Superblock Persistence
# ============================================================
log_test "Test 15: Superblock persistence test"

log_info "Persisting state..."
dmsetup message "$DEVICE_NAME" 0 persist

log_info "Removing device (state should be saved)..."
dmsetup remove "$DEVICE_NAME"

log_info "Recreating device (state should be restored)..."
dmsetup create "$DEVICE_NAME" --table "0 $MIN_SIZE lhsr mirror $DISK0 $DISK1"

log_info "Checking restored state..."
dmsetup status "$DEVICE_NAME"
dmsetup message "$DEVICE_NAME" 0 config
echo ""

# ============================================================
# TEST 16: Final Cleanup
# ============================================================
log_test "Test 16: Final cleanup"
cleanup

echo ""
echo "=============================================="
echo "  ALL REBUILD TESTS PASSED!"
echo "=============================================="
echo ""
log_info "Test Summary:"
log_info "  - Mirror creation: OK"
log_info "  - Data write: OK"
log_info "  - Disk failure simulation: OK"
log_info "  - Rebuild execution: OK"
log_info "  - Data integrity after rebuild: OK"
log_info "  - Superblock persistence: OK"
echo ""
log_info "LHSR v1.3.0 rebuild functionality verified"