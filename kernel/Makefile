CC = x86_64-elf-gcc
AS = nasm
LD = x86_64-elf-ld

CFLAGS = -std=gnu11 -ffreestanding -nostdlib -nostdinc -mno-red-zone \
         -mcmodel=kernel -Wall -Wextra -O2 -Iinclude -isystem include/compat \
         -DCONFIG_SMP=1
ASFLAGS = -f elf64
LDFLAGS = -T linker.ld -nostdlib -z max-page-size=0x1000

BUILD_DIR = ../build

# Source files (including lib/)
C_SOURCES = $(shell find . -name "*.c")
ASM_SOURCES = $(shell find . -name "*.asm")

# Object files
C_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS = $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))

all: $(BUILD_DIR)/kernel.elf

$(BUILD_DIR)/kernel.elf: $(C_OBJECTS) $(ASM_OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

clean:
	rm -rf $(BUILD_DIR)/kernel

.PHONY: all clean
