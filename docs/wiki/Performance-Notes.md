# Performance notes

Measured numbers only. Every line needs hardware + build ID + workload,
or it gets deleted.

## What we have (Oct 2026, VM, 4 vCPU, virtio)

| Workload | Throughput | Build |
|----------|-----------|-------|
| btrfs 4K randwrite, RAID5 64KB chunks | ~3–4MB/s | 20261010-01 |
| btrfs 4K randwrite, RAID6 64KB chunks | ~2.9MB/s | 20261010-01+ |
| btrfs 4K randwrite, RAID5 4KB chunks | ~4.9MB/s | 20261010-01+ |
| btrfs scrub (reads) | ~123MB/s | — |

Writes cost ~4 sync IOs per RMW cycle plus a workqueue drain per
barrier; that is where the time goes. Reads fly (see scrub).

## What we don't have

- Any post-fix mdadm comparison (the old claim is 5–11x; treat it as
  stale until remeasured).
- Any real-hardware number. VM virtio numbers tell you about
  correctness under load, not about production throughput.
- Barrier latency of the `rmw_wq` drain in isolation.

## How to measure

`tests/bench.sh` and `tests/test-benchmark.sh` are the harnesses
(max_active/chunk-size sweeps). Run them on real disks, record the
table above with the hardware line filled in.
