# Audio Subsystem Makefile

CC = x86_64-elf-gcc
AR = x86_64-elf-ar
CFLAGS = -Wall -Wextra -Werror -O2 -ffreestanding -nostdlib -mcmodel=kernel \
         -mno-red-zone -fno-builtin -fno-stack-protector -I../../include

OBJS = audio_core.o

all: audio.a

audio.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

audio_core.o: audio_core.c ../../include/audio.h ../../include/hda.h
	$(CC) $(CFLAGS) -c audio_core.c -o audio_core.o

clean:
	rm -f *.o *.a

.PHONY: all clean
