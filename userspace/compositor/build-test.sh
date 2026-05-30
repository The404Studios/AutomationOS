#!/bin/bash
# Quick build test for compositor integration

set -e

echo "=== Compositor Integration Build Test ==="
echo ""

# Check if we're in the right directory
if [ ! -f "fb.c" ] || [ ! -f "main.c" ]; then
    echo "ERROR: Must run from userspace/compositor directory"
    exit 1
fi

echo "[1/3] Cleaning previous builds..."
make clean 2>/dev/null || true

echo ""
echo "[2/3] Building hosted compositor..."
if make hosted; then
    echo "✓ Hosted build successful"
else
    echo "✗ Hosted build failed"
    exit 1
fi

echo ""
echo "[3/3] Checking outputs..."

BUILD_DIR="../../build/userspace/compositor"

if [ -f "$BUILD_DIR/compositor-hosted" ]; then
    echo "✓ compositor-hosted exists"
    ls -lh "$BUILD_DIR/compositor-hosted"
else
    echo "✗ compositor-hosted not found"
    exit 1
fi

if [ -f "$BUILD_DIR/test-integration" ]; then
    echo "✓ test-integration exists"
    ls -lh "$BUILD_DIR/test-integration"
else
    echo "✗ test-integration not found"
    exit 1
fi

echo ""
echo "=== Build Test Complete ==="
echo ""
echo "To run integration test:"
echo "  $BUILD_DIR/test-integration"
echo ""
