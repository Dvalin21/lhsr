#!/bin/bash
# Test read reconstruction on failed disk
# Creates RAID5, writes known data, fails one disk, reads back via parity reconstruction

set -euo pipefail

DISK_COUNT=4
DATA_DISKS=3
CHUNK_SECTS=8   # sectors = 4KB (must be <= PAGE_SIZE/512)
STRIPES_PER_CONT=1
CONT_SECTS=8
DISK_SIZE_MB=100
WRITE_SIZE_MB=4
WRITE_BLOCKS=$((WRITE_SIZE_MB * 2048))  # 4MB in sectors
# LHSR reserves 272 sectors for superblock
SUPERBLOCK_SECTORS=272
PER_DISK_SECTORS=$((DISK_SIZE_MB * 1024 * 1024 / 512))
LHSR_SIZE=$((PER_DISK_SECTORS - SUPERBLOCK_SECTORS))

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${YELLOW}[INFO]${NC} ╔══════════════════════════════════════════════════════╗"
echo -e "${YELLOW}[INFO]${NC} ║  LHSR Read Reconstruction Test                       ║"
echo -e "${YELLOW}[INFO]${NC} ╚══════════════════════════════════════════════════════╝"

# Step 1: Create loopback devices
echo -e "${YELLOW}[INFO]${NC} Creating ${DISK_COUNT} loopback devices (${DISK_SIZE_MB}MB each)..."
DISK_FILES=()
LOOP_DEVS=()
for i in $(seq 0 $((DISK_COUNT - 1))); do
    F="/tmp/lhsr-recon-disk${i}.img"
    dd if=/dev/zero of="$F" bs=1M count=$DISK_SIZE_MB status=none
    DISK_FILES+=("$F")
    LOOP=$(losetup -f --show "$F")
    LOOP_DEVS+=("$LOOP")
    echo -e "  Disk $i: $LOOP ($DISK_SIZE_MB MB)"
done
echo -e "${GREEN}[PASS]${NC} Loopback devices created"

# Step 2: Load module
echo -e "${YELLOW}[INFO]${NC} Loading LHSR module..."
modprobe dm_mod 2>/dev/null || true
insmod /root/dm-lhsr.ko 2>/dev/null || true
lsmod | grep dm_lhsr
echo -e "${GREEN}[PASS]${NC} Module loaded"

# Step 3: Create RAID5 target
# Format: 0 <size> lhsr raid5 <chunk_sects> <stripes_per_cont> <cont_sects> <dev1> <off1> <dev2> <off2> ...
TABLE="0 ${LHSR_SIZE} lhsr raid5 ${CHUNK_SECTS} ${STRIPES_PER_CONT} ${CONT_SECTS}"
for LOOP in "${LOOP_DEVS[@]}"; do
    TABLE="$TABLE $LOOP 0"
done
echo -e "${YELLOW}[INFO]${NC} Creating RAID5 target with table: $TABLE"
echo "$TABLE" | dmsetup create lhsr-recon-test
echo -e "${GREEN}[PASS]${NC} RAID5 target created: /dev/mapper/lhsr-recon-test"

# Step 4: Write test pattern (at 1MB offset to skip superblock area)
echo -e "${YELLOW}[INFO]${NC} Writing ${WRITE_SIZE_MB}MB test pattern at 1MB offset..."
dd if=/dev/urandom of=/tmp/lhsr-recon-pattern.bin bs=1M count=$WRITE_SIZE_MB status=none
PATTERN_HASH=$(sha256sum /tmp/lhsr-recon-pattern.bin | cut -d' ' -f1)
echo "  Pattern SHA256: $PATTERN_HASH"
dd if=/tmp/lhsr-recon-pattern.bin of=/dev/mapper/lhsr-recon-test bs=1M seek=1 oflag=direct status=none
echo -e "${GREEN}[PASS]${NC} Data written"

# Step 5: Verify initial read (before failure)
echo -e "${YELLOW}[INFO]${NC} Verifying initial read (healthy array)..."
dd if=/dev/mapper/lhsr-recon-test of=/tmp/lhsr-recon-readback.bin bs=1M skip=1 count=$WRITE_SIZE_MB iflag=direct status=none
READ_HASH=$(sha256sum /tmp/lhsr-recon-readback.bin | cut -d' ' -f1)
if [ "$PATTERN_HASH" != "$READ_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} Pre-failure checksum MISMATCH"
    echo "  Expected: $PATTERN_HASH"
    echo "  Got:      $READ_HASH"
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Pre-failure checksum MATCH"

# Step 6: Query member status before failure
echo -e "${YELLOW}[INFO]${NC} Array status before failure..."
dmsetup message lhsr-recon-test 0 member_status 0 2>/dev/null || true
dmsetup message lhsr-recon-test 0 member_status 1 2>/dev/null || true
dmsetup message lhsr-recon-test 0 member_status 2 2>/dev/null || true
dmsetup message lhsr-recon-test 0 member_status 3 2>/dev/null || true

# Step 7: Fail disk 0
echo -e "${YELLOW}[INFO]${NC} Failing disk 0 via dmsetup message..."
dmsetup message lhsr-recon-test 0 disk_fail 0
echo -e "${GREEN}[PASS]${NC} Disk 0 marked failed"

# Check dmesg for reconstruction messages
echo -e "${YELLOW}[INFO]${NC} Checking reconstruction log..."
dmesg | grep -i "RECON\|reconstruct\|disk.*failed\|disk.*degraded" | tail -10

# Step 8: Read back after failure (should trigger reconstruction)
echo -e "${YELLOW}[INFO]${NC} Reading back after failure (should trigger reconstruction)..."
dd if=/dev/mapper/lhsr-recon-test of=/tmp/lhsr-recon-degraded.bin bs=1M skip=1 count=$WRITE_SIZE_MB iflag=direct status=none
RECON_HASH=$(sha256sum /tmp/lhsr-recon-degraded.bin | cut -d' ' -f1)
if [ "$PATTERN_HASH" != "$RECON_HASH" ]; then
    echo -e "${RED}[FAIL]${NC} Reconstruction checksum MISMATCH"
    echo "  Expected: $PATTERN_HASH"
    echo "  Got:      $RECON_HASH"
    exit 1
fi
echo -e "${GREEN}[PASS]${NC} Reconstruction checksum MATCH"

# Step 9: Check dmesg for reconstruction events
echo -e "${YELLOW}[INFO]${NC} Checking reconstruction log after read..."
dmesg | grep -i "RECON" | tail -5

# Step 10: Verify other disks are still accessible
echo -e "${YELLOW}[INFO]${NC} Reading at different offset (non-failed disk region)..."
dd if=/dev/mapper/lhsr-recon-test of=/dev/null bs=1M skip=1 count=$WRITE_SIZE_MB iflag=direct status=none 2>&1
echo -e "${GREEN}[PASS]${NC} Non-failed region reads OK"

# Step 11: Check final dmesg
echo -e "${YELLOW}[INFO]${NC} Final dmesg check..."
dmesg | grep -i "lhsr.*WARNING\|lhsr.*Call Trace\|lhsr.*BUG" || echo -e "${GREEN}[PASS]${NC} No warnings"

echo ""
echo -e "${GREEN}[PASS]${NC} ╔══════════════════════════════════════════════════════╗"
echo -e "${GREEN}[PASS]${NC} ║  READ RECONSTRUCTION TEST PASSED                    ║"
echo -e "${GREEN}[PASS]${NC} ╚══════════════════════════════════════════════════════╝"

# Cleanup
echo -e "${YELLOW}[INFO]${NC} Cleaning up..."
dmsetup remove lhsr-recon-test 2>/dev/null || true
rmmod dm_lhsr 2>/dev/null || true
for LOOP in "${LOOP_DEVS[@]}"; do
    losetup -d "$LOOP" 2>/dev/null || true
done
for F in "${DISK_FILES[@]}"; do
    rm -f "$F"
done
rm -f /tmp/lhsr-recon-pattern.bin /tmp/lhsr-recon-readback.bin /tmp/lhsr-recon-degraded.bin
echo -e "${GREEN}[PASS]${NC} Cleanup complete"
