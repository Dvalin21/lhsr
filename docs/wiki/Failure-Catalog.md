# Failure catalog

Every entry: what you see, what it is, which commit fixed it. Add rows,
don't rewrite history.

## Orphaned stripe lock (Oct 2026, fixed)

- **See:** `rmw_locks` shows buckets with climbing age; holder PIDs gone
  (`ps -p` finds nothing); `works` frozen above `done`; data reads fine.
- **Is:** an RMW worker died while holding a stripe mutex. Dead holder,
  wedged bucket. Data stays consistent (nothing was half-written).
- **Was:** `full` (stripe fully touched) vs `chunk_full[l]` (chunk fully
  contained) mismatch — bio-edge stripes used `od[l]==NULL`, wild kmap,
  worker GPF. Btrfs writeback shapes trigger it; aligned fio never did.
- **Fixed in:** per-chunk `od`, true full-stripe, partial-only reads.

## Forced read-only on barrier (Oct 2026, fixed)

- **See:** `BTRFS error ... errs: flush 1` (later: `wr N`), then
  `forced readonly`. `rmw_stats` frozen (flushes aren't RMW work).
- **Is:** piggybacked barriers failing. Standalone `REQ_OP_FLUSH` bios
  are rejected by `submit_bio`; the member-wave replacement wedged.
- **Fixed in:** drain `rmw_wq` as the barrier. Note this is also plain
  filesystem behavior: one EIO on a single-device fs means ro, even
  when the error is real. Monitor `errors`.

## Heap smash in recovery/reshape/regraded-read (Oct 2026, fixed)

- **See:** oops/panic during `dmsetup create` with dirty bitmap regions,
  or scheduler `general protection fault` + panic on degraded reads.
- **Is:** single `alloc_page` used with `chunk_bytes` (≤64KB) IO/memset —
  up to 60KB past the page on recovery, 1MB DMA smash on degraded reads.
- **Fixed in:** compound pages everywhere chunk IO happens.

## Degraded-read garbage past 4KB (Oct 2026, fixed)

- **See:** degraded reads match for the first 4KB, garbage after.
- **Is:** reconstruction copy-back stopped after the first bio segment.
- **Fixed in:** walk all orig segments.

## Watch item: wild `ctr` memset (Oct 2026, likely fallout)

- **See:** `lhsr_ctr` rep-stos fault to a +850GB address during dirty
  recovery; box panic.
- **Believed:** fallout of the heap smash above (corrupted base pointer).
  Never recurred after the compound-page fix (2 clean dirty recoveries).
  Evidence saved under `vm-work/lhsr-gpf/serial-panic-ctr-20261010.log`.
  If it returns, it gets its own row and its own hunt.
