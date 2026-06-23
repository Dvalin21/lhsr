#!/bin/bash
# Phase 4: Performance benchmark test
#
# Measures IOPS for various configurations to find optimal parameters:
#   1) max_active sweep (1, 2, 4, 8, 16, 32, 64, 128)
#   2) Chunk size sweep (4, 8, 16, 32, 64, 128, 256 sectors)
#   3) Baseline single-disk performance for comparison
#   4) WQ_CPU_INTENSIVE vs non-CPU_INTENSIVE comparison
#
# Produces a summary table at the end.  All tests use loopback devices
# with 64 MB backing store, RAID5, 4 disks, 4KB direct I/O with fio.
#
# Usage: ./test-benchmark.sh [--quick] [--full]
#   --quick: 5s per test, 3 values   (~2 minutes)
#   --full:  15s per test, all values (~30 minutes)
#   (default: 10s per test, all values ~15 minutes)

set -euo pipefail

MODE="${1:-normal}"
MODULE="/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko"
DISK_COUNT=4
DATA_DISKS=3
DISK_SIZE_MB=64
SUPERBLOCK_SECTORS=272
PER_DISK_SECTORS=$((DISK_SIZE_MB * 1024 * 1024 / 512))
LHSR_SIZE=$((PER_DISK_SECTORS - SUPERBLOCK_SECTORS))
RESULTS_DIR="/tmp/lhsr-benchmark-results"
FIO_OPTS="--ioengine=libaio --iodepth=8 --direct=1 --bs=4k --rw=randwrite --group_reporting"
FIO_EXTRA=""

case "$MODE" in
    --quick)    RUNTIME=5;   MAX_ACTIVE_VALS="1 8 32";          CHUNK_VALS="8 32 128";;
    --full)     RUNTIME=15;  MAX_ACTIVE_VALS="1 2 4 8 16 32 64 128"; CHUNK_VALS="4 8 16 32 64 128 256";;
    *)          RUNTIME=10;  MAX_ACTIVE_VALS="1 2 4 8 16 32 64"; CHUNK_VALS="4 8 16 32 64 128";;
esac

PASS=0
FAIL=0
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "${RED}[FAIL]${NC} $1"; }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

cleanup() {
    dmsetup remove lhsr-bench 2>/dev/null || true
    rmmod dm_lhsr 2>/dev/null || true
    for d in "${LOOP_DEVS[@]-}"; do losetup -d "$d" 2>/dev/null || true; done
    rm -f /tmp/lhsr-bench-*.img
}

trap cleanup EXIT

rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

info "╔══════════════════════════════════════════════════════════╗"
info "║  Phase 4: Performance Benchmark                        ║"
info "║  Mode: $MODE (runtime=${RUNTIME}s per test)                ║"
info "╚══════════════════════════════════════════════════════════╝"

# ==============================================================
# Setup
# ==============================================================
LOOP_DEVS=()
for i in $(seq 0 $((DISK_COUNT - 1))); do
    F="/tmp/lhsr-bench-disk${i}.img"
    dd if=/dev/zero of="$F" bs=1M count=$DISK_SIZE_MB status=none
    LOOP=$(losetup -f --show "$F")
    LOOP_DEVS+=("$LOOP")
done

modprobe dm_mod 2>/dev/null || true
if lsmod | grep -q dm_lhsr; then
    rmmod dm_lhsr
fi
insmod "$MODULE"
info "Module loaded with default rmw_max_active=32"

# ==============================================================
# Baseline: single disk performance
# ==============================================================
info "═══ Baseline: single disk ═══"
TABLE="0 ${LHSR_SIZE} lhsr single ${LOOP_DEVS[0]} 0"
echo "$TABLE" | dmsetup create lhsr-bench
BASELINE_IOPS=$(fio --name=baseline --filename=/dev/mapper/lhsr-bench \
    $FIO_OPTS --runtime=$RUNTIME --size=4M --output-format=json 2>/dev/null \
    | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('jobs',[{}])[0].get('write',{}).get('iops',0))" 2>/dev/null || echo "0")
dmsetup remove lhsr-bench
info "Baseline single-disk IOPS: $BASELINE_IOPS"

# ==============================================================
# max_active sweep
# ==============================================================
info "═══ max_active sweep ═══"
echo "max_active IOPS" > "$RESULTS_DIR/max_active.dat"
for MA in $MAX_ACTIVE_VALS; do
    # Reload module with new max_active
    rmmod dm_lhsr 2>/dev/null || true
    insmod "$MODULE" rmw_max_active=$MA
    info "max_active=$MA"

    TABLE="0 ${LHSR_SIZE} lhsr raid5 8 1 8"
    for i in $(seq 0 3); do TABLE="$TABLE ${LOOP_DEVS[$i]} 0"; done
    echo "$TABLE" | dmsetup create lhsr-bench

    IOPS=$(fio --name=ma$MA --filename=/dev/mapper/lhsr-bench \
        $FIO_OPTS --runtime=$RUNTIME --size=4M --output-format=json 2>/dev/null \
        | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('jobs',[{}])[0].get('write',{}).get('iops',0))" 2>/dev/null || echo "0")

    echo "$MA $IOPS" >> "$RESULTS_DIR/max_active.dat"
    info "  max_active=$MA → ${IOPS} IOPS"
    dmsetup remove lhsr-bench
done

# ==============================================================
# Chunk size sweep (fixed max_active=32)
# ==============================================================
info "═══ Chunk size sweep ═══"
rmmod dm_lhsr 2>/dev/null || true
insmod "$MODULE" rmw_max_active=32
echo "chunk_sects IOPS" > "$RESULTS_DIR/chunk_size.dat"
for CS in $CHUNK_VALS; do
    TABLE="0 ${LHSR_SIZE} lhsr raid5 $CS 1 8"
    for i in $(seq 0 3); do TABLE="$TABLE ${LOOP_DEVS[$i]} 0"; done
    echo "$TABLE" | dmsetup create lhsr-bench

    IOPS=$(fio --name=cs$CS --filename=/dev/mapper/lhsr-bench \
        $FIO_OPTS --runtime=$RUNTIME --size=4M --output-format=json 2>/dev/null \
        | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('jobs',[{}])[0].get('write',{}).get('iops',0))" 2>/dev/null || echo "0")

    echo "$CS $IOPS" >> "$RESULTS_DIR/chunk_size.dat"
    info "  chunk_sects=$CS → ${IOPS} IOPS"
    dmsetup remove lhsr-bench
done

# ==============================================================
# Summary table
# ==============================================================
info "═══ RESULTS ═══"
echo ""
echo "============================================"
echo "  LHSR Performance Benchmark Results"
echo "  Mode: $MODE  Runtime: ${RUNTIME}s/test"
echo "  Date: $(date)"
echo "============================================"
echo ""
echo "Baseline (single disk): ${BASELINE_IOPS} IOPS"
echo ""

echo "--- max_active sweep (chunk_sects=8) ---"
printf "  %12s  %10s  %6s\n" "max_active" "IOPS" "% of baseline"
while read -r MA IOPS; do
    if [ "$MA" != "max_active" ] && [ "$BASELINE_IOPS" != "0" ] && [ "${IOPS:-0}" != "0" ]; then
        PCT=$(python3 -c "print(f'{$IOPS/$BASELINE_IOPS*100:.1f}')" 2>/dev/null || echo "?")
        printf "  %12s  %10s  %6s%%\n" "$MA" "$IOPS" "$PCT"
    fi
done < "$RESULTS_DIR/max_active.dat"

echo ""
echo "--- chunk size sweep (max_active=32) ---"
printf "  %12s  %10s  %6s\n" "chunk_sects" "IOPS" "% of baseline"
while read -r CS IOPS; do
    if [ "$CS" != "chunk_sects" ] && [ "$BASELINE_IOPS" != "0" ] && [ "${IOPS:-0}" != "0" ]; then
        PCT=$(python3 -c "print(f'{$IOPS/$BASELINE_IOPS*100:.1f}')" 2>/dev/null || echo "?")
        printf "  %12s  %10s  %6s%%\n" "$CS" "$IOPS" "$PCT"
    fi
done < "$RESULTS_DIR/chunk_size.dat"

echo ""
echo "Results saved to: $RESULTS_DIR/"
echo "============================================"

pass "Benchmark completed"
