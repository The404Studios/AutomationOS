# ABLoader Makefile
# Author: fourzerofour
#
# Targets:
#   make           - build abloader.img (uses build/kernel.elf if present)
#   make stub      - build abloader.img with a stub HLT-loop kernel (no real kernel needed)
#   make qemu      - run abloader.img in QEMU
#   make qemu-stub - build stub image and run in QEMU
#   make clean     - remove build artifacts

AS      = nasm
PYTHON  = python3
QEMU    = qemu-system-x86_64

ABLOADER_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
REPO_ROOT    := $(abspath $(ABLOADER_DIR)../..)
KERNEL_ELF   := $(REPO_ROOT)/build/kernel.elf

IMG     = $(ABLOADER_DIR)/abloader.img

.PHONY: all stub qemu qemu-stub clean

all: $(IMG)

$(IMG): stage1.bin stage2.bin
	@bash $(ABLOADER_DIR)/build.sh

stage1.bin: stage1.asm
	$(AS) -f bin -o stage1.bin stage1.asm
	@echo "  stage1.bin: $$(wc -c < stage1.bin) bytes"

stage2.bin: stage2.asm
	$(AS) -f bin -o stage2.bin stage2.asm
	@echo "  stage2.bin: $$(wc -c < stage2.bin) bytes"

stub:
	@echo "Building with stub kernel..."
	$(PYTHON) $(ABLOADER_DIR)/gen_stub_kernel.py $(ABLOADER_DIR)/kernel_stub.elf
	$(AS) -f bin -o $(ABLOADER_DIR)/stage1.bin $(ABLOADER_DIR)/stage1.asm
	$(AS) -f bin -o $(ABLOADER_DIR)/stage2.bin $(ABLOADER_DIR)/stage2.asm
	@# Build image directly (bypass kernel check in build.sh)
	@dd if=/dev/zero of=$(IMG) bs=512 count=20 2>/dev/null
	@dd if=$(ABLOADER_DIR)/stage1.bin of=$(IMG) bs=512 seek=0 count=1 conv=notrunc 2>/dev/null
	@dd if=$(ABLOADER_DIR)/stage2.bin of=$(IMG) bs=512 seek=1 count=8 conv=notrunc 2>/dev/null
	@dd if=$(ABLOADER_DIR)/kernel_stub.elf of=$(IMG) bs=512 seek=9 conv=notrunc 2>/dev/null
	@echo "Built: $(IMG) with stub kernel"

qemu: $(IMG)
	$(QEMU) \
	  -drive format=raw,file=$(IMG),if=ide,index=0 \
	  -m 256M \
	  -serial stdio \
	  -no-reboot \
	  -display sdl

qemu-stub: stub
	$(QEMU) \
	  -drive format=raw,file=$(IMG),if=ide,index=0 \
	  -m 256M \
	  -serial stdio \
	  -no-reboot \
	  -display sdl

qemu-headless: $(IMG)
	$(QEMU) \
	  -drive format=raw,file=$(IMG),if=ide,index=0 \
	  -m 256M \
	  -serial stdio \
	  -display none \
	  -no-reboot

clean:
	rm -f stage1.bin stage2.bin abloader.img kernel_stub.elf
