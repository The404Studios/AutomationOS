#!/bin/bash
# QEMU Testing Script for AutomationOS UEFI Bootloader
# This script creates a bootable ESP image and tests it in QEMU

set -e

# Configuration
BUILD_DIR="../build"
ESP_IMG="esp.img"
ESP_SIZE_MB=64
KERNEL_ELF="${BUILD_DIR}/kernel.elf"
BOOTLOADER="${BUILD_DIR}/BOOTX64.EFI"
OVMF_PATH="/usr/share/ovmf/OVMF.fd"
QEMU_MEMORY="2G"

echo "==================================="
echo "AutomationOS Bootloader Test"
echo "==================================="

# Check prerequisites
command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    echo "ERROR: qemu-system-x86_64 not found"
    exit 1
}

if [ ! -f "$OVMF_PATH" ]; then
    echo "WARNING: OVMF firmware not found at $OVMF_PATH"
    echo "Trying alternative locations..."

    if [ -f "/usr/share/edk2-ovmf/x64/OVMF.fd" ]; then
        OVMF_PATH="/usr/share/edk2-ovmf/x64/OVMF.fd"
    elif [ -f "/usr/share/qemu/OVMF.fd" ]; then
        OVMF_PATH="/usr/share/qemu/OVMF.fd"
    else
        echo "ERROR: OVMF firmware not found"
        echo "Install with: sudo apt install ovmf"
        exit 1
    fi
fi

# Check if bootloader exists
if [ ! -f "$BOOTLOADER" ]; then
    echo "ERROR: Bootloader not found at $BOOTLOADER"
    echo "Build with: make"
    exit 1
fi

echo "[1/5] Creating ESP image (${ESP_SIZE_MB}MB)..."
dd if=/dev/zero of="$ESP_IMG" bs=1M count=$ESP_SIZE_MB status=none
mkfs.fat -F 32 "$ESP_IMG" >/dev/null 2>&1

echo "[2/5] Mounting ESP image..."
mkdir -p mnt
sudo mount -o loop "$ESP_IMG" mnt

echo "[3/5] Copying bootloader..."
sudo mkdir -p mnt/EFI/BOOT
sudo cp "$BOOTLOADER" mnt/EFI/BOOT/BOOTX64.EFI

# Copy kernel if it exists
if [ -f "$KERNEL_ELF" ]; then
    echo "[4/5] Copying kernel..."
    sudo cp "$KERNEL_ELF" mnt/EFI/BOOT/KERNEL.ELF
else
    echo "[4/5] WARNING: Kernel not found at $KERNEL_ELF"
    echo "           Creating dummy kernel for bootloader test..."

    # Create minimal ELF stub
    cat > /tmp/stub.c << 'EOF'
void kernel_main(void* boot_info) {
    // Dummy kernel - just halt
    while(1) __asm__ volatile("hlt");
}
EOF

    x86_64-elf-gcc -ffreestanding -nostdlib -c /tmp/stub.c -o /tmp/stub.o
    x86_64-elf-ld -T <(echo "SECTIONS { . = 0xFFFFFFFF80100000; .text : { *(.text) } }") \
                   /tmp/stub.o -o /tmp/kernel.elf
    sudo cp /tmp/kernel.elf mnt/EFI/BOOT/KERNEL.ELF
fi

echo "[5/5] Unmounting ESP..."
sudo umount mnt
rmdir mnt

echo ""
echo "==================================="
echo "Starting QEMU..."
echo "==================================="
echo ""
echo "Press Ctrl-A then X to exit QEMU"
echo ""

# Launch QEMU with UEFI firmware
qemu-system-x86_64 \
    -bios "$OVMF_PATH" \
    -drive file="$ESP_IMG",format=raw,if=ide \
    -m "$QEMU_MEMORY" \
    -serial stdio \
    -vga std \
    -no-reboot \
    -no-shutdown \
    "$@"

echo ""
echo "QEMU exited"
