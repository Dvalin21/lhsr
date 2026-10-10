# Diagnostics

Three commands. Run them in this order.

## 1. Is the right build loaded?

```bash
sudo dmesg | grep 'Module loaded'
```

See [[Build-ID Discipline]]. Wrong ID → stop.

## 2. Is the RMW pipeline balanced?

```bash
sudo dmsetup message <dev> 0 rmw_stats
# works=555969 done=555969 full=6915 partial=549022 errors=0 ...
```

- `works` must equal `done` (a small gap is fine — that's work in flight).
  A gap that never closes means stranded workers.
- `errors` must stay 0. Anything else is a stripe that failed loud;
  correlate with dmesg before continuing.

## 3. Is any stripe lock wedged?

```bash
sudo dmsetup message <dev> 0 rmw_locks
# held: 41:pid=844,age=136s 58:pid=868,age=136s
```

- Empty `held:` is healthy. Entries with age 0s are just active workers.
- A bucket whose age keeps climbing past ~60s is wedged. Check whether
  the holder PID still exists (`ps -p <pid>`). A gone PID with a held
  lock means the worker died — look for an oops in dmesg, then check
  the [[Failure Catalog]].

## Per-disk state

```bash
sudo dmsetup message <dev> 0 'member_status 0'   # per disk: state, errors, gen
sudo dmsetup status <dev>                        # OK RAID6 4/4 err=0
```

`errors=0` on every member plus `errors=0` in `rmw_stats` is a healthy
array. Anything else: stop the workload, capture `dmesg`, then touch
nothing until you know which counter moved first.
