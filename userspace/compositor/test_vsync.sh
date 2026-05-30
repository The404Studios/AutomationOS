#!/bin/bash
# Quick test script for VSync and double-buffering implementation

set -e

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  AutomationOS VSync & Double-Buffering Test Suite           ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Build everything
echo "Step 1: Building..."
echo "─────────────────────────────────────────────────────────────"
make clean -f Makefile.fb 2>&1 | grep -v "No such file"
make all tests demos -f Makefile.fb

if [ $? -eq 0 ]; then
    echo "✓ Build successful"
else
    echo "✗ Build failed"
    exit 1
fi

echo ""
echo "Step 2: Running automated benchmark..."
echo "─────────────────────────────────────────────────────────────"

# Check if binaries exist
if [ ! -f "../../build/userspace/compositor/test_vsync_benchmark" ]; then
    echo "✗ Benchmark binary not found"
    exit 1
fi

# Run benchmark
../../build/userspace/compositor/test_vsync_benchmark

echo ""
echo "Step 3: Visual testing instructions"
echo "─────────────────────────────────────────────────────────────"
echo ""
echo "To run visual tests, use these commands:"
echo ""
echo "  # Test WITH VSync (no tearing expected):"
echo "  make demo-vsync -f Makefile.fb"
echo ""
echo "  # Test WITHOUT VSync (tearing visible):"
echo "  make demo-no-vsync -f Makefile.fb"
echo ""
echo "Compare the two runs to see the difference."
echo "With VSync ON, windows should move smoothly with no tearing."
echo "With VSync OFF, you should see horizontal tearing artifacts."
echo ""

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Test Suite Complete                                         ║"
echo "╚══════════════════════════════════════════════════════════════╝"
