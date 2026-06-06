# LHSR Device Mapper Makefile

obj-m += dm-lhsr.o

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build

# Build directory
PWD := $(shell pwd)

# Extra compiler flags
# $(src) = kernel/dm-lhsr/; we need project root's include/ for <lhsr.h>
ccflags-y := -I$(src)/../../include

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