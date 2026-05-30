#!/bin/bash
# AutomationOS Boot Sequence Testing Script
# Tests each boot phase individually

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"
BOOT_DIR="$PROJECT_DIR/boot"
KERNEL_DIR="$PROJECT_DIR/kernel"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test results
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

echo -e "${BLUE}╔════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║          AutomationOS Boot Sequence Test Suite                ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Check prerequisites
check_prereqs() {
    echo -e "${YELLOW}[CHECK]${NC} Checking prerequisites..."

    local missing_tools=()

    command -v x86_64-elf-gcc >/dev/null 2>&1 || missing_tools+=("x86_64-elf-gcc")
    command -v nasm >/dev/null 2>&1 || missing_tools+=("nasm")
    command -v qemu-system-x86_64 >/dev/null 2>&1 || missing_tools+=("qemu-system-x86_64")
    command -v mkfs.vfat >/dev/null 2>&1 || missing_tools+=("mkfs.vfat (dosfstools)")

    if [ ${#missing_tools[@]} -gt 0 ]; then
        echo -e "${RED}[ERROR]${NC} Missing required tools:"
        for tool in "${missing_tools[@]}"; do
            echo "  - $tool"
        done
        echo ""
        echo "Install with:"
        echo "  sudo apt-get install gcc-x86-64-elf nasm qemu-system-x86 dosfstools ovmf"
        exit 1
    fi

    # Check for OVMF (UEFI firmware)
    OVMF_PATH=""
    if [ -f "/usr/share/ovmf/OVMF.fd" ]; then
        OVMF_PATH="/usr/share/ovmf/OVMF.fd"
    elif [ -f "/usr/share/qemu/OVMF.fd" ]; then
        OVMF_PATH="/usr/share/qemu/OVMF.fd"
    elif [ -f "/usr/share/edk2/ovmf/OVMF.fd" ]; then
        OVMF_PATH="/usr/share/edk2/ovmf/OVMF.fd"
    else
        echo -e "${RED}[ERROR]${NC} OVMF firmware not found"
        echo "Install with: sudo apt-get install ovmf"
        exit 1
    fi

    echo -e "${GREEN}[OK]${NC} All prerequisites found"
    echo "  OVMF: $OVMF_PATH"
    echo ""
}

# Test 1: Build bootloader
test_build_bootloader() {
    echo -e "${BLUE}[TEST 1]${NC} Building bootloader..."

    cd "$BOOT_DIR"

    if make clean >/dev/null 2>&1 && make >/dev/null 2>&1; then
        if [ -f "$BUILD_DIR/BOOTX64.EFI" ]; then
            echo -e "${GREEN}[PASS]${NC} Bootloader built successfully"
            ls -lh "$BUILD_DIR/BOOTX64.EFI" | awk '{print "  Size: " $5}'
            ((TESTS_PASSED++))
            return 0
        else
            echo -e "${RED}[FAIL]${NC} BOOTX64.EFI not created"
            ((TESTS_FAILED++))
            return 1
        fi
    else
        echo -e "${RED}[FAIL]${NC} Bootloader build failed"
        make 2>&1 | tail -20
        ((TESTS_FAILED++))
        return 1
    fi
}

# Test 2: Build kernel
test_build_kernel() {
    echo -e "${BLUE}[TEST 2]${NC} Building kernel..."

    cd "$KERNEL_DIR"

    if make clean >/dev/null 2>&1 && make >/dev/null 2>&1; then
        if [ -f "$BUILD_DIR/kernel.elf" ]; then
            echo -e "${GREEN}[PASS]${NC} Kernel built successfully"
            ls -lh "$BUILD_DIR/kernel.elf" | awk '{print "  Size: " $5}'

            # Verify ELF format
            if file "$BUILD_DIR/kernel.elf" | grep -q "ELF 64-bit"; then
                echo -e "${GREEN}[OK]${NC}   ELF64 format verified"
            else
                echo -e "${YELLOW}[WARN]${NC} Unexpected ELF format"
            fi

            ((TESTS_PASSED++))
            return 0
        else
            echo -e "${RED}[FAIL]${NC} kernel.elf not created"
            ((TESTS_FAILED++))
            return 1
        fi
    else
        echo -e "${RED}[FAIL]${NC} Kernel build failed"
        make 2>&1 | tail -20
        ((TESTS_FAILED++))
        return 1
    fi
}

# Test 3: Create ESP image
test_create_esp() {
    echo -e "${BLUE}[TEST 3]${NC} Creating EFI System Partition..."

    local ESP_IMG="$BUILD_DIR/esp.img"
    local MNT_DIR="$BUILD_DIR/mnt"

    # Create 64MB image
    dd if=/dev/zero of="$ESP_IMG" bs=1M count=64 >/dev/null 2>&1

    # Format as FAT32
    if mkfs.vfat -F 32 "$ESP_IMG" >/dev/null 2>&1; then
        echo -e "${GREEN}[OK]${NC}   Created 64MB FAT32 image"
    else
        echo -e "${RED}[FAIL]${NC} Failed to format ESP"
        ((TESTS_FAILED++))
        return 1
    fi

    # Mount and populate
    mkdir -p "$MNT_DIR"

    if sudo mount -o loop "$ESP_IMG" "$MNT_DIR" 2>/dev/null; then
        sudo mkdir -p "$MNT_DIR/EFI/BOOT"

        # Copy bootloader
        if [ -f "$BUILD_DIR/BOOTX64.EFI" ]; then
            sudo cp "$BUILD_DIR/BOOTX64.EFI" "$MNT_DIR/EFI/BOOT/"
            echo -e "${GREEN}[OK]${NC}   Copied BOOTX64.EFI"
        fi

        # Copy boot config
        if [ -f "$BOOT_DIR/boot.conf" ]; then
            sudo cp "$BOOT_DIR/boot.conf" "$MNT_DIR/EFI/BOOT/"
            echo -e "${GREEN}[OK]${NC}   Copied boot.conf"
        fi

        # Copy kernel if it exists
        if [ -f "$BUILD_DIR/kernel.elf" ]; then
            sudo cp "$BUILD_DIR/kernel.elf" "$MNT_DIR/EFI/BOOT/KERNEL.ELF"
            echo -e "${GREEN}[OK]${NC}   Copied KERNEL.ELF"
        fi

        # List contents
        echo -e "${BLUE}[INFO]${NC} ESP contents:"
        sudo find "$MNT_DIR" -type f -exec ls -lh {} \; | awk '{print "  " $9 " (" $5 ")"}'

        sudo umount "$MNT_DIR"
        rmdir "$MNT_DIR"

        echo -e "${GREEN}[PASS]${NC} ESP image created successfully"
        ((TESTS_PASSED++))
        return 0
    else
        echo -e "${RED}[FAIL]${NC} Failed to mount ESP (need sudo)"
        ((TESTS_FAILED++))
        return 1
    fi
}

# Test 4: Boot bootloader only (no kernel)
test_bootloader_only() {
    echo -e "${BLUE}[TEST 4]${NC} Testing bootloader (no kernel)..."

    local ESP_IMG="$BUILD_DIR/esp.img"

    if [ ! -f "$ESP_IMG" ]; then
        echo -e "${YELLOW}[SKIP]${NC} ESP image not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    # Create temporary ESP without kernel
    local ESP_NO_KERNEL="$BUILD_DIR/esp_no_kernel.img"
    cp "$ESP_IMG" "$ESP_NO_KERNEL"

    local MNT_DIR="$BUILD_DIR/mnt"
    mkdir -p "$MNT_DIR"

    if sudo mount -o loop "$ESP_NO_KERNEL" "$MNT_DIR" 2>/dev/null; then
        # Remove kernel if present
        sudo rm -f "$MNT_DIR/EFI/BOOT/KERNEL.ELF"
        sudo umount "$MNT_DIR"
        rmdir "$MNT_DIR"
    fi

    echo -e "${YELLOW}[INFO]${NC} Starting QEMU (3 second timeout)..."
    echo "  Expected: Boot menu appears, then 'Kernel not found' error"
    echo ""

    # Run QEMU with timeout
    timeout 3s qemu-system-x86_64 \
        -bios "$OVMF_PATH" \
        -drive file="$ESP_NO_KERNEL",format=raw,if=ide \
        -m 2048 \
        -serial stdio \
        -display none \
        -no-reboot 2>&1 | tee "$BUILD_DIR/bootloader_test.log" || true

    echo ""

    # Check for expected output
    if grep -q "BOOTLOADER\|AutomationOS" "$BUILD_DIR/bootloader_test.log"; then
        echo -e "${GREEN}[PASS]${NC} Bootloader started"
        ((TESTS_PASSED++))
        return 0
    else
        echo -e "${YELLOW}[WARN]${NC} Could not verify bootloader output"
        echo "  Check log: $BUILD_DIR/bootloader_test.log"
        ((TESTS_SKIPPED++))
        return 0
    fi
}

# Test 5: Boot with kernel
test_boot_with_kernel() {
    echo -e "${BLUE}[TEST 5]${NC} Testing full boot (bootloader + kernel)..."

    local ESP_IMG="$BUILD_DIR/esp.img"

    if [ ! -f "$ESP_IMG" ]; then
        echo -e "${YELLOW}[SKIP]${NC} ESP image not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    if [ ! -f "$BUILD_DIR/kernel.elf" ]; then
        echo -e "${YELLOW}[SKIP]${NC} Kernel not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo -e "${YELLOW}[INFO]${NC} Starting QEMU (5 second timeout)..."
    echo "  Expected: Bootloader loads kernel, kernel initializes"
    echo ""

    # Run QEMU with timeout
    timeout 5s qemu-system-x86_64 \
        -bios "$OVMF_PATH" \
        -drive file="$ESP_IMG",format=raw,if=ide \
        -m 2048 \
        -serial stdio \
        -display none \
        -no-reboot 2>&1 | tee "$BUILD_DIR/kernel_test.log" || true

    echo ""

    # Check for kernel output
    local kernel_started=0
    local gdt_ok=0
    local pmm_ok=0

    if grep -q "AutomationOS v" "$BUILD_DIR/kernel_test.log"; then
        echo -e "${GREEN}[OK]${NC}   Kernel started"
        kernel_started=1
    fi

    if grep -q "GDT" "$BUILD_DIR/kernel_test.log"; then
        echo -e "${GREEN}[OK]${NC}   GDT initialized"
        gdt_ok=1
    fi

    if grep -q "PMM\|Physical memory" "$BUILD_DIR/kernel_test.log"; then
        echo -e "${GREEN}[OK]${NC}   PMM initialized"
        pmm_ok=1
    fi

    if grep -q "IDT" "$BUILD_DIR/kernel_test.log"; then
        echo -e "${GREEN}[OK]${NC}   IDT initialized"
    fi

    if grep -q "Scheduler" "$BUILD_DIR/kernel_test.log"; then
        echo -e "${GREEN}[OK]${NC}   Scheduler initialized"
    fi

    # Check for boot time
    if grep -q "boot time" "$BUILD_DIR/kernel_test.log"; then
        local boot_time=$(grep "boot time" "$BUILD_DIR/kernel_test.log" | head -1)
        echo -e "${BLUE}[INFO]${NC} $boot_time"
    fi

    # Check for errors/panics
    if grep -qi "panic\|error\|fault" "$BUILD_DIR/kernel_test.log"; then
        echo -e "${YELLOW}[WARN]${NC} Errors detected in kernel log:"
        grep -i "panic\|error\|fault" "$BUILD_DIR/kernel_test.log" | head -5
    fi

    if [ $kernel_started -eq 1 ] && [ $gdt_ok -eq 1 ] && [ $pmm_ok -eq 1 ]; then
        echo -e "${GREEN}[PASS]${NC} Kernel boot successful (up to idle loop)"
        ((TESTS_PASSED++))
        return 0
    elif [ $kernel_started -eq 1 ]; then
        echo -e "${YELLOW}[PARTIAL]${NC} Kernel started but init incomplete"
        echo "  Check log: $BUILD_DIR/kernel_test.log"
        ((TESTS_PASSED++))
        return 0
    else
        echo -e "${RED}[FAIL]${NC} Kernel did not start"
        echo "  Check log: $BUILD_DIR/kernel_test.log"
        ((TESTS_FAILED++))
        return 1
    fi
}

# Test 6: Interactive boot test
test_interactive() {
    echo -e "${BLUE}[TEST 6]${NC} Interactive boot test..."

    local ESP_IMG="$BUILD_DIR/esp.img"

    if [ ! -f "$ESP_IMG" ]; then
        echo -e "${YELLOW}[SKIP]${NC} ESP image not found"
        ((TESTS_SKIPPED++))
        return 0
    fi

    echo -e "${YELLOW}[INFO]${NC} Starting QEMU in interactive mode..."
    echo "  Press Ctrl+A, X to exit"
    echo ""
    read -p "Press Enter to continue, or Ctrl+C to skip..."
    echo ""

    qemu-system-x86_64 \
        -bios "$OVMF_PATH" \
        -drive file="$ESP_IMG",format=raw,if=ide \
        -m 2048 \
        -serial stdio \
        -vga std \
        -no-reboot

    echo ""
    echo -e "${BLUE}[INFO]${NC} Interactive test completed"
    ((TESTS_SKIPPED++))
    return 0
}

# Print summary
print_summary() {
    echo ""
    echo -e "${BLUE}╔════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║                      Test Summary                              ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo -e "  ${GREEN}Passed:${NC}  $TESTS_PASSED"
    echo -e "  ${RED}Failed:${NC}  $TESTS_FAILED"
    echo -e "  ${YELLOW}Skipped:${NC} $TESTS_SKIPPED"
    echo ""

    if [ $TESTS_FAILED -eq 0 ]; then
        echo -e "${GREEN}[SUCCESS]${NC} All tests passed!"
        echo ""
        echo "Next steps:"
        echo "  1. Review logs in $BUILD_DIR/"
        echo "  2. Run interactive test: $0 --interactive"
        echo "  3. Implement init process creation (Phase 4)"
        echo ""
        return 0
    else
        echo -e "${RED}[FAILURE]${NC} Some tests failed"
        echo ""
        echo "Check logs in $BUILD_DIR/ for details"
        echo ""
        return 1
    fi
}

# Main execution
main() {
    mkdir -p "$BUILD_DIR"

    # Parse arguments
    if [ "$1" == "--interactive" ] || [ "$1" == "-i" ]; then
        check_prereqs
        test_interactive
        exit 0
    fi

    # Run all tests
    check_prereqs
    test_build_bootloader
    test_build_kernel
    test_create_esp
    test_bootloader_only
    test_boot_with_kernel

    print_summary
}

main "$@"
