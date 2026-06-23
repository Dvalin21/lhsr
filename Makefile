# LHSR Makefile
# Linux Hybrid Self-Healing RAID

.PHONY: all clean install modules userspace

# Directories
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
UID := $(shell id -u)

# Default target
all: lib modules userspace

# Build kernel modules
modules:
	$(MAKE) -C kernel/dm-lhsr KERNEL_DIR=$(KERNEL_DIR)

# Build userspace tools
userspace:
	$(MAKE) -C lib
	$(MAKE) -C userspace/cli
	$(MAKE) -C userspace/daemon
	$(MAKE) -C userspace/recovery

# Clean all
clean:
	$(MAKE) -C kernel/dm-lhsr clean
	$(MAKE) -C userspace/cli clean
	$(MAKE) -C userspace/daemon clean
	$(MAKE) -C userspace/recovery clean
	$(MAKE) -C lib clean
	rm -rf build/

# Install
install: lib modules userspace
	$(MAKE) -C kernel/dm-lhsr install
	cp userspace/cli/lhsrctl /usr/local/bin/lhsrctl
	cp userspace/cli/lhsrctl /usr/local/sbin/lhsrctl
	cp userspace/daemon/lhsrd /usr/local/bin/lhsrd
	cp userspace/daemon/lhsrd /usr/local/sbin/lhsrd
	cp userspace/recovery/lhsr-scan /usr/local/bin/lhsr-scan
	cp userspace/recovery/lhsr-scan /usr/local/sbin/lhsr-scan

# Uninstall
uninstall:
	rmmod dm-lhsr 2>/dev/null || true
	rm -f /usr/local/bin/lhsrctl /usr/local/sbin/lhsrctl
	rm -f /usr/local/bin/lhsrd /usr/local/sbin/lhsrd
	rm -f /usr/local/bin/lhsr-scan /usr/local/sbin/lhsr-scan

# Package
package:
	bash scripts/build_deb.sh
	bash scripts/build_rpm.sh

# Test
test-all: all
	@echo "Running loopback test suite (self-contained)..."
	@sudo tests/runner.sh

test: test-all

test-full: all
	@echo "Running ALL tests (loopback + hardware-dependent)..."
	@sudo tests/runner.sh --root-tests

test-list:
	@tests/runner.sh --list

test-list-all:
	@tests/runner.sh --root-tests --list

test-red: all
	@echo "Running RED-phase tests (expected failures)..."
	@sudo tests/runner.sh --red

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