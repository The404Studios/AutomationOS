# userspace/init/Makefile - Build init process

CC = gcc
LD = ld

CFLAGS = -std=gnu11 -nostdlib -fno-builtin -fno-stack-protector \
         -fno-pic -fno-pie -mno-red-zone -Wall -O2
LDFLAGS = -nostdlib -static -n -no-pie

BUILD_DIR = ../../build/userspace/init
OBJ_DIR = $(BUILD_DIR)/obj

# Source files (no lib/syscall or start.asm - inline syscalls only)
OBJS = $(OBJ_DIR)/main.o

all: $(BUILD_DIR)/init

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_DIR)/main.o: main.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/init: $(OBJS) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) -e _start -T../userspace.ld $(OBJS) -o $@
	@echo "Built: $@"
	@file $@ || true

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
