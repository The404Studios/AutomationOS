# Makefile for ld.so dynamic linker

CC = x86_64-elf-gcc
LD = x86_64-elf-ld
AR = x86_64-elf-ar

# Compiler flags for position-independent code
CFLAGS = -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
         -mno-red-zone -fno-stack-protector -Wall -Wextra -O2 \
         -fPIC -fpie -I../../kernel/include -I.

# Linker flags for shared object
LDFLAGS = -nostdlib -shared -Bsymbolic --hash-style=gnu \
          -z max-page-size=4096 -z common-page-size=4096

BUILD_DIR = ../../build/userspace/ld.so
OUTPUT = $(BUILD_DIR)/ld-linux-x86-64.so.2

# Source files
SRCS = linker.c elf_dyn.c symbol_resolution.c relocation.c plt_got.c dlopen.c ld_main.c
OBJS = $(addprefix $(BUILD_DIR)/, $(SRCS:.c=.o))

.PHONY: all clean install

all: $(OUTPUT)

$(OUTPUT): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "Built ld.so: $@"

$(BUILD_DIR)/%.o: %.c linker.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

install: $(OUTPUT)
	@mkdir -p ../../initrd/lib64
	cp $(OUTPUT) ../../initrd/lib64/
	@echo "Installed ld.so to initrd/lib64/"

# Show build info
info:
	@echo "Dynamic Linker Build Configuration"
	@echo "==================================="
	@echo "CC:       $(CC)"
	@echo "LD:       $(LD)"
	@echo "CFLAGS:   $(CFLAGS)"
	@echo "LDFLAGS:  $(LDFLAGS)"
	@echo "OUTPUT:   $(OUTPUT)"
	@echo "SOURCES:  $(SRCS)"
