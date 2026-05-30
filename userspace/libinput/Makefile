# libinput Makefile

CC = x86_64-elf-gcc
AR = x86_64-elf-ar

CFLAGS = -std=c11 -Wall -Wextra -O2 -I. -ffreestanding -nostdlib -mno-red-zone

SOURCES = event.c
OBJECTS = $(SOURCES:.c=.o)
LIBRARY = libinput.a

all: $(LIBRARY)

$(LIBRARY): $(OBJECTS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(LIBRARY)

.PHONY: all clean
