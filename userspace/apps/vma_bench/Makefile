CC = x86_64-elf-gcc
AS = nasm
LD = x86_64-elf-ld

CFLAGS = -std=gnu11 -ffreestanding -nostdlib -mno-red-zone -O2 \
         -I../../include -Wall -Wextra

BUILD_DIR = ../../../build/userspace/apps/vma_bench
BIN_NAME = vma_bench

all: $(BUILD_DIR)/$(BIN_NAME)

$(BUILD_DIR)/$(BIN_NAME): vma_bench.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c vma_bench.c -o $(BUILD_DIR)/vma_bench.o
	$(LD) -T ../../userspace.ld -o $@ $(BUILD_DIR)/vma_bench.o

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
