#!/bin/bash
#
# AutomationOS initrd creation script
#
# Creates an initial ramdisk image (TAR format) containing the init
# process and minimal userspace needed for early boot.
#
# Usage:
#   ./scripts/mkinitrd.sh              # Build initrd from default paths
#   ./scripts/mkinitrd.sh --output X   # Write to custom path
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$KERNEL_ROOT/build"
USERSPACE_DIR="$BUILD_DIR/userspace"
INITRD_ROOT="$BUILD_DIR/initrd_root"
INITRD_IMG="$BUILD_DIR/initrd.img"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -o|--output)
            INITRD_IMG="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "========================================"
echo "  AutomationOS Initrd Builder"
echo "========================================"
echo ""

# Clean and create initrd directory structure
rm -rf "$INITRD_ROOT"
mkdir -p "$INITRD_ROOT/sbin"
mkdir -p "$INITRD_ROOT/bin"
mkdir -p "$INITRD_ROOT/lib"
mkdir -p "$INITRD_ROOT/etc"
mkdir -p "$INITRD_ROOT/dev"
mkdir -p "$INITRD_ROOT/proc"
mkdir -p "$INITRD_ROOT/sys"
mkdir -p "$INITRD_ROOT/tmp"
mkdir -p "$INITRD_ROOT/models"
mkdir -p "$INITRD_ROOT/home"
mkdir -p "$INITRD_ROOT/usr/share/fonts"
mkdir -p "$INITRD_ROOT/usr/share/icons"
mkdir -p "$INITRD_ROOT/usr/share/wallpapers"

echo "[1/6] Creating directory structure..."

# Copy init binary (required for boot)
INIT_PATHS=(
    "$USERSPACE_DIR/init/init"
    "$BUILD_DIR/init"
)

FOUND_INIT=0
for INIT_PATH in "${INIT_PATHS[@]}"; do
    if [ -f "$INIT_PATH" ]; then
        cp "$INIT_PATH" "$INITRD_ROOT/sbin/init"
        chmod +x "$INITRD_ROOT/sbin/init"
        echo "  [OK] Copied init: $INIT_PATH"
        FOUND_INIT=1
        break
    fi
done

if [ $FOUND_INIT -eq 0 ]; then
    echo "  [ERROR] No init ELF binary found; refusing to package a non-ELF placeholder"
    exit 1
fi

echo ""
echo "[2/5] Copying userspace binaries..."

# Copy shell (to sbin/shell and bin/sh)
SHELL_PATHS=(
    "$USERSPACE_DIR/shell/shell"
)

for SHELL_PATH in "${SHELL_PATHS[@]}"; do
    if [ -f "$SHELL_PATH" ]; then
        cp "$SHELL_PATH" "$INITRD_ROOT/sbin/shell"
        cp "$SHELL_PATH" "$INITRD_ROOT/bin/sh"
        chmod +x "$INITRD_ROOT/sbin/shell"
        chmod +x "$INITRD_ROOT/bin/sh"
        echo "  [OK] Copied shell: $SHELL_PATH"
        break
    fi
done

# Copy compositor (to sbin/compositor)
COMPOSITOR_PATHS=(
    "$USERSPACE_DIR/compositor/compositor"
)

for COMP_PATH in "${COMPOSITOR_PATHS[@]}"; do
    if [ -f "$COMP_PATH" ]; then
        cp "$COMP_PATH" "$INITRD_ROOT/sbin/compositor"
        chmod +x "$INITRD_ROOT/sbin/compositor"
        echo "  [OK] Copied compositor: $COMP_PATH"
        break
    fi
done

# Copy window manager (to sbin/wm)
WM_PATHS=(
    "$USERSPACE_DIR/wm/wm"
)

for WM_PATH in "${WM_PATHS[@]}"; do
    if [ -f "$WM_PATH" ]; then
        cp "$WM_PATH" "$INITRD_ROOT/sbin/wm"
        chmod +x "$INITRD_ROOT/sbin/wm"
        echo "  [OK] Copied window manager: $WM_PATH"
        break
    fi
done

# Copy desktop shell (to sbin/desktop-shell)
DESKTOP_SHELL_PATHS=(
    "$USERSPACE_DIR/shell/desktop/desktop_shell"
    "$BUILD_DIR/shell/desktop/desktop_shell"
)

for DS_PATH in "${DESKTOP_SHELL_PATHS[@]}"; do
    if [ -f "$DS_PATH" ]; then
        cp "$DS_PATH" "$INITRD_ROOT/sbin/desktop-shell"
        chmod +x "$INITRD_ROOT/sbin/desktop-shell"
        echo "  [OK] Copied desktop shell: $DS_PATH"
        break
    fi
done

# Copy AI agent daemon
LLM_PATHS=(
    "$USERSPACE_DIR/llm/llm"
)

for LLM_PATH in "${LLM_PATHS[@]}"; do
    if [ -f "$LLM_PATH" ]; then
        cp "$LLM_PATH" "$INITRD_ROOT/sbin/aid"
        chmod +x "$INITRD_ROOT/sbin/aid"
        echo "  [OK] Copied AI daemon: $LLM_PATH"
        break
    fi
done

# Copy model file if available (Qwen GGUF).
# GATED: model files are large (300+ MB) and dramatically inflate the initrd.
# Set INITRD_INCLUDE_MODEL=1 to include them; omitted by default for fast boot.
if [ "${INITRD_INCLUDE_MODEL:-0}" = "1" ]; then
    MODEL_PATHS=(
        "$KERNEL_ROOT/models/qwen.gguf"
        "$KERNEL_ROOT/models/qwen2.5-0.5b-q4_0.gguf"
    )

    for MODEL_PATH in "${MODEL_PATHS[@]}"; do
        if [ -f "$MODEL_PATH" ]; then
            cp "$MODEL_PATH" "$INITRD_ROOT/models/qwen.gguf"
            echo "  [OK] Copied model: $MODEL_PATH ($(stat -c%s "$MODEL_PATH" 2>/dev/null || stat -f%z "$MODEL_PATH" 2>/dev/null || echo "?") bytes)"
            break
        fi
    done
else
    echo "  [--] Model file skipped (set INITRD_INCLUDE_MODEL=1 to include)"
fi

# Copy terminal (to bin/terminal)
TERMINAL_PATHS=(
    "$USERSPACE_DIR/apps/terminal/terminal"
)

for TERM_PATH in "${TERMINAL_PATHS[@]}"; do
    if [ -f "$TERM_PATH" ]; then
        cp "$TERM_PATH" "$INITRD_ROOT/bin/terminal"
        chmod +x "$INITRD_ROOT/bin/terminal"
        echo "  [OK] Copied terminal: $TERM_PATH"
        break
    fi
done

# Copy task manager (to bin/taskmanager)
TASKMANAGER_PATHS=(
    "$USERSPACE_DIR/apps/taskmanager/taskmanager"
)

for TASK_PATH in "${TASKMANAGER_PATHS[@]}"; do
    if [ -f "$TASK_PATH" ]; then
        cp "$TASK_PATH" "$INITRD_ROOT/bin/taskmanager"
        chmod +x "$INITRD_ROOT/bin/taskmanager"
        echo "  [OK] Copied task manager: $TASK_PATH"
        break
    fi
done

# Copy file explorer (to bin/files)
FILES_APP_PATHS=(
    "$USERSPACE_DIR/apps/files/explorer"
)

for FILES_PATH in "${FILES_APP_PATHS[@]}"; do
    if [ -f "$FILES_PATH" ]; then
        cp "$FILES_PATH" "$INITRD_ROOT/bin/files"
        chmod +x "$INITRD_ROOT/bin/files"
        echo "  [OK] Copied file explorer: $FILES_PATH"
        break
    fi
done

# Copy batch syscall benchmark (to bin/bench_batch).
# GATED: test/benchmark binaries are not needed for production boot.
# Set INITRD_INCLUDE_BENCH=1 to include them.
if [ "${INITRD_INCLUDE_BENCH:-0}" = "1" ]; then
    BENCH_BATCH_PATHS=(
        "$USERSPACE_DIR/tests/bench_batch"
        "$BUILD_DIR/userspace/tests/bench_batch"
    )

    for BENCH_PATH in "${BENCH_BATCH_PATHS[@]}"; do
        if [ -f "$BENCH_PATH" ]; then
            cp "$BENCH_PATH" "$INITRD_ROOT/bin/bench_batch"
            chmod +x "$INITRD_ROOT/bin/bench_batch"
            echo "  [OK] Copied batch benchmark: $BENCH_PATH"
            break
        fi
    done
else
    echo "  [--] Benchmark binary skipped (set INITRD_INCLUDE_BENCH=1 to include)"
fi

echo ""
echo "[3/6] Copying libraries..."

# Copy userspace libraries
if [ -d "$USERSPACE_DIR/libc" ]; then
    find "$USERSPACE_DIR/libc" -name "*.a" -o -name "*.so*" | while read -r lib; do
        cp "$lib" "$INITRD_ROOT/lib/" 2>/dev/null || true
    done
    echo "  [OK] Copied userspace libraries"
fi

echo ""
echo "[4/6] Copying fonts, icons, and wallpapers..."

# Copy fonts if available
FONT_PATHS=(
    "$KERNEL_ROOT/userspace/lib/font/DejaVuSans.ttf"
    "$KERNEL_ROOT/assets/fonts/DejaVuSans.ttf"
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
)

for FONT_PATH in "${FONT_PATHS[@]}"; do
    if [ -f "$FONT_PATH" ]; then
        cp "$FONT_PATH" "$INITRD_ROOT/usr/share/fonts/DejaVuSans.ttf"
        echo "  [OK] Copied font: $FONT_PATH"
        break
    fi
done

# Copy icons if available
if [ -d "$KERNEL_ROOT/assets/icons" ]; then
    cp -r "$KERNEL_ROOT/assets/icons/"* "$INITRD_ROOT/usr/share/icons/" 2>/dev/null || true
    echo "  [OK] Copied icons"
fi

# Copy wallpaper if available
WALLPAPER_PATHS=(
    "$KERNEL_ROOT/assets/wallpapers/default.png"
    "$KERNEL_ROOT/assets/wallpaper.png"
)

for WALLPAPER_PATH in "${WALLPAPER_PATHS[@]}"; do
    if [ -f "$WALLPAPER_PATH" ]; then
        cp "$WALLPAPER_PATH" "$INITRD_ROOT/usr/share/wallpapers/default.png"
        echo "  [OK] Copied wallpaper: $WALLPAPER_PATH"
        break
    fi
done

echo ""
echo "[5/6] Creating configuration files..."

# Create minimal /etc/fstab
cat > "$INITRD_ROOT/etc/fstab" << 'EOF'
# AutomationOS filesystem table
proc    /proc   proc    defaults    0   0
sysfs   /sys    sysfs   defaults    0   0
devpts  /dev/pts devpts defaults    0   0
tmpfs   /tmp    tmpfs   defaults    0   0
EOF

# Create /etc/inittab
cat > "$INITRD_ROOT/etc/inittab" << 'EOF'
# AutomationOS init configuration
::sysinit:/sbin/init
::respawn:/bin/sh
tty1::respawn:/sbin/compositor
tty2::respawn:/sbin/wm
EOF

echo "  [OK] Created configuration files"

echo ""
echo "[6/6] Building TAR archive..."

# Build TAR archive using ustar format (POSIX)
if command -v tar &> /dev/null; then
    cd "$INITRD_ROOT"
    tar --format=ustar -cf "$INITRD_IMG" .
    cd "$KERNEL_ROOT"
    echo "  [OK] Used system tar"
elif [ -f "$BUILD_DIR/mkinitrd" ]; then
    "$BUILD_DIR/mkinitrd" -o "$INITRD_IMG" -d "$INITRD_ROOT"
    echo "  [OK] Used custom mkinitrd"
else
    echo "  [ERROR] Neither tar nor mkinitrd available"
    exit 1
fi

INITRD_SIZE=$(stat -c%s "$INITRD_IMG" 2>/dev/null || stat -f%z "$INITRD_IMG" 2>/dev/null || echo "unknown")

echo ""
echo "========================================"
echo "  Initrd Created Successfully!"
echo "========================================"
echo "  Location: $INITRD_IMG"
echo "  Size:     $INITRD_SIZE bytes"
echo ""
echo "Contents:"
find "$INITRD_ROOT" -type f -o -type d | sed 's|'"$INITRD_ROOT"'||' | sort
echo ""
