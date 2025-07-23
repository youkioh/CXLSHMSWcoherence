# Makefile for CXL SHM kernel modules

obj-m += cxl_shm.o
obj-m += test_cxl_sender.o

# Kernel source directory - adjust path as needed
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Build targets
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules_install

help:
	@echo "Available targets:"
	@echo "  all     - Build all kernel modules"
	@echo "  clean   - Clean build files"
	@echo "  install - Install modules to system"

.PHONY: all clean install help
