# Phase 10-D: Performance Baseline Benchmarks

## Summary

Establish regression baseline for LHSR RAID5 kernel module performance.
All benchmarks run on VM (192.168.122.137, Debian 13, kernel 6.12.90+deb13.1-amd64)
with 3x80M loopback devices on tmpfs. Comparisons: raw single disk, mdadm RAID5, LHSR RAID5.

**Module**: dm_lhsr (loaded, size 77824)
**Date**: 2026-06-17 03:06 UTC
**fio**: 3.39
**Test size**: 60M per job, 15s runtime, direct I/O

---

## Results

### 1M Sequential Read (QD=4)

| Target   | IOPS  | Bandwidth | Lat(avg) | Lat(p99) | Lat(max) |
|----------|-------|-----------|----------|----------|----------|
| RAW      | 5169  | 5.0 GB/s  | 773us    | 889us    | 5095us   |
| MDADM    | 4226  | 4.1 GB/s  | 945us    | 2310us   | 4709us   |
| **LHSR** | 1169  | 1.1 GB/s  | 3420us   | 7438us   | 11971us  |

### 1M Sequential Write (QD=4)

| Target   | IOPS  | Bandwidth | Lat(avg) | Lat(p99) | Lat(max) |
|----------|-------|-----------|----------|----------|----------|
| RAW      | 3520  | 3.4 GB/s  | 1135us   | 1237us   | 5163us   |
| MDADM    | 668   | 0.7 GB/s  | 5981us   | 11076us  | 17206us  |
| **LHSR** | 15    | 15 MB/s   | 270341us | 396362us | 437183us |

### 4K Random Read (QD=1 / 16 / 32)

| QD | Target   | IOPS   | Lat(avg) | Lat(p99) | Lat(max) |
|----|----------|--------|----------|----------|----------|
| 1  | RAW      | 63862  | 15.1us   | 30.6us   | 2345us   |
| 1  | MDADM    | 55839  | 17.3us   | 33.0us   | 1427us   |
| 1  | **LHSR** | 51008  | 18.8us   | 36.6us   | 417us    |
| 16 | RAW      | 102212 | 156.0us  | 561.2us  | 4078us   |
| 16 | MDADM    | 66431  | 240.2us  | 790.5us  | 1523us   |
| 16 | **LHSR** | 69016  | 231.2us  | 716.8us  | 1117us   |
| 32 | RAW      | 102090 | 312.9us  | 1220.6us | 2046us   |
| 32 | MDADM    | 76560  | 417.4us  | 1417.2us | 6384us   |
| 32 | **LHSR** | 76344  | 418.5us  | 1417.2us | 1938us   |

### 4K Random Write (QD=1 / 16 / 32)

| QD | Target   | IOPS  | Lat(avg) | Lat(p99) | Lat(max) |
|----|----------|-------|----------|----------|----------|
| 1  | RAW      | 50078 | 19.2us   | 37.1us   | 1717us   |
| 1  | MDADM    | 22786 | 43.2us   | 73.2us   | 897us    |
| 1  | **LHSR** | 5620  | 177.1us  | 464.9us  | 958us    |
| 16 | RAW      | 77544 | 205.7us  | 626.7us  | 1504us   |
| 16 | MDADM    | 69798 | 228.8us  | 423.9us  | 1482us   |
| 16 | **LHSR** | 5725  | 2794.1us | 7307.3us | 8571us   |
| 32 | RAW      | 96608 | 330.6us  | 1056.8us | 2074us   |
| 32 | MDADM    | 75436 | 423.7us  | 733.2us  | 4532us   |
| 32 | **LHSR** | 5146  | 6216.7us | 14614.5us| 16301us  |

### Mixed 70/30 Random Read/Write (4K, QD=8)

| Target   | Direction | IOPS  | Lat(avg) | Lat(p99) | Lat(max) |
|----------|-----------|-------|----------|----------|----------|
| MDADM    | Read      | 50608 | 61.5us   | 118.3us  | 1706us   |
| MDADM    | Write     | 50514 | 96.0us   | 162.8us  | 1874us   |
| **LHSR** | Read     | 5366  | 26.0us   | 58.6us   | 883us    |
| **LHSR** | Write    | 5342  | 1469.8us | 3784.7us | 5367us   |

---

## Analysis

### Strengths (Read Performance)
- **4K random reads**: LHSR is close to mdadm at higher queue depths (QD=16: 69016 vs 66431; QD=32: 76344 vs 76560). Both approach raw device throughput at QD=32.
- **Single-thread reads**: LHSR is 51008 IOPS at 18.8us avg latency — within 20% of mdadm (55839) and raw (63862). Acceptable for a first implementation.
- **Read scalability**: LHSR scales well with queue depth for reads (QD1: 51K → QD32: 76K), similar to mdadm.

### Weaknesses (Write Performance)
- **Sequential writes are broken**: 15 IOPS/15 MB/s vs mdadm's 668 IOPS/0.7 GB/s. Latency p99 is 396ms vs mdadm's 11ms. **This is the worst regression vs mdadm** — LHSR is ~45x slower.
- **Random writes don't scale**: IOPS drops from QD1 (5620) to QD16 (5725, same) and stays flat at QD32 (5146). mdadm scales from QD1 (22786) → QD16 (69798, 3x). LHSR hits a wall at ~5700 IOPS regardless of queue depth.
- **Write latency is high**: At QD=16, p99 latency is 7307us vs mdadm's 424us (17x worse).
- **Mixed workloads**: LHSR read IOPS drops to 5366 when writes are in-flight (vs 69016 pure-read). The read path stalls during writes.

### Root Cause
The write bottleneck is caused by the current kernel module's **serial I/O dispatch**. Each write to RAID5 requires:
1. Read old data + parity (RMW cycle)
2. XOR compute new parity
3. Write data + parity to all disks

This is done **synchronously per stripe** with no pipelining. The completion of one write must complete before the next begins. mdadm's kernel module (raid5.c) pipelines multiple stripes and uses asynchronous RAID offload (async_tx).

### What This Baseline Enables
When Phase 10-A (kernel reshape) adds parallel I/O dispatch and the RMW pipeline, we can:
- Compare the new write IOPS against these numbers
- Measure whether read performance regressed
- Quantify the improvement factor (target: within 2x of mdadm for writes)

---

## Raw Results File

`/home/keith/lhsr/docs/plans/2026-06-17-benchmark-results.txt`

---

## Benchmark Script

`/home/keith/lhsr/docs/plans/2026-06-17-phase10-benchmarks.sh`
