# PTY Driver Makefile

CC = x86_64-elf-gcc
AR = x86_64-elf-ar
CFLAGS = -ffreestanding -nostdlib -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -Wall -Wextra -O2 -I../../include

OBJS = pty.o pty_dev.o

all: libpty.a

pty.o: pty.c pty.h
	$(CC) $(CFLAGS) -c pty.c -o pty.o

pty_dev.o: pty_dev.c pty.h
	$(CC) $(CFLAGS) -c pty_dev.c -o pty_dev.o

libpty.a: $(OBJS)
	$(AR) rcs libpty.a $(OBJS)

clean:
	rm -f *.o libpty.a

.PHONY: all clean
