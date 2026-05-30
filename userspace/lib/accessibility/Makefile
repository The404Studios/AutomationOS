# Accessibility Library Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lm

TARGET = libaccessibility.a
SOURCES = accessibility.c
OBJECTS = $(SOURCES:.c=.o)
HEADERS = accessibility.h

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	ar rcs $@ $(OBJECTS)
	@echo "Built $@"

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	@echo "Building accessibility validator..."
	$(CC) $(CFLAGS) ../../../tools/accessibility_validator.c $(TARGET) $(LDFLAGS) -o ../../../tools/accessibility_validator
	@echo "Running tests..."
	../../../tools/accessibility_validator

clean:
	rm -f $(OBJECTS) $(TARGET)
	rm -f ../../../tools/accessibility_validator
	@echo "Cleaned accessibility library"

install: $(TARGET)
	@echo "Installing accessibility library..."
	# Would install to system locations in production

.PHONY: help
help:
	@echo "Accessibility Library Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build library (default)"
	@echo "  test    - Build and run WCAG 2.1 tests"
	@echo "  clean   - Remove build artifacts"
	@echo "  install - Install library"
	@echo "  help    - Show this help"
