#!/bin/bash
# Phase 10-D: LHSR Performance Baseline Benchmarks
# Run on VM only. All targets are loopback on tmpfs.
# Measures: IOPS, latency (avg/p99/max), throughput
# Patterns: sequential, random, mixed; 4K, 1M blocks; QD 1, 4, 16
#
# Usage: on VM: bash /tmp/phase10-benchmarks.sh
set -e

# Clean up stale loop devices from previous runs
for i in $(seq 0 20); do
    sudo /sbin/losetup -d /dev/loop$i 2>/dev/null || true
done
# Clean up stale md and dm
sudo /sbin/mdadm --stop /dev/md/bench-md 2>/dev/null || true
sudo /sbin/mdadm --stop /dev/md127 2>/dev/null || true
sudo /sbin/dmsetup remove bench-lhsr 2>/dev/null || true
# Clean up any leftover backing files from previous runs
rm -f /tmp/bench_* /tmp/t[123]

RESULTS="/tmp/lhsr-benchmark-results-$(date +%Y%m%d-%H%M%S).txt"
echo "LHSR Performance Baseline" | tee "$RESULTS"
echo "Date: $(date)" | tee -a "$RESULTS"
echo "Kernel: $(uname -r)" | tee -a "$RESULTS"
echo "Module version: $(modprobe -q dm_lhsr 2>/dev/null; lsmod | grep dm_lhsr | awk '{print $1,$2}')" | tee -a "$RESULTS"
echo "----------------------------------------" | tee -a "$RESULTS"

bench_fio() {
    local label="$1" target="$2" rw="$3" bs="$4" iodepth="$5" size="$6"

    echo "" | tee -a "$RESULTS"
    echo "--- ${label}: ${rw} bs=${bs} qd=${iodepth} size=${size} ---" | tee -a "$RESULTS"

    sudo fio --name="bench" \
        --filename="$target" \
        --ioengine=libaio \
        --rw="$rw" \
        --bs="$bs" \
        --direct=1 \
        --iodepth="$iodepth" \
        --size="$size" \
        --runtime=15 \
        --time_based \
        --group_reporting \
        --output-format=json 2>/dev/null | python3 -c "
import sys, json
d = json.load(sys.stdin)
j = d['jobs'][0]
read = j['read']
write = j['write']
# p99: read might have zero iops (write-only job), check for percentiles
r_p99 = read['clat_ns'].get('percentile', {}).get('99.000000', 0)
w_p99 = write['clat_ns'].get('percentile', {}).get('99.000000', 0)
if read['iops'] > 0:
    print('  Read:  IOPS={:.0f}  bw={}B/s  lat(avg)={:.1f}us  lat(p99)={:.1f}us  lat(max)={:.0f}us'.format(
        read['iops'], read['bw_bytes'], read['lat_ns']['mean']/1000,
        r_p99/1000, read['lat_ns']['max']/1000))
if write['iops'] > 0:
    print('  Write: IOPS={:.0f}  bw={}B/s  lat(avg)={:.1f}us  lat(p99)={:.1f}us  lat(max)={:.0f}us'.format(
        write['iops'], write['bw_bytes'], write['lat_ns']['mean']/1000,
        w_p99/1000, write['lat_ns']['max']/1000))
" || echo "  [FAILED]"
}

# ---- Setup ----
echo "" | tee -a "$RESULTS"
echo "=== Creating test targets ===" | tee -a "$RESULTS"

# /tmp is ~984MB — layout:
#   raw baseline: 80M
#   mdadm RAID5:  3 x 80M = 240M
#   LHSR RAID5:   3 x 80M = 240M
#   fio test size: 60M per job (fits in all targets)
LOSETUP=/sbin/losetup
MDADM=/sbin/mdadm
DMSETUP=/sbin/dmsetup

# 1. Raw loopback (single disk baseline)
dd if=/dev/zero of=/tmp/bench_raw bs=1M count=80 status=none
RAW_DEV=$(sudo $LOSETUP -f --show /tmp/bench_raw)
echo "  raw: $RAW_DEV (80M)"

# 2. mdadm RAID5 (software baseline)
MD_DISKS=""
for i in 1 2 3; do
    dd if=/dev/zero of=/tmp/bench_md$i bs=1M count=80 status=none
    MD_DISK=$(sudo $LOSETUP -f --show /tmp/bench_md$i)
    MD_DISKS="$MD_DISKS $MD_DISK"
    eval "MD_DEV$i=$MD_DISK"
done
echo n | sudo $MDADM --create /dev/md/bench-md --level=5 --raid-devices=3 \
    $MD_DISKS --force --assume-clean 2>&1 | grep -v mdadm: || true
sleep 2
echo "  mdadm RAID5: /dev/md/bench-md (3x80M) on $MD_DISKS"

# 3. LHSR RAID5 (our target)
LS_DISKS=""
for i in 1 2 3; do
    dd if=/dev/zero of=/tmp/bench_lhsr$i bs=1M count=80 status=none
    LS_DISK=$(sudo $LOSETUP -f --show /tmp/bench_lhsr$i)
    LS_DISKS="$LS_DISKS $LS_DISK 0"
    eval "LS_DEV$i=$LS_DISK"
done
sudo $DMSETUP create bench-lhsr --table \
    "0 250000 lhsr raid5 8 1 8 $LS_DISKS"
echo "  LHSR RAID5: /dev/mapper/bench-lhsr (3x80M)"
sleep 1

# ---- Warm-up: write each target before read tests ----
echo "" | tee -a "$RESULTS"
echo "=== Warm-up ===" | tee -a "$RESULTS"
for dev in /dev/mapper/bench-lhsr /dev/md/bench-md "$RAW_DEV"; do
    if [ -e "$dev" ]; then
        sudo dd if=/dev/zero of="$dev" bs=1M count=10 oflag=direct 2>/dev/null || true
        echo "  wrote $dev"
    fi
done

# ---- Benchmarks ----
echo "" | tee -a "$RESULTS"
echo "============================================" | tee -a "$RESULTS"
echo " RUNNING BENCHMARKS" | tee -a "$RESULTS"
echo "============================================" | tee -a "$RESULTS"
echo "" | tee -a "$RESULTS"

# Sequential read/write: 1M blocks, QD=4, 80M test size
bench_fio "RAW"        "$RAW_DEV"              "read"   "1M" "4" "60M"
bench_fio "MDADM"     "/dev/md/bench-md"          "read"   "1M" "4" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "read"   "1M" "4" "60M"

bench_fio "RAW"        "$RAW_DEV"              "write"  "1M" "4" "60M"
bench_fio "MDADM"     "/dev/md/bench-md"          "write"  "1M" "4" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "write"  "1M" "4" "60M"

# Random 4K, QD=16, 80M test size
bench_fio "RAW"        "$RAW_DEV"              "randread"  "4K" "16" "60M"
bench_fio "MDADM"     "/dev/md/bench-md"          "randread"  "4K" "16" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "randread"  "4K" "16" "60M"

bench_fio "RAW"        "$RAW_DEV"              "randwrite" "4K" "16" "60M"
bench_fio "MDADM"     "/dev/md/bench-md"          "randwrite" "4K" "16" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "randwrite" "4K" "16" "60M"

# Random 4K, QD=1 (single-thread latency)
bench_fio "RAW"        "$RAW_DEV"              "randread"  "4K" "1" "60M"
bench_fio "MDADM"     "/dev/md/bench-md"          "randread"  "4K" "1" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "randread"  "4K" "1" "60M"

bench_fio "RAW"        "$RAW_DEV"              "randwrite" "4K" "1" "60M"
bench_fio "MDADM"     "/dev/md/bench-md"          "randwrite" "4K" "1" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "randwrite" "4K" "1" "60M"

# Random 4K, QD=32 (high concurrency)
bench_fio "RAW"        "$RAW_DEV"              "randread"  "4K" "32" "60M"
bench_fio "MDADM"     "/dev/md/bench-md"          "randread"  "4K" "32" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "randread"  "4K" "32" "60M"

bench_fio "RAW"        "$RAW_DEV"              "randwrite" "4K" "32" "60M"
bench_fio "MDADM"     "/dev/md/bench-md"          "randwrite" "4K" "32" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "randwrite" "4K" "32" "60M"

# Mixed 70/30, 4K, QD=8
bench_fio "MDADM"     "/dev/md/bench-md"          "rw" "4K" "8" "60M"
bench_fio "LHSR"      "/dev/mapper/bench-lhsr"    "rw" "4K" "8" "60M"

echo "" | tee -a "$RESULTS"
echo "============================================" | tee -a "$RESULTS"
echo " BENCHMARKS COMPLETE" | tee -a "$RESULTS"
echo " Results: $RESULTS" | tee -a "$RESULTS"
echo "============================================" | tee -a "$RESULTS"

# Print summary table
echo "" | tee -a "$RESULTS"
echo "=== SUMMARY TABLE ===" | tee -a "$RESULTS"
echo "----------------------------------------" | tee -a "$RESULTS"
printf "%-8s %-10s %-8s %-10s %-12s %-10s\n" "Target" "Pattern" "BS" "QD" "IOPS" "Lat(p99)" | tee -a "$RESULTS"
echo "----------------------------------------" | tee -a "$RESULTS"
grep -E '^---|^  (Read|Write):' "$RESULTS" | head -60 | tee -a "$RESULTS"
echo "============================================" | tee -a "$RESULTS"

# Leave arrays for inspection
echo "" | tee -a "$RESULTS"
echo "To cleanup:" | tee -a "$RESULTS"
echo "  sudo $DMSETUP remove bench-lhsr" | tee -a "$RESULTS"
echo "  sudo $MDADM --stop /dev/md/bench-md" | tee -a "$RESULTS"
echo "  sudo $LOSETUP -d $RAW_DEV $MD_DEV1 $MD_DEV2 $MD_DEV3 $LS_DEV1 $LS_DEV2 $LS_DEV3" | tee -a "$RESULTS"
echo "  rm -f /tmp/bench_raw /tmp/bench_md* /tmp/bench_lhsr*" | tee -a "$RESULTS"
