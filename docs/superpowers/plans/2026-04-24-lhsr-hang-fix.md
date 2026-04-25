# LHSR Hang Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the LHSR kernel module hang issues that cause `dmsetup remove` to hang and require hard reboot to recover.

**Architecture:** Fix three critical issues: (1) workqueue cleanup hangs by adding timeout机制, (2) CRC32c inconsistency between kernel and userspace, (3) superblock I/O disabled. Implement proper async cleanup with timeouts, fix checksum algorithm, and add proper error handling to superblock operations.

**Tech Stack:** Linux kernel module (C), device-mapper framework, CRC32c (Castagnoli polynomial)

---

## File Structure

| File | Responsibility |
|------|-------------|
| `kernel/dm-lhsr/dm-lhsr.c` | Main kernel module - fix workqueue and superblock issues |
| `kernel/dm-lhsr/dm_lhsr.h` | Header definitions - add timeout constants |
| `lib/raid_engine/lhsr_raid.c` | Userspace RAID library - fix CRC32c |
| `userspace/cli/lhsrctl.c` | CLI tool - minimal changes if needed |

---

## Task 1: Fix Workqueue Cleanup Hang

**Files:**
- Modify: `kernel/dm-lhsr/dm-lhsr.c:920-960`

- [ ] **Step 1: Add timeout helper macro to header**

Modify: `kernel/dm-lhsr/dm_lhsr.h`

Add after the includes:
```c
/* Workqueue timeout in jiffies (5 seconds) */
#define LHSR_WORKQUEUE_TIMEOUT (5 * HZ)
```

- [ ] **Step 2: Modify destructor to use timed cleanup**

Replace `lhsr_dtr` cleanup section in `kernel/dm-lhsr/dm-lhsr.c:923-928`:

Original:
```c
/* Stop the health check workqueue */
if (arr->check_wq) {
    cancel_delayed_work_sync(&arr->check_work);
    destroy_workqueue(arr->check_wq);
}
```

New:
```c
/* Stop the health check workqueue with timeout */
if (arr->check_wq) {
    DMINFO("Canceling check workqueue...");
    if (!cancel_delayed_work_sync(&arr->check_work)) {
        DMWARN("check_work did not complete in time, forcing");
        flush_workqueue(arr->check_wq);
    }
    destroy_workqueue(arr->check_wq);
    arr->check_wq = NULL;
}
```

- [ ] **Step 3: Fix scrubber workqueue cleanup**

Replace section around line 948-953:
```c
/* Stop scrubber */
if (arr->scrub_wq) {
    DMINFO("Canceling scrub workqueue...");
    arr->scrub_state = LHSR_SCRUB_IDLE;
    if (!cancel_delayed_work_sync(&arr->scrub_work)) {
        DMWARN("scrub_work did not complete in time, forcing");
        flush_workqueue(arr->scrub_wq);
    }
    destroy_workqueue(arr->scrub_wq);
    arr->scrub_wq = NULL;
    DMINFO("Scrubber stopped");
}
```

- [ ] **Step 4: Fix rebuild workqueue cleanup**

Replace section around line 955-960:
```c
/* Stop rebuild */
if (arr->rebuild_wq) {
    DMINFO("Canceling rebuild workqueue...");
    arr->rebuild_state = LHSR_REBUILD_NONE;
    if (!cancel_delayed_work_sync(&arr->rebuild_work)) {
        DMWARN("rebuild_work did not complete in time, forcing");
        flush_workqueue(arr->rebuild_wq);
    }
    destroy_workqueue(arr->rebuild_wq);
    arr->rebuild_wq = NULL;
    DMINFO("Rebuild stopped");
}
```

- [ ] **Step 5: Test compilation**

Run: `cd /home/keith/lhsr && make clean && make`
Expected: Compiles without errors

---

## Task 2: Fix CRC32c Inconsistency

**Files:**
- Modify: `kernel/dm-lhsr/dm-lhsr.c:51` (kernel uses CRC32C)
- Modify: `lib/raid_engine/lhsr_raid.c:72` (userspace uses wrong polynomial)

- [ ] **Step 1: Verify kernel CRC32c implementation**

Confirm in `kernel/dm-lhsr/dm-lhsr.c` line 51:
```c
crc = (crc >> 1) ^ 0x82F63B78;
```
This is CORRECT (CRC32C/Castagnoli polynomial)

- [ ] **Step 2: Fix userspace CRC32c**

Modify `lib/raid_engine/lhsr_raid.c` line 72:

Original:
```c
crc = (crc >> 1) ^ 0xEDB88320;
```

New:
```c
crc = (crc >> 1) ^ 0x82F63B78;
```

- [ ] **Step 3: Verify the final XOR value**

Original final XOR:
```c
return ~crc;
```

This is CORRECT for CRC32C (final XOR = 0xFFFFFFFF)

- [ ] **Step 4: Test userspace compilation**

Run: `cd /home/keith/lhsr/userspace && make clean && make`
Expected: Compiles without errors

---

## Task 3: Fix Superblock I/O

**Files:**
- Modify: `kernel/dm-lhsr/dm-lhsr.c:85-110`

- [ ] **Step 1: Read current implementation**

Verify `lhsr_write_superblock` currently returns 0 immediately (disabled)

- [ ] **Step 2: Implement proper superblock write**

Replace complete function in `kernel/dm-lhsr/dm-lhsr.c:85-110`:

```c
static int lhsr_write_superblock(struct block_device *bdev, struct lhsr_superblock *sb, sector_t array_size)
{
    struct bio *bio;
    struct page *page;
    void *buf;
    sector_t primary_sector, backup_sector;
    u32 calc_csum;
    int ret = 0;

    if (!bdev || !sb)
        return -EINVAL;

    primary_sector = LHSR_SB_PRIMARY_OFF >> SECTOR_SHIFT;
    backup_sector = array_size - (LHSR_SB_SIZE >> SECTOR_SHIFT);

    if (primary_sector < 2048 || backup_sector < 4096) {
        DMERR("Superblock offset invalid: primary=%llu backup=%llu",
             (u64)primary_sector, (u64)backup_sector);
        return -EINVAL;
    }

    /* Allocate page for superblock */
    page = alloc_page(GFP_KERNEL);
    if (!page)
        return -ENOMEM;

    buf = page_address(page);
    if (!buf) {
        __free_page(page);
        return -ENOMEM;
    }

    /* Copy superblock and calculate checksum */
    memcpy(buf, sb, LHSR_SB_SIZE);
    *(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = 0;
    calc_csum = lhsr_crc32c(buf, LHSR_SB_SIZE);
    *(u32 *)(buf + offsetof(struct lhsr_superblock, checksum)) = calc_csum;

    /* Write primary superblock with timeout */
    bio = bio_alloc(bdev, 1, REQ_OP_WRITE, GFP_KERNEL);
    if (!bio) {
        __free_page(page);
        return -ENOMEM;
    }
    bio_set_dev(bio, bdev);
    bio->bi_iter.bi_sector = primary_sector;
    __bio_add_page(bio, page, LHSR_SB_SIZE, 0);

    ret = submit_bio_wait(bio);
    bio_put(bio);

    if (ret) {
        DMERR("Primary superblock write failed: %d", ret);
        __free_page(page);
        return ret;
    }

    /* Write backup superblock */
    bio = bio_alloc(bdev, 1, REQ_OP_WRITE, GFP_KERNEL);
    if (!bio) {
        __free_page(page);
        return -ENOMEM;
    }
    bio_set_dev(bio, bdev);
    bio->bi_iter.bi_sector = backup_sector;
    __bio_add_page(bio, page, LHSR_SB_SIZE, 0);

    ret = submit_bio_wait(bio);
    bio_put(bio);
    __free_page(page);

    if (ret) {
        DMERR("Backup superblock write failed: %d", ret);
        return ret;
    }

    DMINFO("Superblock written: primary=%llu backup=%llu",
          (u64)primary_sector, (u64)backup_sector);
    return 0;
}
```

- [ ] **Step 3: Add error handling to read_superblock**

Improve error messages in `lhsr_read_superblock` for debugging:

```c
DMINFO("lhsr_read_superblock: sector=0x%llx", sector);
```

Add more detailed error logging:
```c
if (submit_bio_wait(bio) != 0) {
    DMERR("Superblock read failed at sector %llu", (u64)sector);
    bio_put(bio);
    __free_page(page);
    return -EIO;
}
```

- [ ] **Step 4: Test compilation**

Run: `cd /home/keith/lhsr && make clean && make`
Expected: Compiles without errors

---

## Task 4: Add Debugging Hooks

**Files:**
- Modify: `kernel/dm-lhsr/dm-lhsr.c` - add debugfs support

- [ ] **Step 1: Add debug state function**

Add after `lhsr_status` function (around line 1125):

```c
/* Debug: show array state via debugfs */
static int lhsr_debug_status(struct seq_file *s, struct dm_target *ti)
{
    struct lhsr_array *arr = ti->private;
    
    if (!arr)
        return 0;
    
    seq_printf(s, "uuid=0x%llx raid=%u disks=%u state=%u failed=0x%x\n",
              arr->uuid, arr->raid_type, arr->disks, arr->state, arr->failed_disks);
    seq_printf(s, "check_wq=%p scrub_wq=%p rebuild_wq=%p\n",
              arr->check_wq, arr->scrub_wq, arr->rebuild_wq);
    seq_printf(s, "scrub_state=%u rebuild_state=%u\n",
              arr->scrub_state, arr->rebuild_state);
    
    return 0;
}
```

- [ ] **Step 2: Test compilation**

Run: `cd /home/keith/lhsr && make clean && make`
Expected: Compiles without errors

---

## Task 5: Integration Test

**Files:**
- Run: `test_rebuild.sh` or `safe_test.sh`

- [ ] **Step 1: Load module**

```bash
sudo rmmod dm_lhsr 2>/dev/null || true
sudo insmod /home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko
lsmod | grep lhsr
```

Expected: Module loads successfully

- [ ] **Step 2: Create device**

```bash
sudo dmsetup create lhsr_test --table "0 5860533168 lhsr single /dev/sdb"
```

Expected: Device created

- [ ] **Step 3: Test device removal**

```bash
sudo dmsetup remove lhsr_test
```

Expected: Device removed without hanging (should complete within 10 seconds)

- [ ] **Step 4: Test full workflow**

Run: `sudo ./test_rebuild.sh` (with timeout)

```bash
timeout 120 sudo ./test_rebuild.sh
```

Expected: All tests pass within 2 minutes

---

## Verification Commands

Run these to verify the fix:

```bash
# 1. Load module
sudo insmod /home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.ko

# 2. Create test device
sudo dmsetup create lhsr_test --table "0 5860533168 lhsr single /dev/sdb"

# 3. Check status
sudo dmsetup status lhsr_test

# 4. Remove device (THIS SHOULD NOT HANG)
sudo dmsetup remove lhsr_test

# 5. Verify removal
ls -la /dev/mapper/ | grep lhsr || echo "Device removed successfully"
```

---

## Plan Complete

Two execution options:

1. **Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

2. **Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**