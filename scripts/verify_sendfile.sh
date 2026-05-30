#!/bin/bash
#
# Verify sendfile() implementation compilation
#

echo "======================================"
echo "  sendfile() Implementation Verification"
echo "======================================"
echo ""

# Check if sendfile.c exists
echo "[CHECK] Verifying source files..."
if [ -f "kernel/core/syscall/sendfile.c" ]; then
    echo "✓ kernel/core/syscall/sendfile.c found"
else
    echo "✗ kernel/core/syscall/sendfile.c missing"
    exit 1
fi

if [ -f "userspace/test_sendfile.c" ]; then
    echo "✓ userspace/test_sendfile.c found"
else
    echo "✗ userspace/test_sendfile.c missing"
    exit 1
fi

# Check syscall number definitions
echo ""
echo "[CHECK] Verifying syscall definitions..."
if grep -q "SYS_SENDFILE.*71" kernel/include/syscall.h; then
    echo "✓ SYS_SENDFILE defined in kernel header"
else
    echo "✗ SYS_SENDFILE not defined in kernel header"
    exit 1
fi

if grep -q "SYS_SENDFILE.*71" userspace/libc/syscall.h; then
    echo "✓ SYS_SENDFILE defined in userspace header"
else
    echo "✗ SYS_SENDFILE not defined in userspace header"
    exit 1
fi

# Check syscall registration
echo ""
echo "[CHECK] Verifying syscall registration..."
if grep -q "syscall_table\[SYS_SENDFILE\].*=.*sys_sendfile" kernel/core/syscall/syscall.c; then
    echo "✓ sendfile registered in syscall table"
else
    echo "✗ sendfile not registered in syscall table"
    exit 1
fi

# Check function declarations
echo ""
echo "[CHECK] Verifying function declarations..."
if grep -q "sys_sendfile" kernel/include/syscall.h; then
    echo "✓ sys_sendfile declared in kernel header"
else
    echo "✗ sys_sendfile not declared in kernel header"
    exit 1
fi

if grep -q "sendfile" userspace/libc/unistd.h; then
    echo "✓ sendfile declared in userspace unistd.h"
else
    echo "✗ sendfile not declared in userspace unistd.h"
    exit 1
fi

# Try to compile sendfile.c in isolation
echo ""
echo "[CHECK] Attempting compilation check..."
x86_64-elf-gcc -std=gnu11 -ffreestanding -nostdlib -nostdinc -mno-red-zone \
    -mcmodel=kernel -Wall -Wextra -O2 -Ikernel/include -isystem kernel/include/compat \
    -fsyntax-only kernel/core/syscall/sendfile.c 2>&1

if [ $? -eq 0 ]; then
    echo "✓ sendfile.c compiles without errors"
else
    echo "✗ sendfile.c has compilation errors"
    exit 1
fi

echo ""
echo "======================================"
echo "  All Checks Passed!"
echo "======================================"
echo ""
echo "Implementation is ready for full build and test."
echo ""
echo "To build and test:"
echo "  1. make clean"
echo "  2. make all"
echo "  3. make qemu"
echo "  4. Run: /test_sendfile"
echo ""
