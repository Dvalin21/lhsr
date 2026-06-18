#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# RAID6 data+P failure reconstruction test for LHSR
# Tests RS decode single-equation: fail data disk 0 AND P disk, read data disk 0
# D_target = inv(g^target) * (Q ^ sum(g^i * surviving))
#
# Prerequisites:
#   - dm-lhsr.ko loaded
#   - root privileges
#   - 4 free loop devices

set -euo pipefail

NUM_DISKS=4
DISK_SIZE_MB=100
BLOCK_SIZE=512
PER_DISK_SECTORS=$((DISK_SIZE_MB * 1024 * 1024 / BLOCK_SIZE))
SUPERBLOCK_SECTORS=272
TOTAL_SECTORS=$((PER_DISK_SECTORS - SUPERBLOCK_SECTORS))
PATTERN_OFFSET=$((1 * 1024 * 1024))  # 1MB in bytes
PATTERN_SIZE=$((4 * 1024 * 1024))    # 4MB

PASS=0
FAIL=0

clean_exit() {
    local ec=$1
    dmsetup remove lhsr-dp-test 2>/dev/null || true
    for d in /dev/loop1[0-3]; do
        losetup -d "$d" 2>/dev/null || true
    done
    rm -f /tmp/lhsr-dp-disk*.img /tmp/lhsr-dp-pattern.bin
    exit "$ec"
}

log_pass()  { echo -e "\033[0;32m[PASS]\033[0m $1"; PASS=$((PASS+1)); }
log_fail()  { echo -e "\033[0;31m[FAIL]\033[0m $1"; FAIL=$((FAIL+1)); clean_exit 1; }
log_info()  { echo -e "\033[1;33m[INFO]\033[0m $1"; }

# --- Setup ---
log_info "╔══════════════════════════════════════════════════════╗"
log_info "║  LHSR RAID6 DATA+P FAILURE RECONSTRUCTION TEST      ║"
log_info "╚══════════════════════════════════════════════════════╝"

log_info "Creating $NUM_DISKS loopback devices (${DISK_SIZE_MB}MB each)..."
for i in 0 1 2 3; do
    dd if=/dev/zero of="/tmp/lhsr-dp-disk${i}.img" bs=1M count="$DISK_SIZE_MB" status=none
    losetup "/dev/loop1${i}" "/tmp/lhsr-dp-disk${i}.img"
done
log_pass "Loopback devices created"

log_info "Loading LHSR module..."
rmmod dm_lhsr 2>/dev/null || true
insmod /root/dm-lhsr.ko
lsmod | grep dm_lhsr >/dev/null
log_pass "Module loaded"

# --- Create RAID6 (4 disks: D0 D1 P Q) ---
RAID6_TABLE="0 ${TOTAL_SECTORS} lhsr raid6 8 1 8"
for d in /dev/loop1{0,1,2,3}; do RAID6_TABLE="$RAID6_TABLE $d 0"; done
log_info "Creating RAID6 target..."
echo "$RAID6_TABLE" | dmsetup create lhsr-dp-test
log_pass "RAID6 target created: /dev/mapper/lhsr-dp-test"

# --- Write test data ---
SEEK_SECTORS=$((PATTERN_OFFSET / BLOCK_SIZE))
WRITE_COUNT=$((PATTERN_SIZE / BLOCK_SIZE))

log_info "Writing ${PATTERN_SIZE}B test pattern at offset $PATTERN_OFFSET..."
dd if=/dev/urandom of=/tmp/lhsr-dp-pattern.bin bs="$PATTERN_SIZE" count=1 status=none
PATTERN_SHA=$(sha256sum /tmp/lhsr-dp-pattern.bin | cut -d' ' -f1)
log_info "  Pattern SHA256: $PATTERN_SHA"
dd if=/tmp/lhsr-dp-pattern.bin of=/dev/mapper/lhsr-dp-test bs="$BLOCK_SIZE" seek="$SEEK_SECTORS" count="$WRITE_COUNT" status=none 2>&1
log_pass "Data written"

# --- Verify healthy read ---
READBACK_SHA=$(dd if=/dev/mapper/lhsr-dp-test bs="$BLOCK_SIZE" skip="$SEEK_SECTORS" count="$WRITE_COUNT" status=none 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$READBACK_SHA" != "$PATTERN_SHA" ]; then
    log_fail "Pre-failure checksum MISMATCH (got $READBACK_SHA)"
fi
log_pass "Pre-failure checksum MATCH"

# --- Check member status ---
log_info "Member status before failure:"
dmsetup message lhsr-dp-test 0 member_status 0 2>&1 || true
dmsetup message lhsr-dp-test 0 member_status 1 2>&1 || true
dmsetup message lhsr-dp-test 0 member_status 2 2>&1 || true
dmsetup message lhsr-dp-test 0 member_status 3 2>&1 || true

# --- Fail data disk 0 AND P disk (disk 2 in 4-disk RAID6) ---
# Layout: D0(0) D1(1) P(2) Q(3)
P_DISK=2
log_info "Failing data disk 0 and P disk (disk $P_DISK)..."
dmsetup message lhsr-dp-test 0 disk_fail 0 2>&1 || { log_fail "Failed to fail disk 0"; }
sleep 0.5
dmsetup message lhsr-dp-test 0 disk_fail "$P_DISK" 2>&1 || { log_fail "Failed to fail P disk"; }
sleep 0.5
log_pass "Disks 0 and P marked failed"

# --- Read from degraded array (triggers RS decode single-equation from Q) ---
log_info "Reading back after data+P failure..."
READBACK_SHA2=$(dd if=/dev/mapper/lhsr-dp-test bs="$BLOCK_SIZE" skip="$SEEK_SECTORS" count="$WRITE_COUNT" status=none 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$READBACK_SHA2" != "$PATTERN_SHA" ]; then
    log_fail "Reconstruction checksum MISMATCH (got $READBACK_SHA2)"
fi
log_pass "Reconstruction checksum MATCH"

# --- Check member status ---
log_info "Member status after failure:"
dmsetup message lhsr-dp-test 0 member_status 0 2>&1 || true
dmsetup message lhsr-dp-test 0 member_status 1 2>&1 || true
dmsetup message lhsr-dp-test 0 member_status 2 2>&1 || true
dmsetup message lhsr-dp-test 0 member_status 3 2>&1 || true

# --- Check dmesg for RS decode events ---
log_info "Reconstruction event log:"
dmesg | grep -i "RECON\|RS decode" | tail -10

# --- Final dmesg check ---
DMESG_WARN=$(dmesg | grep -i "WARNING\|Call Trace\|BUG\|general protection" | tail -5 || true)
if [ -n "$DMESG_WARN" ]; then
    log_fail "Kernel warnings detected: $DMESG_WARN"
fi
log_pass "No warnings"

# --- Done ---
log_pass "╔══════════════════════════════════════════════════════╗"
log_pass "║  RAID6 DATA+P FAILURE TEST PASSED                   ║"
log_pass "╚══════════════════════════════════════════════════════╝"
echo "PASS=$PASS FAIL=$FAIL"

clean_exit 0
