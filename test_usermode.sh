#!/bin/bash
# test_usermode.sh - Test user mode switching

set -e

echo "========================================="
echo "User Mode Switch Test"
echo "========================================="
echo ""

# Check if build directory exists
if [ ! -d "build" ]; then
    echo "[ERROR] Build directory not found. Run 'make' first."
    exit 1
fi

# Check if kernel exists
if [ ! -f "build/kernel/kernel.elf" ]; then
    echo "[ERROR] Kernel not built. Run 'make' first."
    exit 1
fi

echo "[INFO] Checking compiled files..."
echo ""

# Check for usermode object files
echo "User mode implementation files:"
ls -lh build/kernel/arch/x86_64/usermode.o 2>/dev/null || echo "  [MISSING] usermode.asm"
ls -lh build/kernel/arch/x86_64/gdt.o 2>/dev/null || echo "  [MISSING] gdt.c"
ls -lh build/kernel/core/usermode.o 2>/dev/null || echo "  [MISSING] usermode.c"
ls -lh build/kernel/core/test_usermode.o 2>/dev/null || echo "  [MISSING] test_usermode.c"
ls -lh build/kernel/core/init_usermode.o 2>/dev/null || echo "  [MISSING] init_usermode.c"
echo ""

# Check for TSS symbols
echo "[INFO] Checking for TSS symbols in kernel..."
if nm build/kernel/kernel.elf | grep -i tss >/dev/null 2>&1; then
    echo "  [OK] TSS symbols found:"
    nm build/kernel/kernel.elf | grep -i tss | head -10
else
    echo "  [WARNING] No TSS symbols found"
fi
echo ""

# Check for usermode symbols
echo "[INFO] Checking for user mode symbols in kernel..."
if nm build/kernel/kernel.elf | grep -i usermode >/dev/null 2>&1; then
    echo "  [OK] User mode symbols found:"
    nm build/kernel/kernel.elf | grep -i usermode | head -10
else
    echo "  [WARNING] No user mode symbols found"
fi
echo ""

# Check GDT entries
echo "[INFO] Verifying GDT has user mode segments..."
if objdump -d build/kernel/kernel.elf | grep "0x1b\|0x23" >/dev/null 2>&1; then
    echo "  [OK] Found references to user segments (0x1B, 0x23)"
else
    echo "  [INFO] No direct references to user segments in disassembly"
fi
echo ""

echo "========================================="
echo "Build Verification Complete"
echo "========================================="
echo ""
echo "To test user mode switching:"
echo "  1. Build the kernel: make"
echo "  2. Run in QEMU: make qemu"
echo "  3. Watch for '[USERMODE]' messages in serial output"
echo ""
echo "Expected behavior:"
echo "  - TSS initialized with kernel stack"
echo "  - Switch to ring 3 (CPL=3)"
echo "  - User code executes syscall"
echo "  - Returns to kernel mode temporarily"
echo "  - Back to user mode"
echo ""
