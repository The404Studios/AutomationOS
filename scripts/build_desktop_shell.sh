#!/bin/bash
#
# Build Desktop Shell
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "========================================="
echo "  Building Desktop Shell"
echo "========================================="

# Build desktop shell components
cd "$PROJECT_ROOT/userspace/shell/desktop"

echo "[1/3] Cleaning build artifacts..."
make clean

echo "[2/3] Building desktop shell..."
make -j$(nproc)

echo "[3/3] Verifying build..."
if [ -f ../../../build/shell/desktop/desktop_shell ]; then
    echo "✓ Desktop shell binary created"
    ls -lh ../../../build/shell/desktop/desktop_shell
else
    echo "✗ ERROR: Desktop shell binary not found"
    exit 1
fi

echo ""
echo "========================================="
echo "  Desktop Shell Build Complete!"
echo "========================================="
echo ""
echo "To install:"
echo "  sudo make install"
echo ""
echo "To test:"
echo "  ../../../build/shell/desktop/desktop_shell --help"
echo ""
