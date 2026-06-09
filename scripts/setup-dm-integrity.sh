#!/bin/bash
# setup-dm-integrity.sh - Create or tear down dm-integrity devices for LHSR
#
# Creates a dm-integrity target on top of each raw device, providing
# persistent per-block CRC32c checksums.  LHSR can then be stacked on
# top with the "integrity" flag, delegating integrity verification to
# the block layer.
#
# Usage:
#   sudo ./setup-dm-integrity.sh create /dev/sdb [/dev/sdc ...]
#   sudo ./setup-dm-integrity.sh remove  [prefix...]
#   sudo ./setup-dm-integrity.sh status [prefix...]
#
# Examples:
#   # Create integrity devices for 4 drives
#   sudo ./setup-dm-integrity.sh create /dev/sdb /dev/sdc /dev/sdd /dev/sde
#
#   # Create only for sdb and sdc
#   sudo ./setup-dm-integrity.sh create /dev/sdb /dev/sdc
#
#   # Show status of all integrity-* devices
#   sudo ./setup-dm-integrity.sh status
#
#   # Remove all integrity-* devices
#   sudo ./setup-dm-integrity.sh remove
#
# Output: prints the dm device names (e.g., integrity-sdb, integrity-sdc)
# suitable for constructing LHSR table lines:
#   dmsetup create lhsr-array --table \
#     "0 <size> lhsr raid5 8 <stripe_size> \
#      /dev/mapper/integrity-sdb 0 \
#      /dev/mapper/integrity-sdc 0 \
#      integrity"

set -euo pipefail

LOG_FILE="/tmp/lhsr-dm-integrity-$(date +%Y%m%d-%H%M%S).log"

log() {
    echo "[$(date '+%H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

die() {
    echo "ERROR: $1" >&2
    exit 1
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        die "This script must be run as root (sudo)"
    fi
}

check_deps() {
    if ! command -v integritysetup &>/dev/null; then
        die "integritysetup not found. Install cryptsetup (>= 2.4): sudo apt install cryptsetup"
    fi
    if ! command -v dmsetup &>/dev/null; then
        die "dmsetup not found. Install dmsetup: sudo apt install dmsetup"
    fi
}

# Derive a short device name from a full path like /dev/sdb -> sdb
dev_shortname() {
    local dev="$1"
    basename "$(readlink -f "$dev")" 2>/dev/null || basename "$dev"
}

create_integrity() {
    local raw_dev="$1"
    local short
    local dm_name
    local size

    short=$(dev_shortname "$raw_dev")
    dm_name="integrity-${short}"

    # Check if already exists
    if dmsetup info "$dm_name" &>/dev/null; then
        log "SKIP: $dm_name already exists"
        echo "/dev/mapper/$dm_name"
        return 0
    fi

    # Validate raw device exists
    if [[ ! -b "$raw_dev" ]]; then
        log "ERROR: $raw_dev is not a block device"
        return 1
    fi

    # Check if any partition on this device is mounted
    local mounts
    mounts=$(lsblk -n -o MOUNTPOINT "$raw_dev" 2>/dev/null | grep -v '^$' || true)
    if [[ -n "$mounts" ]]; then
        log "ERROR: $raw_dev has mounted partitions: $mounts"
        return 1
    fi

    # Get device size in sectors (512-byte)
    size=$(blockdev --getsz "$raw_dev")
    if [[ -z "$size" || "$size" -eq 0 ]]; then
        log "ERROR: Cannot get size of $raw_dev"
        return 1
    fi

    log "Creating $dm_name ($raw_dev, ${size} sectors)"

    # NOTE: We MUST use integritysetup (not raw dmsetup) here.
    #
    # Raw dmsetup create with the integrity target FAILS with
    # "Invalid tag size" because the kernel dm-integrity target
    # requires the block_size and tag_size to be set correctly
    # in the on-disk superblock, which integritysetup format handles.
    #
    # integritysetup is part of cryptsetup (>= 2.4) and is available
    # on all modern distros (Debian 12+, Ubuntu 22.04+).
    #
    # Two-step process:
    #   1. integritysetup format: initializes the superblock and
    #      writes initial integrity tags (CRC32c of existing data).
    #   2. integritysetup open: activates the dm-integrity device.

    # Step 1: Format the device (initializes integrity metadata)
    if ! integritysetup format "$raw_dev" --integrity crc32c --tag-size 4 \
        --block-size 4096 --no-wipe 2>&1 | tee -a "$LOG_FILE"; then
        log "ERROR: integritysetup format failed for $raw_dev"
        return 1
    fi
    log "Format OK"

    # Step 2: Open the device (activate dm-integrity)
    if ! integritysetup open "$raw_dev" "$dm_name" 2>&1 | tee -a "$LOG_FILE"; then
        log "ERROR: integritysetup open failed for $raw_dev -> $dm_name"
        return 1
    fi
    log "Open OK"

    # Verify creation
    if ! dmsetup info "$dm_name" &>/dev/null; then
        log "ERROR: $dm_name not found after creation"
        return 1
    fi

    # Wait for the device node to appear
    udevadm settle 2>/dev/null || true
    sleep 0.5

    local dm_dev="/dev/mapper/$dm_name"
    if [[ ! -b "$dm_dev" ]]; then
        log "WARNING: $dm_dev not found after settle, continuing anyway"
    fi

    log "Created $dm_dev"
    echo "$dm_dev"
}

remove_integrity() {
    local filter="${1:-}"

    # Use integritysetup close for proper teardown.
    # integritysetup close uses DM device remove, the same as dmsetup remove,
    # but handles the metadata sync properly.
    local count=0

    # List all integrity-* devices
    local devs
    if [[ -n "$filter" ]]; then
        devs=$(dmsetup ls --target integrity 2>/dev/null | grep "$filter" | awk '{print $1}')
    else
        devs=$(dmsetup ls --target integrity 2>/dev/null | awk '{print $1}')
    fi

    if [[ -z "$devs" ]]; then
        log "No dm-integrity devices found to remove"
        return 0
    fi

    for dev in $devs; do
        log "Closing $dev via integritysetup"
        integritysetup close "$dev" 2>&1 | tee -a "$LOG_FILE" || \
            dmsetup remove "$dev" 2>&1 | tee -a "$LOG_FILE" || true
        count=$((count + 1))
    done

    log "Closed $count integrity device(s)"
    udevadm settle 2>/dev/null || true
}

status_integrity() {
    local filter="${1:-}"

    if [[ -n "$filter" ]]; then
        dmsetup ls --target integrity 2>/dev/null | grep "$filter" || \
            log "No integrity devices matching '$filter'"
    else
        local devs
        devs=$(dmsetup ls --target integrity 2>/dev/null | awk '{print $1}')
        if [[ -z "$devs" ]]; then
            log "No dm-integrity devices found"
            return 0
        fi
        echo ""
        echo "dm-integrity devices:"
        echo "====================="
        for dev in $devs; do
            local status_raw
            status_raw=$(dmsetup status "$dev" 2>/dev/null || true)
            echo "  $dev: $status_raw"
        done
        echo ""
    fi
}

show_usage() {
    cat <<'EOF'
Usage: setup-dm-integrity.sh <command> [args...]

Commands:
    create <devices...>    Create dm-integrity targets on raw block devices
    remove  [prefix]       Remove dm-integrity devices (all, or matching prefix)
    status  [prefix]       Show dm-integrity device status
    help                   Show this help message

Examples:
    sudo ./setup-dm-integrity.sh create /dev/sdb /dev/sdc /dev/sdd
    sudo ./setup-dm-integrity.sh remove
    sudo ./setup-dm-integrity.sh status
EOF
    exit 0
}

main() {
    local cmd="${1:-help}"

    check_root

    case "$cmd" in
        help|--help|-h)
            show_usage
            ;;
    esac

    # All commands except help need dependencies
    check_deps

    case "$cmd" in
        create)
            shift
            if [[ $# -lt 1 ]]; then
                die "Usage: $0 create <devices...>"
            fi
            echo ""
            log "Creating dm-integrity devices..."
            log "=========================================="
            local created=()
            for dev in "$@"; do
                if result=$(create_integrity "$dev"); then
                    created+=("$result")
                else
                    log "FAILED to create integrity device for $dev"
                fi
            done
            log "=========================================="
            log "Created ${#created[@]} dm-integrity device(s)"
            echo ""
            if [[ ${#created[@]} -gt 0 ]]; then
                echo "Use these devices for LHSR:"
                echo "---------------------------"
                for d in "${created[@]}"; do
                    echo "  $d"
                done
                echo ""
                echo "LHSR table example:"
                echo "  dmsetup create lhsr-array --table \\"
                echo "    \"0 <size> lhsr raid5 ${#created[@]} 8 "
                for d in "${created[@]}"; do
                    echo "                      $d 0 \\"
                done
                echo "                      integrity\""
            fi
            echo ""
            ;;
        remove)
            shift
            remove_integrity "${1:-}"
            ;;
        status)
            shift
            status_integrity "${1:-}"
            ;;
        *)
            die "Unknown command: $cmd. Use 'help' for usage."
            ;;
    esac
}

main "$@"
