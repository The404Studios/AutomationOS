# AutomationOS IDE Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -I.
LDFLAGS =

# Source files
SOURCES = ide_main.c editor.c blueprint.c debugger.c project.c
OBJECTS = $(SOURCES:.c=.o)

# Target binary
TARGET = autoos-ide

# Build directory
BUILD_DIR = ../bin

# Build target
all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "[LD] Linking $@..."
	@$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)
	@echo "[OK] IDE built successfully"

%.o: %.c
	@echo "[CC] Compiling $<..."
	@$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "[CLEAN] Removing object files and binary..."
	@rm -f $(OBJECTS) $(TARGET)
	@echo "[OK] Clean complete"

# Install to userspace/bin
install: $(TARGET)
	@echo "[INSTALL] Installing $(TARGET) to $(BUILD_DIR)..."
	@mkdir -p $(BUILD_DIR)
	@cp $(TARGET) $(BUILD_DIR)/
	@echo "[OK] Installation complete"

# Run IDE
run: $(TARGET)
	./$(TARGET)

# Run with test project
test: $(TARGET)
	./$(TARGET) /test/project

# Build with debug symbols
debug: CFLAGS += -g -DDEBUG
debug: clean $(TARGET)

# Show help
help:
	@echo "AutomationOS IDE - Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build the IDE (default)"
	@echo "  clean   - Remove build artifacts"
	@echo "  install - Install to bin directory"
	@echo "  run     - Build and run the IDE"
	@echo "  test    - Build and run with test project"
	@echo "  debug   - Build with debug symbols"
	@echo "  help    - Show this help message"

# Dependencies
ide_main.o: ide_main.c ide.h editor.h blueprint.h debugger.h project.h ui.h
editor.o: editor.c editor.h ide.h
blueprint.o: blueprint.c blueprint.h ide.h
debugger.o: debugger.c debugger.h ide.h
project.o: project.c project.h ide.h

.PHONY: all clean install run test debug help
