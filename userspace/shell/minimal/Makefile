# Minimal Desktop Shell Makefile

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -g
LDFLAGS = -lm

# Directories
SRC_DIR = .
BUILD_DIR = ../../../build/shell/minimal
OBJ_DIR = $(BUILD_DIR)/obj

# Source files
SRCS = main.c \
       shell.c \
       desktop.c \
       taskbar.c \
       launcher.c \
       render.c

# Object files
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)

# Output
TARGET = $(BUILD_DIR)/shell

# Default target
all: $(TARGET)

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Build shell executable
$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Built minimal shell: $(TARGET)"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory"

# Install (copy to system location)
install: $(TARGET)
	@echo "Installing minimal shell..."
	install -D -m 755 $(TARGET) /usr/bin/shell
	@echo "Shell installed to /usr/bin/shell"

# Run shell (for testing)
run: $(TARGET)
	@echo "Running minimal shell..."
	$(TARGET)

# Debug with GDB
debug: $(TARGET)
	gdb $(TARGET)

# Test build
test: all
	@echo "Build test passed"
	@ls -lh $(TARGET)

.PHONY: all clean install run debug test
