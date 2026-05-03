# LHSR RAID5/6 Read Reconstruction Design

**Date:** 2026-05-02  
**Status:** Approved  
**Author:** keith (LHSR Team)

## Overview

Add read reconstruction for RAID5/6 arrays to handle:
1. **Degraded mode:** Reading from a disk that's already marked as failed
2. **On-the-fly reconstruction:** Detecting read errors and reconstructing data using parity

## Industry Standard Compliance

Based on research of Linux MD RAID5/6 (`drivers/md/raid5.c`):
- MD uses `raid5_end_read_request()` for ALL reads
- On error: sets `R5_ReadError` flag → `handle_stripe()` state machine picks it up
- Reconstruction happens via state machine, not pre-emptive handlers

**LHSR adapted design (simpler than MD, follows same principles):**
- Normal reads use `lhsr_raid_5_read_endio` (lightweight handler)
- On error: reconstruct using parity
- No overhead for working disks beyond normal completion

## Architecture

### Current State (Broken)
```
RAID5/6 READ path (lines 1625-1644 in dm-lhsr.c):
  └─→ Read from FIRST working disk only
       └─→ If read fails → error out (NO reconstruction)
```

### New Design
```
lhsr_map() for RAID5/6 READ (modifying lines 1625-1644):

  └─→ DETECT if target disk (first working) has failed
       │
       ├─→ If target disk FAILED (degraded mode):
       │    └─→ ALLOCATE lhsr_raid_5_read_ctx
       │         Submit clone reads to ALL working disks + parity
       │         bi_end_io = lhsr_raid_5_read_endio
       │         return DM_MAPIO_SUBMITTED
       │
       ├─→ If target disk WORKING:
       │    └─→ Submit NORMAL read with bi_end_io = lhsr_raid_5_read_endio
       │         (on error, lhsr_raid_5_read_endio will start reconstruction)
       │
       └─→ If NO working disks: error out (existing behavior)

lhsr_raid_5_read_endio():
  └─→ if (bio->bi_status == 0) {
           └─→ Complete orig_bio normally (read succeeded)
      }
      └─→ else {
           └─→ START RECONSTRUCTION:
                Allocate ctx (if not already done)
                Submit reads to working disks + parity
                Set bi_end_io = lhsr_raid_5_read_endio (for reconstruction reads)
                /* On reconstruction completion, copy data to orig_bio and complete */
      }
```

## Data Flow for Reconstruction

```
On read error (in lhsr_raid_5_read_endio):

1. Allocate lhsr_raid_5_read_ctx (if not already allocated)
2. Determine working disks (arr->failed_disks bitmap)
3. Allocate data_bufs array for working disks
4. For EACH working disk:
   └─→ Clone orig_bio
   └─→ Set bi_end_io = lhsr_raid_5_read_endio (reuse same handler!)
   └─→ Submit read
   └─→ atomic_inc(&ctx->pending)

5. Also submit parity read(s):
   └─→ RAID5: read P parity
   └─→ RAID6: read P and Q parity

6. When ALL reads complete (atomic_dec_and_test(&ctx->pending)):
   └─→ If single failure: XOR data disks + P parity
   └─→ If double failure (RAID6 only): Reed-Solomon reconstruction
   └─→ Copy reconstructed data to orig_bio
   └─→ Complete orig_bio
   └─→ Free ctx and buffers
```

## Data Structures

### Already Added (line 64 in dm-lhsr.c):

```c
/* RAID5/6 read reconstruction context */
struct lhsr_raid_5_read_ctx {
    struct bio *orig_bio;        /* Original bio to complete */
    struct lhsr_array *arr;        /* Array context */
    atomic_t pending;            /* Count of pending reads */
    int status;                /* Final status */
    void *recon_buf;            /* Reconstructed data buffer */
    unsigned int target_disk;    /* Which disk we're reconstructing for */
    unsigned int num_disks;    /* Total data disks */
    unsigned int working;        /* Number of working data disks */
    sector_t offset;            /* Sector offset */
    void **data_bufs;            /* Array of data buffers (working only) */
    unsigned int disk_map[0];    /* Flex array: working_idx -> disk_idx */
};
```

## Changes Required

| Location | Change | Purpose |
|----------|--------|---------|
| **New function** | `lhsr_raid_5_read_endio()` | Handle read completion + reconstruction |
| **Lines 1625-1644** | Modify READ path | Detect failed disk, set up reconstruction |
| **New forward decl** | At top (around line 38) | Forward declare `lhsr_raid_5_read_endio` |
| **Helper function** | `lhsr_raid_5_read_reconstruct()` | Perform XOR/RS reconstruction |

## Error Handling

1. **Memory allocation failure:** Return `BLK_STS_RESOURCE`, complete bio with error
2. **No working disks:** Return `BLK_STS_IOERR` (existing behavior)
3. **Reconstruction failure:** Complete orig_bio with error after all retries exhausted
4. **Partial read failure:** If some disks fail during reconstruction, try with remaining disks

## Testing Strategy

### Build Test
```bash
make -C kernel/dm-lhsr KERNEL_DIR=/lib/modules/$(uname -r)/build
```
- Must pass with 0 errors
- No new warnings (or only expected ones)

### Functional Tests

1. **Module loads successfully:**
   ```bash
   insmod kernel/dm-lhsr/dm-lhsr.ko
   ```

2. **Create RAID5 device:**
   ```bash
   dmsetup create lhsr_test --table '0 1000000 lhsr raid5 /dev/sdb 0 /dev/sdc 0 /dev/sdd 0'
   ```

3. **Write test data:**
   ```bash
   dd if=/dev/urandom of=/dev/mapper/lhsr_test bs=4K count=100
   ```

4. **Test degraded read (simulate disk failure):**
   - Mark disk as failed in metadata
   - Read data and verify reconstruction works

5. **Test on-the-fly error reconstruction:**
   - Inject read error (or use faulty disk)
   - Verify data reconstructed from parity

6. **Cleanup:**
   ```bash
   dmsetup remove lhsr_test && rmmod dm_lhsr
   ```

## Success Criteria

- [ ] Build passes with 0 errors
- [ ] No new warnings (or only expected ones)
- [ ] `lhsr_raid_5_read_endio()` function added
- [ ] READ path (lines 1625-1644) modified
- [ ] Forward declaration added
- [ ] Module loads successfully
- [ ] RAID5 read reconstruction works in degraded mode
- [ ] RAID6 read reconstruction works in degraded mode
- [ ] On-the-fly error reconstruction works

## Notes

- The current RAID5/6 READ path is simplified (no striping) - reads from first working disk
- For reconstruction, we need to read from ALL working disks + parity
- Reuses existing `lhsr_xor_parity()` and `lhsr_rs_parity()` functions from write path
- Follows Linux MD RAID principles but adapted for device-mapper simplicity
