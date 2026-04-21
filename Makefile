# LHSR Makefile
# Linux Hybrid Self-Healing RAID

.PHONY: all clean install modules userspace

# Directories
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
UID := $(shell id -u)

# Default target
all: modules userspace

# Build kernel modules
modules:
	$(MAKE) -C kernel/dm-lhsr KERNEL_DIR=$(KERNEL_DIR)

# Build userspace tools
userspace:
	$(MAKE) -C userspace/cli
	$(MAKE) -C userspace/daemon
	$(MAKE) -C userspace/recovery
	$(MAKE) -C lib

# Clean all
clean:
	$(MAKE) -C kernel/dm-lhsr clean
	$(MAKE) -C userspace/cli clean
	$(MAKE) -C userspace/daemon clean
	$(MAKE) -C userspace/recovery clean
	$(MAKE) -C lib clean
	rm -rf build/

# Install
install: modules
	$(MAKE) -C kernel/dm-lhsr install
	cp -r userspace/cli/lhsrctl /usr/local/bin/
	cp -r userspace/daemon/lhsrd /usr/local/bin/
	cp -r userspace/recovery/lhsr-scan /usr/local/bin/

# Uninstall
uninstall:
	rmmod dm-lhsr 2>/dev/null || true
	rm -f /usr/local/bin/lhsrctl
	rm -f /usr/local/bin/lhsrd
	rm -f /usr/local/bin/lhsr-scan

# Package
package:
	bash scripts/build_deb.sh
	bash scripts/build_rpm.sh

# Test
test: all
	@echo "Running tests..."
	@test -x tests/unit/lhsr-test && ./tests/unit/lhsr-test || echo "Build tests first"

# Help
help:
	@echo "LHSR Build Targets:"
	@echo "  all       - Build kernel modules and userspace"
	@echo "  modules   - Build kernel modules only"
	@echo "  userspace - Build userspace tools only"
	@echo "  clean     - Clean all build artifacts"
	@echo "  install   - Install to system"
	@echo "  uninstall - Remove from system"
	@echo "  package   - Create distribution packages"
	@echo "  test      - Run tests"
	@echo ""
	@echo "Requirements:"
	@echo "  make kernel-debs (Debian/Ubuntu)"
	@echo "  make kernel-rpms (RHEL/CentOS)"