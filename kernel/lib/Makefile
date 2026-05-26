CC = x86_64-elf-gcc
CFLAGS = -std=gnu11 -ffreestanding -nostdlib -mno-red-zone \
         -mcmodel=kernel -Wall -Wextra -O2 -I../include

OBJS = string.o printf.o panic.o

all: $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o ../../build/$@

clean:
	rm -f ../../build/string.o ../../build/printf.o ../../build/panic.o

.PHONY: all clean
