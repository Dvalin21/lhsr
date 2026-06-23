#!/bin/dash
#
# lhsr-assemble — boot-time LHSR array autodiscovery and assembly
#
# Scans block devices for LHSR superblocks, groups them by array_uuid,
# validates disk count, and calls dmsetup create to assemble each array.
#
# Designed to run from initramfs (busybox dash + dd + hexdump) or
# from a running system.
#
# Usage:
#   lhsr-assemble              # assemble all discovered arrays
#   lhsr-assemble --status     # show scanned devices and arrays
#   lhsr-assemble /dev/sdX     # assemble just the array containing sdX
#   lhsr-assemble --no-act     # dry run: show what would be assembled
#
# Returns 0 if all arrays assembled, 1 on error.

set -e

# LHSR superblock constants (from include/lhsr.h)
LHSR_MAGIC="LHSRDISK"
LHSR_SB_SIZE=128
LHSR_SB_SECTORS=16
LHSR_BITMAP_TOTAL_SECTORS=256
LHSR_META_BASE_SECTORS=$((LHSR_BITMAP_TOTAL_SECTORS + LHSR_SB_SECTORS))  # 272
LHSR_SB_OFFSET_SECTORS=$LHSR_BITMAP_TOTAL_SECTORS  # bitmap lives before superblock

LHSR_RAID_SINGLE=0
LHSR_RAID_MIRROR=1
LHSR_RAID5=2
LHSR_RAID6=3

LHSR_DISK_HEALTHY=0
LHSR_DISK_DEGRADED=1
LHSR_DISK_FAILED=2

# Superblock field offsets (packed struct)
OFF_MAGIC=0          # 8 bytes
OFF_VERSION=8        # 4 bytes
OFF_ARRAY_UUID=12    # 8 bytes
OFF_DISK_INDEX=44    # 4 bytes
OFF_DISK_STATE=48    # 4 bytes
OFF_RAID_TYPE=52     # 4 bytes
OFF_DISK_COUNT=56    # 4 bytes
OFF_TOTAL_SECTORS=60 # 8 bytes
OFF_GENERATION=68    # 8 bytes
OFF_FLAGS=80         # 4 bytes

VERBOSE=0
NO_ACT=0
STATUS_ONLY=0

# Simple logging to kernel printk (runs during initramfs boot)
log()    { echo "lhsr-assemble: $*" >&2; }
dbg()    { [ "$VERBOSE" = 1 ] && echo "lhsr-assemble: $*" >&2; }

# Read hex value from a binary blob at given offset with given byte count.
# Uses hexdump if available, fallback to od.
read_uint32() {
    _blob="$1"
    _offset=$2
    # 4 little-endian bytes starting at offset
    _byte0=$(dd if="$_blob" bs=1 skip=$_offset count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    _byte1=$(dd if="$_blob" bs=1 skip=$((_offset + 1)) count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    _byte2=$(dd if="$_blob" bs=1 skip=$((_offset + 2)) count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    _byte3=$(dd if="$_blob" bs=1 skip=$((_offset + 3)) count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    echo $((_byte0 | (_byte1 << 8) | (_byte2 << 16) | (_byte3 << 24)))
}

read_uint64() {
    _blob="$1"
    _offset=$2
    _lo=$(read_uint32 "$_blob" $_offset)
    _hi=$(read_uint32 "$_blob" $((_offset + 4)))
    echo $((_lo | (_hi << 32)))
}

read_bytes() {
    _blob="$1"
    _offset=$2
    _count=$3
    dd if="$_blob" bs=1 skip=$_offset count=$_count 2>/dev/null | tr -d '\0'
}

# Read superblock from a block device.
# Returns 0 and sets SB_* variables if valid LHSR superblock found.
read_superblock() {
    _dev="$1"
    _tmp="/tmp/lhsr-sb-$$"

    # Get device size in sectors
    _size_sects=$(blockdev --getsz "$_dev" 2>/dev/null) || return 1
    [ "$_size_sects" -le "$LHSR_META_BASE_SECTORS" ] && return 1

    # Metadata starts at _size_sects - LHSR_META_BASE_SECTORS
    _meta_start=$((_size_sects - LHSR_META_BASE_SECTORS))
    _sb_sector=$((_meta_start + LHSR_SB_OFFSET_SECTORS))

    # Read the superblock (128 bytes at sector boundary)
    dd if="$_dev" of="$_tmp" bs=512 skip=$_sb_sector count=1 2>/dev/null || { rm -f "$_tmp"; return 1; }

    # Read filesize
    _sb_filesize=$(stat -c%s "$_tmp" 2>/dev/null || echo 0)
    [ "$_sb_filesize" -lt "$LHSR_SB_SIZE" ] && { rm -f "$_tmp"; return 1; }

    # Check magic
    _magic=$(read_bytes "$_tmp" 0 8)
    [ "$_magic" != "$LHSR_MAGIC" ] && { rm -f "$_tmp"; return 1; }

    # Check version
    _version=$(read_uint32 "$_tmp" $OFF_VERSION)
    [ "$_version" -lt 1 ] && { rm -f "$_tmp"; return 1; }

    # Parse fields
    SB_VERSION=$_version
    SB_ARRAY_UUID=$(read_uint64 "$_tmp" $OFF_ARRAY_UUID)
    SB_DISK_INDEX=$(read_uint32 "$_tmp" $OFF_DISK_INDEX)
    SB_DISK_STATE=$(read_uint32 "$_tmp" $OFF_DISK_STATE)
    SB_RAID_TYPE=$(read_uint32 "$_tmp" $OFF_RAID_TYPE)
    SB_DISK_COUNT=$(read_uint32 "$_tmp" $OFF_DISK_COUNT)
    SB_TOTAL_SECTORS=$(read_uint64 "$_tmp" $OFF_TOTAL_SECTORS)

    rm -f "$_tmp"

    [ "$SB_DISK_COUNT" -eq 0 ] && return 1
    [ "$SB_TOTAL_SECTORS" -eq 0 ] && return 1

    return 0
}

raid_type_name() {
    case "$1" in
        0) echo "single" ;;
        1) echo "mirror" ;;
        2) echo "raid5" ;;
        3) echo "raid6" ;;
        *) echo "raid$1" ;;
    esac
}

disk_state_name() {
    case "$1" in
        0) echo "healthy" ;;
        1) echo "degraded" ;;
        2) echo "failed" ;;
        3) echo "rebuilding" ;;
        *) echo "state$1" ;;
    esac
}

# Find all block devices that might be LHSR members
find_candidate_devices() {
    # Walk /dev for block devices. Filter out partitions of devices we already have,
    # RAM disks, loop devices without backing, and md/dm devices.
    for _dev in /dev/sd?[a-z] /dev/sd??[a-z] /dev/nvme*n1 /dev/nvme*n[0-9]p1 /dev/vd? /dev/xvd? /dev/mmcblk? /dev/mmcblk?p1; do
        [ -b "$_dev" ] || continue
        echo "$_dev"
    done 2>/dev/null | sort -u
}

# Build dmsetup table line for an array
build_table() {
    _uuid="$1"
    _devices="$2"    # space-separated device paths
    _disk_count="$3"
    _raid_type="$4"
    _total_sects="$5"
    _chunk_sects=8   # default 4K chunks; could read from superblock in future

    _table_devs=""
    _disk_idx=0
    for _d in $_devices; do
        _table_devs="$_table_devs $_d 0"
        _disk_idx=$((_disk_idx + 1))
    done

    case "$_raid_type" in
        $LHSR_RAID_SINGLE)
            echo "0 $_total_sects lhsr single"
            ;;
        $LHSR_RAID_MIRROR)
            echo "0 $_total_sects lhsr mirror $_disk_count"
            ;;
        $LHSR_RAID5)
            echo "0 $_total_sects lhsr raid5 $_chunk_sects 1 $_disk_count"
            ;;
        $LHSR_RAID6)
            echo "0 $_total_sects lhsr raid6 $_chunk_sects 1 $_disk_count"
            ;;
        *)
            log "ERROR: unsupported raid type $_raid_type"
            return 1
            ;;
    esac

    echo  # separator between table header and device list
    _disk_idx=0
    for _d in $_devices; do
        echo "  $_d 0"
        _disk_idx=$((_disk_idx + 1))
    done
}

# ==============================================================
# Main
# ==============================================================

# Parse args
for _arg in "$@"; do
    case "$_arg" in
        --verbose|-v)  VERBOSE=1 ;;
        --no-act|-n)   NO_ACT=1 ;;
        --status|-s)   STATUS_ONLY=1 ;;
        --help|-h)
            echo "Usage: $0 [--verbose] [--no-act] [--status] [device...]"
            echo "  Scans for LHSR superblocks and assembles arrays."
            echo "  --no-act   dry run"
            echo "  --status   show what would be assembled"
            exit 0
            ;;
        *)
            # Could be a specific device path
            [ -b "$_arg" ] && USER_DEVS="$USER_DEVS $_arg"
            ;;
    esac
done

# Discover candidate block devices
if [ -n "$USER_DEVS" ]; then
    CANDIDATES="$USER_DEVS"
else
    CANDIDATES=$(find_candidate_devices)
fi

if [ -z "$CANDIDATES" ]; then
    log "No block devices found to scan."
    exit 1
fi

# Scan each device for superblock
log "Scanning for LHSR arrays..."
SCANNED_DEVS=""
ARRAY_DEVS=""   # uuid=dev1,idx1,state1:dev2,idx2,state2:...

for _dev in $CANDIDATES; do
    if read_superblock "$_dev" 2>/dev/null; then
        SCANNED_DEVS="$_dev ($(raid_type_name $SB_RAID_TYPE) disk $SB_DISK_INDEX)"
        dbg "Found LHSR on $_dev: array_uuid=$SB_ARRAY_UUID disk=$SB_DISK_INDEX state=$(disk_state_name $SB_DISK_STATE)"

        # Check for failed disks
        if [ "$SB_DISK_STATE" = "$LHSR_DISK_FAILED" ]; then
            log "WARNING: $_dev is in FAILED state"
        fi

        # Append to the array group
        _found=0
        _new_entry=""
        _new_arrays=""
        IFS=':'
        for _entry in $ARRAY_DEVS; do
            _array_uuid=$(echo "$_entry" | cut -d= -f1)
            if [ "$_array_uuid" = "$SB_ARRAY_UUID" ]; then
                _found=1
                _new_entry="${_entry},${_dev},${SB_DISK_INDEX},${SB_DISK_STATE}"
            else
                [ -n "$_new_arrays" ] && _new_arrays="${_new_arrays}:"
                _new_arrays="${_new_arrays}${_entry}"
            fi
        done
        IFS=' '
        if [ "$_found" = 1 ]; then
            ARRAY_DEVS="$_new_arrays:$_new_entry"
        else
            [ -n "$ARRAY_DEVS" ] && ARRAY_DEVS="${ARRAY_DEVS}:"
            ARRAY_DEVS="${ARRAY_DEVS}${SB_ARRAY_UUID}=${_dev},${SB_DISK_INDEX},${SB_DISK_STATE}"
        fi
    fi
done

if [ -z "$ARRAY_DEVS" ]; then
    log "No LHSR arrays found."
    [ "$STATUS_ONLY" = 1 ] && echo "Status: no LHSR arrays found on any device."
    exit 0
fi

if [ "$STATUS_ONLY" = 1 ]; then
    echo "=== LHSR Array Status ==="
    IFS=':'
    for _entry in $ARRAY_DEVS; do
        _uuid=$(echo "$_entry" | cut -d= -f1)
        _devs=$(echo "$_entry" | cut -d= -f2-)
        echo "Array UUID: $_uuid"
        IFS=','
        _idx=0
        for _field in $_devs; do
            case $((_idx % 3)) in
                0) _dev=$_field ;;
                1) _disk_idx=$_field ;;
                2) _disk_state=$_field
                   echo "  Disk $_disk_idx: $_dev ($(disk_state_name $_disk_state))" ;;
            esac
            _idx=$((_idx + 1))
        done
        IFS=':'
        echo ""
    done
    IFS=' '
    exit 0
fi

# Assemble each array
_RET=0
IFS=':'
for _entry in $ARRAY_DEVS; do
    _uuid=$(echo "$_entry" | cut -d= -f1)
    _devs_raw=$(echo "$_entry" | cut -d= -f2-)

    # Re-read one superblock for array params
    _first_dev=$(echo "$_devs_raw" | cut -d, -f1)
    if ! read_superblock "$_first_dev" 2>/dev/null; then
        log "ERROR: Cannot read superblock from $_first_dev"
        _RET=1
        continue
    fi

    _dev_count=$SB_DISK_COUNT
    _raid_type=$SB_RAID_TYPE
    _total_sects=$SB_TOTAL_SECTORS

    # Sort devices by disk_index
    _sorted_devs=""
    _idx=0
    _devices_seen="" _indices_seen="" _states_seen=""
    IFS=','
    for _field in $_devs_raw; do
        case $((_idx % 3)) in
            0) _dev=$_field ;;
            1) _di=$_field
               _ss=$(echo "$_states_seen" | tr ' ' '\n' | grep -c "^${_di}$" 2>/dev/null || echo 0) ;;
            2) _ds=$_field
               # Store by index
               _devices_seen="$_devices_seen $_dev"
               _indices_seen="$_indices_seen $_di"
               _states_seen="$_states_seen $_ds" ;;
        esac
        _idx=$((_idx + 1))
    done

    # Build sorted device list
    _sorted=""
    for _need_idx in $(seq 0 $((_dev_count - 1))); do
        _found=""
        _idx=0
        for _di in $_indices_seen; do
            if [ "$_di" = "$_need_idx" ]; then
                _found=1
                _d=$(echo "$_devices_seen" | cut -d' ' -f$((_idx + 1)))
                _sorted="$_sorted $_d"
                break
            fi
            _idx=$((_idx + 1))
        done
        if [ -z "$_found" ]; then
            log "WARNING: Array $_uuid is missing disk $_need_idx (degraded mode)"
            # Insert a dash placeholder for missing disks
            _sorted="$_sorted -"
        fi
    done

    # Check for failed disks
    _has_failed=0
    for _ds in $_states_seen; do
        [ "$_ds" = "$LHSR_DISK_FAILED" ] && _has_failed=1
    done

    _name="lhsr-$(echo "$_uuid" | cut -c1-8)"

    if [ "$NO_ACT" = 1 ]; then
        log "[NO-ACT] Would assemble $_name: $(raid_type_name $_raid_type) with$_sorted"
        continue
    fi

    # Build table fragments
    case $_raid_type in
        $LHSR_RAID_SINGLE)
            _type="single"
            _params=""
            ;;
        $LHSR_RAID_MIRROR)
            _type="mirror"
            _params="$_dev_count"
            ;;
        $LHSR_RAID5)
            _type="raid5"
            _params="8 1 $_dev_count"
            ;;
        $LHSR_RAID6)
            _type="raid6"
            _params="8 1 $_dev_count"
            ;;
        *)
            log "ERROR: Unsupported raid type $_raid_type on $_name"
            _RET=1
            continue
            ;;
    esac

    TABLE="0 $_total_sects lhsr $_type $_params"
    for _d in $_sorted; do
        [ "$_d" = "-" ] && continue  # skip missing disks
        TABLE="$TABLE $_d 0"
    done

    log "Assembling $_name ($(raid_type_name $_raid_type), $_dev_count disks)"
    if echo "$TABLE" | dmsetup create "$_name" 2>&1; then
        log "SUCCESS: /dev/mapper/$_name ready"
    else
        log "ERROR: Failed to assemble $_name"
        _RET=1
    fi
done
IFS=' '

exit $_RET
