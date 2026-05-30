# Userspace Audio Library Makefile

CC = x86_64-elf-gcc
AR = x86_64-elf-ar
CFLAGS = -Wall -Wextra -O2 -ffreestanding -nostdlib -I.

OBJS = audio.o

all: libaudio.a

libaudio.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

audio.o: audio.c audio.h
	$(CC) $(CFLAGS) -c audio.c -o audio.o

clean:
	rm -f *.o *.a

install: libaudio.a
	mkdir -p ../../build/lib
	cp libaudio.a ../../build/lib/
	mkdir -p ../../build/include
	cp audio.h ../../build/include/

.PHONY: all clean install
