# userspace/shell/Makefile - Build shell

CC = gcc
LD = ld

CFLAGS = -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
         -mno-red-zone -Wall -Wextra -O2 -I..

LDFLAGS = -nostdlib -static -n -no-pie -e _start -T../../userspace/userspace.ld

BUILD_DIR = ../../build/userspace/shell
LIBC = ../../build/userspace/libc/libc.a

OBJS = shell.o parser.o

all: $(BUILD_DIR)/shell

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/shell: $(addprefix $(BUILD_DIR)/, $(OBJS)) $(LIBC)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(addprefix $(BUILD_DIR)/, $(OBJS)) $(LIBC)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
