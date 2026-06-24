#!/bin/dash
#
# lhsr-migrate — migrate an LHSR array to a different RAID level
#
# WARNING: This script performs OFFLINE migration.  Because different
# RAID levels use different stripe layouts (virtual → physical mapping),
# a block-level copy between old and new arrays does NOT preserve data.
# Data MUST be backed up and restored at the filesystem level.
#
# Migration paths supported:
#   single → raid5 (add 2+ disks, gain redundancy)
#   single → mirror (change to 2-disk mirror, not stripe)
#   raid5  → raid6 (add 1+ disks, gain double parity)
#
# NOTE: For RAID5→RAID6, consider lhsr-reshape(8) instead.
#   lhsr-reshape adds a Q parity disk IN-PLACE without destroying the
#   array — no backup, no downtime beyond the reshape window.
#   Only use lhsr-migrate for raid5→raid6 when the array needs to
#   change size or the chunk layout.
#   See: lhsr-reshape --help
#
# Usage:
#   lhsr-migrate <old-dev> <new-raid-type> <new-disks...> [--backup <path>]
#
# The script walks through:
#   1. Validates source array health
#   2. Creates backup (optional, can use existing backup)
#   3. Destroys old array
#   4. Creates new array with lhsr-create
#   5. Guides restore
#
# Example:
#   lhsr-migrate /dev/mapper/lhsr-old raid6 /dev/sdb /dev/sdc /dev/sdd /dev/sde /dev/sdf
#
# Returns 0 on success, 1 on error or user abort.

set -e

NAME=""
BACKUP_PATH=""
SKIP_BACKUP=0
FORCE=0

log()  { echo "lhsr-migrate: $*" >&2; }
die()  { log "ERROR: $*"; exit 1; }
warn() { log "WARNING: $*"; }
info() { log "$*"; }

usage() {
    cat >&2 <<'EOF'
Usage: lhsr-migrate <source-device> <new-type> <new-disks...> [options]

Migrate an LHSR array to a different RAID level.

WARNING: This is an OFFLINE migration.  You need:
  1. A backup of the filesystem on the source array
  2. Enough spare disks for the new array
  3. The new array will be EMPTY after migration

Arguments:
  source-device   Path to existing LHSR device (/dev/mapper/<name>)
  new-type        Target RAID type: raid5, raid6, or mirror
  new-disks       Block devices for the new array (must be >= requirement)

Options:
  --backup DIR    Backup filesystem to this directory before migration
  --no-backup     Skip backup (use when backup already exists)
  --name NAME     Name for the new array (default: auto-generated)
  --force         Skip confirmation prompts
  --help          Show this help

RAID requirements:
  ✓ single → raid5: need at least 3 disks (source disk + 2+ new)
  ✓ single → mirror: need exactly 2 disks
  ✓ raid5 → raid6: need at least 4 disks (existing + 1+ new)
  ✓ raid5 → raid5: nop (no change)
  ✓ raid6 → raid6: nop (no change)

NOTE: For RAID5→RAID6, consider lhsr-reshape(8) instead.
  It migrates IN-PLACE — no backup/restore needed.
EOF
    exit 0
}

# Parse args
SRC_DEV=""
NEW_TYPE=""
NEW_DISKS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) usage ;;
        --backup) shift; BACKUP_PATH="$1" ;;
        --no-backup) SKIP_BACKUP=1 ;;
        --name) shift; NAME="$1" ;;
        --force) FORCE=1 ;;
        single|mirror|raid5|raid6)
            [ -z "$SRC_DEV" ] && SRC_DEV="$1" && shift && continue
            NEW_TYPE="$1"
            ;;
        *)
            if [ -z "$SRC_DEV" ]; then
                SRC_DEV="$1"
            else
                NEW_DISKS="$NEW_DISKS $1"
            fi
            ;;
    esac
    shift
done

# Trim whitespace
NEW_DISKS=$(echo "$NEW_DISKS" | sed 's/^ *//;s/ *$//')
[ -z "$SRC_DEV" ] && die "Missing source device"
[ -z "$NEW_TYPE" ] && die "Missing target RAID type"
[ -z "$NEW_DISKS" ] && die "Missing target devices"

# ==============================================================
# Validate source array
# ==============================================================
[ -b "$SRC_DEV" ] || die "Source device not found: $SRC_DEV"

# Check if it's an LHSR DM device
_SRC_NAME=$(basename "$SRC_DEV")
_SRC_TABLE=$(dmsetup table "$_SRC_NAME" 2>/dev/null || true)
if [ -z "$_SRC_TABLE" ]; then
    die "Cannot get DM table for $_SRC_NAME. Is it a device-mapper device?"
fi
echo "$_SRC_TABLE" | grep -q "lhsr" || die "$SRC_DEV is not an LHSR device (missing 'lhsr' in table)"

# Detect source RAID type from table
_SRC_RAID=$(echo "$_SRC_TABLE" | awk '{print $4}')
case "$_SRC_RAID" in
    single) SRC_TYPE="single";;
    mirror) SRC_TYPE="mirror";;
    raid5)  SRC_TYPE="raid5";;
    raid6)  SRC_TYPE="raid6";;
    *) die "Unknown source RAID type: $_SRC_RAID";;
esac

_SRC_SIZE=$(echo "$_SRC_TABLE" | awk '{print $2}')
info "Source array: $_SRC_NAME ($SRC_TYPE, $_SRC_SIZE sectors)"

# Check if migration is meaningful
[ "$SRC_TYPE" = "$NEW_TYPE" ] && die "Source and target RAID types are the same ($SRC_TYPE). Nothing to migrate."

# Validate migration path
case "$SRC_TYPE:$NEW_TYPE" in
    single:raid5|single:mirror|raid5:raid6)
        # Valid path
        ;;
    *)
        die "Migration from $SRC_TYPE to $NEW_TYPE is not supported. Valid paths: single→raid5, single→mirror, raid5→raid6"
        ;;
esac

# Count new disks
set -- $NEW_DISKS
NEW_COUNT=$#
_TARGET_DATA=0
_TARGET_PARITY=0

case "$NEW_TYPE" in
    mirror)
        [ "$NEW_COUNT" -ne 2 ] && die "mirror requires exactly 2 disks, got $NEW_COUNT"
        _TARGET_PARITY=0; _TARGET_DATA=1
        ;;
    raid5)
        [ "$NEW_COUNT" -lt 3 ] && die "raid5 requires at least 3 disks, got $NEW_COUNT"
        _TARGET_PARITY=1; _TARGET_DATA=$((NEW_COUNT - 1))
        ;;
    raid6)
        [ "$NEW_COUNT" -lt 4 ] && die "raid6 requires at least 4 disks, got $NEW_COUNT"
        _TARGET_PARITY=2; _TARGET_DATA=$((NEW_COUNT - 2))
        ;;
esac

# Validate new disks
for _dev in $NEW_DISKS; do
    [ -b "$_dev" ] || die "Not a block device: $_dev"
done

# ==============================================================
# Confirm with user
# ==============================================================
info "╔══════════════════════════════════════════════════════════╗"
info "║  LHSR RAID Migration                                    ║"
info "╠══════════════════════════════════════════════════════════╣"
info "║  Source: $SRC_TYPE ($_SRC_NAME)"
info "║  Target: $NEW_TYPE ($NEW_COUNT disks)"
info "║  New devices: $NEW_DISKS"
info "╚══════════════════════════════════════════════════════════╝"
info ""
info "IMPORTANT: This migration destroys the source array."
info "Data must be backed up and restored to the new array."
info ""
info "Steps:"
info "  1. Source array is removed (dmsetup remove)"
info "  2. New array is created with target RAID type"
info "  3. The new array starts with a clean superblock (no data)"
info "  4. YOU must restore your data from backup"

if [ "$FORCE" != 1 ]; then
    echo ""
    printf "Proceed with migration? [y/N] " >&2
    read _ANS
    case "$_ANS" in
        y|Y|yes|YES) ;;
        *) die "Migration aborted by user" ;;
    esac
fi

# ==============================================================
# Step 1: Backup (optional)
# ==============================================================
if [ -n "$BACKUP_PATH" ]; then
    info "Creating backup at $BACKUP_PATH ..."
    mkdir -p "$BACKUP_PATH" || die "Cannot create backup directory: $BACKUP_PATH"
    _BSIZE=$((_SRC_SIZE / 2 * 512))  # rough size in bytes
    _FREE=$(df -P "$BACKUP_PATH" | tail -1 | awk '{print $4}')
    _FREE_B=$((_FREE * 1024))
    if [ "$_FREE_B" -lt "$_BSIZE" ]; then
        die "Insufficient space for backup. Need ~${_BSIZE} bytes, have ${_FREE_B} at $BACKUP_PATH"
    fi
    info "Running: dd if=$SRC_DEV of=$BACKUP_PATH/lhsr-backup.img bs=1M"
    dd if="$SRC_DEV" of="$BACKUP_PATH/lhsr-backup.img" bs=1M status=progress
    info "Backup created: $BACKUP_PATH/lhsr-backup.img ($(stat -c%s "$BACKUP_PATH/lhsr-backup.img" 2>/dev/null) bytes)"
elif [ "$SKIP_BACKUP" != 1 ]; then
    warn "No backup specified.  If source data is valuable, use --backup <dir>."
    if [ "$FORCE" != 1 ]; then
        printf "Continue without backup? [y/N] " >&2
        read _ANS
        case "$_ANS" in
            y|Y|yes|YES) ;;
            *) die "Migration aborted by user" ;;
        esac
    fi
fi

# ==============================================================
# Step 2: Destroy source array
# ==============================================================
info "Removing source array $_SRC_NAME ..."
if ! dmsetup remove "$_SRC_NAME" 2>&1; then
    die "Failed to remove $_SRC_NAME. Is it still in use? (umount first)"
fi
info "Source array removed."

# ==============================================================
# Step 3: Create new array
# ==============================================================
_CREATE_ARGS="--chunk-size 8 --verbose"
[ -n "$NAME" ] && _CREATE_ARGS="$_CREATE_ARGS --name $NAME"

# Build device list
_DEV_ARGS=""
for _d in $NEW_DISKS; do
    _DEV_ARGS="$_DEV_ARGS $_d"
done

info "Creating new $NEW_TYPE array with $NEW_COUNT disks..."
_NEW_DEV=$(/usr/lib/lhsr/lhsr-create.sh "$NEW_TYPE" $_CREATE_ARGS $_DEV_ARGS 2>&1)
_CREATE_RC=$?

if [ "$_CREATE_RC" -ne 0 ] || [ -z "$_NEW_DEV" ]; then
    die "Failed to create new array.  Source array $_SRC_NAME has been removed and data may need recovery."
fi

_NEW_NAME=$(basename "$_NEW_DEV")
info "New array created: $_NEW_DEV"

# ==============================================================
# Step 4: Print restore instructions
# ==============================================================
echo ""
echo "============================================================"
echo "  MIGRATION COMPLETE"
echo "============================================================"
echo ""
echo "  Source array removed:  $_SRC_NAME ($SRC_TYPE)"
echo "  New array created:     $_NEW_DEV ($NEW_TYPE)"
echo ""
if [ -n "$BACKUP_PATH" ]; then
    echo "  To restore data from backup:"
    echo "    dd if=$BACKUP_PATH/lhsr-backup.img of=$_NEW_DEV bs=1M status=progress"
    echo ""
    echo "  Then verify the filesystem:"
    echo "    fsck $_NEW_DEV"
fi
echo "  New array size: $_NEW_DEV ($(blockdev --getsz $_NEW_DEV 2>/dev/null || echo '?') sectors)"
echo ""
echo "============================================================"

exit 0
