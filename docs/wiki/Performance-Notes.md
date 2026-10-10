# Performance notes

Measured numbers only. Every line needs hardware + build ID + workload,
or it gets deleted.

## mdadm shootout (Oct 2026, VM, 4 vCPU, virtio, 3 disks, 4K chunks)

`tests/perf/bench.sh both 3`, fio direct, 100MB, 8s runs. LHSR build
20261010-04, mdadm 4.2 RAID5 `--assume-clean`. Medians of 3 reps:

| Workload | mdadm | LHSR | Ratio |
|----------|-------|------|-------|
| seqwrite 1M qd4 | 61 MB/s | 22 MB/s | 2.8x slower |
| randwrite 4K qd16 | 2.0 MB/s | 3.0 MB/s | 1.5x faster |
| randread 4K qd16 | 8.6 MB/s | 9.0 MB/s | even |

The old "5-11x slower" claim is dead, at least for 4K chunks on a VM —
the multi-chunk worker and the delta path did their job. Sequential
writes still trail (per-stripe RMW round-trips); random writes lead.

## Older numbers (same VM class, btrfs file IO, pre-shootout)

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
