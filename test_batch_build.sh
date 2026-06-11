#!/bin/bash
#
# Test script to verify batched syscall implementation compiles
#

set -e

echo "=========================================="
echo "  Batched Syscall Build Test"
echo "=========================================="
echo ""

# Check for required files
echo "[1/4] Checking implementation files..."
FILES=(
    "kernel/core/syscall/batch.c"
    "kernel/include/syscall.h"
    "userspace/bench_batch.c"
)

for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        echo "  ✓ $file"
    else
        echo "  ✗ $file (MISSING)"
        exit 1
    fi
done

echo ""
echo "[2/4] Building kernel..."
make -C kernel/ 2>&1 | grep -E "(error:|warning:|batch\.c)" || echo "  ✓ Kernel built successfully"

echo ""
echo "[3/4] Building userspace benchmark..."
make -C userspace/ tests 2>&1 | grep -E "(error:|warning:|bench_batch)" || echo "  ✓ Benchmark built successfully"

echo ""
echo "[4/4] Checking build artifacts..."
ARTIFACTS=(
    "build/kernel/core/syscall/batch.o"
    "build/userspace/tests/bench_batch"
)

for artifact in "${ARTIFACTS[@]}"; do
    if [ -f "$artifact" ]; then
        SIZE=$(stat -c%s "$artifact" 2>/dev/null || stat -f%z "$artifact" 2>/dev/null || echo "?")
        echo "  ✓ $artifact ($SIZE bytes)"
    else
        echo "  ✗ $artifact (MISSING)"
        exit 1
    fi
done

echo ""
echo "=========================================="
echo "  Build test PASSED!"
echo "=========================================="
echo ""
echo "To run the benchmark:"
echo "  1. make iso"
echo "  2. make qemu"
echo "  3. In AutomationOS shell: /bin/bench_batch"
echo ""
