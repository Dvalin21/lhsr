#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# RAID6 bitmap_recover Q parity test for LHSR
# 1) Create RAID6, write data (dirties bitmap, computes P+Q)
# 2) Remove device (bitmap persists to disk)
# 3) Re-create device (triggers bitmap_recover: reads data, recomputes P+Q)
# 4) Verify data integrity
# 5) Optionally: fail data+P, read via Q (proves Q was reconstructed)
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
PATTERN_OFFSET=$((1 * 1024 * 1024))
PATTERN_SIZE=$((4 * 1024 * 1024))

PASS=0
FAIL=0

clean_exit() {
    local ec=$1
    dmsetup remove lhsr-bm-test 2>/dev/null || true
    for d in /dev/loop1[0-3]; do
        losetup -d "$d" 2>/dev/null || true
    done
    rm -f /tmp/lhsr-bm-disk*.img
    exit "$ec"
}

log_pass()  { echo -e "\033[0;32m[PASS]\033[0m $1"; PASS=$((PASS+1)); }
log_fail()  { echo -e "\033[0;31m[FAIL]\033[0m $1"; FAIL=$((FAIL+1)); clean_exit 1; }
log_info()  { echo -e "\033[1;33m[INFO]\033[0m $1"; }

create_raid6() {
    local NAME=$1
    local TABLE="0 ${TOTAL_SECTORS} lhsr raid6 8 1 8"
    for d in /dev/loop1{0,1,2,3}; do TABLE="$TABLE $d 0"; done
    echo "$TABLE" | dmsetup create "$NAME"
}

# --- Setup ---
log_info "╔══════════════════════════════════════════════════════╗"
log_info "║  LHSR BITMAP_RECOVER Q PARITY TEST                   ║"
log_info "╚══════════════════════════════════════════════════════╝"

log_info "Creating $NUM_DISKS loopback devices (${DISK_SIZE_MB}MB each)..."
for i in 0 1 2 3; do
    dd if=/dev/zero of="/tmp/lhsr-bm-disk${i}.img" bs=1M count="$DISK_SIZE_MB" status=none
    losetup "/dev/loop1${i}" "/tmp/lhsr-bm-disk${i}.img"
done
log_pass "Loopback devices created"

log_info "Loading LHSR module..."
rmmod dm_lhsr 2>/dev/null || true
insmod /root/dm-lhsr.ko
lsmod | grep dm_lhsr >/dev/null
log_pass "Module loaded"

# --- Phase 1: Create RAID6 and write data ---
log_info "╔══ Phase 1: Initial write ═══════════════════════════╗"

create_raid6 lhsr-bm-test
log_pass "RAID6 target created"

SEEK_SECTORS=$((PATTERN_OFFSET / BLOCK_SIZE))
WRITE_COUNT=$((PATTERN_SIZE / BLOCK_SIZE))

dd if=/dev/urandom of=/tmp/lhsr-bm-pattern.bin bs="$PATTERN_SIZE" count=1 status=none
PATTERN_SHA=$(sha256sum /tmp/lhsr-bm-pattern.bin | cut -d' ' -f1)
log_info "Pattern SHA256: $PATTERN_SHA"

dd if=/tmp/lhsr-bm-pattern.bin of=/dev/mapper/lhsr-bm-test \
    bs="$BLOCK_SIZE" seek="$SEEK_SECTORS" count="$WRITE_COUNT" \
    oflag=dsync status=none 2>&1
log_pass "Data written to RAID6 (O_SYNC — synchronous write)"

# Verify healthy read
READBACK_SHA=$(dd if=/dev/mapper/lhsr-bm-test bs="$BLOCK_SIZE" \
    skip="$SEEK_SECTORS" count="$WRITE_COUNT" status=none 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$READBACK_SHA" != "$PATTERN_SHA" ]; then
    log_fail "Phase 1 checksum MISMATCH (got $READBACK_SHA)"
fi
log_pass "Phase 1 checksum MATCH"

# --- Phase 2: Remove and re-create to trigger bitmap_recover ---
log_info "╔══ Phase 2: bitmap_recover trigger ══════════════════╗"

# Record dmesg line count so we can filter new messages
DMESG_LINE=$(dmesg | wc -l)

log_info "Removing DM device (flushes dirty bitmap to disk)..."
dmsetup remove lhsr-bm-test
log_pass "DM device removed"

# --- Crash simulation: corrupt P+Q and set bitmap dirty ---
# Disk mapping: data0=loop10 data1=loop11 P=loop12 Q=loop13
# Region 0 covers stripes 0-7 = sectors 0-2047 on each disk
P_DISK=/dev/loop12
Q_DISK=/dev/loop13
BITMAP_PAGE_SECTOR=204520           # disk_offset + arr->disk_sectors where
                                    # disk_sectors = 204800 - 280 (WIB+superblock+bitmap meta)

log_info "╔══ Crash simulation ══════════════════════════════════╗"

log_info "Corrupting P parity (disk 2, sectors 0-2047)..."
dd if=/dev/zero of=$P_DISK bs=512 seek=0 count=2048 oflag=dsync status=none 2>&1
log_pass "P parity corrupted"

log_info "Corrupting Q parity (disk 3, sectors 0-2047)..."
dd if=/dev/zero of=$Q_DISK bs=512 seek=0 count=2048 oflag=dsync status=none 2>&1
log_pass "Q parity corrupted"

log_info "Setting bitmap page 0 bit 0 dirty (sector $BITMAP_PAGE_SECTOR)..."
/root/set_bitmap_bit /dev/loop10 $BITMAP_PAGE_SECTOR 0
log_pass "Bitmap dirty bit set"

log_info "╔══ End crash simulation ══════════════════════════════╝"

# Re-create — this triggers bitmap_load + bitmap_recover
log_info "Re-creating device (triggers bitmap_load + bitmap_recover)..."
create_raid6 lhsr-bm-test
log_pass "Device re-created (bitmap_recover ran)"

# --- Phase 3: Check recovery messages ---
log_info "╔══ Phase 3: Verification ════════════════════════════╗"

log_info "New dmesg entries (post bitmap_recover):"
dmesg | tail -n +$DMESG_LINE 2>/dev/null || dmesg | grep -i "bitmap" || true

# Check for bitmap recovery messages
if dmesg | tail -n +$DMESG_LINE 2>/dev/null | grep -q "bitmap: recovering"; then
    log_pass "bitmap_recover processed dirty regions"
else
    log_fail "bitmap_recover did NOT process any regions (no dirty bits found)"
fi

# Check for Q write messages
if dmesg | tail -n +$DMESG_LINE 2>/dev/null | grep -q "bitmap: recover write Q disk"; then
    log_pass "Q parity was written during recovery"
else
    log_info "Q write messages not expected if Q is healthy (no errors)"
    log_info "(Q write only logs on error — success is silent)"
fi

# Check for warnings
DMESG_WARN=$(dmesg | tail -n +$DMESG_LINE 2>/dev/null | \
    grep -i "WARNING\|Call Trace\|BUG\|general protection" | tail -5 || true)
if [ -n "$DMESG_WARN" ]; then
    log_fail "Kernel warnings after bitmap_recover: $DMESG_WARN"
fi
log_pass "No warnings after bitmap_recover"

# Verify data integrity after recovery (sync first to ensure writeback)
sync
READBACK_SHA2=$(dd if=/dev/mapper/lhsr-bm-test bs="$BLOCK_SIZE" \
    skip="$SEEK_SECTORS" count="$WRITE_COUNT" status=none 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$READBACK_SHA2" != "$PATTERN_SHA" ]; then
    log_fail "Phase 3 (post-recovery) checksum MISMATCH (got $READBACK_SHA2)"
fi
log_pass "Phase 3 checksum MATCH — data intact after bitmap_recover"

# --- Phase 4: Prove Q was correctly reconstructed ---
# Fail a data disk AND P disk. Read via single-equation RS decode from Q.
# This only works if bitmap_recover correctly reconstructed Q parity.
log_info "╔══ Phase 4: Q parity validation (data+P failure) ═══╝"
log_info "Failing data disk 1 and P disk (disk 2)..."
dmsetup message lhsr-bm-test 0 disk_fail 1 2>&1 || log_fail "Failed to fail disk 1"
sleep 0.5
dmsetup message lhsr-bm-test 0 disk_fail 2 2>&1 || log_fail "Failed to fail disk 2"
sleep 0.5
log_pass "Disks 1 and 2 marked failed"

# Read back — this requires Q parity (single-equation RS decode: data+P failure)
log_info "Reading back after data+P failure (requires correct Q)..."
sync
READBACK_SHA3=$(dd if=/dev/mapper/lhsr-bm-test bs="$BLOCK_SIZE" \
    skip="$SEEK_SECTORS" count="$WRITE_COUNT" status=none 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$READBACK_SHA3" != "$PATTERN_SHA" ]; then
    log_fail "Q-validate checksum MISMATCH (got $READBACK_SHA3)"
fi
log_pass "Q-validate checksum MATCH — Q parity correctly reconstructed by bitmap_recover"

# --- Final dmesg check ---
DMESG_WARN2=$(dmesg | tail -n +$DMESG_LINE 2>/dev/null | \
    grep -i "WARNING\|Call Trace\|BUG\|general protection" | tail -5 || true)
if [ -n "$DMESG_WARN2" ]; then
    log_fail "Final kernel warnings: $DMESG_WARN2"
fi
log_pass "Zero kernel warnings"

# --- Done ---
log_pass "╔══════════════════════════════════════════════════════╗"
log_pass "║  BITMAP_RECOVER Q PARITY TEST PASSED                 ║"
log_pass "╚══════════════════════════════════════════════════════╝"
echo "PASS=$PASS FAIL=$FAIL"

clean_exit 0
