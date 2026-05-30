# Image Library Makefile

CC = gcc
AR = ar
CFLAGS = -Wall -Wextra -std=c11 -O2 -g -fPIC
ARFLAGS = rcs

# Directories
SRC_DIR = .
BUILD_DIR = ../../../build/lib/image
OBJ_DIR = $(BUILD_DIR)/obj
INSTALL_DIR = /usr/lib

# Source files
SRCS = image.c

# Object files
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)

# Output
LIB_STATIC = $(BUILD_DIR)/libimage.a
LIB_SHARED = $(BUILD_DIR)/libimage.so
TEST_BIN = $(BUILD_DIR)/test_image

# Default target
all: $(LIB_STATIC) $(LIB_SHARED)

# Test target
test: $(TEST_BIN)

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Build static library
$(LIB_STATIC): $(OBJS) | $(BUILD_DIR)
	$(AR) $(ARFLAGS) $@ $(OBJS)
	@echo "Built static library: $@"

# Build shared library
$(LIB_SHARED): $(OBJS) | $(BUILD_DIR)
	$(CC) -shared -o $@ $(OBJS)
	@echo "Built shared library: $@"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c image.h stb_image.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build test program
$(TEST_BIN): test_image.c $(LIB_STATIC)
	$(CC) $(CFLAGS) test_image.c -o $@ -L$(BUILD_DIR) -limage -lm
	@echo "Built test program: $@"

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory"

# Install
install: $(LIB_STATIC) $(LIB_SHARED)
	@echo "Installing image library..."
	install -D -m 644 $(LIB_STATIC) $(INSTALL_DIR)/libimage.a
	install -D -m 755 $(LIB_SHARED) $(INSTALL_DIR)/libimage.so
	install -D -m 644 image.h /usr/include/image.h
	@echo "Image library installed"

# Run tests
run-test: $(TEST_BIN)
	@echo "Running image library tests..."
	$(TEST_BIN)

.PHONY: all clean install test run-test
