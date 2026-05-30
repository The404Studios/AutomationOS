# userspace/wm/Makefile - Build window manager with full integration

CC = gcc
LD = ld

CFLAGS = -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-pic -fno-pie \
         -mno-red-zone -Wall -Wextra -O2 -I.. -I../compositor

LDFLAGS = -nostdlib -static -n -T../../userspace/userspace.ld

BUILD_DIR = ../../build/userspace/wm
LIBC = ../../build/userspace/libc/libc.a

# Core window manager object files
OBJS = main.o \
       window_manager.o \
       window.o \
       focus.o \
       decorations.o \
       drag.o \
       resize.o \
       integration.o \
       input.o \
       ipc.o

# Optional: Minimal standalone version
MINIMAL_OBJS = wm_minimal.o

# Test program
TEST_OBJS = test_wm_integration.o \
            window_manager.o \
            window.o \
            focus.o \
            decorations.o \
            drag.o \
            resize.o \
            integration.o \
            input.o \
            ipc.o

all: $(BUILD_DIR)/wm

minimal: $(BUILD_DIR)/wm_minimal

test: $(BUILD_DIR)/test_wm_integration

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/wm: $(addprefix $(BUILD_DIR)/, $(OBJS)) $(LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -e _start $(addprefix $(BUILD_DIR)/, $(OBJS)) $(LIBC) -o $@
	@echo "Built full window manager: $@"

$(BUILD_DIR)/wm_minimal: $(addprefix $(BUILD_DIR)/, $(MINIMAL_OBJS)) $(LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -e _start $(addprefix $(BUILD_DIR)/, $(MINIMAL_OBJS)) $(LIBC) -o $@
	@echo "Built minimal window manager: $@"

$(BUILD_DIR)/test_wm_integration: $(addprefix $(BUILD_DIR)/, $(TEST_OBJS)) $(LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -e _start $(addprefix $(BUILD_DIR)/, $(TEST_OBJS)) $(LIBC) -o $@
	@echo "Built window manager integration test: $@"

install: $(BUILD_DIR)/wm
	@mkdir -p ../../initrd/bin
	cp $(BUILD_DIR)/wm ../../initrd/bin/
	@echo "Installed window manager to initrd"

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all minimal test install clean