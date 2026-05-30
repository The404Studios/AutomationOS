# ACPI Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../include -fno-stack-protector -nostdlib -ffreestanding
AS = nasm
ASFLAGS = -f elf64

OBJS = acpi.o

all: $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

.PHONY: all clean
