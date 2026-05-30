#!/bin/bash
#
# Test initrd building and verification
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$KERNEL_ROOT/build"

echo "========================================"
echo "  Initrd Build Test"
echo "========================================"
echo ""

# Step 1: Build mkinitrd tool
echo "[1/4] Building mkinitrd tool..."
bash "$SCRIPT_DIR/build_mkinitrd.sh"
echo ""

# Step 2: Build userspace binaries (minimal set)
echo "[2/4] Building userspace (minimal)..."
cd "$KERNEL_ROOT"
make userspace || {
    echo "  [WARN] Userspace build had issues, continuing with placeholders"
}
echo ""

# Step 3: Build initrd
echo "[3/4] Building initrd..."
bash "$SCRIPT_DIR/mkinitrd.sh"
echo ""

# Step 4: Verify initrd
echo "[4/4] Verifying initrd..."
if [ -f "$BUILD_DIR/initrd.img" ]; then
    echo "  [OK] Initrd file exists: $BUILD_DIR/initrd.img"

    SIZE=$(stat -c%s "$BUILD_DIR/initrd.img" 2>/dev/null || stat -f%z "$BUILD_DIR/initrd.img" 2>/dev/null)
    SIZE_KB=$((SIZE / 1024))
    echo "  [OK] Size: $SIZE bytes ($SIZE_KB KB)"

    # Extract and verify TAR format
    echo ""
    echo "  Verifying TAR format..."
    TEMP_DIR="$BUILD_DIR/initrd_verify"
    rm -rf "$TEMP_DIR"
    mkdir -p "$TEMP_DIR"

    cd "$TEMP_DIR"
    tar -tf "$BUILD_DIR/initrd.img" > /dev/null 2>&1 && {
        echo "  [OK] TAR format is valid"
        echo ""
        echo "  Contents:"
        tar -tf "$BUILD_DIR/initrd.img" | head -20

        # Count files
        FILE_COUNT=$(tar -tf "$BUILD_DIR/initrd.img" | wc -l)
        echo ""
        echo "  [OK] Total entries: $FILE_COUNT"
    } || {
        echo "  [ERROR] TAR format verification failed"
        exit 1
    }

    cd "$KERNEL_ROOT"
else
    echo "  [ERROR] Initrd not found: $BUILD_DIR/initrd.img"
    exit 1
fi

echo ""
echo "========================================"
echo "  Initrd Build Test: PASSED"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Run 'make iso' to build bootable ISO with initrd"
echo "  2. Run 'make qemu' to test in QEMU"
echo ""
