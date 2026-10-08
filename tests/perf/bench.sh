#!/bin/bash
# bench.sh — reproducible mdadm-vs-LHSR comparison.
# RUN ONLY IN THE TEST VM (needs root, /dev/vdc /dev/vdd /dev/vde as scratch).
# Usage: sudo ./bench.sh [mdadm|lhsr|both] [reps]
set -u

DISKS="/dev/vdc /dev/vdd /dev/vde"
LEN=$(( $(blockdev --getsz /dev/vdc) - 4096 ))
REPS=${2:-1}

run_fio() { # $1=dev $2=rw $3=bs $4=qd
    fio --name=t --rw="$2" --bs="$3" --iodepth="$4" --size=100M \
        --runtime=8 --time_based --direct=1 --group_reporting \
        --filename="$1" 2>/dev/null | grep -E "READ:|WRITE:" | head -2
}

wipe() {
    for d in $DISKS; do dd if=/dev/zero of="$d" bs=1M count=32 status=none; done
    mdadm --zero-superblock $DISKS 2>/dev/null || true
}

bench_target() { # $1=dev $2=label
    echo "== $2: $1 =="
    echo -n "  seqwrite 1M qd4:  "; run_fio "$1" write 1M 4
    echo -n "  randwrite 4k qd16:"; run_fio "$1" randwrite 4K 16
    echo -n "  randread  4k qd16:"; run_fio "$1" randread 4K 16
}

do_md() {
    echo "### mdadm RAID5 (chunk 4K) ###"
    mdadm --create /dev/md0 --level=5 --chunk=4 --raid-devices=3 \
        $DISKS --force --assume-clean 2>&1 | tail -1
    for r in $(seq 1 "$REPS"); do
        echo "--- rep $r ---"
        bench_target /dev/md0 mdadm
    done
    mdadm --stop /dev/md0
    mdadm --zero-superblock $DISKS 2>/dev/null || true
}

do_lhsr() {
    echo "### LHSR RAID5 (chunk 4K) ###"
    modprobe dm-mod 2>/dev/null || true
    lsmod | grep -q dm_lhsr || insmod /tmp/lhsr/kernel/dm-lhsr/dm-lhsr.ko
    dmsetup create lhsr0 --table "0 $LEN lhsr raid5 8 1 8 /dev/vdc 0 /dev/vdd 0 /dev/vde 0"
    dmsetup status lhsr0
    for r in $(seq 1 "$REPS"); do
        echo "--- rep $r ---"
        bench_target /dev/mapper/lhsr0 lhsr
    done
    dmsetup remove lhsr0
}

WHAT=${1:-both}
case "$WHAT" in
    mdadm) wipe; do_md ;;
    lhsr)  wipe; do_lhsr ;;
    both)  wipe; do_md; wipe; do_lhsr ;;
    *) echo "usage: $0 [mdadm|lhsr|both] [reps]"; exit 1 ;;
esac
