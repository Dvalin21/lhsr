# LHSR Test Hardware Baseline

## Current System

| Component | Spec |
|-----------|------|
| **CPU** | 2× Intel Xeon Silver 4114 @ 2.20 GHz (10 cores / 20 threads each) |
| **RAM** | 62 GiB DDR4 |
| **Storage** | NVMe SSD (system + test images) |
| **Kernel** | 6.12.90+deb13.1-amd64 (Debian 13 Trixie) |
| **Test images** | Loopback files on NVMe (no hot-swap drives) |

## VM / Resource Policy

**Principle:** Keep usage as small as possible until absolutely needed. Do not crash the host by over-allocating to a VM.

- **Test image size:** 64–100 MiB per loopback device (current tests use 3–4 loops max = 400 MiB total backing storage). Do not increase without justification.
- **Concurrent test VMs:** 1 at a time. No parallel test execution.
- **Memory overhead:** Tests use ~200 MiB RSS (module + userspace tools). Do not pre-allocate large buffers in test infrastructure.
- **CPU:** RMW worker pool max_active=32 (tunable). Default chosen conservatively for 40-core host. Reduce if host becomes unresponsive under I/O load.
- **Swap:** Host has swap configured. Tests must handle ENOMEM gracefully; do not rely on overcommit.

## Deviation Policy

1. A test may temporarily increase allocation (e.g., `DISK_SIZE_MB=512` for a one-off benchmark) if the task requires it.
2. If the test **passes**, revert the allocation to baseline immediately. Note the deviation in the test comment.
3. If the test **fails**, the elevated allocation stays — it helps debugging. File an issue explaining why the baseline was insufficient.
4. Before committing, all tests must pass at baseline allocation.

## Why Loanback Loops

No hardware RAID controller. No hot-swap backplane. Loopback devices over image files on the NVMe system disk provide sufficient isolation for testing:

- Crash recovery: images persist across reboots
- Fault injection: replace loop file with broken data
- Performance: NVMe latency is low enough that loop overhead doesn't mask module-level bugs
- Resource control: easy to size down when needed

This is fine for development testing. Production certification requires real drives.

## Rationale for Conservatism

- This is a single workstation, not a CI cluster.
- A crash forced by OOM or OVS eats development time.
- Loopback devices at 64 MB catch the same module bugs as 1 TB drives.
- Scale up only when the problem demands it (e.g., NCQ queue-depth testing, multi-controller paths).
