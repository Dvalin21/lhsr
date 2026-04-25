#!/bin/bash
# LHSR Test Commands - Run after reboot

# 1. Load dm-mod first
sudo modprobe dm-mod

# 2. Load our fixed module
sudo insmod /home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko

# 3. Test device create
sudo dmsetup create lhsr_test --table "0 5860533168 lhsr single /dev/sdb"

# 4. Test device remove  
sudo dmsetup remove lhsr_test
echo "Exit code: $?"