# AutomationOS Settings Application Makefile

CC = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -I../../libc -I../../shell/desktop
LDFLAGS = -L../../libc -L../../compositor
LIBS = -lc -lcompositor

# Directories
BUILD_DIR = ../../../build/userspace/apps/settings
OBJ_DIR = $(BUILD_DIR)/obj
BIN_DIR = $(BUILD_DIR)/bin

# Source files
SRCS = settings.c \
       panels.c \
       render.c \
       system_integration.c \
       main.c

OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(SRCS))

# Target binary
TARGET = $(BIN_DIR)/settings

# Default target
all: $(TARGET)

# Create directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Compile source files
$(OBJ_DIR)/%.o: %.c settings.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Link binary
$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $(TARGET)
	@echo "Built settings application: $(TARGET)"

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Install to system
install: $(TARGET)
	install -D $(TARGET) /usr/bin/settings
	@echo "Installed settings to /usr/bin/settings"

# Run the application
run: $(TARGET)
	$(TARGET)

.PHONY: all clean install run
