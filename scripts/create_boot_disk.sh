#!/bin/bash
# Create bootable UEFI disk image for AutomationOS

set -e

# Configuration
DISK_SIZE_MB=256
BUILD_DIR="build"
MOUNT_POINT="mnt"
DISK_IMAGE="$BUILD_DIR/automationos.img"

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "This script must be run as root (for mounting)"
    echo "Usage: sudo $0"
    exit 1
fi

# Create build directory
mkdir -p "$BUILD_DIR"
mkdir -p "$MOUNT_POINT"

echo "========================================"
echo "AutomationOS Boot Disk Creator"
echo "========================================"
echo ""
echo "Disk size: ${DISK_SIZE_MB}MB"
echo "Output: $DISK_IMAGE"
echo ""

# Clean up old disk image
if [ -f "$DISK_IMAGE" ]; then
    echo "Removing old disk image..."
    rm -f "$DISK_IMAGE"
fi

# Create disk image
echo "[1/8] Creating disk image..."
dd if=/dev/zero of="$DISK_IMAGE" bs=1M count=$DISK_SIZE_MB status=progress

# Create GPT partition table
echo "[2/8] Creating GPT partition table..."
parted "$DISK_IMAGE" mklabel gpt

# Create EFI System Partition (100MB)
echo "[3/8] Creating EFI System Partition..."
parted "$DISK_IMAGE" mkpart ESP fat32 1MiB 101MiB
parted "$DISK_IMAGE" set 1 esp on

# Create data partition (remaining space)
echo "[4/8] Creating data partition..."
parted "$DISK_IMAGE" mkpart primary ext4 101MiB 100%

# Setup loop device
echo "[5/8] Setting up loop device..."
LOOP_DEVICE=$(losetup -f)
losetup -P "$LOOP_DEVICE" "$DISK_IMAGE"

# Format ESP partition
echo "[6/8] Formatting ESP partition..."
mkfs.vfat -F 32 "${LOOP_DEVICE}p1"

# Format data partition
echo "[6/8] Formatting data partition..."
mkfs.ext4 "${LOOP_DEVICE}p2"

# Mount ESP partition
echo "[7/8] Mounting ESP partition..."
mount "${LOOP_DEVICE}p1" "$MOUNT_POINT"

# Create directory structure
mkdir -p "$MOUNT_POINT/EFI/BOOT"

# Copy bootloader
echo "[8/8] Installing bootloader..."
if [ -f "$BUILD_DIR/BOOTX64.EFI" ]; then
    cp "$BUILD_DIR/BOOTX64.EFI" "$MOUNT_POINT/EFI/BOOT/"
    echo "  ✓ Bootloader installed"
else
    echo "  ✗ Warning: BOOTX64.EFI not found"
fi

# Copy kernel
if [ -f "$BUILD_DIR/kernel.elf" ]; then
    cp "$BUILD_DIR/kernel.elf" "$MOUNT_POINT/EFI/BOOT/KERNEL.ELF"
    echo "  ✓ Kernel installed"
else
    echo "  ✗ Warning: kernel.elf not found"
fi

# Copy initrd
if [ -f "$BUILD_DIR/initrd.img" ]; then
    cp "$BUILD_DIR/initrd.img" "$MOUNT_POINT/EFI/BOOT/"
    echo "  ✓ Initrd installed"
else
    echo "  ✗ Warning: initrd.img not found"
fi

# Copy boot configuration
if [ -f "boot/boot.conf" ]; then
    cp "boot/boot.conf" "$MOUNT_POINT/EFI/BOOT/"
    echo "  ✓ Boot configuration installed"
else
    echo "  ✗ Warning: boot.conf not found"
fi

# Sync and unmount
echo ""
echo "Finalizing disk image..."
sync
umount "$MOUNT_POINT"
losetup -d "$LOOP_DEVICE"

# Clean up
rmdir "$MOUNT_POINT" 2>/dev/null || true

echo ""
echo "========================================"
echo "Boot disk created successfully!"
echo "========================================"
echo ""
echo "Output: $DISK_IMAGE"
echo "Size: $(du -h $DISK_IMAGE | cut -f1)"
echo ""
echo "To test in QEMU:"
echo "  qemu-system-x86_64 \\"
echo "    -bios /usr/share/ovmf/OVMF.fd \\"
echo "    -drive file=$DISK_IMAGE,format=raw \\"
echo "    -m 2048 \\"
echo "    -serial stdio"
echo ""
