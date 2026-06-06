#!/bin/bash
#
# LHSR RAID5 Smoke Test
#
# Creates loopback devices, loads the LHSR module, creates a RAID5 array,
# writes and verifies data, then tears everything down.
#
# ╔══════════════════════════════════════════════════════════════════╗
# ║  ⚠  THIS SCRIPT MODIFIES BLOCK DEVICES.                        ║
# ║  Review the test plan below before running.                     ║
# ║  Run with:  sudo bash smoke-test-raid5.sh                       ║
# ╚══════════════════════════════════════════════════════════════════╝
#
# Copyright (C) 2026 LHSR Team
# License: GPLv3
#

set -euo pipefail

# ────────────────────────────────────────────────────────────
# Configuration
# ────────────────────────────────────────────────────────────
DISK_SIZE_MB=64                     # Per-loopback size
NUM_DISKS=3                         # RAID5 minimum
LHSR_MODULE="../kernel/dm-lhsr/dm-lhsr.ko"
LHSR_DEV_NAME="lhsr-smoke-raid5"
LHSR_SIZE_SECTORS=$(( (DISK_SIZE_MB - 2) * 2048 * NUM_DISKS ))  # conservative

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

pass()  { echo -e "${GREEN}[PASS]${NC} $1"; }
fail()  { echo -e "${RED}[FAIL]${NC} $1"; }
info()  { echo -e "${YELLOW}[INFO]${NC} $1"; }

# Trap for cleanup on exit/failure
CLEANUP_LOOPS=()

cleanup() {
    local rc=$?
    info "Cleaning up..."

    # Remove DM device
    if dmsetup info "$LHSR_DEV_NAME" &>/dev/null 2>&1; then
        dmsetup remove "$LHSR_DEV_NAME" 2>/dev/null || true
        info "Removed DM device: $LHSR_DEV_NAME"
    fi

    # Unload module
    if lsmod | grep -q dm_lhsr; then
        rmmod dm-lhsr 2>/dev/null || true
        info "Unloaded dm-lhsr module"
    fi

    # Detach loopbacks
    for ldev in "${CLEANUP_LOOPS[@]}"; do
        if losetup "$ldev" &>/dev/null 2>&1; then
            losetup -d "$ldev" 2>/dev/null || true
            info "Detached loop: $ldev"
        fi
    done

    # Remove temp files
    for ((i=0; i<NUM_DISKS; i++)); do
        local f="/tmp/lhsr-disk-${i}.img"
        [ -f "$f" ] && rm -f "$f"
    done

    if [ $rc -eq 0 ]; then
        echo ""
        pass "Smoke test PASSED"
    else
        echo ""
        fail "Smoke test FAILED (exit code $rc)"
    fi
    exit $rc
}
trap cleanup EXIT

# ────────────────────────────────────────────────────────────
echo ""
info "╔══════════════════════════════════════════════════════╗"
info "║  LHSR RAID5 Smoke Test                              ║"
info "╚══════════════════════════════════════════════════════╝"
echo ""

# ────────────────────────────────────────────────────────────
# Step 1: Create loopback devices
# ────────────────────────────────────────────────────────────
info "Step 1/8: Creating $NUM_DISKS loopback devices (${DISK_SIZE_MB}MB each)..."
LOOP_DEVS=()
for ((i=0; i<NUM_DISKS; i++)); do
    F="/tmp/lhsr-disk-${i}.img"
    dd if=/dev/zero of="$F" bs=1M count="$DISK_SIZE_MB" status=none 2>/dev/null
    LDEV=$(losetup --show -f "$F")
    LOOP_DEVS+=("$LDEV")
    CLEANUP_LOOPS+=("$LDEV")
    info "  Device $i: $LDEV (${DISK_SIZE_MB}MB)"
done
pass "Loopback devices created"

# ────────────────────────────────────────────────────────────
# Step 2: Verify module exists
# ────────────────────────────────────────────────────────────
info "Step 2/8: Checking LHSR module..."
if [ ! -f "$LHSR_MODULE" ]; then
    fail "Module not found: $LHSR_MODULE"
    exit 1
fi

MODINFO=$(modinfo "$LHSR_MODULE" 2>/dev/null || true)
if [ -z "$MODINFO" ]; then
    fail "Cannot read module info (modinfo not available or module broken)"
    exit 1
fi
pass "Module exists"

# ────────────────────────────────────────────────────────────
# Step 3: Load module
# ────────────────────────────────────────────────────────────
info "Step 3/8: Loading LHSR kernel module..."
if lsmod | grep -q dm_lhsr; then
    info "  Module already loaded, removing first..."
    rmmod dm-lhsr || true
    sleep 1
fi

insmod "$LHSR_MODULE" || {
    fail "Failed to load module (dmesg for details)"
    exit 1
}
sleep 1  # Let init complete

if ! lsmod | grep -q dm_lhsr; then
    fail "Module not loaded after insmod"
    exit 1
fi
pass "Module loaded successfully"

# ────────────────────────────────────────────────────────────
# Step 4: Create RAID5 DM target
# ────────────────────────────────────────────────────────────
info "Step 4/8: Creating RAID5 target ($LHSR_DEV_NAME)..."

# Build table line: raid5 <dev1> <off1> <dev2> <off2> <dev3> <off3>
TABLE="raid5"
for ldev in "${LOOP_DEVS[@]}"; do
    TABLE+=" $ldev 0"
done

echo "0 $LHSR_SIZE_SECTORS lhsr $TABLE" | dmsetup create "$LHSR_DEV_NAME" || {
    fail "Failed to create DM target"
    dmesg | tail -20
    exit 1
}
sleep 1

if ! dmsetup info "$LHSR_DEV_NAME" &>/dev/null 2>&1; then
    fail "DM device not created"
    exit 1
fi

DM_DEV="/dev/mapper/$LHSR_DEV_NAME"
pass "RAID5 target created: $DM_DEV"

# ────────────────────────────────────────────────────────────
# Step 5: Write and verify data
# ────────────────────────────────────────────────────────────
info "Step 5/8: Writing test pattern..."

# Write a deterministic pattern
dd if=/dev/urandom bs=1M count=4 of=/tmp/lhsr-test-pattern.bin 2>/dev/null
PATTERN_CKSUM=$(sha256sum /tmp/lhsr-test-pattern.bin | awk '{print $1}')

# Write to DM device (at 1MB offset to avoid superblock area)
dd if=/tmp/lhsr-test-pattern.bin of="$DM_DEV" bs=1M seek=1 oflag=direct status=none 2>/dev/null || {
    fail "Write to DM device failed"
    dmesg | tail -10
    exit 1
}
pass "Data written to RAID5 array"

# Sync & flush
blockdev --flushbufs "$DM_DEV" 2>/dev/null || true
sleep 1

info "Step 6/8: Reading back and verifying..."

# Read back and checksum
dd if="$DM_DEV" of=/tmp/lhsr-test-readback.bin bs=1M skip=1 count=4 iflag=direct status=none 2>/dev/null || {
    fail "Read from DM device failed"
    dmesg | tail -10
    exit 1
}

READBACK_CKSUM=$(sha256sum /tmp/lhsr-test-readback.bin | awk '{print $1}')
rm -f /tmp/lhsr-test-readback.bin /tmp/lhsr-test-pattern.bin

if [ "$PATTERN_CKSUM" != "$READBACK_CKSUM" ]; then
    fail "Data corruption!"
    echo "  Expected: $PATTERN_CKSUM"
    echo "  Got:      $READBACK_CKSUM"
    exit 1
fi
pass "Data verification: checksum MATCH"

# ────────────────────────────────────────────────────────────
# Step 7: Status message (admin info)
# ────────────────────────────────────────────────────────────
info "Step 7/8: Querying array status..."
MSG_OUT=$(dmsetup message "$LHSR_DEV_NAME" 0 "config" 2>/dev/null || true)
info "  Array config: $MSG_OUT"

# ────────────────────────────────────────────────────────────
# Step 8: Tear down
# ────────────────────────────────────────────────────────────
info "Step 8/8: Tearing down..."

dmsetup remove "$LHSR_DEV_NAME" || {
    fail "Failed to remove DM device"
    exit 1
}
pass "DM device removed"

rmmod dm-lhsr || {
    fail "Failed to unload module"
    exit 1
}
sleep 1

if lsmod | grep -q dm_lhsr; then
    fail "Module still loaded after rmmod"
    exit 1
fi
pass "Module unloaded"

for ldev in "${LOOP_DEVS[@]}"; do
    losetup -d "$ldev" || true
done
pass "Loopback devices detached"

for ((i=0; i<NUM_DISKS; i++)); do
    rm -f "/tmp/lhsr-disk-${i}.img"
done
pass "Temp files cleaned up"

# ────────────────────────────────────────────────────────────
echo ""
pass "╔══════════════════════════════════════════════════════╗"
pass "║  ALL SMOKE TESTS PASSED                             ║"
pass "╚══════════════════════════════════════════════════════╝"
echo ""
echo "Verification steps completed:"
echo "  1. Loopback devices created: ${#LOOP_DEVS[@]}"
echo "  2. Module loaded: dm-lhsr.ko"
echo "  3. RAID5 target created"
echo "  4. Data write successful"
echo "  5. Data read-back checksum MATCH"
echo "  6. Clean teardown"
echo ""
echo "Next: Remove module and test files before running again."
echo ""
