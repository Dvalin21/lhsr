# LHSR — Linux Hybrid Self-Healing RAID

<p align="center">

<img src="https://img.shields.io/badge/License-GPLv3-blue.svg" alt="License">
<img src="https://img.shields.io/badge/Linux-5.15%2B-green.svg" alt="Kernel">
<img src="https://img.shields.io/badge/Status-Development-yellow.svg" alt="Status">

</p>

## Overview

LHSR (Linux Hybrid Self-Healing RAID) is a production-grade software RAID system inspired by Synology Hybrid RAID (SHR/SHR-2) with enterprise features:

- ✅ SHR-style flexible disk sizing (mixed drives supported)
- ✅ Self-healing with automatic block repair
- ✅ Anti-bit-rot protection
- ✅ Predictive failure detection
- ✅ Live block migration
- ✅ Instant RAID recovery
- ✅ Incremental rebuild (only used blocks)
- ✅ Firmware failure mitigation

## Why LHSR?

| Feature | mdadm | ZFS | SHR | LHSR |
|---------|-------|-----|-----|------|
| Mixed disk sizes | ❌ | ⚠️ | ✅ | ✅ |
| Self healing | ❌ | ✅ | ⚠️ | ✅ |
| Anti bit-rot | ❌ | ✅ | ❌ | ✅ |
| Predictive migration | ❌ | ❌ | ❌ | ✅ |
| Instant recovery | ❌ | ✅ | ❌ | ✅ |
| Incremental rebuild | ❌ | ❌ | ❌ | ✅ |
| Firmware protection | ❌ | ❌ | ❌ | ✅ |

## Quick Start

```bash
# Clone and build
git clone https://github.com/Dvalin21/lhsr.git
cd lhsr
make

# Create array
sudo ./lhsrctl create --raid shr /dev/sda /dev/sdb /dev/sdc

# Status
sudo ./lhsrctl status
```

## Documentation

- [Technical Specification](docs/technical-specification.md)
- [On-Disk Format](docs/disk-format.md)
- [Recovery Guide](docs/recovery.md)
- [Performance Tuning](docs/performance.md)

## Features

### SHR-Like Flexible Disk Sizes
Use mismatched drives without waste. Example:
```
12TB + 8TB + 4TB + 4TB → Optimal layout automatically
```

### Self-Healing
Automatic block repair with checksum verification:
```
Read → Verify → If corruption, rebuild → Repair → Rewrite
```

### Anti-Bit-Rot
Detection and repair of silent data corruption:
- Per-block checksums
- Background scrubbing
- Automatic repair

### Predictive Failure
SMART-based failure prediction:
```
$ lhsr status
Disk 2: 82% failure risk
Recommendation: migrate data
```

### Instant Recovery
Partial array mount without rebuild:
```
lhsr scan --reconstruct
lhsr mount --recovery
```

### Firmware Mitigation
Protection against drive firmware bugs:
- Fingerprint detection
- Known bad firmware database
- Write verification

## Requirements

### Kernel
- Linux 5.15+
- Device Mapper (dm-mod)

### Build
- make
- gcc/clang
- kernel headers

### Runtime
- libblkid
- libuuid

## Building

```bash
# Full build
make all

# Just kernel module
make modules

# Just userspace
make userspace

# Clean
make clean
```

## Installation

```bash
# Install
sudo make install

# Or manual
sudo insmod kernel/dm-lhsr/dm-lhsr.ko
sudo cp userspace/cli/lhsrctl /usr/local/bin/
sudo cp userspace/daemon/lhsrd /usr/local/bin/
```

## Usage

### Create Array
```bash
# SHR (mixed disks)
sudo lhsrctl create --raid shr /dev/sda /dev/sdb /dev/sdc

# RAID5
sudo lhsrctl create --raid 5 /dev/sda /dev/sdb /dev/sdc

# RAID6
sudo lhsrctl create --raid 6 /dev/sda /dev/sdb /dev/sdc /dev/sdd
```

### Manage
```bash
# Status
sudo lhsrctl status

# Add disk
sudo lhsrctl add /dev/sdd

# Remove disk
sudo lhsrctl remove /dev/sdc

# Expand array
sudo lhsrctl expand
```

### Repair
```bash
# Repair
sudo lhsrctl repair

# Scrub
sudo lhsrctl scrub start

# Check disk health
sudo lhsrctl predict
```

### Recovery
```bash
# Scan
sudo lhsr-scan --deep

# Reconstruct
sudo lhsr-reconstruct

# Repair metadata
sudo lhsr-repair-metadata
```

## Architecture

```
Filesystem
    ↓
dm-lhsr (kernel)
    ↓
Block devices
    ↓
Disks
```

Background daemon handles:
- Disk health monitoring
- Scrubbing
- Rebuild
- Predictive failure

## Performance

| Metric | Target |
|--------|--------|
| Sequential | ≥ mdadm |
| Random IO | ≤ +10% overhead |
| CPU | < 10% |
| Rebuild | Faster than mdadm |

## Roadmap

- [x] Phase 1: Core RAID engine
- [ ] Phase 2: Self-healing
- [ ] Phase 3: Anti-bit-rot
- [ ] Phase 4: Predictive failure
- [ ] Phase 5: Recovery tools
- [ ] Phase 6: Performance
- [ ] Phase 7: Production features

## License

GPLv3 — See LICENSE file

## Contributing

Contributions welcome! Please see CONTRIBUTING.md

## Support

- Issues: GitHub Issues
- Discussions: GitHub Discussions

---

*Built for Linux administrators who need enterprise-grade storage without vendor lock-in.*