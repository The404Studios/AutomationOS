# Filesystem Layer Makefile

CC = x86_64-elf-gcc
CFLAGS = -std=gnu11 -ffreestanding -nostdlib -nostdinc -mno-red-zone \
         -mcmodel=kernel -Wall -Wextra -O2 -I../include -isystem ../include/compat

# Object files
OBJS = vfs.o elf_loader.o exec.o elf_test.o

all: $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o

.PHONY: all clean
