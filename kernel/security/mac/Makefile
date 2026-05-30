# Makefile for MAC (Mandatory Access Control) Subsystem

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -fno-stack-protector
CFLAGS += -ffreestanding -nostdlib -nostdinc
CFLAGS += -I../../include
CFLAGS += -mcmodel=kernel -mno-red-zone
CFLAGS += -fno-pic -fno-pie

# Source files
SOURCES = label.c policy.c enforce.c audit.c
OBJECTS = $(SOURCES:.c=.o)

# Output
TARGET = mac.a

# Build rules
all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "  AR      $@"
	@ar rcs $@ $^

%.o: %.c
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "  CLEAN   MAC subsystem"
	@rm -f $(OBJECTS) $(TARGET)

# Dependencies
label.o: label.c ../../include/mac.h ../../include/kernel.h ../../include/mem.h ../../include/sched.h
policy.o: policy.c ../../include/mac.h ../../include/kernel.h ../../include/mem.h
enforce.o: enforce.c ../../include/mac.h ../../include/kernel.h ../../include/sched.h ../../include/mem.h
audit.o: audit.c ../../include/mac.h ../../include/kernel.h ../../include/mem.h ../../include/sched.h

.PHONY: all clean
