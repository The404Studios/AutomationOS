# Power Management Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../include -fno-stack-protector -nostdlib -ffreestanding
AS = nasm
ASFLAGS = -f elf64

OBJS = power.o battery.o cpufreq.o cpuidle.o thermal.o display.o profile.o events.o

all: $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

.PHONY: all clean
