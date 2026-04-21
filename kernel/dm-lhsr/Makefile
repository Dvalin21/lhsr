# LHSR Device Mapper Makefile

obj-m += dm-lhsr.o
dm-lhsr-objs := dm_lhsr.o dm_lhsr_map.o dm_lhsr_io.o dm_lhsr_checksum.o

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build

# Build directory
PWD := $(shell pwd)

# Extra compiler flags
ccflags-y := -I$(PWD)/../include -DLHSR_VERSION='"1.0"'

# Default target
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# Help
help:
	@echo "LHSR Kernel Module Build"
	@echo "Requirements: kernel headers installed"
	@echo ""
	@echo "Usage:"
	@echo "  make              - Build module"
	@echo "  make clean        - Clean"
	@echo "  make install     - Install module"