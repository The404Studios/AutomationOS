# Minimal Terminal Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2
LDFLAGS =

# Build directory
BUILD_DIR = ../../build/userspace/terminal

# Source files
SOURCES = main.c window.c font.c shell.c
OBJECTS = $(SOURCES:%.c=$(BUILD_DIR)/%.o)

# Target binary
TARGET = $(BUILD_DIR)/terminal

.PHONY: all clean directories

all: directories $(TARGET)

directories:
	@mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJECTS)
	@echo "Linking $@..."
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)
	@echo "Built terminal: $@"
	@ls -lh $@

$(BUILD_DIR)/%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@echo "Cleaning terminal build..."
	rm -rf $(BUILD_DIR)

# Dependencies
$(BUILD_DIR)/main.o: main.c window.h font.h shell.h
$(BUILD_DIR)/window.o: window.c window.h font.h
$(BUILD_DIR)/font.o: font.c font.h
$(BUILD_DIR)/shell.o: shell.c shell.h window.h
