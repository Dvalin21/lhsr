#!/bin/dash
#
# lhsr-reshape — in-place RAID5→RAID6 migration via kernel reshape
#
# Adds a new disk for Q (double) parity to an existing RAID5 array
# without destroying the array or requiring data backup/restore.
# Parity data is computed incrementally by the kernel module.
#
# WARNING: Writes to the array are REJECTED during reshape.
#   - Remount any filesystem read-only before starting
#   - Reads continue to work normally
#   - Reshape is crash-safe: resumes from where it left off
#
# Usage:
#   lhsr-reshape <device> add <new-device>
#   lhsr-reshape <device> status
#   lhsr-reshape --help
#
# Examples:
#   lhsr-reshape /dev/mapper/lhsr-abc add /dev/sdf
#   lhsr-reshape /dev/mapper/lhsr-abc status
#
# Returns 0 on success, 1 on error or user abort.

set -e

VERBOSE=0

log()  { echo "lhsr-reshape: $*" >&2; }
dbg()  { [ "$VERBOSE" = 1 ] && echo "lhsr-reshape: $*" >&2; }
die()  { log "ERROR: $*"; exit 1; }
warn() { log "WARNING: $*"; }
info() { log "$*"; }

usage() {
    cat >&2 <<'EOF'
Usage: lhsr-reshape <device> <command> [args]

In-place RAID5 to RAID6 migration (add Q parity disk).

Commands:
  add <new-device>    Start reshape: add Q parity disk to RAID5 array
  status              Query reshape progress

Arguments:
  device              Path to LHSR RAID5 device (/dev/mapper/<name>)
  new-device          Path to block device for Q parity (must be unused)

Options:
  --verbose           Show debug information
  --no-warn           Skip initial warnings
  --help              Show this help

Examples:
  lhsr-reshape /dev/mapper/lhsr-abc add /dev/sdf
  lhsr-reshape /dev/mapper/lhsr-abc status

Requirements:
  • Array must be RAID5 (not already RAID6 or other type)
  • All disks must be healthy (no degraded array)
  • Chunk size must be <= PAGE_SIZE (typically 4 KB = 8 sectors)
  • New device must be at least as large as existing disks
  • Write filesystem(s) read-only before starting reshape
  • Array must have room for one more disk (< 32 total)
EOF
    exit 0
}

# ==============================================================
# Parse arguments
# ==============================================================
DEVICE=""
CMD=""
NEW_DEV=""
NO_WARN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) usage ;;
        --verbose|-v) VERBOSE=1 ;;
        --no-warn) NO_WARN=1 ;;
        add)
            [ -z "$DEVICE" ] && { shift; continue; }
            CMD="add"
            ;;
        status)
            [ -z "$DEVICE" ] && { shift; continue; }
            CMD="status"
            ;;
        /dev/*)
            if [ -z "$DEVICE" ]; then
                DEVICE="$1"
            elif [ -z "$NEW_DEV" ]; then
                NEW_DEV="$1"
            else
                die "Unexpected argument: $1"
            fi
            ;;
        *)
            die "Unknown option: $1"
            ;;
    esac
    shift
done

[ -z "$DEVICE" ] && die "Missing device path. See --help."
[ -z "$CMD" ] && die "Missing command (add|status). See --help."

# ==============================================================
# Validate device
# ==============================================================
[ -b "$DEVICE" ] || die "Device not found: $DEVICE"

_DEV_NAME=$(basename "$DEVICE")
_DEV_TABLE=$(dmsetup table "$_DEV_NAME" 2>/dev/null || true)
if [ -z "$_DEV_TABLE" ]; then
    die "Cannot get DM table for $_DEV_NAME. Is it a device-mapper device?"
fi
echo "$_DEV_TABLE" | grep -q "lhsr" || \
    die "$DEVICE is not an LHSR device (missing 'lhsr' in table)"

# Detect RAID type from table
_RAID_TYPE=$(echo "$_DEV_TABLE" | awk '{print $4}')
_DEV_SIZE=$(echo "$_DEV_TABLE" | awk '{print $2}')
_DEV_CHUNK=$(echo "$_DEV_TABLE" | awk '{print $5}')

# ==============================================================
# status command
# ==============================================================
if [ "$CMD" = "status" ]; then
    _STATUS=$(dmsetup message "$_DEV_NAME" 0 "reshape" 2>/dev/null || true)
    if [ -z "$_STATUS" ]; then
        # Older kernel or reshape not supported
        _KMOD_MSG=$(dmsetup message "$_DEV_NAME" 0 "reshape status" 2>/dev/null || true)
        if [ -z "$_KMOD_MSG" ]; then
            die "Failed to query reshape status. Is the kernel module loaded?"
        fi
        _STATUS="$_KMOD_MSG"
    fi

    echo "$_STATUS"

    # Parse and show summary
    _STATE=$(echo "$_STATUS" | sed -n 's/.*state=\([^ ]*\).*/\1/p')
    _PCT=$(echo "$_STATUS" | sed -n 's/.*progress=\([0-9]*\)%.*/\1/p')
    _PROC=$(echo "$_STATUS" | sed -n 's/.*(\([0-9]*\)\/\([0-9]*\) sectors).*/\1 \2/p')
    _TGT=$(echo "$_STATUS" | sed -n 's/.*target=\(.*\)/\1/p')

    case "$_STATE" in
        RUNNING)
            info "Reshape in progress: ${_PCT}% complete"
            info "  $([ -n "$_PROC" ] && echo "$_PROC sectors processed")"
            info "  Target: $_TGT"
            # Estimate remaining time is not useful without timing info
            ;;
        COMPLETE)
            info "Reshape is complete. Array is now RAID6."
            info "Run 'dmsetup table $_DEV_NAME' to verify."
            ;;
        FAILED)
            warn "Reshape failed. Check dmesg for details."
            warn "Array is in an indeterminate state."
            ;;
        NONE|PENDING)
            info "No reshape in progress on $_DEV_NAME."
            info "  Array type: $_RAID_TYPE"
            info "  Size: $_DEV_SIZE sectors"
            info "  To start: lhsr-reshape $DEVICE add <new-device>"
            ;;
    esac
    exit 0
fi

# ==============================================================
# add command
# ==============================================================
# Must have RAID5
if [ "$_RAID_TYPE" != "raid5" ]; then
    die "Array is '$_RAID_TYPE', not RAID5. Reshape only supports RAID5→RAID6."
fi

[ -z "$NEW_DEV" ] && die "Missing new device path for 'add' command"
[ -b "$NEW_DEV" ] || die "New device not found: $NEW_DEV"

# ==============================================================
# Safety checks
# ==============================================================

# 1. New device not already part of an LHSR array
_NEW_DEV_NAME=$(basename "$NEW_DEV")
if dmsetup table 2>/dev/null | grep -q "$_NEW_DEV_NAME"; then
    die "New device $NEW_DEV appears to be in use by a DM device"
fi

# 2. Check new device isn't already mounted
_mounted=0
for _mp in $(mount | awk '{print $1}'); do
    if [ "$_mp" = "$NEW_DEV" ]; then
        _mounted=1
        break
    fi
done
if [ "$_mounted" -eq 1 ]; then
    die "New device $NEW_DEV is currently mounted. Unmount first."
fi

# 3. Check the source array isn't already reshaping
_CHECK=$(dmsetup message "$_DEV_NAME" 0 "reshape" 2>/dev/null || true)
case "$_CHECK" in
    *state=RU*|*state=PE*)
        die "Reshape is already in progress on $_DEV_NAME."
        ;;
    *state=CO*)
        die "Reshape already completed on $_DEV_NAME. Array is already RAID6."
        ;;
    *state=FA*)
        warn "Previous reshape failed. Check dmesg before retrying."
        if [ "$FORCE" != 1 ]; then
            printf "Continue anyway? [y/N] " >&2
            read _ANS
            case "$_ANS" in y|Y|yes|YES) ;; *) die "Aborted" ;; esac
        fi
        ;;
esac

# 4. Verify the filesystem isn't mounted rw on source device
_mounted_rw=0
while IFS= read -r _line; do
    _mp_dev=$(echo "$_line" | awk '{print $1}')
    _mp_opts=$(echo "$_line" | awk '{print $4}')
    if [ "$_mp_dev" = "$DEVICE" ] && echo "$_mp_opts" | grep -q "rw"; then
        _mounted_rw=1
    fi
done <<EOF
$(mount 2>/dev/null || true)
EOF

if [ "$_mounted_rw" -eq 1 ]; then
    die "Source device $DEVICE is mounted read-write. \
Remount read-only first: mount -o remount,ro $DEVICE"
fi

# ==============================================================
# Warn user
# ==============================================================
if [ "$NO_WARN" != 1 ] && [ "$FORCE" != 1 ]; then
    echo ""
    echo "╔══════════════════════════════════════════════════════════╗"
    echo "║  LHSR RAID5 → RAID6 Reshape                            ║"
    echo "╠══════════════════════════════════════════════════════════╣"
    echo "║  Device:   $DEVICE"
    echo "║  Adding:   $NEW_DEV (Q parity disk)"
    echo "║  Chunk:    $_DEV_CHUNK sectors"
    echo "║  Size:     $_DEV_SIZE sectors"
    echo "╚══════════════════════════════════════════════════════════╝"
    echo ""
    echo "IMPORTANT:"
    echo "  • Writes to the array are REJECTED during reshape"
    echo "  • Reads continue to work normally"
    echo "  • Reshape is crash-safe (resumes if interrupted)"
    echo "  • A new superblock will be written to $NEW_DEV"
    echo "  • After completion, the array is permanently RAID6"
    echo ""
    printf "Start reshape? [y/N] " >&2
    read _ANS
    case "$_ANS" in
        y|Y|yes|YES) ;;
        *) die "Reshape aborted by user" ;;
    esac
fi

# ==============================================================
# Start reshape via dmsetup message
# ==============================================================
info "Starting RAID5→RAID6 reshape, adding $NEW_DEV ..."

_OUT=$(dmsetup message "$_DEV_NAME" 0 "reshape raid6 add $NEW_DEV" 2>&1) || {
    _RC=$?
    echo "$_OUT" >&2
    die "Failed to start reshape (exit $_RC)"
}

echo "$_OUT"

# ==============================================================
# Monitor progress
# ==============================================================
info "Reshape started. Monitoring progress (Ctrl+C to stop monitoring)..."
echo ""

_PREV_PCT=""
_SAME_COUNT=0
while true; do
    _STATUS=$(dmsetup message "$_DEV_NAME" 0 "reshape" 2>/dev/null || echo "failed")
    _STATE=$(echo "$_STATUS" | sed -n 's/.*state=\([^ ]*\).*/\1/p')
    _PCT=$(echo "$_STATUS" | sed -n 's/.*progress=\([0-9]*\)%.*/\1/p')

    case "$_STATE" in
        COMPLETE)
            echo ""
            info "═══ RESHAPE COMPLETE ═══"
            info "Array $_DEV_NAME is now RAID6 (${_PCT}%)"
            echo ""
            info "Superblocks updated on all disks."
            info "New disk $NEW_DEV is now the Q parity disk."
            echo ""
            info "To verify:"
            info "  dmsetup table $_DEV_NAME"
            info "  dmsetup status $_DEV_NAME"
            exit 0
            ;;
        FAILED)
            echo ""
            die "Reshape FAILED. Check 'dmesg' for kernel error details."
            ;;
        RUNNING)
            if [ "$_PCT" != "$_PREV_PCT" ]; then
                printf "\r  Progress: %3d%% (%s)" "$_PCT" "$_STATUS"
                _PREV_PCT=$_PCT
                _SAME_COUNT=0
            else
                _SAME_COUNT=$((_SAME_COUNT + 1))
                if [ "$_SAME_COUNT" -gt 20 ]; then
                    # Might be stalled
                    printf "\r  Progress: %3d%% (stalled? check dmesg)  " "$_PCT"
                fi
            fi
            sleep 2
            ;;
        *)
            printf "\r  Reshape state: %s " "$_STATE"
            sleep 2
            ;;
    esac
done
