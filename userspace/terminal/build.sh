#!/bin/bash
# Build script for minimal terminal

set -e

echo "=== Building Minimal Terminal ==="

# Create build directory
mkdir -p ../../build/userspace/terminal

# Compile sources
echo "Compiling main.c..."
gcc -Wall -Wextra -std=c11 -O2 -c main.c -o ../../build/userspace/terminal/main.o

echo "Compiling window.c..."
gcc -Wall -Wextra -std=c11 -O2 -c window.c -o ../../build/userspace/terminal/window.o

echo "Compiling font.c..."
gcc -Wall -Wextra -std=c11 -O2 -c font.c -o ../../build/userspace/terminal/font.o

echo "Compiling shell.c..."
gcc -Wall -Wextra -std=c11 -O2 -c shell.c -o ../../build/userspace/terminal/shell.o

# Link
echo "Linking terminal..."
gcc -o ../../build/userspace/terminal/terminal \
    ../../build/userspace/terminal/main.o \
    ../../build/userspace/terminal/window.o \
    ../../build/userspace/terminal/font.o \
    ../../build/userspace/terminal/shell.o

echo ""
echo "✓ Build successful!"
echo "Binary: ../../build/userspace/terminal/terminal"
ls -lh ../../build/userspace/terminal/terminal
echo ""
echo "Run with: ../../build/userspace/terminal/terminal"
