#!/bin/bash
#
# Test script for minimal desktop shell
#

set -e

echo "======================================"
echo "  Minimal Shell Build Test"
echo "======================================"
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test directory structure
echo "[1/5] Checking directory structure..."
required_files=(
    "main.c"
    "shell.c"
    "shell.h"
    "desktop.c"
    "desktop.h"
    "taskbar.c"
    "taskbar.h"
    "launcher.c"
    "launcher.h"
    "render.c"
    "render.h"
    "Makefile"
)

for file in "${required_files[@]}"; do
    if [ -f "$file" ]; then
        echo "  ✓ $file"
    else
        echo -e "${RED}  ✗ Missing: $file${NC}"
        exit 1
    fi
done

echo ""

# Clean build
echo "[2/5] Cleaning previous build..."
make clean > /dev/null 2>&1 || true
echo "  ✓ Clean successful"
echo ""

# Build
echo "[3/5] Building minimal shell..."
if make -j$(nproc) 2>&1 | tee build.log; then
    echo -e "${GREEN}  ✓ Build successful${NC}"
else
    echo -e "${RED}  ✗ Build failed${NC}"
    cat build.log
    exit 1
fi
echo ""

# Check output
echo "[4/5] Verifying build output..."
TARGET="../../../build/shell/minimal/shell"

if [ -f "$TARGET" ]; then
    echo "  ✓ Binary exists: $TARGET"

    SIZE=$(stat -c%s "$TARGET" 2>/dev/null || stat -f%z "$TARGET" 2>/dev/null)
    echo "  ✓ Size: $SIZE bytes"

    if [ -x "$TARGET" ]; then
        echo "  ✓ Executable: yes"
    else
        echo -e "${RED}  ✗ Not executable${NC}"
        exit 1
    fi
else
    echo -e "${RED}  ✗ Binary not found${NC}"
    exit 1
fi
echo ""

# Code statistics
echo "[5/5] Code statistics..."
echo "  Source files:"
wc -l *.c *.h | tail -1
echo ""

# Summary
echo "======================================"
echo -e "${GREEN}  ✓ All tests passed!${NC}"
echo "======================================"
echo ""
echo "Next steps:"
echo "  1. Run: make run (for simulated mode)"
echo "  2. Run: sudo make install (to install)"
echo "  3. Run: /usr/bin/shell (after install)"
echo ""
