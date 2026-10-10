# Build-ID discipline

Every LHSR module build stamps an ID into the binary. The kernel prints
it on load:

```bash
sudo dmesg | grep 'Module loaded'
# dm_lhsr: Module loaded: 1.4.0 build 20261010-04 (target registered) ...
```

**Rule: the reported ID must match the commit you think you are testing.
If it doesn't, stop.** You are testing the wrong binary and every result
after this point is garbage.

We learned this the hard way: stale modules survived across VM rebuilds,
a 6.12-built `.ko` got insmodded into a 6.1 guest (vermagic refused it,
but the attempt wasted an hour), and two separate debug sessions compared
numbers from different builds. The ID exists so this class of mistake is
a five-second check instead of a lost evening.

When you cut a new binary, bump the ID. One ID per binary, no reuse.
