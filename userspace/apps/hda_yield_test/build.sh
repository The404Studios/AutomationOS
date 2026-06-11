#!/usr/bin/env bash
# =============================================================================
# build.sh - Build HDA Yield Test
# =============================================================================
#
# Builds the HDA yield behavior validation test as a freestanding ELF binary.
#
# Usage:
#   bash userspace/apps/hda_yield_test/build.sh
#
# Output:
#   build/hda_yield_test.elf
#
# =============================================================================

set -e  # Exit on error

PROJECT_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$PROJECT_ROOT"

echo "[BUILD] HDA Yield Test"
echo "======================"

# Ensure build directory exists
mkdir -p build

# Compile
echo "[1/2] Compiling hda_yield_test.c..."
gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -O2 \
    -c userspace/apps/hda_yield_test/hda_yield_test.c \
    -o /tmp/hda_yield_test.o

# Link
echo "[2/2] Linking hda_yield_test.elf..."
ld -nostdlib -static -n -no-pie -e _start \
    -T userspace/userspace.ld /tmp/hda_yield_test.o \
    -o build/hda_yield_test.elf

# Verify no stack canary
echo ""
echo "[VERIFY] Checking for stack canary (should be empty)..."
if objdump -d build/hda_yield_test.elf | grep -q 'fs:0x28'; then
    echo "WARNING: Stack canary detected! Binary may not work in freestanding environment."
    objdump -d build/hda_yield_test.elf | grep 'fs:0x28'
else
    echo "OK: No stack canary found."
fi

# Show binary info
echo ""
echo "[INFO] Binary information:"
ls -lh build/hda_yield_test.elf
file build/hda_yield_test.elf

echo ""
echo "[BUILD] Complete: build/hda_yield_test.elf"
echo ""
echo "Next steps:"
echo "  1. Add to initrd: cp build/hda_yield_test.elf <initrd_dir>/sbin/"
echo "  2. Run manually: /sbin/hda_yield_test"
echo "  3. Or integrate into smoke tests"
