# AutomationOS Terminal Emulator Makefile

# Compiler and flags
CC = gcc
LD = ld
CFLAGS = -Wall -Wextra -std=gnu11 -ffreestanding -nostdlib -fno-builtin -mno-red-zone
CFLAGS += -I../../../userspace/libc -I../../../userspace/compositor
LDFLAGS = -nostdlib -static -n -no-pie -T../../../userspace/userspace.ld

# Directories
BUILD_DIR = ../../../build/userspace/apps/terminal
LIBC_DIR = ../../../build/userspace/libc
COMPOSITOR_DIR = ../../../build/userspace/compositor

# Source files
SOURCES = terminal.c \
          vt_parser.c \
          renderer.c \
          tabs.c \
          profiles.c \
          buffer.c \
          utils.c

COMPOSITOR_SOURCES = \
          ../../../userspace/compositor/gpu.c

# Object files
OBJECTS = $(SOURCES:%.c=$(BUILD_DIR)/%.o)
COMPOSITOR_OBJECTS = $(patsubst ../../../userspace/compositor/%.c,$(BUILD_DIR)/compositor_%.o,$(COMPOSITOR_SOURCES))

# Output binary
TARGET = $(BUILD_DIR)/terminal

# Dependencies
LIBC = $(LIBC_DIR)/libc.a

.PHONY: all clean directories

all: directories $(TARGET)

directories:
	@mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS) $(COMPOSITOR_OBJECTS) $(LIBC)
	@echo "Linking $@..."
	$(LD) $(LDFLAGS) -e _start -o $@ $(OBJECTS) $(COMPOSITOR_OBJECTS) $(LIBC)
	@echo "Built terminal: $@"
	@ls -lh $@

$(BUILD_DIR)/%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/compositor_%.o: ../../../userspace/compositor/%.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning terminal build..."
	rm -rf $(BUILD_DIR)

# Dependencies (generated with gcc -MM)
$(BUILD_DIR)/terminal.o: terminal.c terminal.h
$(BUILD_DIR)/vt_parser.o: vt_parser.c terminal.h
$(BUILD_DIR)/renderer.o: renderer.c terminal.h
$(BUILD_DIR)/tabs.o: tabs.c terminal.h
$(BUILD_DIR)/profiles.o: profiles.c terminal.h
$(BUILD_DIR)/buffer.o: buffer.c terminal.h
$(BUILD_DIR)/utils.o: utils.c terminal.h
