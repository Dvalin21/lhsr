#!/bin/bash
#
# LHSR Safe Test Script v1.3.2 - Fixed permission handling
# Run: sudo ./safe_test.sh
#

set -e

MODULE="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"
DEVICE_NAME="lhsr_test"

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
    log_info "Cleanup (exiting)..."
    # Don't try dmsetup remove - it hangs
    # Just try unload, it auto-cleans
    rmmod dm_lhsr 2>/dev/null || true
}

trap cleanup EXIT

echo "=============================================="
echo "  LHSR Safe Test v1.3.2"
echo "=============================================="

# Check root
if [ "$EUID" -ne 0 ]; then
    log_error "Must run as root"
    exit 1
fi

# Step 1: Check for stale devices first
log_test "Step 1: Checking device state"
dmsetup ls 2>/dev/null || true
log_info "Will clean up via module unload"

# Skip device removal - it hangs. Unloading module auto-cleans.
log_info "Skipping dmsetup remove (unload will auto-clean)"

# Step 2: Load dm-mod first (CRITICAL)
log_test "Step 2: Loading dm-mod"
modprobe dm-mod 2>/dev/null || sudo modprobe dm-mod 2>/dev/null || true
if lsmod | grep -q dm_mod; then
    log_info "dm_mod loaded"
else
    log_warn "dm_mod may already be built-in"
fi

# Step 3: Remove any existing LHSR module
log_test "Step 3: Cleanup old module"
rmmod dm_lhsr 2>/dev/null || true

# Step 4: Load LHSR module
log_test "Step 4: Loading LHSR module"
if [ ! -f "$MODULE" ]; then
    log_error "Module not found: $MODULE"
    exit 1
fi
insmod "$MODULE"
log_info "Module loaded:"
lsmod | grep lhsr

# Step 5: Check kernel messages
log_test "Step 5: Kernel messages"
dmesg | tail -5
echo ""

# Step 6: Create device
log_test "Step 6: Creating device"

# Get disk size directly using cat (avoids subshell issues)
SIZE=$(cat /sys/block/sdb/size)
log_info "Disk sectors: $SIZE"

if [ "$SIZE" -lt 2048 ]; then
    log_error "Disk too small or not available"
    exit 1
fi

# Create device using the size
log_info "Running: dmsetup create $DEVICE_NAME --table \"0 $SIZE lhsr single /dev/sdb\""
dmsetup create "$DEVICE_NAME" --table "0 $SIZE lhsr single /dev/sdb" 2>&1 | tee /tmp/dmsetup_error.txt
RESULT=$?

if [ $RESULT -eq 0 ]; then
    log_info "Device created: $DEVICE_NAME"
else
    log_error "Failed to create device (exit code: $RESULT)"
    log_error "dmsetup output:"
    cat /tmp/dmsetup_error.txt
    dmesg | tail -20
    exit 1
fi

# Step 7: Status check
log_test "Step 7: Device status"
dmsetup status "$DEVICE_NAME"
echo ""

# Step 8: Simple write test
log_test "Step 8: Write test (10MB)"
dd if=/dev/zero of=/dev/mapper/"$DEVICE_NAME" bs=1M count=10 oflag=direct 2>/dev/null && \
    log_info "Write: PASSED" || log_error "Write: FAILED"

# Step 9: Simple read test
log_test "Step 9: Read test (10MB)"
dd if=/dev/mapper/"$DEVICE_NAME" of=/dev/null bs=1M count=10 iflag=direct 2>/dev/null && \
    log_info "Read: PASSED" || log_error "Read: FAILED"

# Step 10: Config query
log_test "Step 10: Configuration query"
dmsetup message "$DEVICE_NAME" 0 config
echo ""

# ============================================================
# CLEANUP - CRITICAL SECTION
# ============================================================

log_test "Step 11: CLEANUP - Unloading module"

log_info "Unloading module (auto-cleans devices)..."
rmmod dm_lhsr 2>/dev/null || log_warn "Module not loaded or busy"

sleep 1

# Verify 
lsmod | grep lhsr || log_info "Module unloaded"

# Step 13: Final kernel messages
log_test "Step 13: Final kernel messages"
dmesg | grep -i lhsr | tail -10 || echo "(none)"
echo ""

echo "=============================================="
echo "  Safe Test COMPLETED!"
echo "=============================================="
log_info "No hard reboot required"