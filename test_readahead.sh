#!/bin/bash
#
# Test Read-Ahead Implementation
#
# This script builds the kernel with read-ahead support and runs the benchmark.
#

set -e

echo "=== AutomationOS Read-Ahead Test Script ==="
echo ""

# Navigate to kernel directory
cd "$(dirname "$0")"

echo "[1/6] Cleaning build..."
make clean > /dev/null 2>&1

echo "[2/6] Building kernel with read-ahead support..."
make kernel 2>&1 | grep -E "(error|warning|Building)" || true

echo "[3/6] Building read-ahead benchmark..."
cd userspace
x86_64-elf-gcc -static -nostdlib -fno-stack-protector \
    -o readahead_benchmark ../tests/readahead_benchmark.c 2>&1 | head -10 || true

# Copy to initrd
mkdir -p ../initrd/bin
cp readahead_benchmark ../initrd/bin/ 2>&1 || echo "Warning: Could not copy to initrd"

cd ..

echo "[4/6] Rebuilding initrd..."
bash scripts/mkinitrd.sh 2>&1 | grep -E "(Creating|Adding)" || true

echo "[5/6] Building ISO..."
python3 scripts/build-iso.py 2>&1 | grep -E "(Building|Writing)" || true

echo "[6/6] Starting QEMU..."
echo ""
echo "==========================================="
echo "  QEMU Starting"
echo "==========================================="
echo "Once AutomationOS boots, run:"
echo "  /bin/readahead_benchmark"
echo ""
echo "Expected output:"
echo "  - 1MB test file created"
echo "  - 2nd sequential read should be 2-4x faster"
echo "  - Speedup ratio displayed"
echo "==========================================="
echo ""

bash scripts/run-qemu.sh
