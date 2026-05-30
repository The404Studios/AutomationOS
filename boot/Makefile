AS = nasm
CC = gcc
LD = ld
OBJCOPY = objcopy

BUILD_DIR = ../build

all: $(BUILD_DIR)/BOOTX64.EFI

$(BUILD_DIR)/boot.o: boot.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/loader.o: loader.c
	@mkdir -p $(dir $@)
	$(CC) -ffreestanding -nostdlib -fno-stack-protector \
	      -mno-red-zone -c $< -o $@

# Link as ELF first, then convert to PE/COFF for UEFI
$(BUILD_DIR)/boot.elf: $(BUILD_DIR)/boot.o $(BUILD_DIR)/loader.o
	@mkdir -p $(dir $@)
	$(LD) -T boot.ld -nostdlib $(BUILD_DIR)/boot.o $(BUILD_DIR)/loader.o \
	      -o $@

# Convert ELF to PE/COFF format required by UEFI firmware
$(BUILD_DIR)/BOOTX64.EFI: $(BUILD_DIR)/boot.elf
	@mkdir -p $(dir $@)
	$(OBJCOPY) -j .text -j .rodata -j .data -j .bss \
	    --target efi-app-x86_64 \
	    --subsystem=10 \
	    $(BUILD_DIR)/boot.elf $(BUILD_DIR)/BOOTX64.EFI
	@echo "  PE/COFF UEFI binary: $(BUILD_DIR)/BOOTX64.EFI"

clean:
	rm -f $(BUILD_DIR)/boot.o $(BUILD_DIR)/loader.o \
	      $(BUILD_DIR)/boot.elf $(BUILD_DIR)/BOOTX64.EFI

.PHONY: all clean
