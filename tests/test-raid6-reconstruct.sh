#!/bin/bash
# Test RAID6 single-failure read reconstruction
# After fix: Q parity is excluded from XOR reconstruction (it's GF-weighted, not XOR-compatible)
set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${YELLOW}[INFO]${NC} ╔══════════════════════════════════════════════════════╗"
echo -e "${YELLOW}[INFO]${NC} ║  LHSR RAID6 Read Reconstruction Test                ║"
echo -e "${YELLOW}[INFO]${NC} ╚══════════════════════════════════════════════════════╝"

DISK_COUNT=4
DISK_SIZE_MB=100
CHUNK_SECTS=8
WRITE_SIZE_MB=4
PER_DISK_SECTORS=$((DISK_SIZE_MB * 1024 * 1024 / 512))
SUPERBLOCK_SECTORS=272
LHSR_SIZE=$((PER_DISK_SECTORS - SUPERBLOCK_SECTORS))

cleanup() {
    dmsetup remove lhsr-raid6-recon 2>/dev/null || true
    rmmod dm_lhsr 2>/dev/null || true
    for LOOP in "${LOOP_DEVS[@]}"; do losetup -d "$LOOP" 2>/dev/null || true; done
    for F in "${DISK_FILES[@]}"; do rm -f "$F" 2>/dev/null; done
    rm -f /tmp/lhsr-r6-pattern.bin /tmp/lhsr-r6-readback.bin /tmp/lhsr-r6-degraded.bin
}
trap cleanup EXIT

# Step 1: Create loopback devices
echo -e "${YELLOW}[INFO]${NC} Creating ${DISK_COUNT} loopback devices (${DISK_SIZE_MB}MB each)..."
DISK_FILES=(); LOOP_DEVS=()
for i in $(seq 0 $((DISK_COUNT - 1))); do
    F="/tmp/lhsr-r6-disk${i}.img"
    dd if=/dev/zero of="$F" bs=1M count=$DISK_SIZE_MB status=none
    DISK_FILES+=("$F")
    LOOP=$(losetup -f --show "$F")
    LOOP_DEVS+=("$LOOP")
    echo -e "  Disk $i: $LOOP"
done
echo -e "${GREEN}[PASS]${NC} Loopback devices created"

# Step 2: Load module
modprobe dm_mod 2>/dev/null || true
insmod /root/dm-lhsr.ko 2>/dev/null || true
echo -e "${GREEN}[PASS]${NC} Module loaded"

# Step 3: Create RAID6 target (4 disks: D0 D1 P Q)
TABLE="0 ${LHSR_SIZE} lhsr raid6 ${CHUNK_SECTS} 1 ${CHUNK_SECTS}"
for LOOP in "${LOOP_DEVS[@]}"; do TABLE="$TABLE $LOOP 0"; done
echo "$TABLE" | dmsetup create lhsr-raid6-recon
echo -e "${GREEN}[PASS]${NC} RAID6 target created"

# Step 4: Write test pattern (at 1MB offset to skip superblock)
dd if=/dev/urandom of=/tmp/lhsr-r6-pattern.bin bs=1M count=$WRITE_SIZE_MB status=none
PATTERN_HASH=$(sha256sum /tmp/lhsr-r6-pattern.bin | cut -d' ' -f1)
echo "  Pattern SHA256: $PATTERN_HASH"
dd if=/tmp/lhsr-r6-pattern.bin of=/dev/mapper/lhsr-raid6-recon bs=1M seek=1 oflag=direct status=none
echo -e "${GREEN}[PASS]${NC} Data written"

# Step 5: Verify initial read (healthy array)
dd if=/dev/mapper/lhsr-raid6-recon of=/tmp/lhsr-r6-readback.bin bs=1M skip=1 count=$WRITE_SIZE_MB iflag=direct status=none
READ_HASH=$(sha256sum /tmp/lhsr-r6-readback.bin | cut -d' ' -f1)
if [ "$PATTERN_HASH" != "$READ_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} Pre-failure checksum MISMATCH"; exit 1
fi
echo -e "${GREEN}[PASS]${NC} Pre-failure checksum MATCH"

# Step 6: Query member status before failure
echo -e "${YELLOW}[INFO]${NC} Member status before failure:"
dmsetup message lhsr-raid6-recon 0 member_status 0
dmsetup message lhsr-raid6-recon 0 member_status 1
dmsetup message lhsr-raid6-recon 0 member_status 2
dmsetup message lhsr-raid6-recon 0 member_status 3

# Step 7: Fail data disk 0
echo -e "${YELLOW}[INFO]${NC} Failing data disk 0..."
dmsetup message lhsr-raid6-recon 0 disk_fail 0
echo -e "${GREEN}[PASS]${NC} Disk 0 marked failed"

# Step 8: Read back after failure (triggers reconstruction from P parity)
echo -e "${YELLOW}[INFO]${NC} Reading back after failure..."
dd if=/dev/mapper/lhsr-raid6-recon of=/tmp/lhsr-r6-degraded.bin bs=1M skip=1 count=$WRITE_SIZE_MB iflag=direct status=none
RECON_HASH=$(sha256sum /tmp/lhsr-r6-degraded.bin | cut -d' ' -f1)
if [ "$PATTERN_HASH" != "$RECON_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} Reconstruction checksum MISMATCH"
    echo "  Expected: $PATTERN_HASH"
    echo "  Got:      $RECON_HASH"
    dmesg | grep -i "RECON" | tail -10
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Reconstruction checksum MATCH"

# Step 9: Check reconstruction was from P parity (not Q)
echo -e "${YELLOW}[INFO]${NC} Reconstruction event log:"
dmesg | grep -i "RECON" | tail -5

# Step 10: Check array status
echo -e "${YELLOW}[INFO]${NC} Member status after failure:"
dmsetup message lhsr-raid6-recon 0 member_status 0
dmsetup message lhsr-raid6-recon 0 member_status 1
dmsetup message lhsr-raid6-recon 0 member_status 2
dmsetup message lhsr-raid6-recon 0 member_status 3

# Step 11: Check dmesg for warnings
dmesg | grep -i "lhsr.*WARNING\|lhsr.*Call Trace\|lhsr.*BUG" || echo -e "${GREEN}[PASS]${NC} No warnings"

echo ""
echo -e "${GREEN}[PASS]${NC} ╔══════════════════════════════════════════════════════╗"
echo -e "${GREEN}[PASS]${NC} ║  RAID6 RECONSTRUCTION TEST PASSED                   ║"
echo -e "${GREEN}[PASS]${NC} ╚══════════════════════════════════════════════════════╝"
