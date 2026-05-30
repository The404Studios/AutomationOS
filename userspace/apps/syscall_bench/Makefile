# Syscall Benchmark Makefile
TARGET = syscall_bench
OBJS = syscall_bench.o

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -fno-stack-protector -nostdlib -nostartfiles -static
LDFLAGS = -static

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
