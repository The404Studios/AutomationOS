#!/bin/bash
#
# AutomationOS Build Validation Script
#
# Validates that all components are built correctly and the ISO is ready to boot.
#
# Usage:
#   ./scripts/validate-build.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$KERNEL_ROOT/build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Counters
PASS=0
FAIL=0
WARN=0

echo "========================================"
echo "  AutomationOS Build Validation"
echo "========================================"
echo ""

# Helper functions
check_file() {
    local file=$1
    local name=$2
    local required=${3:-true}

    if [ -f "$file" ]; then
        local size=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null || echo "0")
        if [ "$size" -gt 0 ]; then
            echo -e "${GREEN}[PASS]${NC} $name: $file (${size} bytes)"
            ((PASS++))
            return 0
        else
            echo -e "${RED}[FAIL]${NC} $name: $file exists but is empty"
            ((FAIL++))
            return 1
        fi
    else
        if [ "$required" = "true" ]; then
            echo -e "${RED}[FAIL]${NC} $name: $file not found (REQUIRED)"
            ((FAIL++))
            return 1
        else
            echo -e "${YELLOW}[WARN]${NC} $name: $file not found (optional)"
            ((WARN++))
            return 2
        fi
    fi
}

check_dir() {
    local dir=$1
    local name=$2

    if [ -d "$dir" ]; then
        local count=$(find "$dir" -type f 2>/dev/null | wc -l)
        echo -e "${GREEN}[PASS]${NC} $name: $dir ($count files)"
        ((PASS++))
        return 0
    else
        echo -e "${RED}[FAIL]${NC} $name: $dir not found"
        ((FAIL++))
        return 1
    fi
}

echo "=== Kernel Components ==="
check_file "$BUILD_DIR/kernel.elf" "Kernel ELF"
check_file "$BUILD_DIR/kernel/kernel.elf" "Kernel Binary (alt path)" false

# Check for key kernel object files
echo ""
echo "=== Kernel Subsystems (Object Files) ==="
check_file "$BUILD_DIR/kernel/ipc/msgqueue.o" "IPC Message Queue"
check_file "$BUILD_DIR/kernel/ipc/shm.o" "IPC Shared Memory"
check_file "$BUILD_DIR/kernel/drivers/pty/pty.o" "PTY Driver"
check_file "$BUILD_DIR/kernel/drivers/pty/pty_dev.o" "PTY Device"
check_file "$BUILD_DIR/kernel/fs/vfs.o" "VFS Core"
check_file "$BUILD_DIR/kernel/core/syscall/handlers.o" "Syscall Handlers"
check_file "$BUILD_DIR/kernel/core/signal/signal.o" "Signal Handling" false

echo ""
echo "=== Userspace Library ==="
check_file "$BUILD_DIR/userspace/libc/libc.a" "libc"

# Check if libc contains IPC symbols
if [ -f "$BUILD_DIR/userspace/libc/libc.a" ]; then
    if nm "$BUILD_DIR/userspace/libc/libc.a" 2>/dev/null | grep -q "msgqueue_create\|shm_create"; then
        echo -e "${GREEN}[PASS]${NC} libc contains IPC symbols"
        ((PASS++))
    else
        echo -e "${YELLOW}[WARN]${NC} libc may not contain IPC symbols"
        ((WARN++))
    fi
fi

echo ""
echo "=== Userspace Binaries ==="
check_file "$BUILD_DIR/userspace/init/init" "Init Process"
check_file "$BUILD_DIR/userspace/shell/shell" "Shell" false
check_file "$BUILD_DIR/userspace/compositor/compositor" "Compositor"
check_file "$BUILD_DIR/userspace/wm/wm" "Window Manager"
check_file "$BUILD_DIR/userspace/apps/terminal/terminal" "Terminal" false
check_file "$BUILD_DIR/userspace/apps/files/explorer" "File Manager" false
check_file "$BUILD_DIR/userspace/apps/settings/settings" "Settings App" false

echo ""
echo "=== Bootloader ==="
check_file "$BUILD_DIR/boot/BOOTX64.EFI" "UEFI Bootloader" false

echo ""
echo "=== Initrd ==="
check_file "$BUILD_DIR/initrd.img" "Initrd Image"

if [ -f "$BUILD_DIR/initrd.img" ]; then
    # Verify initrd is a valid tar archive
    if tar -tf "$BUILD_DIR/initrd.img" >/dev/null 2>&1; then
        echo -e "${GREEN}[PASS]${NC} Initrd is a valid TAR archive"
        ((PASS++))

        # Check for critical files in initrd
        if tar -tf "$BUILD_DIR/initrd.img" | grep -q "sbin/init"; then
            echo -e "${GREEN}[PASS]${NC} Initrd contains /sbin/init"
            ((PASS++))
        else
            echo -e "${RED}[FAIL]${NC} Initrd missing /sbin/init"
            ((FAIL++))
        fi
    else
        echo -e "${RED}[FAIL]${NC} Initrd is not a valid TAR archive"
        ((FAIL++))
    fi

    # List initrd contents
    echo ""
    echo "Initrd contents:"
    tar -tf "$BUILD_DIR/initrd.img" 2>/dev/null | head -30
    local total=$(tar -tf "$BUILD_DIR/initrd.img" 2>/dev/null | wc -l)
    echo "... ($total total entries)"
fi

echo ""
echo "=== ISO Image ==="
check_file "$BUILD_DIR/automationos.iso" "Bootable ISO" false

echo ""
echo "=== Build Directories ==="
check_dir "$BUILD_DIR/kernel" "Kernel Build Directory"
check_dir "$BUILD_DIR/userspace" "Userspace Build Directory"

echo ""
echo "========================================"
echo "  Validation Summary"
echo "========================================"
echo -e "${GREEN}PASS:${NC} $PASS"
echo -e "${YELLOW}WARN:${NC} $WARN"
echo -e "${RED}FAIL:${NC} $FAIL"
echo ""

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}✓ BUILD VALIDATION PASSED${NC}"
    echo ""
    echo "The build is complete and ready for testing."
    echo ""
    echo "Next steps:"
    echo "  - Run 'make qemu' to test in QEMU"
    echo "  - Run 'make test' to run integration tests"
    echo "  - Burn ISO to USB with 'dd if=build/automationos.iso of=/dev/sdX'"
    exit 0
else
    echo -e "${RED}✗ BUILD VALIDATION FAILED${NC}"
    echo ""
    echo "Some required components are missing or invalid."
    echo "Please review the errors above and rebuild."
    echo ""
    echo "Suggested fixes:"
    echo "  - Run 'make clean && make all' for a clean build"
    echo "  - Check build logs for compilation errors"
    echo "  - Ensure all dependencies are installed (see docs/TOOLCHAIN.md)"
    exit 1
fi
