# Font Rendering Library Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -std=c11 -I.
LDFLAGS = -lm

# Source files
SRCS = ttf_parser.c rasterizer.c cache.c
OBJS = $(SRCS:.c=.o)

# Test program
TEST_SRCS = test_font.c
TEST_OBJS = $(TEST_SRCS:.c=.o)
TEST_BIN = test_font

# Library output
LIB = libfont.a

.PHONY: all clean test download-stb check-stb

all: check-stb $(LIB)

# Check if stb_truetype.h exists
check-stb:
	@if [ ! -f stb_truetype.h ]; then \
		echo "ERROR: stb_truetype.h not found"; \
		echo ""; \
		echo "Please download it by running:"; \
		echo "  make download-stb"; \
		echo ""; \
		echo "Or manually download from:"; \
		echo "  https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h"; \
		echo ""; \
		exit 1; \
	fi

# Download stb_truetype.h
download-stb:
	@echo "Downloading stb_truetype.h..."
	@curl -L -o stb_truetype.h \
		"https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h"
	@if [ -f stb_truetype.h ]; then \
		echo "✓ Downloaded stb_truetype.h"; \
		echo "License: Public Domain / MIT (Sean Barrett)"; \
	else \
		echo "✗ Download failed"; \
		exit 1; \
	fi

# Build library
$(LIB): $(OBJS)
	ar rcs $@ $^
	@echo "Built $@"

# Build object files
%.o: %.c font.h font_internal.h
	$(CC) $(CFLAGS) -c $< -o $@

# Build test program
$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Built $@"

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)
	@echo ""
	@echo "Test images saved:"
	@ls -lh font_test_*.ppm

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TEST_OBJS) $(LIB) $(TEST_BIN)
	rm -f font_test_*.ppm

# Clean everything including downloaded files
distclean: clean
	rm -f stb_truetype.h

# Install to system (for userspace integration)
install: $(LIB)
	@echo "Installing font library..."
	mkdir -p ../../include
	cp font.h ../../include/
	mkdir -p ../../lib
	cp $(LIB) ../../lib/
	@echo "✓ Installed to userspace/include and userspace/lib"

# Help
help:
	@echo "Font Rendering Library Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build library (checks for stb_truetype.h)"
	@echo "  make download-stb - Download stb_truetype.h from GitHub"
	@echo "  make test         - Build and run test program"
	@echo "  make install      - Install library to userspace"
	@echo "  make clean        - Remove build artifacts"
	@echo "  make distclean    - Remove all generated files"
	@echo ""
	@echo "Requirements:"
	@echo "  - GCC or compatible C compiler"
	@echo "  - stb_truetype.h (run 'make download-stb')"
	@echo "  - curl (for downloading stb_truetype.h)"
	@echo ""
	@echo "Testing:"
	@echo "  - Provide DejaVuSans.ttf in current directory, or"
	@echo "  - Have a TrueType font in /usr/share/fonts or C:\\Windows\\Fonts"
