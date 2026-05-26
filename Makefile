# AutomationOS Build System
.PHONY: all clean bootloader kernel userspace iso qemu

# Toolchain
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
AS = nasm
PYTHON = python3

# Directories
BUILD_DIR = build
ISO_DIR = iso

# Targets
all: bootloader kernel userspace iso

bootloader:
	$(MAKE) -C boot/

kernel:
	$(MAKE) -C kernel/

userspace:
	$(MAKE) -C userspace/

iso: bootloader kernel userspace
	$(PYTHON) scripts/build-iso.py

qemu: iso
	bash scripts/run-qemu.sh

qemu-debug: iso
	bash scripts/run-qemu.sh --debug

clean:
	$(MAKE) -C boot/ clean
	$(MAKE) -C kernel/ clean
	$(MAKE) -C userspace/ clean
	rm -rf $(BUILD_DIR) $(ISO_DIR)

.PHONY: all clean bootloader kernel userspace iso qemu qemu-debug
