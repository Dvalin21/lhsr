# LHSR Kernel Development Notes

## Kernel Version Compatibility
- **Tested against**: Linux 6.12.74+deb13+1-amd64 (Debian)
- **Knowledge cutoff**: September 2024 (training data)

## Linux 6.12+ API Changes (2024-2026)

### Block Layer (bio) APIs

| Old API (pre-6.x) | New API (6.12+) | Notes |
|-------------------|-----------------|-------|
| `bioset_create()` | `bioset_init()` | Function renamed |
| `bioset_free()` | `bioset_exit()` | Function renamed |
| `bio_clone_fast(bio, gfp)` | **NOT FOUND** | May have been removed |
| `bio_clone(bio, gfp)` | `bio_clone_bioset()` | Now requires bio_set |
| `bio_alloc(bdev, vecs, opf, gfp)` | Still available | Standard allocation |
| `blkdev_get_by_dev()` | `blkdev_get_no_open()` | New API |
| `DM_TARGET_PASSES_BIOSET` | **NOT FOUND** | May have been removed |

### Device Mapper APIs

| Old API | New API | Notes |
|---------|---------|-------|
| `.ioctl` in target_type | **NOT FOUND** | Use `.message` or `.prepare_ioctl` |
| `.features = DM_TARGET_*` | Optional | Use defined flags |

### Bio Operation Checking

```c
// Old (deprecated)
bio->bi_rw

// New (6.x+)
bio_op(bio)        // returns enum req_op
bio_data_dir(bio) // returns READ or WRITE
bio_opf(bio)      // returns full opf flags (includesREQ_OP_*)
```

### Workqueue APIs

```c
// Still valid in 6.12
alloc_workqueue("name", WQ_MEM_RECLAIM | WQ_UNBOUND, 1);
INIT_WORK(&work, handler_function);
queue_work(lhsr_wq, &work);
cancel_work_sync(&work);
destroy_workqueue(lhsr_wq);
```

## Kernel Module Parameters

Module parameters in 6.x work the same:
```c
static unsigned int default_block_size = 128 * 1024;
module_param(default_block_size, uint, 0644);
MODULE_PARM_DESC(default_block_size, "Default block size in KB");
```

## Building Against Kernel Headers

```bash
# Install headers
apt install linux-headers-$(uname -r)

# Build module
make -C kernel/dm-lhsr KERNEL_DIR=/lib/modules/$(uname -r)/build

# Check module info
modinfo kernel/dm-lhsr/dm-lhsr.ko
```

## Debugging Kernel Module

```bash
# Load module
insmod kernel/dm-lhsr/dm-lhsr.ko

# Check dmesg
dmesg | tail -50

# Remove module
rmmod dm-lhsr
```

## Common Issues & Solutions

1. **Implicit declarations**: Ensure all function prototypes are declared before use
2. **Forward declarations**: Required for functions called before defined
3. **EXPORT_SYMBOL**: Must be at file scope, after function definition
4. **bio_clone_fast**: Use `bio_alloc()` + `bio_copy_data()` instead

## dm-lhsr.ko Current State

- **Version**: 1.1.0
- **Features**: RAID0, RAID1, RAID5, RAID6, SHR, Single, Mirror
- **Scrubber**: Background integrity verification with CRC32c
- **Rebuild**: Disk replacement state machine
- **Status**: Superblock persistence + scrubber + rebuild tracking implemented

### Superblock Persistence
- Primary at 4MB offset, backup at end-8MB
- CRC32c checksummed with auto-backup recovery
- Generation counter tracks state transitions

### Scrubber
- Rate-limited 128KB block verification
- Corruption detection tracking
- Control: `dmsetup message <dev> scrub start|stop`

### Rebuild Tracking
- States: NONE, PENDING, RUNNING, COMPLETE
- Control: `dmsetup message <dev> rebuild start <disk>|status`

---
Updated: 2026-04-21
