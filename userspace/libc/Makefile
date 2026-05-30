# userspace/libc/Makefile - Build userspace C library

CC = gcc
AR = ar

CFLAGS = -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
         -mno-red-zone -fno-stack-protector -Wall -Wextra -O2 -I.

BUILD_DIR = ../../build/userspace/libc

OBJS = syscall.o string.o stdio.o stdlib.o time.o dirent.o signal.o ipc.o math.o \
       sys_stat.o errno.o assert.o selftest.o

all: $(BUILD_DIR)/libc.a

$(BUILD_DIR)/libc.a: $(addprefix $(BUILD_DIR)/, $(OBJS))
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
