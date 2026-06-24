#!/bin/dash
#
# lhsr-create — create a new LHSR RAID array
#
# Initializes block devices with LHSR superblock metadata and creates
# a device-mapper target.  The kernel module writes superblocks
# automatically when the dmsetup table is loaded.
#
# Usage:
#   lhsr-create <type> <devices...>           # 4K chunks, auto-size
#   lhsr-create <type> --chunk-size <N> <devices...>  # custom chunk (sectors)
#   lhsr-create <type> --name <name> <devices...>     # custom DM name
#   lhsr-create --help
#
# RAID types:
#   single    1 disk, no redundancy
#   mirror    2 disks, mirror
#   raid5     3+ disks, single parity
#   raid6     4+ disks, double parity
#
# Examples:
#   lhsr-create raid5 /dev/sdb /dev/sdc /dev/sdd /dev/sde
#   lhsr-create raid6 --chunk-size 16 /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf
#   lhsr-create mirror --name mymirror /dev/sdb /dev/sdc
#
# Returns 0 on success, 1 on error.
# On success, prints "/dev/mapper/<name>" to stdout.

set -e

# Defaults
CHUNK_SECTS=8
STRIPES_PER_CONT=1
CONT_SECTS=8
NAME=""
VERBOSE=0

log()  { echo "lhsr-create: $*" >&2; }
dbg()  { [ "$VERBOSE" = 1 ] && echo "lhsr-create: $*" >&2; }
die()  { log "ERROR: $*"; exit 1; }

usage() {
    cat >&2 <<'EOF'
Usage: lhsr-create <type> [options] <device> [device...]

Create a new LHSR RAID array.

RAID types:
  single           1 disk, no redundancy
  mirror           2 disks, mirrored
  raid5            3+ disks, single distributed parity (reshapeable to raid6)
  raid6            4+ disks, double distributed parity

Options:
  --chunk-size N   Chunk size in sectors (default: 8 = 4 KiB)
  --name NAME      Device mapper name (default: auto-generated)
  --verbose        Show debug information
  --help           Show this help
EOF
    exit 0
}

# Parse arguments
TYPE=""
DEVICES=""
while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) usage ;;
        --verbose|-v) VERBOSE=1 ;;
        --chunk-size)
            shift; [ $# -lt 1 ] && die "--chunk-size requires a value"
            CHUNK_SECTS="$1"
            ;;
        --name)
            shift; [ $# -lt 1 ] && die "--name requires a value"
            NAME="$1"
            ;;
        single|mirror|raid5|raid6)
            [ -n "$TYPE" ] && die "Duplicate RAID type: $1"
            TYPE="$1"
            ;;
        -*)
            die "Unknown option: $1"
            ;;
        *)
            DEVICES="$DEVICES $1"
            ;;
    esac
    shift
done

# Validate
[ -z "$TYPE" ] && die "Missing RAID type. Use: single, mirror, raid5, or raid6"
DEVICES=$(echo "$DEVICES" | sed 's/^ *//;s/ *$//')
[ -z "$DEVICES" ] && die "No devices specified"

# Count devices
set -- $DEVICES
DISK_COUNT=$#

# Validate device count per type
case "$TYPE" in
    single) [ "$DISK_COUNT" -ne 1 ] && die "single requires exactly 1 disk, got $DISK_COUNT" ;;
    mirror) [ "$DISK_COUNT" -ne 2 ] && die "mirror requires exactly 2 disks, got $DISK_COUNT" ;;
    raid5)  [ "$DISK_COUNT" -lt 3 ] && die "raid5 requires at least 3 disks, got $DISK_COUNT" ;;
    raid6)  [ "$DISK_COUNT" -lt 4 ] && die "raid6 requires at least 4 disks, got $DISK_COUNT" ;;
esac

# Validate chunk size
case "$CHUNK_SECTS" in
    ''|*[!0-9]*) die "Invalid chunk size (must be positive integer): $CHUNK_SECTS" ;;
esac
[ "$CHUNK_SECTS" -lt 1 ] && die "Chunk size must be >= 1"

# ==============================================================
# Validate devices
# ==============================================================
for _dev in $DEVICES; do
    [ -b "$_dev" ] || die "Not a block device: $_dev"
    # Simple sanity: device must be writable
    dd if="$_dev" of=/dev/null bs=512 count=1 2>/dev/null || die "Cannot read $_dev"
done

# ==============================================================
# Determine smallest device size (in sectors) for array sizing
# ==============================================================
_MIN_SIZE=0
for _dev in $DEVICES; do
    _sz=$(blockdev --getsz "$_dev" 2>/dev/null) || die "Cannot get size of $_dev"
    if [ "$_MIN_SIZE" -eq 0 ] || [ "$_sz" -lt "$_MIN_SIZE" ]; then
        _MIN_SIZE=$_sz
    fi
done

# Account for metadata reservation (272 sectors base + WIB)
# Minimum usable device size needs at least metadata + 1 sector
_META_MIN=272
[ "$_MIN_SIZE" -le "$_META_MIN" ] && die "Device too small: $_MIN_SIZE sectors (need > $_META_MIN)"

# Usable sectors per disk = device size minus metadata reservation
# (kernel subtracts metadata in CTOR, so we compute the result)
_USABLE=$((_MIN_SIZE - _META_MIN))
# Sanity: WIB overhead ~1 page per ~32K sectors.  For large disks this is negligible.
# For our computation, the kernel does the exact WIB sizing.  We pass the raw min size
# and let the kernel figure it out.  BUT we need the resulting array size for the table.
#
# Actually, the kernel CTOR subtracts metadata before computing usable size.
# We pass the device size that matches all disks, and the kernel computes:
#   usable = device_sectors - v2_meta_sectors
# The table line is: 0 <usable*data_disks> lhsr raid5 <chunk> 1 8 <dev> 0 ...
# Where data_disks = disk_count - parity_count.
#
# Since the kernel does the subtraction, we need to account for it in the size param.
# The size param in the table line is the size AFTER metadata subtraction.
# For single/mirror: size = usable
# For RAID5/6: size = usable * data_disks

case "$TYPE" in
    single)
        _DATA_DISKS=1
        _PARITY=0
        _MIN_DISKS=1
        _DEV_ARG_FORMAT="device offset [device offset]..."
        ;;
    mirror)
        _DATA_DISKS=1
        _PARITY=0
        _MIN_DISKS=2
        _DEV_ARG_FORMAT="device offset device offset"
        ;;
    raid5)
        _DATA_DISKS=$((DISK_COUNT - 1))
        _PARITY=1
        _MIN_DISKS=3
        ;;
    raid6)
        _DATA_DISKS=$((DISK_COUNT - 2))
        _PARITY=2
        _MIN_DISKS=4
        ;;
esac

# The kernel metadata includes WIB which scales with device size.
# Compute a reasonable estimate.  The kernel's actual computation is:
#   wib_sectors = pages * 8, pages = ceil(usable / WIB_CHUNK_SECTORS / bits_per_page)
#   WIB_CHUNK_SECTORS = 2048, bits_per_page = 32672
#   => each wib page covers 2048 * 32672 = ~66M sectors = ~33 GB
#   For small test disks (64 MB = 131072 sectors): 131072 / 2048 = 64 bits -> 1 page -> 8 sectors
#   For large disks (4 TB = 7814037168 sectors): 7814037168 / 2048 = 3815450 bits -> 117 pages -> 936 sectors
#
# We need the array size for the DM table line.  The kernel computes it, but we need
# to provide it.  Let's be conservative and compute worst-case WIB overhead:
_WIB_PAGES=$(( (_MIN_SIZE / 2048 + 32672 - 1) / 32672 ))
[ "$_WIB_PAGES" -lt 1 ] && _WIB_PAGES=1
_WIB_SECTORS=$((_WIB_PAGES * 8))
_META_TOTAL=$((_META_MIN + _WIB_SECTORS))

[ "$_MIN_SIZE" -le "$_META_TOTAL" ] && die "Device too small for v2 metadata: $_MIN_SIZE <= $_META_TOTAL sectors"
_USABLE=$((_MIN_SIZE - _META_TOTAL))

# Compute virtual array size
case "$TYPE" in
    single|mirror)
        _ARRAY_SIZE=$_USABLE
        ;;
    raid5)
        _ARRAY_SIZE=$((_USABLE * _DATA_DISKS))
        ;;
    raid6)
        _ARRAY_SIZE=$((_USABLE * _DATA_DISKS))
        ;;
esac

dbg "Device raw size: $_MIN_SIZE sectors"
dbg "Estimated WIB: $_WIB_PAGES pages = $_WIB_SECTORS sectors"
dbg "Total metadata: $_META_TOTAL sectors"
dbg "Usable per disk: $_USABLE sectors"
dbg "Data disks: $_DATA_DISKS"
dbg "Array (virtual) size: $_ARRAY_SIZE sectors"

# ==============================================================
# Build and load the DM table
# ==============================================================
# Generate array name if not provided
if [ -z "$NAME" ]; then
    _RAND=$(od -An -N4 -tu4 /dev/urandom 2>/dev/null | tr -d ' ' | cut -c1-8)
    [ -z "$_RAND" ] && _RAND="$$"
    NAME="lhsr-${_RAND}"
fi

# Build device pairs
_DEV_PAIRS=""
for _dev in $DEVICES; do
    _DEV_PAIRS="$_DEV_PAIRS $_dev 0"
done

case "$TYPE" in
    single)
        TABLE="0 $_ARRAY_SIZE lhsr single$_DEV_PAIRS"
        ;;
    mirror)
        TABLE="0 $_ARRAY_SIZE lhsr mirror$_DEV_PAIRS"
        ;;
    raid5)
        TABLE="0 $_ARRAY_SIZE lhsr raid5 $CHUNK_SECTS $STRIPES_PER_CONT $CONT_SECTS$_DEV_PAIRS"
        ;;
    raid6)
        TABLE="0 $_ARRAY_SIZE lhsr raid6 $CHUNK_SECTS $STRIPES_PER_CONT $CONT_SECTS$_DEV_PAIRS"
        ;;
esac

dbg "DM table: echo '$TABLE' | dmsetup create $NAME"

if ! echo "$TABLE" | dmsetup create "$NAME" 2>&1; then
    die "dmsetup create failed"
fi

# ==============================================================
# Verify
# ==============================================================
if [ -b "/dev/mapper/$NAME" ]; then
    _SZ=$(blockdev --getsz "/dev/mapper/$NAME" 2>/dev/null || echo 0)
    log "Created: /dev/mapper/$NAME ($_SZ sectors, $TYPE, $DISK_COUNT disks)"
    echo "/dev/mapper/$NAME"
else
    die "Device /dev/mapper/$NAME not found after creation"
fi
