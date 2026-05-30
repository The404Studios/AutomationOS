# AutoFS Filesystem Makefile

CC = x86_64-elf-gcc
CFLAGS = -std=gnu11 -ffreestanding -nostdlib -nostdinc -mno-red-zone \
         -mcmodel=kernel -Wall -Wextra -O2 -I../../include -isystem ../../include/compat \
         -mno-sse -mno-mmx -mno-sse2
LDFLAGS = -shared

# Source files
SRCS = autofs.c journal.c cow.c snapshots.c compression.c \
       encryption.c xattr.c cache.c file.c dir.c

# Object files
OBJS = $(SRCS:.c=.o)

# Target library
TARGET = libautofs.so

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Built AutoFS library: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/lib/
	@echo "Installed $(TARGET) to /usr/lib/"

.PHONY: all clean install
