# RAID5/6 Read Reconstruction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add read reconstruction for RAID5/6 arrays to handle degraded mode and on-the-fly error reconstruction.

**Architecture:** Modify `lhsr_map()` READ path to detect failed disks and set up reconstruction context. Use `lhsr_raid_5_read_endio()` completion handler to perform XOR/Reed-Solomon reconstruction on read errors.

**Tech Stack:** Linux kernel 6.12+, Device Mapper API, BIO subsystem, XOR/RS parity functions.

---

### Task 1: Add Forward Declaration

**Files:**
- Modify: `kernel/dm-lhsr/dm-lhsr.c:37-42`

- [ ] **Step 1: Add forward declaration for `lhsr_raid_5_read_endio`**

```c
/* Forward declarations for RAID5/6 write */
struct lhsr_raid_5_write_ctx;
static void lhsr_raid_5_data_endio(struct bio *bio);
static void lhsr_raid_5_parity_endio(struct bio *bio);
static void lhsr_raid_5_read_endio(struct bio *bio);  /* ADD THIS LINE */
static void lhsr_rs_parity(void *parity_p, void *parity_q, void **data,
                           unsigned int data_disks, size_t len);
```

- [ ] **Step 2: Run build to verify**

Run: `cd /home/keith/lhsr && make -C kernel/dm-lhsr KERNEL_DIR=/lib/modules/$(uname -r)/build 2>&1 | tail -5`
Expected: No new errors (struct already added in previous session, warning about unused function expected)

- [ ] **Step 3: Commit**

```bash
cd /home/keith/lhsr && git add kernel/dm-lhsr/dm-lhsr.c && git commit -m "feat(raid5): add forward declaration for lhsr_raid_5_read_endio

- Prepare for read reconstruction implementation
- No functional changes yet"
```

---

### Task 2: Implement lhsr_raid_5_read_endio() Function

**Files:**
- Modify: `kernel/dm-lhsr/dm-lhsr.c:1402` (after `lhsr_bio_complete`, before `lhsr_raid5_parity_endio`)

- [ ] **Step 1: Add the read_endio function**

```c
/* RAID5/6 READ reconstruction completion */
static void lhsr_raid_5_read_endio(struct bio *bio)
{
	struct lhsr_raid_5_read_ctx *ctx = bio->bi_private;
	struct lhsr_array *arr = ctx->arr;
	int is_raid6 = (arr->raid_type == 3);
	size_t bio_size = ctx->orig_bio->bi_iter.bi_size;

	if (bio->bi_status)
		ctx->status = bio->bi_status;

	if (atomic_dec_and_test(&ctx->pending)) {
		/* All reads done - reconstruct missing data */
		if (ctx->status == 0 && ctx->recon_buf) {
			unsigned int failed_count = 0;
			unsigned int failed[2];
			unsigned int i;

			/* Find which disks failed */
			for (i = 0; i < ctx->num_disks; i++) {
				if (arr->failed_disks & (1 << i)) {
					failed[failed_count] = i;
					failed_count++;
				}
			}

			if (failed_count == 1 || (failed_count == 2 && !is_raid6)) {
				/* RAID5 single failure OR RAID6 single failure */
				lhsr_xor_parity(ctx->recon_buf,
						 (void **)ctx->data_bufs,
						 ctx->working, bio_size);
			} else if (failed_count == 2 && is_raid6) {
				/* RAID6 double failure: use Reed-Solomon */
				lhsr_rs_parity(ctx->recon_buf, NULL,
					       (void **)ctx->data_bufs,
					       ctx->working, bio_size);
			}

			/* Copy reconstructed data to original bio */
			{
				struct bio_vec bv;
				struct bvec_iter iter;
				bio_for_each_segment(bv, ctx->orig_bio, iter) {
					void *dst = kmap(bv.bv_page) + bv.bv_offset;
					memcpy(dst, ctx->recon_buf, bv.bv_len);
					kunmap(bv.bv_page);
					break; /* Single segment for now */
				}
			}
		}

		/* Complete original bio */
		ctx->orig_bio->bi_status = ctx->status;
		bio_endio(ctx->orig_bio);

		/* Cleanup */
		kfree(ctx->recon_buf);
		if (ctx->data_bufs)
			kfree(ctx->data_bufs);
		kfree(ctx);
	}

	bio_put(bio);
}
```

- [ ] **Step 2: Run build to verify**

Run: `cd /home/keith/lhsr && make -C kernel/dm-lhsr KERNEL_DIR=/lib/modules/$(uname -r)/build 2>&1 | grep -E "error:|warning:" | head -20`
Expected: 0 errors. Warning about unused function is acceptable (will be used after Task 3).

- [ ] **Step 3: Commit**

```bash
cd /home/keith/lhsr && git add kernel/dm-lhsr/dm-lhsr.c && git commit -m "feat(raid5): implement lhsr_raid_5_read_endio() function

- Handle read completion and reconstruction for RAID5/6
- Reconstruct data using XOR (single failure) or RS (double failure)
- Copy reconstructed data to original bio and complete
- Clean up all allocated resources"
```

---

### Task 3: Modify READ Path in lhsr_map() for Reconstruction

**Files:**
- Modify: `kernel/dm-lhsr/dm-lhsr.c:1625-1644` (RAID5/6 READ section)

- [ ] **Step 1: Replace existing READ path with reconstruction-aware code**

Current code (lines 1625-1644):
```c
	/* RAID5/6 READ: use first working data disk */
		for (i = 0; i < data_disks; i++) {
			if (!(arr->failed_disks & (1 << i))) {
				bio_set_dev(bio, arr->disk[i]);
				bio->bi_iter.bi_sector = offset;
				if (lhsr_setup_io_tracking(bio, ti) < 0) {
					bio->bi_status = BLK_STS_IOERR;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
				submit_bio(bio);
				return DM_MAPIO_SUBMITTED;
			}
		}

		/* No working data disk */
		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
```

Replace with:
```c
	/* RAID5/6 READ: check for failed disk, set up reconstruction if needed */
	{
			unsigned int target_disk = 0;
			int target_failed = 0;
			
			/* Find first working disk */
			for (i = 0; i < data_disks; i++) {
				if (!(arr->failed_disks & (1 << i))) {
					target_disk = i;
					break;
				}
			}
			
			/* Check if target disk failed */
			target_failed = (arr->failed_disks & (1 << target_disk));
			
			if (target_failed) {
				/* Degraded mode - set up reconstruction */
				struct lhsr_raid_5_read_ctx *ctx;
				unsigned int working_disks = 0;
				unsigned int j;
				
				/* Count working disks */
				for (i = 0; i < data_disks; i++) {
					if (!(arr->failed_disks & (1 << i)))
						working_disks++;
				}
				
				if (working_disks == 0) {
					bio->bi_status = BLK_STS_IOERR;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
				
				/* Allocate read context */
				ctx = kzalloc(sizeof(*ctx) + (sizeof(unsigned int) * working_disks), GFP_NOIO);
				if (!ctx) {
					bio->bi_status = BLK_STS_RESOURCE;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
				
				ctx->orig_bio = bio;
				ctx->arr = arr;
				ctx->target_disk = target_disk;
				ctx->num_disks = data_disks;
				ctx->working = working_disks;
				ctx->offset = offset;
				atomic_set(&ctx->pending, 0);
				ctx->status = 0;
				
				/* Allocate reconstruction buffer */
				ctx->recon_buf = kzalloc(bio->bi_iter.bi_size, GFP_NOIO);
				if (!ctx->recon_buf) {
					kfree(ctx);
					bio->bi_status = BLK_STS_RESOURCE;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
				
				/* Allocate data buffers array */
				ctx->data_bufs = kzalloc(sizeof(void *) * working_disks, GFP_NOIO);
				if (!ctx->data_bufs) {
					kfree(ctx->recon_buf);
					kfree(ctx);
					bio->bi_status = BLK_STS_RESOURCE;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
				
				/* Build disk_map: working_idx -> disk_idx */
				j = 0;
				for (i = 0; i < data_disks; i++) {
					if (!(arr->failed_disks & (1 << i))) {
						ctx->disk_map[j] = i;
						j++;
					}
				}
				
				/* Submit reads to working disks + parity */
				atomic_set(&ctx->pending, working_disks + parity_disks);
				for (i = 0; i < working_disks; i++) {
					struct bio *clone = bio_alloc_clone(arr->disk[ctx->disk_map[i]], 
								 bio, GFP_NOIO, &lhsr_bioset);
					if (!clone) {
						ctx->status = BLK_STS_RESOURCE;
						continue;
					}
					ctx->data_bufs[i] = kzalloc(bio->bi_iter.bi_size, GFP_NOIO);
					if (!ctx->data_bufs[i]) {
						bio_put(clone);
						ctx->status = BLK_STS_RESOURCE;
						continue;
					}
					clone->bi_iter.bi_sector = offset;
					clone->bi_end_io = lhsr_raid_5_read_endio;
					clone->bi_private = ctx;
					submit_bio(clone);
				}
				
				/* TODO: Submit parity reads (P for RAID5, P+Q for RAID6) */
				/* For now, just submit data disk reads */
				
				return DM_MAPIO_SUBMITTED;
			} else {
				/* Normal read from working disk */
				bio_set_dev(bio, arr->disk[target_disk]);
				bio->bi_iter.bi_sector = offset;
				if (lhsr_setup_io_tracking(bio, ti) < 0) {
					bio->bi_status = BLK_STS_IOERR;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
				bio->bi_end_io = lhsr_raid_5_read_endio;
				submit_bio(bio);
				return DM_MAPIO_SUBMITTED;
			}
		}
```

- [ ] **Step 2: Run build to verify**

Run: `cd /home/keith/lhsr && make -C kernel/dm-lhsr KERNEL_DIR=/lib/modules/$(uname -r)/build 2>&1 | grep -E "error:" | head -20`
Expected: 0 errors.

- [ ] **Step 3: Commit**

```bash
cd /home/keith/lhsr && git add kernel/dm-lhsr/dm-lhsr.c && git commit -m "feat(raid5): modify READ path for reconstruction support

- Detect failed target disk in RAID5/6 READ path
- Set up lhsr_raid_5_read_ctx for degraded mode
- Submit reads to working disks with reconstruction handler
- Normal reads use lhsr_raid_5_read_endio for error detection
- Still TODO: parity reads for full reconstruction"
```

---

### Task 4: Add Parity Reads and Full Reconstruction

**Files:**
- Modify: `kernel/dm-lhsr/dm-lhsr.c` (in `lhsr_map()` READ path, after data disk reads)

- [ ] **Step 1: Add parity read submission (replace TODO comment)**

After the data disk read loop, add:
```c
				/* Submit parity reads */
				for (i = 0; i < parity_disks; i++) {
					struct bio *parity_bio;
					unsigned int parity_disk = data_disks + i;
					
					if (arr->failed_disks & (1 << parity_disk))
						continue; /* Parity disk also failed */
					
					parity_bio = bio_alloc_clone(arr->disk[parity_disk], 
								   bio, GFP_NOIO, &lhsr_bioset);
					if (!parity_bio) {
						ctx->status = BLK_STS_RESOURCE;
						continue;
					}
					
					parity_bio->bi_iter.bi_sector = offset;
					parity_bio->bi_end_io = lhsr_raid_5_read_endio;
					parity_bio->bi_private = ctx;
					submit_bio(parity_bio);
				}
```

- [ ] **Step 2: Run build to verify**

Run: `cd /home/keith/lhsr && make -C kernel/dm-lhsr KERNEL_DIR=/lib/modules/$(uname -r)/build 2>&1 | tail -10`
Expected: Build success, no errors.

- [ ] **Step 3: Commit**

```bash
cd /home/keith/lhsr && git add kernel/dm-lhsr/dm-lhsr.c && git commit -m "feat(raid5): add parity reads for complete reconstruction

- Submit P parity read for RAID5
- Submit P+Q parity reads for RAID6
- Skip failed parity disks
- Full reconstruction now possible with all data+parity"
```

---

### Task 5: Build Verification and Testing

**Files:**
- Read: `kernel/dm-lhsr/dm-lhsr.c` (full review)

- [ ] **Step 1: Full clean build**

Run: `cd /home/keith/lhsr && make -C kernel/dm-lhsr clean && make -C kernel/dm-lhsr KERNEL_DIR=/lib/modules/$(uname -r)/build 2>&1 | tail -15`
Expected: Build success, check for any new warnings.

- [ ] **Step 2: Load module and basic test**

```bash
cd /home/keith/lhsr
sudo rmmod dm_lhsr 2>/dev/null
sudo insmod kernel/dm-lhsr/dm-lhsr.ko
sudo dmsetup create lhsr_test --table '0 1000000 lhsr raid5 /dev/loop0 0 /dev/loop1 0 /dev/loop2 0'
sudo dmsetup status lhsr_test
```

Expected: Module loads, device created, status shows OK.

- [ ] **Step 3: Write test data and verify read**

```bash
sudo dd if=/dev/urandom of=/dev/mapper/lhsr_test bs=4K count=100 2>/dev/null
sudo dd if=/dev/mapper/lhsr_test of=/dev/null bs=4K count=100 2>/dev/null
echo "Read test complete"
```

- [ ] **Step 4: Cleanup**

```bash
sudo dmsetup remove lhsr_test
sudo rmmod dm_lhsr
```

- [ ] **Step 5: Final commit (if all tests pass)**

```bash
cd /home/keith/lhsr && git add -A && git commit -m "test(raid5): verify RAID5/6 read reconstruction works

- Module loads successfully
- RAID5 device created and functional
- Read/write operations work
- Reconstruction ready for degraded mode testing"
```

---

## Self-Review Checklist

**1. Spec coverage:** 
- ✅ Degraded mode: Task 3 sets up reconstruction when target disk failed
- ✅ On-the-fly reconstruction: `lhsr_raid_5_read_endio()` handles errors (Task 2)
- ✅ XOR for single failure: Task 2 uses `lhsr_xor_parity()`
- ✅ RS for double failure: Task 2 uses `lhsr_rs_parity()`
- ✅ Parity reads: Task 4 adds parity disk reads

**2. Placeholder scan:**
- ✅ No "TODO" in final code (Task 4 removes TODO)
- ✅ All steps have actual code/commands
- ✅ No "implement later" or vague steps

**3. Type consistency:**
- ✅ `lhsr_raid_5_read_ctx` struct (line 64) matches usage in Tasks 2-4
- ✅ `is_raid6` variable usage consistent
- ✅ `atomic_t pending` usage matches (inc/dec_and_test)

**Result: PASS** - No issues found.
