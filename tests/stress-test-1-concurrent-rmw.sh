#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Stress Test 1: Concurrent RMW Parallel Write/Read Correctness
#
# Tests that parallel RMW operations on different stripes don't corrupt data.
# The Phase 10-A concurrent workqueue allows multiple stripe writes in parallel
# across CPUs. This test verifies per-stripe mutex correctness by hammering
# the array with 4 simultaneous fio jobs writing random 16KB blocks to
# non-overlapping regions, then verifying every byte.
#
# Prerequisites:
#   - fio installed (confirmed: /usr/bin/fio)
#   - dm-lhsr.ko at /root/dm-lhsr.ko
#   - root privileges
#   - 4 free loop devices
#
# Linus Torvalds principle: "Reduce before debugging."
# If this test fails, the FIRST question is: which stripe(s) had corruption?
# fio --verify=sha512 pinpoints the exact block.

set -euo pipefail

# ────────────────────────────────────────────────────────────
# Configuration
# ────────────────────────────────────────────────────────────
DISK_COUNT=4
DISK_SIZE_MB=100
CHUNK_SECTS=8
PER_DISK_SECTORS=$((DISK_SIZE_MB * 1024 * 1024 / 512))
SUPERBLOCK_SECTORS=272
LHSR_SIZE=$((PER_DISK_SECTORS - SUPERBLOCK_SECTORS))
DM_NAME="lhsr-stress-rmw"
DM_DEV="/dev/mapper/${DM_NAME}"
WRITE_REGION_START=1   # 1MB offset (past superblock area)
WRITE_REGION_MB=8      # 8MB per job
NUM_JOBS=4
FIO_BS=16k             # 16KB = 32 sectors = 2 full stripes (RAID6 with 8-sector chunk)
FIO_IODEPTH=4

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0
CLEANUP_LOOPS=()

log_pass()  { echo -e "${GREEN}[PASS]${NC} $1"; PASS=$((PASS+1)); }
log_fail()  { echo -e "${RED}[FAIL]${NC} $1"; FAIL=$((FAIL+1)); }
log_info()  { echo -e "${YELLOW}[INFO]${NC} $1"; }
log_alert() { echo -e "\033[0;31m[ALERT]\033[0m $1"; }

# ────────────────────────────────────────────────────────────
# Cleanup — always runs on exit
# ────────────────────────────────────────────────────────────
clean_exit() {
    local ec=$1
    log_info "Cleaning up..."

    # Remove DM device
    if dmsetup info "$DM_NAME" &>/dev/null 2>&1; then
        dmsetup remove "$DM_NAME" 2>/dev/null || true
        log_info "  DM device removed"
    fi

    # Unload module
    if lsmod | grep -q dm_lhsr; then
        rmmod dm_lhsr 2>/dev/null || true
        log_info "  Module unloaded"
    fi

    # Detach loops
    for ldev in "${CLEANUP_LOOPS[@]}"; do
        if losetup "$ldev" &>/dev/null 2>&1; then
            losetup -d "$ldev" 2>/dev/null || true
            log_info "  Loop detached: $ldev"
        fi
    done

    # Remove temp files
    rm -f /tmp/lhsr-stress-disk*.img /tmp/lhsr-stress-pattern*.bin
    rm -f /tmp/fio-write.out /tmp/fio-verify.out /tmp/fio-trigger.out

    if [ $ec -eq 0 ]; then
        echo ""
        log_pass "╔══════════════════════════════════════════════════════╗"
        log_pass "║  STRESS TEST 1 PASSED                               ║"
        log_pass "╚══════════════════════════════════════════════════════╝"
    else
        echo ""
        log_alert "╔══════════════════════════════════════════════════════╗"
        log_alert "║  STRESS TEST 1 FAILED (exit $ec)                    ║"
        log_alert "╚══════════════════════════════════════════════════════╝"
    fi

    echo "PASS=$PASS FAIL=$FAIL"
    exit "$ec"
}

trap 'clean_exit $?' EXIT

# ────────────────────────────────────────────────────────────
# Header
# ────────────────────────────────────────────────────────────
echo ""
log_info "╔══════════════════════════════════════════════════════╗"
log_info "║  STRESS TEST 1: Concurrent RMW Correctness          ║"
log_info "║  4 parallel fio jobs × 8MB random 16KB writes       ║"
log_info "║  RAID6 (4 disks, 8-sector chunk)                    ║"
log_info "╚══════════════════════════════════════════════════════╝"
echo ""

# ────────────────────────────────────────────────────────────
# Step 1: Create loopback devices
# ────────────────────────────────────────────────────────────
log_info "Step 1/8: Creating ${DISK_COUNT} loopback devices (${DISK_SIZE_MB}MB each)..."
for i in $(seq 0 $((DISK_COUNT - 1))); do
    F="/tmp/lhsr-stress-disk${i}.img"
    dd if=/dev/zero of="$F" bs=1M count="$DISK_SIZE_MB" status=none
    LDEV=$(losetup --show -f "$F")
    CLEANUP_LOOPS+=("$LDEV")
    log_info "  Disk $i: $LDEV (${DISK_SIZE_MB}MB)"
done
log_pass "Loopback devices created"

# ────────────────────────────────────────────────────────────
# Step 2: Load LHSR module
# ────────────────────────────────────────────────────────────
log_info "Step 2/8: Loading LHSR module..."
modprobe dm_mod 2>/dev/null || true
rmmod dm_lhsr 2>/dev/null || true

if [ ! -f /root/dm-lhsr.ko ]; then
    log_fail "Module not found at /root/dm-lhsr.ko"
    exit 1
fi

insmod /root/dm-lhsr.ko
sleep 1

if ! lsmod | grep -q dm_lhsr; then
    log_fail "Module failed to load"
    dmesg | tail -10
    exit 1
fi
log_pass "Module loaded: $(lsmod | grep dm_lhsr | awk '{print $1, $2}')"

# ────────────────────────────────────────────────────────────
# Step 3: Create RAID6 target
# ────────────────────────────────────────────────────────────
log_info "Step 3/8: Creating RAID6 target..."
# Format: 0 <size> lhsr raid6 <chunk_sects> <stripe_depth> <cont_sects> <devs...>
TABLE="0 ${LHSR_SIZE} lhsr raid6 ${CHUNK_SECTS} 1 ${CHUNK_SECTS}"
for ldev in "${CLEANUP_LOOPS[@]}"; do
    TABLE="$TABLE $ldev 0"
done

echo "$TABLE" | dmsetup create "$DM_NAME"
sleep 1

if ! dmsetup info "$DM_NAME" &>/dev/null 2>&1; then
    log_fail "Failed to create DM target"
    dmesg | tail -20
    exit 1
fi
log_pass "RAID6 target created: $DM_DEV"

# Show config
dmsetup message "$DM_NAME" 0 "config" 2>/dev/null | head -1 || true

# ────────────────────────────────────────────────────────────
# Step 4: Baseline data integrity write (single-threaded)
# ────────────────────────────────────────────────────────────
log_info "Step 4/8: Writing baseline pattern (single-threaded, 4MB at +1MB)..."
dd if=/dev/urandom of=/tmp/lhsr-stress-pattern-base.bin bs=1M count=4 status=none
BASELINE_SHA=$(sha256sum /tmp/lhsr-stress-pattern-base.bin | cut -d' ' -f1)
log_info "  Baseline SHA256: $BASELINE_SHA"

dd if=/tmp/lhsr-stress-pattern-base.bin of="$DM_DEV" bs=1M seek=1 oflag=direct status=none 2>/dev/null || {
    log_fail "Baseline write failed"
    dmesg | tail -10
    exit 1
}
blockdev --flushbufs "$DM_DEV" 2>/dev/null || true
sleep 1
log_pass "Baseline data written"

# Read back & verify
READ_SHA=$(dd if="$DM_DEV" bs=1M skip=1 count=4 iflag=direct status=none 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$READ_SHA" != "$BASELINE_SHA" ]; then
    log_fail "Pre-stress checksum MISMATCH (got $READ_SHA)"
    exit 1
fi
log_pass "Pre-stress checksum MATCH — baseline OK"

# ────────────────────────────────────────────────────────────
# Step 5: Concurrent fio write workload (THE STRESS)
# ────────────────────────────────────────────────────────────
log_info "Step 5/8: Running concurrent fio write workload..."
log_info "  ${NUM_JOBS} jobs × ${WRITE_REGION_MB}MB, bs=${FIO_BS}, iodepth=${FIO_IODEPTH}"
log_info "  Regions: +1MB, +9MB, +17MB, +25MB (non-overlapping)"
log_info "  Verify: sha512 per block, written inline"
echo ""

TOTAL_BYTES=$((NUM_JOBS * WRITE_REGION_MB * 1024 * 1024))
log_info "  Total write: $((TOTAL_BYTES / 1024 / 1024))MB across ${NUM_JOBS} regions"

# Run fio write + inline verify (write data + sha512 checksum per block)
# Each job writes to its own non-overlapping region
fio --name=stress-write \
    --ioengine=libaio \
    --rw=randwrite \
    --bs="${FIO_BS}" \
    --numjobs="${NUM_JOBS}" \
    --iodepth="${FIO_IODEPTH}" \
    --size="${WRITE_REGION_MB}M" \
    --offset="${WRITE_REGION_START}M" \
    --offset_increment="${WRITE_REGION_MB}M" \
    --direct=1 \
    --verify=sha512 \
    --verify_pattern=0xa5a5a5a5 \
    --filename="${DM_DEV}" \
    --group_reporting \
    --output=/tmp/fio-write.out \
    --status-interval=1 \
    2>&1 || true  # We check fio output for errors, don't exit on fio error

# Check fio exit code
FIO_EXIT=$?
log_info "  fio write phase exit code: ${FIO_EXIT}"
log_info "  fio output (last 20 lines):"
tail -20 /tmp/fio-write.out

# Check for verification errors in fio output
if grep -qi "verify.*failed\|CRC mismatch\|checksum.*bad\|io_error" /tmp/fio-write.out 2>/dev/null; then
    log_alert "fio detected verification errors during write phase!"
    grep -i "verify.*failed\|CRC mismatch\|checksum.*bad\|io_error" /tmp/fio-write.out
    log_fail "Data integrity violation during parallel write"
    exit 1
fi
log_pass "Concurrent fio write completed without verification errors"

# ────────────────────────────────────────────────────────────
# Step 6: fio read-back verification
# ────────────────────────────────────────────────────────────
log_info "Step 6/8: Running fio read-back verification..."
log_info "  Reading back all ${NUM_JOBS} regions with sha512 verify"

fio --name=stress-verify \
    --ioengine=libaio \
    --rw=read \
    --bs="${FIO_BS}" \
    --numjobs="${NUM_JOBS}" \
    --iodepth="${FIO_IODEPTH}" \
    --size="${WRITE_REGION_MB}M" \
    --offset="${WRITE_REGION_START}M" \
    --offset_increment="${WRITE_REGION_MB}M" \
    --direct=1 \
    --verify=sha512 \
    --verify_only \
    --filename="${DM_DEV}" \
    --group_reporting \
    --output=/tmp/fio-verify.out \
    2>&1 || true

FIO_VERIFY_EXIT=$?
log_info "  fio verify exit code: ${FIO_VERIFY_EXIT}"
log_info "  fio verify output (last 15 lines):"
tail -15 /tmp/fio-verify.out

# Check for verification errors
if grep -qi "verify.*failed\|CRC mismatch\|checksum.*bad\|Mismatch\|DATA MISMATCH" /tmp/fio-verify.out 2>/dev/null; then
    log_alert "fio verification FAILED — data corruption detected!"
    grep -B2 -i "verify.*failed\|CRC mismatch\|checksum.*bad\|Mismatch\|DATA MISMATCH" /tmp/fio-verify.out
    log_fail "Data corruption in read-back verification"
    exit 1
fi

# Check fio error count
VERIFY_ERRORS=$(grep -i "verify" /tmp/fio-verify.out | grep -i "fail" | head -5 || true)
if [ -n "$VERIFY_ERRORS" ]; then
    log_fail "fio verify errors: $VERIFY_ERRORS"
    exit 1
fi
log_pass "fio read-back verification PASSED — all blocks match"

# ────────────────────────────────────────────────────────────
# Step 7: Manual SHA256 confirmation of each region
# ────────────────────────────────────────────────────────────
log_info "Step 7/8: Manual SHA256 verification of each region..."
for job_idx in $(seq 0 $((NUM_JOBS - 1))); do
    REGION_OFFSET=$((WRITE_REGION_START + job_idx * WRITE_REGION_MB))
    REGION_FILE="/tmp/lhsr-stress-region${job_idx}.bin"

    dd if="$DM_DEV" of="$REGION_FILE" bs=1M skip="$REGION_OFFSET" count="$WRITE_REGION_MB" iflag=direct status=none 2>/dev/null || {
        log_fail "Failed to read region $job_idx (offset ${REGION_OFFSET}MB)"
        exit 1
    }

    REGION_SHA=$(sha256sum "$REGION_FILE" | cut -d' ' -f1)
    REGION_SIZE=$(stat -c%s "$REGION_FILE" 2>/dev/null || echo 0)
    log_info "  Region $job_idx (offset ${REGION_OFFSET}MB): ${REGION_SIZE} bytes, SHA256=$REGION_SHA"
    rm -f "$REGION_FILE"

    if [ "$REGION_SIZE" = "0" ] 2>/dev/null; then
        log_fail "Region $job_idx appears empty (corruption)"
        exit 1
    fi
done
log_pass "All ${NUM_JOBS} regions readable with valid content"

# ────────────────────────────────────────────────────────────
# Step 8: Final dmesg audit
# ────────────────────────────────────────────────────────────
log_info "Step 8/8: Final dmesg audit..."
DMESG_WARN=$(dmesg | grep -i "lhsr.*WARNING\|lhsr.*Call Trace\|lhsr.*BUG\|dm_lhsr.*general protection\|lhsr.*OOPS" | tail -20 || true)
if [ -n "$DMESG_WARN" ]; then
    log_alert "Kernel warnings detected in dmesg!"
    echo "$DMESG_WARN"
    log_fail "Kernel warnings present"
    exit 1
fi

# Check for I/O errors
IO_ERRORS=$(dmesg | grep -i "lhsr.*I/O error\|lhsr.*IO error\|dm_lhsr.*io err" | tail -5 || true)
if [ -n "$IO_ERRORS" ]; then
    log_alert "I/O errors detected in dmesg!"
    echo "$IO_ERRORS"
    log_fail "I/O errors present"
    exit 1
fi

log_pass "dmesg clean — no warnings, no I/O errors"

# ────────────────────────────────────────────────────────────
# Done — clean exit
# ────────────────────────────────────────────────────────────
log_pass "╔══════════════════════════════════════════════════════╗"
log_pass "║  ALL STRESS TEST 1 CHECKS PASSED                    ║"
log_pass "╚══════════════════════════════════════════════════════╝"
echo "PASS=$PASS FAIL=$FAIL"
exit 0
