CC = gcc
LD = ld

CFLAGS = -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
         -mno-red-zone -Wall -O2 -I.. -fno-stack-protector -fno-pic -fno-pie

LDFLAGS = -nostdlib -static -n -no-pie -e _start -T../userspace.ld

BUILD_DIR = ../../build/userspace/llm
LIBC = ../../build/userspace/libc/libc.a

OBJS = main.o llm.o

all: $(BUILD_DIR)/llm

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/llm: $(addprefix $(BUILD_DIR)/, $(OBJS)) $(LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(addprefix $(BUILD_DIR)/, $(OBJS)) $(LIBC)
	@echo "Built: $@"
	@file $@ || true

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
