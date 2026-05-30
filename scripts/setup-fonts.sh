#!/bin/bash
# Font Setup Script for AutomationOS Desktop
# Downloads, builds, and installs font rendering library

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
FONT_DIR="$KERNEL_ROOT/userspace/lib/font"

echo "==============================================="
echo "AutomationOS Font Setup"
echo "==============================================="
echo ""

# Step 1: Download stb_truetype.h
echo "[1/4] Downloading stb_truetype.h..."
cd "$FONT_DIR"
if [ ! -f "stb_truetype.h" ]; then
    make download-stb
    echo "✓ Downloaded stb_truetype.h"
else
    echo "✓ stb_truetype.h already exists"
fi
echo ""

# Step 2: Download fonts
echo "[2/4] Downloading DejaVu fonts..."
if [ ! -f "fonts/DejaVuSans.ttf" ] || [ ! -f "fonts/DejaVuSansMono.ttf" ]; then
    bash DOWNLOAD_FONT.sh
    echo "✓ Downloaded DejaVu fonts"
else
    echo "✓ Fonts already downloaded"
fi
echo ""

# Step 3: Build font library
echo "[3/4] Building font library..."
make clean
make
echo "✓ Built libfont.a"
echo ""

# Step 4: Install to userspace
echo "[4/4] Installing to userspace..."
make install
echo "✓ Installed font.h and libfont.a"
echo ""

# Summary
echo "==============================================="
echo "Font Setup Complete!"
echo "==============================================="
echo ""
echo "Installed files:"
echo "  - $KERNEL_ROOT/userspace/include/font.h"
echo "  - $KERNEL_ROOT/userspace/lib/libfont.a"
echo "  - $FONT_DIR/fonts/DejaVuSans.ttf (~300KB)"
echo "  - $FONT_DIR/fonts/DejaVuSansMono.ttf (~300KB)"
echo ""
echo "Next steps:"
echo "  1. Update application Makefiles to link with -lfont"
echo "  2. Bundle fonts in initrd at /fonts/"
echo "  3. Call font_init() in application startup code"
echo ""
echo "Example initrd integration:"
echo "  mkdir -p initrd_root/fonts"
echo "  cp $FONT_DIR/fonts/*.ttf initrd_root/fonts/"
echo ""
