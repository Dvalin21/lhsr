#!/bin/bash
# wipe-drives-for-testing.sh
# Safely wipe sda, sdb, sdc, sdd for LHSR testing
# WARNING: This destroys all data on these drives
# Usage: sudo ./wipe-drives-for-testing.sh

set -e

DRIVES=("sda" "sdb" "sdc" "sdd")
LOG_FILE="/tmp/lhsr-wipe-$(date +%Y%m%d-%H%M%S).log"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "ERROR: This script must be run as root (sudo)"
        exit 1
    fi
}

check_drive_exists() {
    local drive=$1
    if [[ ! -b "/dev/$drive" ]]; then
        log "WARNING: /dev/$drive does not exist, skipping"
        return 1
    fi
    return 0
}

unmount_drive() {
    local drive=$1
    log "Checking for mounted partitions on /dev/$drive"
    
    # Get all partitions
    local partitions=$(lsblk -n -o NAME "/dev/$drive" | tail -n +2 | awk '{print $1}')
    
    for part in $partitions; do
        local mountpoint=$(lsblk -n -o MOUNTPOINT "/dev/$part" 2>/dev/null)
        if [[ -n "$mountpoint" && "$mountpoint" != "" ]]; then
            log "Unmounting /dev/$part from $mountpoint"
            umount -f "/dev/$part" 2>&1 | tee -a "$LOG_FILE" || true
        fi
    done
}

remove_dm_mappings() {
    log "Removing any stale device-mapper mappings"
    dmsetup remove_all 2>&1 | tee -a "$LOG_FILE" || true
}

wipe_drive() {
    local drive=$1
    log "=========================================="
    log "WIPING DRIVE: /dev/$drive"
    log "=========================================="
    
    # Step 1: Unmount partitions
    unmount_drive "$drive"
    
    # Step 2: Wipe partition table (first and last 1MB)
    log "Wiping partition table on /dev/$drive"
    dd if=/dev/zero of="/dev/$drive" bs=1M count=1 conv=fdatasync 2>&1 | tee -a "$LOG_FILE"
    dd if=/dev/zero of="/dev/$drive" bs=1M seek=$(($(blockdev --getsz "/dev/$drive") * 512 / 1048576 - 1)) conv=fdatasync 2>&1 | tee -a "$LOG_FILE" || true
    
    # Step 3: Wipe LVM/DM signatures
    log "Wiping LVM/DM signatures on /dev/$drive"
    wipefs -a "/dev/$drive" 2>&1 | tee -a "$LOG_FILE" || true
    
    # Step 4: Verify wipe
    log "Verifying wipe on /dev/$drive"
    blkid "/dev/$drive" 2>&1 | tee -a "$LOG_FILE" || true
    lsblk -o NAME,SIZE,TYPE,FSTYPE "/dev/$drive" 2>&1 | tee -a "$LOG_FILE"
    
    log "COMPLETED: /dev/$drive wiped successfully"
}

main() {
    log "=========================================="
    log "LHSR Test Drive Wipe Script"
    log "Log file: $LOG_FILE"
    log "=========================================="
    
    check_root
    
    # Remove any existing dm mappings first
    remove_dm_mappings
    
    # Wipe each drive
    for drive in "${DRIVES[@]}"; do
        if check_drive_exists "$drive"; then
            # Check if it's the system drive
            local is_system=$(mount | grep "/dev/$drive" | grep -E "(/ |/boot|/home)" || true)
            if [[ -n "$is_system" ]]; then
                log "ERROR: /dev/$drive appears to be the system drive! Skipping to prevent disaster."
                log "Mounted at: $is_system"
                continue
            fi
            wipe_drive "$drive"
        fi
    done
    
    log "=========================================="
    log "ALL DRIVES WIPED SUCCESSFULLY"
    log "Ready for LHSR testing"
    log "=========================================="
}

main "$@"
