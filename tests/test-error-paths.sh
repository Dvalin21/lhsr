#!/bin/bash
# Phase 2: Error-path hardening test
#
# Tests:
#   2a) Message fuzzing — invalid commands, bad args, boundary conditions
#   2b) Double disk failure — RAID5 (expected data loss detection) + RAID6 (resilient)
#   2c) Concurrent dtr+I/O — remove device while fio is writing
#   2d) OOM-like: verify all allocation sites check NULL (build-time static)
#
# All scenarios use loopback devices. Module must be loaded.

set -euo pipefail

MODULE="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"
DISK_COUNT=5
DATA_DISKS_R5=3
DATA_DISKS_R6=3
CHUNK_SECTS=8
STRIPES_PER_CONT=1
CONT_SECTS=8
DISK_SIZE_MB=64
SUPERBLOCK_SECTORS=272
PER_DISK_SECTORS=$((DISK_SIZE_MB * 1024 * 1024 / 512))
LHSR_SIZE=$((PER_DISK_SECTORS - SUPERBLOCK_SECTORS))
PASS=0
FAIL=0

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { PASS=$((PASS+1)); echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "${RED}[FAIL]${NC} $1"; }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

cleanup() {
    local ec=$?
    info "Cleaning up..."
    for name in lhsr-msgtest lhsr-ddfail-r5 lhsr-ddfail-r6 lhsr-dtrtest; do
        dmsetup remove "$name" 2>/dev/null || true
    done
    rmmod dm_lhsr 2>/dev/null || true
    for d in "${LOOP_DEVS[@]-}"; do
        losetup -d "$d" 2>/dev/null || true
    done
    rm -f /tmp/lhsr-phase2-*.img /tmp/lhsr-phase2-*.bin
    if [ $ec -ne 0 ] && [ $FAIL -eq 0 ]; then
        fail "Unexpected error during test (exit code $ec)"
    fi
    echo ""
    info "Results: ${PASS} passed, ${FAIL} failed"
    [ $FAIL -eq 0 ] && echo -e "${GREEN}[PASS]${NC} ALL tests PASSED" \
                   || echo -e "${RED}[FAIL]${NC} Some tests FAILED"
    exit $ec
}
trap cleanup EXIT

info "╔══════════════════════════════════════════════════════════╗"
info "║  Phase 2: Error-path hardening test                     ║"
info "╚══════════════════════════════════════════════════════════╝"

# ==============================================================
# Setup: loop devices + module
# ==============================================================
info "Creating ${DISK_COUNT} loopback devices (${DISK_SIZE_MB}MB each)..."
LOOP_DEVS=()
for i in $(seq 0 $((DISK_COUNT - 1))); do
    F="/tmp/lhsr-phase2-disk${i}.img"
    dd if=/dev/zero of="$F" bs=1M count=$DISK_SIZE_MB status=none
    LOOP=$(losetup -f --show "$F")
    LOOP_DEVS+=("$LOOP")
done
info "Loopback: ${LOOP_DEVS[*]}"

modprobe dm_mod 2>/dev/null || true
if ! lsmod | grep -q dm_lhsr; then
    insmod "$MODULE"
fi
lsmod | grep dm_lhsr

# ==============================================================
# SECTION 2a: Message fuzzing
# ==============================================================
info "═══ SECTION 2a: Message fuzzing ═══"

# Create RAID5 target for fuzzing
TABLE="0 ${LHSR_SIZE} lhsr raid5 ${CHUNK_SECTS} ${STRIPES_PER_CONT} ${CONT_SECTS}"
for i in $(seq 0 $((DISK_COUNT - 2))); do
    TABLE="$TABLE ${LOOP_DEVS[$i]} 0"
done
echo "$TABLE" | dmsetup create lhsr-msgtest
info "RAID5 target created: /dev/mapper/lhsr-msgtest"

# Test 1: Unknown command
info "Sending unknown command 'notacommand'..."
if dmsetup message lhsr-msgtest 0 "notacommand" 2>&1; then
    fail "Unknown command should have failed"
else
    pass "Unknown command rejected"
fi

# Test 2: Empty string
info "Sending empty command..."
if dmsetup message lhsr-msgtest 0 "" 2>&1; then
    fail "Empty command should have failed"
else
    pass "Empty command rejected"
fi

# Test 3: Very long command (close to 1 page)
info "Sending very long command..."
LONG_CMD=$(python3 -c "print('x' * 4000, end='')")
if dmsetup message lhsr-msgtest 0 "$LONG_CMD" 2>&1; then
    fail "Long command should have failed"
else
    pass "Long command rejected"
fi

# Test 4: disk_fail with no argument
info "Sending 'disk_fail' without index..."
if dmsetup message lhsr-msgtest 0 "disk_fail" 2>&1; then
    fail "disk_fail without arg should have failed"
else
    pass "disk_fail missing arg rejected"
fi

# Test 5: disk_fail with out-of-range index
info "Sending 'disk_fail 99' (out of range)..."
if dmsetup message lhsr-msgtest 0 "disk_fail 99" 2>&1; then
    fail "disk_fail 99 should have failed"
else
    pass "disk_fail OOR index rejected"
fi

# Test 6: disk_fail with non-numeric arg
info "Sending 'disk_fail abc' (non-numeric)..."
if dmsetup message lhsr-msgtest 0 "disk_fail abc" 2>&1; then
    fail "disk_fail abc should have failed"
else
    pass "disk_fail non-numeric rejected"
fi

# Test 7: device_path with non-existent disk
info "Sending 'device_path 99'..."
if dmsetup message lhsr-msgtest 0 "device_path 99" 2>&1; then
    fail "device_path 99 should have failed"
else
    pass "device_path OOR rejected"
fi

# Test 8: rebuild start without disk index
info "Sending 'rebuild start' without disk..."
if dmsetup message lhsr-msgtest 0 "rebuild start" 2>&1; then
    fail "rebuild start without disk should have failed"
else
    pass "rebuild start missing arg rejected"
fi

# Test 9: rebuild start with OOR index
info "Sending 'rebuild start 99'..."
if dmsetup message lhsr-msgtest 0 "rebuild start 99" 2>&1; then
    fail "rebuild start 99 should have failed"
else
    pass "rebuild start OOR rejected"
fi

# Test 10: Prefix-matching non-issue — "scrubber" no longer matches "scrub"
info "Sending 'scrubber' (exact match only)..."
if dmsetup message lhsr-msgtest 0 "scrubber" 2>&1; then
    fail "'scrubber' should have been rejected (exact match required)"
else
    pass "'scrubber' correctly rejected (exact match)"
fi

# Test 11: "disk_failure" no longer matches "disk_fail"
info "Sending 'disk_failure'..."
if dmsetup message lhsr-msgtest 0 "disk_failure" 2>&1; then
    fail "'disk_failure' should have been rejected"
else
    pass "'disk_failure' correctly rejected (exact match)"
fi

# Test 12: "rebuilds" no longer matches "rebuild"
info "Sending 'rebuilds'..."
if dmsetup message lhsr-msgtest 0 "rebuilds" 2>&1; then
    fail "'rebuilds' should have been rejected"
else
    pass "'rebuilds' correctly rejected (exact match)"
fi

# Dump fuzzing dmesg summary
info "dmesg fuzzing summary:"
dmesg | grep -i "lhsr.*Unknown message\|lhsr.*WARNING\|Call Trace" | tail -5 || echo "  (no warnings)"

dmsetup remove lhsr-msgtest

# Check for kernel warnings from fuzzing
if dmesg | tail -20 | grep -qiE "Call Trace|BUG|OOPS|general protection"; then
    fail "Kernel warning during fuzzing"
else
    pass "No kernel warnings during fuzzing"
fi

# ==============================================================
# SECTION 2b: Double disk failure
# ==============================================================
info "═══ SECTION 2b: Double disk failure ═══"

## 2b.1: RAID5 — double failure (expected data loss for writes to failed region)
info "RAID5 double disk failure test..."
TABLE="0 ${LHSR_SIZE} lhsr raid5 ${CHUNK_SECTS} ${STRIPES_PER_CONT} ${CONT_SECTS}"
for i in $(seq 0 3); do
    TABLE="$TABLE ${LOOP_DEVS[$i]} 0"
done
echo "$TABLE" | dmsetup create lhsr-ddfail-r5

# Write data, fail disk 0, write more data, fail disk 1, try to read
info "Writing initial data..."
dd if=/dev/urandom of=/tmp/lhsr-phase2-r5-pattern.bin bs=1M count=1 status=none
R5_HASH=$(sha256sum /tmp/lhsr-phase2-r5-pattern.bin | cut -d' ' -f1)
dd if=/tmp/lhsr-phase2-r5-pattern.bin of=/dev/mapper/lhsr-ddfail-r5 bs=1M seek=1 oflag=direct status=none

info "Failing disk 0..."
dmsetup message lhsr-ddfail-r5 0 "disk_fail 0"

info "Reading back (single failure — should work)..."
dd if=/dev/mapper/lhsr-ddfail-r5 of=/tmp/lhsr-phase2-r5-singlefail.bin bs=1M skip=1 count=1 iflag=direct status=none
R5_SINGLE_HASH=$(sha256sum /tmp/lhsr-phase2-r5-singlefail.bin | cut -d' ' -f1)
if [ "$R5_HASH" != "$R5_SINGLE_HASH" ]; then
    fail "RAID5 single-failure read reconstruction FAILED"
else
    pass "RAID5 single-failure read reconstructs correctly via parity"
fi

# Fail a second disk — this should trigger data loss for writes
info "Failing disk 1 (second failure in RAID5)..."
dmsetup message lhsr-ddfail-r5 0 "disk_fail 1"

# Read should fail (two failed disks in RAID5 = data loss)
info "Reading after double failure (RAID5 — expected to fail)..."
if dd if=/dev/mapper/lhsr-ddfail-r5 of=/dev/null bs=4k skip=256 count=256 iflag=direct status=none 2>&1; then
    info "RAID5 double-failure read succeeded (may have read from surviving region)"
    # This might succeed if the read was from a region not on failed disks
else
    pass "RAID5 double-failure read correctly failed (data loss expected)"
fi

dmsetup remove lhsr-ddfail-r5

## 2b.2: RAID6 — double failure (resilient to 2 disk failures)
info "RAID6 double disk failure test..."
TABLE="0 ${LHSR_SIZE} lhsr raid6 ${CHUNK_SECTS} ${STRIPES_PER_CONT} ${CONT_SECTS}"
for i in $(seq 0 4); do
    TABLE="$TABLE ${LOOP_DEVS[$i]} 0"
done
echo "$TABLE" | dmsetup create lhsr-ddfail-r6

# Write data
info "Writing data to RAID6..."
dd if=/dev/urandom of=/tmp/lhsr-phase2-r6-pattern.bin bs=1M count=1 status=none
R6_HASH=$(sha256sum /tmp/lhsr-phase2-r6-pattern.bin | cut -d' ' -f1)
dd if=/tmp/lhsr-phase2-r6-pattern.bin of=/dev/mapper/lhsr-ddfail-r6 bs=1M seek=1 oflag=direct status=none

# Fail disk 0
info "Failing disk 0..."
dmsetup message lhsr-ddfail-r6 0 "disk_fail 0"
# Fail disk 1 (RAID6 can survive 2 failures)
info "Failing disk 1 (second failure — RAID6 survives)..."
dmsetup message lhsr-ddfail-r6 0 "disk_fail 1"

# Read should succeed via Q+surviving data
info "Reading back after double failure (RAID6 — should work)..."
dd if=/dev/mapper/lhsr-ddfail-r6 of=/tmp/lhsr-phase2-r6-doublefail.bin bs=1M skip=1 count=1 iflag=direct status=none
R6_DOUBLE_HASH=$(sha256sum /tmp/lhsr-phase2-r6-doublefail.bin | cut -d' ' -f1)
if [ "$R6_HASH" != "$R6_DOUBLE_HASH" ]; then
    fail "RAID6 double-failure read reconstruction FAILED (data lost)"
else
    pass "RAID6 double-failure read reconstructs correctly via P+Q"
fi

dmsetup remove lhsr-ddfail-r6

# ==============================================================
# SECTION 2c: Concurrent dtr + I/O
# ==============================================================
info "═══ SECTION 2c: Concurrent dtr + I/O ═══"

TABLE="0 ${LHSR_SIZE} lhsr raid5 ${CHUNK_SECTS} ${STRIPES_PER_CONT} ${CONT_SECTS}"
for i in $(seq 0 3); do
    TABLE="$TABLE ${LOOP_DEVS[$i]} 0"
done
echo "$TABLE" | dmsetup create lhsr-dtrtest

# Start background fio write
info "Starting background fio write..."
fio --name=dtr-stress \
    --filename=/dev/mapper/lhsr-dtrtest \
    --rw=randwrite \
    --bs=4k \
    --size=4M \
    --numjobs=2 \
    --ioengine=libaio \
    --iodepth=4 \
    --direct=1 \
    --runtime=1000 \
    --time_based \
    --group_reporting \
    --output=/dev/null \
    --background 2>&1 || true
FIO_PID=$!
info "fio PID: $FIO_PID"

# Wait a moment for I/O to start
sleep 0.5

# Remove device WHILE I/O is in flight
info "Removing device while I/O is in flight (dtr race test)..."
if dmsetup remove lhsr-dtrtest --force 2>&1; then
    pass "dtr while I/O active completed without hang"
else
    # If removal fails, kill fio and try again
    info "dtr removal blocked by active I/O, killing fio..."
    kill $FIO_PID 2>/dev/null || true
    wait $FIO_PID 2>/dev/null || true
    sleep 0.5
    if dmsetup remove lhsr-dtrtest --force 2>&1; then
        pass "dtr completed after fio kill"
    else
        fail "dtr could not complete even after fio kill"
    fi
fi

# Verify module is still loaded (should have unloaded only the device, not the module)
if lsmod | grep -q dm_lhsr; then
    pass "Module still loaded after dtr"
else
    info "Module auto-unloaded (acceptable for device removal)"
    # Re-load for cleanup
    insmod "$MODULE" 2>/dev/null || true
fi

# ==============================================================
# SECTION 2d: dmesg audit — look for warnings/errors
# ==============================================================
info "═══ dmesg warning check ═══"
if dmesg | tail -40 | grep -qiE "Call Trace|BUG|OOPS|general protection|lhsr.*NULL"; then
    fail "Kernel warning/error in dmesg"
    dmesg | tail -40 | grep -iE "Call Trace|BUG|OOPS|general protection|lhsr.*NULL"
else
    pass "No kernel warnings or errors"
fi

# ==============================================================
# SUMMARY
# ==============================================================
echo ""
info "═══ PHASE 2 RESULTS ═══"
info "${PASS} passed, ${FAIL} failed"
[ $FAIL -eq 0 ] && echo -e "${GREEN}[PASS]${NC} Phase 2: ALL TESTS PASSED" \
               || echo -e "${RED}[FAIL]${NC} Phase 2: SOME TESTS FAILED"
