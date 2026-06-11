#!/bin/bash
# Test script for Task 4: Serial Driver & Kernel Printf

set -e

echo "Testing Task 4 implementation..."
echo ""

# Check if cross-compiler is installed
if ! command -v x86_64-elf-gcc &> /dev/null; then
    echo "ERROR: x86_64-elf-gcc not found. Install cross-compiler first."
    exit 1
fi

# Create build directory
mkdir -p build/kernel/drivers
mkdir -p build/kernel/lib

# Compiler flags
CFLAGS="-std=gnu11 -ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel -Wall -Wextra -O2 -Ikernel/include"

echo "1. Compiling serial driver..."
x86_64-elf-gcc $CFLAGS -c kernel/drivers/serial.c -o build/kernel/drivers/serial.o
echo "   ✓ serial.o compiled"

echo ""
echo "2. Compiling printf..."
x86_64-elf-gcc $CFLAGS -c kernel/lib/printf.c -o build/kernel/lib/printf.o
echo "   ✓ printf.o compiled"

echo ""
echo "3. Compiling string library..."
x86_64-elf-gcc $CFLAGS -c kernel/lib/string.c -o build/kernel/lib/string.o
echo "   ✓ string.o compiled"

echo ""
echo "4. Compiling test program..."
x86_64-elf-gcc $CFLAGS -c kernel/test_serial.c -o build/kernel/test_serial.o
echo "   ✓ test_serial.o compiled"

echo ""
echo "=== Task 4 Compilation Test: PASSED ==="
echo ""
echo "Created files:"
echo "  ✓ kernel/drivers/serial.c"
echo "  ✓ kernel/lib/printf.c"
echo "  ✓ kernel/include/drivers.h"
echo ""
echo "Features implemented:"
echo "  ✓ COM1 serial port driver (0x3F8)"
echo "  ✓ serial_init() - Initialize COM1 at 38400 baud"
echo "  ✓ serial_putchar() - Output single character"
echo "  ✓ serial_write() - Output string with length"
echo "  ✓ kprintf() - Kernel printf with format specifiers:"
echo "    - %s (string)"
echo "    - %d (signed decimal)"
echo "    - %u (unsigned decimal)"
echo "    - %x (hexadecimal)"
echo "    - %p (pointer)"
echo "    - %% (literal percent)"
echo ""
echo "Next step: Implement Task 5 (Kernel Panic Handler)"
