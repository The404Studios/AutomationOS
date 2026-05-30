#!/bin/bash
#
# AutomationOS USB Boot Creator
#
# Create a bootable USB drive from AutomationOS ISO.
#
# Usage:
#   ./scripts/create-usb.sh /dev/sdX
#   ./scripts/create-usb.sh --device /dev/sdX --verify
#
# WARNING: This will ERASE ALL DATA on the target device!

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
ISO="build/AutomationOS.iso"
DEVICE=""
VERIFY=0
FORCE=0

# Functions
print_header() {
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}  $1${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
}

print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

print_failure() {
    echo -e "${RED}[FAIL]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_help() {
    cat << EOF
AutomationOS USB Boot Creator

Usage: $0 [OPTIONS] <device>

Arguments:
    <device>              USB device (e.g., /dev/sdb, /dev/sdc)
                          WARNING: ALL DATA WILL BE ERASED!

Options:
    --device <device>     USB device (alternative syntax)
    --verify              Verify USB after writing
    --force               Skip confirmation prompt
    --help                Show this help message

Safety:
    - This script requires root/sudo privileges
    - ALL DATA on the target device will be ERASED
    - Double-check device name before proceeding
    - Do NOT use partition (e.g., /dev/sdb1), use device (e.g., /dev/sdb)

Examples:
    # Interactive (with confirmation)
    $0 /dev/sdb

    # With verification
    $0 --device /dev/sdb --verify

    # Skip confirmation (dangerous!)
    $0 --device /dev/sdb --force

How to find USB device:
    # Before inserting USB
    lsblk

    # Insert USB drive

    # After inserting USB (find new device)
    lsblk

    # Or use dmesg
    sudo dmesg | tail -20

Typical device names:
    - /dev/sdb, /dev/sdc, /dev/sdd (Linux)
    - Do NOT use /dev/sda (usually system disk)
    - Do NOT use /dev/nvme0n1 (usually system disk)
    - Do NOT use partition numbers (e.g., /dev/sdb1)

After creating USB:
    1. Safely eject USB drive
    2. Boot target machine from USB
    3. Ensure UEFI boot mode enabled
    4. Disable Secure Boot if needed
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --device)
            DEVICE="$2"
            shift 2
            ;;
        --verify)
            VERIFY=1
            shift
            ;;
        --force)
            FORCE=1
            shift
            ;;
        --help)
            print_help
            exit 0
            ;;
        /dev/*)
            DEVICE="$1"
            shift
            ;;
        *)
            echo -e "${RED}ERROR: Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Validate device
if [ -z "$DEVICE" ]; then
    echo -e "${RED}ERROR: Device not specified${NC}"
    echo ""
    echo "Usage: $0 /dev/sdX"
    echo ""
    echo "Available block devices:"
    lsblk -d -o NAME,SIZE,TYPE,MOUNTPOINT | grep -E "disk|part"
    echo ""
    echo "Use --help for more information"
    exit 1
fi

# Check if device exists
if [ ! -b "$DEVICE" ]; then
    echo -e "${RED}ERROR: Device not found or not a block device: $DEVICE${NC}"
    echo ""
    echo "Available block devices:"
    lsblk -d -o NAME,SIZE,TYPE,MOUNTPOINT | grep -E "disk|part"
    exit 1
fi

# Check if ISO exists
if [ ! -f "$ISO" ]; then
    echo -e "${RED}ERROR: ISO not found: $ISO${NC}"
    echo "Run 'make iso' first to build the ISO image"
    exit 1
fi

# Safety checks
safety_checks() {
    print_header "Safety Checks"

    # Check for root
    if [ "$EUID" -ne 0 ]; then
        echo -e "${RED}ERROR: This script must be run as root${NC}"
        echo "Use: sudo $0 $DEVICE"
        exit 1
    fi

    # Check device is not system disk
    local root_disk=$(df / | tail -1 | awk '{print $1}' | sed 's/[0-9]*$//')
    if [ "$DEVICE" = "$root_disk" ]; then
        echo -e "${RED}ERROR: Cannot use system disk: $DEVICE${NC}"
        echo "This appears to be your system disk!"
        exit 1
    fi

    # Check device is not a partition
    if echo "$DEVICE" | grep -qE '[0-9]$'; then
        print_warning "Device appears to be a partition: $DEVICE"
        print_warning "You should use the device (e.g., /dev/sdb) not partition (e.g., /dev/sdb1)"
        echo ""
        read -p "Continue anyway? (yes/no): " confirm
        if [ "$confirm" != "yes" ]; then
            echo "Aborted"
            exit 1
        fi
    fi

    print_success "Safety checks passed"
    echo ""
}

# Show device info
show_device_info() {
    print_header "Device Information"

    print_info "Target device: $DEVICE"
    echo ""

    # Show device details
    if command -v lsblk &> /dev/null; then
        echo "Device details:"
        lsblk "$DEVICE" || true
        echo ""
    fi

    # Show device size
    if command -v blockdev &> /dev/null; then
        local size=$(blockdev --getsize64 "$DEVICE")
        local size_mb=$((size / 1024 / 1024))
        local size_gb=$((size_mb / 1024))
        print_info "Device size: ${size_mb} MB (${size_gb} GB)"
    fi

    # Check if device is mounted
    if mount | grep -q "^$DEVICE"; then
        print_warning "Device is currently mounted:"
        mount | grep "^$DEVICE"
        echo ""
    fi

    # Show ISO info
    local iso_size=$(stat -c%s "$ISO")
    local iso_mb=$((iso_size / 1024 / 1024))
    print_info "ISO size: ${iso_mb} MB"

    echo ""
}

# Confirm operation
confirm_operation() {
    if [ $FORCE -eq 1 ]; then
        print_warning "Force mode enabled, skipping confirmation"
        return
    fi

    print_header "WARNING: DATA LOSS"

    echo -e "${RED}WARNING: This will ERASE ALL DATA on $DEVICE${NC}"
    echo ""
    echo "Are you ABSOLUTELY SURE?"
    echo ""
    echo "Type the device name to confirm: $DEVICE"
    read -p "Device name: " confirm

    if [ "$confirm" != "$DEVICE" ]; then
        echo ""
        echo "Confirmation failed. Aborted."
        exit 1
    fi

    echo ""
    print_info "Confirmed. Proceeding..."
    echo ""
}

# Unmount device
unmount_device() {
    print_info "Unmounting any mounted partitions..."

    # Unmount all partitions on device
    if mount | grep -q "^$DEVICE"; then
        mount | grep "^$DEVICE" | awk '{print $1}' | while read -r part; do
            print_info "Unmounting $part"
            umount "$part" || true
        done
    fi

    print_success "Device unmounted"
}

# Write ISO to device
write_iso() {
    print_header "Writing ISO to Device"

    print_info "Source: $ISO"
    print_info "Target: $DEVICE"
    print_info "This may take several minutes..."
    echo ""

    # Write ISO with dd
    if dd if="$ISO" of="$DEVICE" bs=4M status=progress oflag=sync; then
        print_success "ISO written successfully"
    else
        print_failure "Failed to write ISO"
        exit 1
    fi

    # Sync filesystem
    print_info "Syncing filesystem..."
    sync

    print_success "Write complete"
    echo ""
}

# Verify USB
verify_usb() {
    print_header "Verifying USB"

    print_info "Reading back from device..."
    print_info "This may take several minutes..."
    echo ""

    # Get ISO size
    local iso_size=$(stat -c%s "$ISO")
    local iso_blocks=$((iso_size / 4096))

    # Read back and compare
    if dd if="$DEVICE" bs=4M count="$iso_blocks" iflag=count_bytes status=progress | \
       cmp -n "$iso_size" "$ISO" -; then
        print_success "Verification passed! USB is bootable."
    else
        print_failure "Verification failed! USB may be corrupted."
        print_warning "Try writing again or use a different USB drive"
        exit 1
    fi

    echo ""
}

# Print success message
print_success_message() {
    print_header "USB Boot Drive Created"

    print_success "AutomationOS USB boot drive created successfully!"
    echo ""
    print_info "Next steps:"
    echo ""
    echo "1. Safely eject USB drive:"
    echo "   sync"
    echo "   eject $DEVICE"
    echo ""
    echo "2. Insert USB into target machine"
    echo ""
    echo "3. Configure BIOS/UEFI:"
    echo "   - Enable UEFI boot mode (not legacy BIOS)"
    echo "   - Set USB as first boot device"
    echo "   - Disable Secure Boot (if needed)"
    echo ""
    echo "4. Boot from USB"
    echo ""
    echo "5. (Optional) Connect serial console:"
    echo "   - Baud rate: 115200"
    echo "   - Data: 8 bits, Parity: none, Stop: 1 bit"
    echo "   - Use: ./scripts/test-on-hardware.sh --serial /dev/ttyUSB0"
    echo ""
    print_info "For troubleshooting, see docs/TROUBLESHOOTING_HARDWARE.md"
}

# Main
main() {
    print_header "AutomationOS USB Boot Creator"

    safety_checks
    show_device_info
    confirm_operation
    unmount_device
    write_iso

    if [ $VERIFY -eq 1 ]; then
        verify_usb
    fi

    print_success_message
}

main
