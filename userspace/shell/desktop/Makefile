# Desktop Shell Makefile

CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -g \
         -I../../lib/compositor_client \
         -I../../lib/ui \
         -I../../lib/font \
         -I../../lib/image
LDFLAGS = -lm -L../../../build/lib/image -limage

# Directories
SRC_DIR = .
BUILD_DIR = ../../../build/shell/desktop
OBJ_DIR = $(BUILD_DIR)/obj
LIB_OBJ_DIR = $(BUILD_DIR)/obj/lib

# Source files
SRCS = main.c \
       desktop_shell.c \
       panel.c \
       dock.c \
       desktop.c \
       overview.c \
       notifications.c \
       quick_settings.c \
       system_menu.c \
       launcher.c

# Library sources
LIB_SRCS = ../../lib/compositor_client/compositor_client.c \
           ../../lib/ui/draw.c

# Object files
OBJS = $(SRCS:%.c=$(OBJ_DIR)/%.o)
LIB_OBJS = $(LIB_SRCS:../../lib/%.c=$(LIB_OBJ_DIR)/%.o)

# Output
TARGET = $(BUILD_DIR)/desktop_shell

# Default target
all: $(TARGET)

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(LIB_OBJ_DIR):
	mkdir -p $(LIB_OBJ_DIR)
	mkdir -p $(LIB_OBJ_DIR)/compositor_client
	mkdir -p $(LIB_OBJ_DIR)/ui

# Build desktop shell executable
$(TARGET): $(OBJS) $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(OBJS) $(LIB_OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Built desktop shell: $(TARGET)"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c desktop_shell.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile library files
$(LIB_OBJ_DIR)/compositor_client/%.o: ../../lib/compositor_client/%.c | $(LIB_OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_OBJ_DIR)/ui/%.o: ../../lib/ui/%.c | $(LIB_OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned build directory"

# Install (copy to system location)
install: $(TARGET)
	@echo "Installing desktop shell..."
	install -D -m 755 $(TARGET) /usr/bin/desktop-shell
	install -D -m 644 ../../../etc/services/desktop-shell.service /etc/services/desktop-shell.service
	@echo "Desktop shell installed to /usr/bin/desktop-shell"

# Run desktop shell (for testing)
run: $(TARGET)
	@echo "Running desktop shell..."
	$(TARGET)

# Debug with GDB
debug: $(TARGET)
	gdb $(TARGET)

.PHONY: all clean install run debug
