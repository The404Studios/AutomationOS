AS = nasm
CC = x86_64-elf-gcc
LD = x86_64-elf-ld

BUILD_DIR = ../build

all: $(BUILD_DIR)/BOOTX64.EFI

$(BUILD_DIR)/boot.o: boot.asm
	$(AS) -f elf64 $< -o $@

$(BUILD_DIR)/loader.o: loader.c
	$(CC) -ffreestanding -nostdlib -fno-stack-protector \
	      -mno-red-zone -c $< -o $@

$(BUILD_DIR)/BOOTX64.EFI: $(BUILD_DIR)/boot.o $(BUILD_DIR)/loader.o
	$(LD) -T boot.ld -nostdlib $(BUILD_DIR)/boot.o $(BUILD_DIR)/loader.o \
	      -o $@

clean:
	rm -f $(BUILD_DIR)/boot.o $(BUILD_DIR)/loader.o $(BUILD_DIR)/BOOTX64.EFI

.PHONY: all clean
