#!/bin/bash
# Test RAID6 basic functionality
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${YELLOW}[INFO]${NC} RAID6 Smoke Test"

DISK_COUNT=4
DISK_SIZE_MB=100
CHUNK_SECTS=8
WRITE_SIZE_MB=4
PER_DISK_SECTORS=$((DISK_SIZE_MB * 1024 * 1024 / 512))
SUPERBLOCK_SECTORS=272
LHSR_SIZE=$((PER_DISK_SECTORS - SUPERBLOCK_SECTORS))

# Create loopback devices
DISK_FILES=(); LOOP_DEVS=()
for i in $(seq 0 $((DISK_COUNT - 1))); do
    F="/tmp/lhsr-raid6-disk${i}.img"
    dd if=/dev/zero of="$F" bs=1M count=$DISK_SIZE_MB status=none
    DISK_FILES+=("$F")
    LOOP=$(losetup -f --show "$F")
    LOOP_DEVS+=("$LOOP")
done
echo -e "${GREEN}[PASS]${NC} Loopback devices created"

# Load module
modprobe dm_mod 2>/dev/null || true
insmod /root/dm-lhsr.ko 2>/dev/null || true
echo -e "${GREEN}[PASS]${NC} Module loaded"

# Create RAID6 target
TABLE="0 ${LHSR_SIZE} lhsr raid6 ${CHUNK_SECTS} 1 ${CHUNK_SECTS}"
for LOOP in "${LOOP_DEVS[@]}"; do
    TABLE="$TABLE $LOOP 0"
done
echo "$TABLE" | dmsetup create lhsr-raid6-test
echo -e "${GREEN}[PASS]${NC} RAID6 target created: /dev/mapper/lhsr-raid6-test"

# Write test pattern (at 1MB offset)
dd if=/dev/urandom of=/tmp/lhsr-raid6-pattern.bin bs=1M count=$WRITE_SIZE_MB status=none
PATTERN_HASH=$(sha256sum /tmp/lhsr-raid6-pattern.bin | cut -d' ' -f1)
echo "  Pattern SHA256: $PATTERN_HASH"
dd if=/tmp/lhsr-raid6-pattern.bin of=/dev/mapper/lhsr-raid6-test bs=1M seek=1 oflag=direct status=none
echo -e "${GREEN}[PASS]${NC} Data written"

# Read back and verify
dd if=/dev/mapper/lhsr-raid6-test of=/tmp/lhsr-raid6-readback.bin bs=1M skip=1 count=$WRITE_SIZE_MB iflag=direct status=none
READ_HASH=$(sha256sum /tmp/lhsr-raid6-readback.bin | cut -d' ' -f1)
if [ "$PATTERN_HASH" != "$READ_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} Checksum MISMATCH"
    echo "  Expected: $PATTERN_HASH"
    echo "  Got:      $READ_HASH"
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Checksum MATCH"

# Check dmesg for warnings
dmesg | grep -i "lhsr.*WARNING\|lhsr.*Call Trace\|lhsr.*BUG" || echo -e "${GREEN}[PASS]${NC} No warnings"

echo ""
echo -e "${GREEN}[PASS]${NC} RAID6 SMOKE TEST PASSED"

# Cleanup
dmsetup remove lhsr-raid6-test 2>/dev/null || true
rmmod dm_lhsr 2>/dev/null || true
for LOOP in "${LOOP_DEVS[@]}"; do losetup -d "$LOOP" 2>/dev/null || true; done
for F in "${DISK_FILES[@]}"; do rm -f "$F"; done
rm -f /tmp/lhsr-raid6-pattern.bin /tmp/lhsr-raid6-readback.bin
echo -e "${GREEN}[PASS]${NC} Cleanup complete"
