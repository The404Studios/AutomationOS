#!/bin/bash
# Build initrd with font files included
# Creates a bootable initrd with fonts at /fonts/

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
INITRD_ROOT="$KERNEL_ROOT/initrd_root"
FONT_DIR="$KERNEL_ROOT/userspace/lib/font/fonts"

echo "=========================================="
echo "Building Initrd with Fonts"
echo "=========================================="
echo ""

# Check if fonts exist
if [ ! -f "$FONT_DIR/DejaVuSans.ttf" ]; then
    echo "Error: DejaVu Sans font not found"
    echo "Run: bash scripts/setup-fonts.sh"
    exit 1
fi

if [ ! -f "$FONT_DIR/DejaVuSansMono.ttf" ]; then
    echo "Error: DejaVu Sans Mono font not found"
    echo "Run: bash scripts/setup-fonts.sh"
    exit 1
fi

# Create initrd structure
echo "[1/5] Creating initrd structure..."
mkdir -p "$INITRD_ROOT/fonts"
mkdir -p "$INITRD_ROOT/bin"
mkdir -p "$INITRD_ROOT/sbin"
mkdir -p "$INITRD_ROOT/dev"
mkdir -p "$INITRD_ROOT/proc"
mkdir -p "$INITRD_ROOT/sys"
mkdir -p "$INITRD_ROOT/tmp"
echo "✓ Directories created"
echo ""

# Copy fonts
echo "[2/5] Copying fonts..."
cp "$FONT_DIR/DejaVuSans.ttf" "$INITRD_ROOT/fonts/"
cp "$FONT_DIR/DejaVuSansMono.ttf" "$INITRD_ROOT/fonts/"
echo "✓ Fonts copied:"
ls -lh "$INITRD_ROOT/fonts/"
echo ""

# Copy binaries (if they exist)
echo "[3/5] Copying binaries..."
BINARY_COUNT=0

copy_if_exists() {
    local src="$1"
    local dst="$2"
    if [ -f "$src" ]; then
        cp "$src" "$dst"
        echo "  ✓ $(basename "$src")"
        ((BINARY_COUNT++))
    fi
}

# Compositor
copy_if_exists "$KERNEL_ROOT/build/userspace/compositor/compositor" "$INITRD_ROOT/bin/compositor"

# Desktop shell
copy_if_exists "$KERNEL_ROOT/build/userspace/shell/desktop_shell" "$INITRD_ROOT/bin/desktop_shell"

# Terminal
copy_if_exists "$KERNEL_ROOT/build/userspace/apps/terminal/terminal" "$INITRD_ROOT/bin/terminal"

# File explorer
copy_if_exists "$KERNEL_ROOT/build/userspace/apps/files/explorer" "$INITRD_ROOT/bin/explorer"

# Settings
copy_if_exists "$KERNEL_ROOT/build/userspace/apps/settings/settings" "$INITRD_ROOT/bin/settings"

# Task manager
copy_if_exists "$KERNEL_ROOT/build/userspace/apps/taskmanager/taskmanager" "$INITRD_ROOT/bin/taskmanager"

if [ $BINARY_COUNT -eq 0 ]; then
    echo "  Warning: No binaries found in build/"
    echo "  Run 'make' to build applications first"
fi
echo ""

# Create init script
echo "[4/5] Creating init script..."
cat > "$INITRD_ROOT/init" << 'EOF'
#!/bin/sh
# AutomationOS Init Script

echo "AutomationOS Init"
echo "Mounting filesystems..."

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo "Font files available:"
ls -lh /fonts/

echo "Starting compositor..."
/bin/compositor &

sleep 1

echo "Starting desktop shell..."
/bin/desktop_shell &

echo "Init complete. Dropping to shell..."
exec /bin/sh
EOF

chmod +x "$INITRD_ROOT/init"
echo "✓ Init script created"
echo ""

# Build initrd image
echo "[5/5] Building initrd image..."
cd "$INITRD_ROOT"
find . | cpio -o -H newc | gzip > "$KERNEL_ROOT/initrd.img"
cd "$KERNEL_ROOT"
echo "✓ Initrd built"
echo ""

# Summary
echo "=========================================="
echo "Initrd Build Complete"
echo "=========================================="
echo ""
echo "Output: initrd.img"
ls -lh initrd.img
echo ""
echo "Contents:"
echo "  - /fonts/DejaVuSans.ttf (~300KB)"
echo "  - /fonts/DejaVuSansMono.ttf (~300KB)"
echo "  - /bin/ ($BINARY_COUNT binaries)"
echo "  - /init (startup script)"
echo ""
echo "Test with QEMU:"
echo "  qemu-system-x86_64 \\"
echo "    -kernel build/kernel.bin \\"
echo "    -initrd initrd.img \\"
echo "    -vga std \\"
echo "    -m 512M \\"
echo "    -serial stdio"
echo ""
