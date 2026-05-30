#!/bin/bash
#
# AutomationOS USB Creator
#
# Creates a bootable USB drive from the AutomationOS ISO image.
#
# Usage:
#   ./scripts/create-bootable-usb.sh /dev/sdX
#
# WARNING: This will erase all data on the target device!
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_banner() {
    echo -e "${BLUE}=========================================${NC}"
    echo -e "${BLUE}  AutomationOS USB Creator${NC}"
    echo -e "${BLUE}=========================================${NC}"
    echo ""
}

print_section() {
    echo ""
    echo -e "${GREEN}>>> $1${NC}"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

# Check arguments
if [ $# -ne 1 ]; then
    echo "Usage: $0 DEVICE"
    echo ""
    echo "Example:"
    echo "  $0 /dev/sdb   # Create bootable USB on /dev/sdb"
    echo ""
    echo "WARNING: This will erase all data on the device!"
    echo ""
    echo "Available devices:"
    lsblk -d -o NAME,SIZE,TYPE,MODEL | grep -E "disk|NAME"
    exit 1
fi

DEVICE="$1"

# Root check
if [ "$EUID" -ne 0 ]; then
    print_error "This script must be run as root"
    echo "Try: sudo $0 $DEVICE"
    exit 1
fi

print_banner

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
ISO_FILE="$ROOT_DIR/build/AutomationOS.iso"

# Validate device
if [ ! -b "$DEVICE" ]; then
    print_error "Device not found: $DEVICE"
    echo ""
    echo "Available block devices:"
    lsblk -d -o NAME,SIZE,TYPE,MODEL
    exit 1
fi

# Check if ISO exists
if [ ! -f "$ISO_FILE" ]; then
    print_error "ISO file not found: $ISO_FILE"
    echo "Run 'make iso' first to build the ISO"
    exit 1
fi

# Show device info
print_section "Device Information"

DEVICE_NAME=$(basename "$DEVICE")
DEVICE_SIZE=$(lsblk -d -n -o SIZE "$DEVICE")
DEVICE_MODEL=$(lsblk -d -n -o MODEL "$DEVICE")

echo "Device: $DEVICE"
echo "Size:   $DEVICE_SIZE"
echo "Model:  $DEVICE_MODEL"
echo ""

# Check if device is mounted
MOUNTED_PARTS=$(lsblk -n -o MOUNTPOINT "$DEVICE" | grep -v "^$" || true)

if [ -n "$MOUNTED_PARTS" ]; then
    print_warning "Device has mounted partitions:"
    echo "$MOUNTED_PARTS"
    echo ""
fi

# Show ISO info
ISO_SIZE=$(du -h "$ISO_FILE" | cut -f1)
echo "ISO file: $ISO_FILE"
echo "ISO size: $ISO_SIZE"
echo ""

# Safety confirmation
print_warning "WARNING: This will erase ALL data on $DEVICE!"
echo ""
read -p "Type 'YES' to continue: " -r
if [ "$REPLY" != "YES" ]; then
    echo "Aborted."
    exit 1
fi

# Unmount all partitions on device
print_section "Unmounting Partitions"

for part in ${DEVICE}*; do
    if [ "$part" != "$DEVICE" ] && [ -b "$part" ]; then
        if mountpoint -q "$part" 2>/dev/null; then
            echo "Unmounting $part..."
            umount "$part" || true
        fi
    fi
done

print_success "Partitions unmounted"

# Write ISO to device
print_section "Writing ISO to Device"

echo "This may take several minutes..."
echo ""

# Use dd with status
if dd --help 2>&1 | grep -q "status=progress"; then
    # GNU dd with progress
    dd if="$ISO_FILE" of="$DEVICE" bs=4M status=progress oflag=sync
else
    # macOS dd without progress
    dd if="$ISO_FILE" of="$DEVICE" bs=4m
fi

print_success "ISO written to device"

# Sync filesystem
print_section "Syncing Filesystem"

sync

print_success "Sync complete"

# Verify (optional)
print_section "Verification"

ISO_BLOCKS=$(stat -c %s "$ISO_FILE" 2>/dev/null || stat -f %z "$ISO_FILE")
DEVICE_BLOCKS=$(dd if="$DEVICE" bs=1 count=$ISO_BLOCKS 2>/dev/null | wc -c)

if [ "$ISO_BLOCKS" -eq "$DEVICE_BLOCKS" ]; then
    print_success "Verification passed"
else
    print_warning "Verification inconclusive"
fi

# Eject device
print_section "Ejecting Device"

if command -v eject &> /dev/null; then
    eject "$DEVICE" 2>/dev/null || true
    print_success "Device ejected"
else
    print_warning "eject command not found, manually remove device"
fi

# Summary
echo ""
echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}  USB Drive Created Successfully${NC}"
echo -e "${GREEN}=========================================${NC}"
echo ""
echo "Device: $DEVICE"
echo "  Size: $DEVICE_SIZE"
echo "  ISO:  $ISO_FILE ($ISO_SIZE)"
echo ""
echo -e "${BLUE}Boot Instructions:${NC}"
echo "  1. Insert USB drive into target computer"
echo "  2. Enter BIOS/UEFI settings (usually F2, F12, Del, or Esc)"
echo "  3. Select boot from USB device"
echo "  4. AutomationOS should start automatically"
echo ""
echo -e "${YELLOW}Notes:${NC}"
echo "  - Ensure UEFI boot mode is enabled"
echo "  - Secure Boot may need to be disabled"
echo "  - Some systems require manually selecting UEFI boot entry"
echo ""
print_success "Done!"
