# RAID5 Write Path Design - Option F: Simplified Full Stripe Write

**Date**: 2026-05-02  
**Status**: Approved  
**Task**: Task B - Integrate `lhsr_xor_parity()` into RAID5 write path

## Problem Statement

Current RAID5 write path only writes to the **first working data disk** and returns immediately. This breaks RAID5 redundancy because:
- No parity is calculated
- No parity is written
- Data on other disks becomes stale
- File systems see data corruption on disk failure

## Industry Analysis

| Implementation | Approach | Complexity |
|---------------|------------|------------|
| Linux MD RAID5 (`drivers/md/raid5.c`) | Stripe cache + RMW for partial stripes | Very High |
| dm-raid | Wraps MD RAID | Very High |
| **Our Option F** | Full stripe write + parity | **Medium** |

## Design: Option F - Simplified Full Stripe Write

### Architecture

```
Incoming BIO (dm-lhsr)
       │
       ▼
  lhsr_map()
       │
       ├── Clone bio for each WORKING data disk
       │   ├── disk[0] → submit_bio()
       │   ├── disk[1] → submit_bio()
       │   └── ...
       │
       ├── Compute parity: lhsr_xor_parity()
       │
       └── Write parity to parity disk[N]
           └── submit_bio()
```

### Components

| Component | Purpose | Notes |
|-----------|---------|-------|
| `lhsr_clone_bio()` | Clone bio for each target disk | Allocate new bio, copy data pages |
| `lhsr_xor_parity()` | Compute XOR parity | Existing function - reuse as-is |
| `lhsr_end_io()` | Completion callback | Track in-flight writes, end original bio when all done |

### Data Flow (RAID5 Write)

1. **Incoming bio** arrives at `lhsr_map()`
2. **Allocate page buffers** for data (one per working data disk)
3. **Copy data** from original bio to page buffers
4. **Clone bio** for each working data disk → `submit_bio()`
5. **Compute parity** using `lhsr_xor_parity(parity_buf, data_bufs, data_disks, len)`
6. **Write parity** to parity disk → `submit_bio()`
7. **Wait for all writes** via completion callback
8. **End original bio** with `bio_endio()`

### Error Handling

| Scenario | Handling |
|----------|----------|
| Data disk failed | Skip it (redundancy handles it) |
| Parity disk failed | Write data only (degraded mode, no parity) |
| Bio clone fails | Return `BLK_STS_IOERR`, end bio |
| Partial stripe write | **Pad to full block** (simplified approach) |

### Key Simplification vs Industry Standard

| Feature | Linux MD RAID5 | Our Option F |
|---------|-----------------|--------------|
| Partial stripe | Read-Modify-Write (RMW) | **Pad to full block** |
| Stripe cache | Yes (complex) | **No (simple)** |
| Write intent bitmap | Yes | **No (TODO)** |
| BIO cloning | Complex stripe handling | **Simple 1:1 clone** |

## Success Criteria

- ✅ Write to RAID5 device succeeds
- ✅ Parity is computed and written
- ✅ Data can be read back correctly
- ✅ If one data disk fails, data still accessible (degraded mode)
- ✅ If parity disk fails, data still writable (degraded mode, no parity)

## Implementation Notes

### Bio Cloning Approach

For kernel 6.12+, use `bio_alloc()` + `__bio_add_page()` (not `bio_alloc_clone()` which requires special bio_set):

```c
static struct bio *lhsr_clone_bio(struct bio *src_bio, struct block_device *bdev,
                                   sector_t sector, gfp_t gfp)
{
    struct bio *clone;
    struct bio_vec bv;
    struct bvec_iter iter;
    
    clone = bio_alloc(bdev, bio_segments(src_bio), REQ_OP_WRITE, gfp);
    if (!clone)
        return NULL;
    
    clone->bi_iter.bi_sector = sector;
    
    /* Copy all segments from source bio */
    bio_for_each_segment(bv, src_bio, iter) {
        __bio_add_page(clone, bv.bv_page, bv.bv_len, bv.bv_offset);
    }
    
    return clone;
}
```

### Parity Calculation Integration

```c
/* After all data writes submitted */
if (parity_disk_working) {
    void *parity_buf = kzalloc(data_len, GFP_KERNEL);
    void *data_bufs[data_disks];
    
    /* Fill data_bufs from cloned bio pages */
    lhsr_xor_parity(parity_buf, data_bufs, data_disks, data_len);
    
    /* Write parity_buf to parity disk */
    /* ... */
}
```

### Completion Tracking

Use `atomic_t` counter for in-flight writes:
- Increment for each submitted bio
- Decrement in completion callback
- When counter reaches 0, end original bio

## Out of Scope (Future Work)

- Partial stripe handling (RMW) - requires stripe cache
- Write intent bitmap - for crash recovery
- Read-modify-write optimization
- RAID6 double parity (Reed-Solomon)
- Async parity calculation (offload to workqueue)

## Affected Files

- `/home/keith/lhsr/kernel/dm-lhsr/dm-lhsr.c`
  - Modify `lhsr_map()` RAID5 write section
  - Add `lhsr_clone_bio()` helper
  - Add completion tracking

## Risks

| Risk | Mitigation |
|------|-------------|
| Memory allocation failure | Graceful degrade, end bio with error |
| BIO completion ordering | Use atomic counter, not assumptions |
| Parity calculation bug | Validate with known data patterns |
| Performance | Clone per-disk is standard approach |
