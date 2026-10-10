# LHSR operator wiki

This is the operator side of LHSR. The README tells you what the project
is; these pages tell you how to run it without getting burned.

Start here:

- [[Build-ID Discipline]] — read this first. It will save you hours.
- [[Diagnostics]] — the three commands that tell you if an array is healthy.
- [[Reshape Runbook]] — RAID5→RAID6, step by step.
- [[Recovery Runbook]] — dead disk drill: fail, verify, rebuild, verify.
- [[Failure Catalog]] — every bug we have caught in the wild, with symptoms.
- [[Test Matrix]] — what has actually been verified, and what has not.
- [[Performance Notes]] — measured numbers only. No folklore.

Notation used everywhere: `<dev>` is the dm name (e.g. `lhsr-0`).
`rmw_stats` / `rmw_locks` come from `dmsetup message <dev> 0 <cmd>`.
