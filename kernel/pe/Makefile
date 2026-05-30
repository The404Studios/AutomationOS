# PE Loader Makefile
# Builds Windows PE loading and compatibility layer

CC := x86_64-elf-gcc
CFLAGS := -std=gnu11 -ffreestanding -nostdlib -nostdinc -mno-red-zone \
          -mcmodel=kernel -Wall -Wextra -I../include -isystem ../include/compat \
          -O2 -fno-strict-aliasing -mno-sse -mno-mmx -mno-sse2
LDFLAGS :=

PE_SOURCES := \
	pe_loader.c \
	dll_loader.c \
	handles.c \
	win32_init.c \
	win32_kernel32.c \
	win32_user32.c \
	win32_gdi32.c \
	registry.c

PE_OBJECTS := $(PE_SOURCES:.c=.o)
PE_LIB := libpe.a

.PHONY: all clean

all: $(PE_LIB)

$(PE_LIB): $(PE_OBJECTS)
	@echo "  AR      $@"
	@ar rcs $@ $^

%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "  CLEAN   pe"
	@rm -f $(PE_OBJECTS) $(PE_LIB)

# Dependencies
pe_loader.o: pe_loader.c ../include/pe_loader.h ../include/process.h
dll_loader.o: dll_loader.c ../include/pe_loader.h
handles.o: handles.c ../include/pe_win32.h
win32_init.o: win32_init.c ../include/pe_win32.h ../include/pe_loader.h
win32_kernel32.o: win32_kernel32.c ../include/pe_win32.h
win32_user32.o: win32_user32.c ../include/pe_win32.h
win32_gdi32.o: win32_gdi32.c ../include/pe_win32.h
registry.o: registry.c ../include/pe_win32.h
