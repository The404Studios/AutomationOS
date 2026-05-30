#!/bin/bash
#
# Build and Install Image Library
#

set -e

echo "========================================="
echo "AutomationOS Image Library Build Script"
echo "========================================="
echo ""

# Build library
echo "[1/4] Building libimage..."
make clean
make -j$(nproc)
echo "✓ Library built successfully"
echo ""

# Generate icons (requires Python + Pillow)
echo "[2/4] Generating default icons..."
if command -v python3 &> /dev/null; then
    if python3 -c "import PIL" &> /dev/null; then
        python3 create_icons.py
        echo "✓ Icons generated"
    else
        echo "⚠ Warning: Python Pillow not installed, skipping icon generation"
        echo "  Install with: pip3 install Pillow"
    fi
else
    echo "⚠ Warning: Python3 not found, skipping icon generation"
fi
echo ""

# Install library
echo "[3/4] Installing library..."
sudo make install
echo "✓ Library installed to /usr/lib"
echo ""

# Install assets
echo "[4/4] Installing icons and wallpapers..."
if [ -d "assets/icons" ]; then
    sudo mkdir -p /usr/share/icons
    sudo cp -r assets/icons/* /usr/share/icons/
    echo "✓ Icons installed to /usr/share/icons"
fi

if [ -d "assets/wallpapers" ]; then
    sudo mkdir -p /usr/share/wallpapers
    sudo cp -r assets/wallpapers/* /usr/share/wallpapers/
    echo "✓ Wallpapers installed to /usr/share/wallpapers"
fi
echo ""

echo "========================================="
echo "Build complete!"
echo "========================================="
echo ""
echo "Library: /usr/lib/libimage.{a,so}"
echo "Header:  /usr/include/image.h"
echo "Icons:   /usr/share/icons/"
echo "Walls:   /usr/share/wallpapers/"
echo ""
echo "Now rebuild desktop shell and file manager:"
echo "  cd ../../shell/desktop && make"
echo "  cd ../../apps/files && make"
echo ""
