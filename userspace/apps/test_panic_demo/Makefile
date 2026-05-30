# Makefile for test_panic_demo
# Demonstrates enhanced kernel panic handler

CC = gcc
CFLAGS = -Wall -Wextra -O2 -fno-omit-frame-pointer
TARGET = test_panic_demo

all: $(TARGET)

$(TARGET): test_panic_demo.c
	$(CC) $(CFLAGS) -o $(TARGET) test_panic_demo.c

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
