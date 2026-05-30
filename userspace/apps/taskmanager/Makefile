# Makefile for Task Manager  (windowed, freestanding, ring 3)
# Build pattern mirrors userspace/apps/sysmon/sysmon.c

CC  = gcc
LD  = ld

# All flags match the project build rules (no libc, no stack protector,
# no PIC/PIE, no red-zone, -mstackrealign to keep 16-byte alignment).
CFLAGS = -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
         -fno-stack-protector -fno-pic -fno-pie \
         -mno-red-zone -mstackrealign -O2 \
         -I ../..

LDFLAGS = -nostdlib -static -n -no-pie -e _start \
          -T ../../userspace.ld

BUILD_DIR = ../../../build/userspace/apps/taskmanager

TARGET  = $(BUILD_DIR)/taskmanager

# Shared libs (same set as sysmon)
UI_O    = $(BUILD_DIR)/ui.o
WLC_O   = $(BUILD_DIR)/wl_client.o
BF_O    = $(BUILD_DIR)/bitfont.o
TM_O    = $(BUILD_DIR)/taskmanager.o

OBJS    = $(TM_O) $(UI_O) $(WLC_O) $(BF_O)

.PHONY: all clean check-canary

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(TM_O): taskmanager.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(UI_O): ../../lib/ui/ui.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(WLC_O): ../../lib/wl/wl_client.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BF_O): ../../lib/font/bitfont.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Verify zero stack-canary references (must produce no output)
check-canary: $(TARGET)
	@echo "--- Checking for fs:0x28 stack canary (must be empty) ---"
	@objdump -d $(TARGET) | grep 'fs:0x28' || echo "  PASS: no fs:0x28 found"

clean:
	rm -rf $(BUILD_DIR)
