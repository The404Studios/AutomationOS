#!/bin/bash
#
# AutomationOS Real Hardware Testing Script
#
# Test AutomationOS on physical hardware with serial console capture.
#
# Usage:
#   ./scripts/test-on-hardware.sh --serial /dev/ttyUSB0
#   ./scripts/test-on-hardware.sh --serial /dev/ttyUSB0 --log hardware.log
#   ./scripts/test-on-hardware.sh --serial /dev/ttyUSB0 --validate

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SERIAL_PORT=""
LOG_FILE="build/hardware-test.log"
BAUD_RATE=115200
TIMEOUT=30
VALIDATE=0
MONITOR_ONLY=0

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
AutomationOS Real Hardware Testing Script

Usage: $0 [OPTIONS]

Options:
    --serial <device>     Serial port device (e.g., /dev/ttyUSB0, /dev/ttyS0)
    --log <file>          Log file path (default: build/hardware-test.log)
    --baud <rate>         Baud rate (default: 115200)
    --timeout <seconds>   Capture timeout (default: 30)
    --validate            Validate boot messages after capture
    --monitor             Monitor only (no validation, live output)
    --help                Show this help message

Prerequisites:
    1. Connect serial cable between test machine and capture machine
    2. Boot test machine from AutomationOS USB/CD
    3. Run this script on capture machine to record boot output

Examples:
    # Capture serial output
    $0 --serial /dev/ttyUSB0

    # Capture and validate
    $0 --serial /dev/ttyUSB0 --validate

    # Live monitoring
    $0 --serial /dev/ttyUSB0 --monitor

    # Custom baud rate
    $0 --serial /dev/ttyS0 --baud 9600

Serial Cable Setup:
    - Use null-modem cable (crossover) for serial-to-serial
    - Or use USB-to-serial adapter
    - Connect TX to RX, RX to TX, GND to GND

BIOS/UEFI Settings (on test machine):
    1. Enable serial port (COM1)
    2. Set baud rate to 115200
    3. Set data: 8 bits, parity: none, stop: 1 bit
    4. Enable serial console redirection (if available)

Notes:
    - Press Ctrl+C to stop capture
    - Use --monitor for live output without timeout
    - Use --validate to check boot messages
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --serial)
            SERIAL_PORT="$2"
            shift 2
            ;;
        --log)
            LOG_FILE="$2"
            shift 2
            ;;
        --baud)
            BAUD_RATE="$2"
            shift 2
            ;;
        --timeout)
            TIMEOUT="$2"
            shift 2
            ;;
        --validate)
            VALIDATE=1
            shift
            ;;
        --monitor)
            MONITOR_ONLY=1
            shift
            ;;
        --help)
            print_help
            exit 0
            ;;
        *)
            echo -e "${RED}ERROR: Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Validate arguments
if [ -z "$SERIAL_PORT" ]; then
    echo -e "${RED}ERROR: Serial port not specified${NC}"
    echo "Use --serial /dev/ttyUSB0 (or appropriate device)"
    echo "Use --help for more information"
    exit 1
fi

# Check serial port exists
if [ ! -e "$SERIAL_PORT" ]; then
    echo -e "${RED}ERROR: Serial port not found: $SERIAL_PORT${NC}"
    echo ""
    echo "Available serial ports:"
    ls -l /dev/ttyS* /dev/ttyUSB* 2>/dev/null || echo "  (none found)"
    exit 1
fi

# Check permissions
if [ ! -r "$SERIAL_PORT" ] || [ ! -w "$SERIAL_PORT" ]; then
    echo -e "${RED}ERROR: No permission to access $SERIAL_PORT${NC}"
    echo ""
    echo "Fix permissions:"
    echo "  sudo chmod 666 $SERIAL_PORT"
    echo "Or add user to dialout group:"
    echo "  sudo usermod -a -G dialout $USER"
    echo "  (then logout and login)"
    exit 1
fi

# Check for required tools
check_tools() {
    local missing=0

    if ! command -v stty &> /dev/null; then
        print_failure "stty not found (required for serial configuration)"
        missing=1
    fi

    if [ $missing -eq 1 ]; then
        echo ""
        echo "Install required tools:"
        echo "  Ubuntu/Debian: sudo apt install coreutils"
        echo "  Arch Linux: (included in base)"
        exit 1
    fi
}

# Configure serial port
configure_serial() {
    print_info "Configuring serial port: $SERIAL_PORT"
    print_info "Baud rate: $BAUD_RATE"

    # Configure serial port: 8N1, no flow control
    stty -F "$SERIAL_PORT" \
        "$BAUD_RATE" \
        cs8 \
        -cstopb \
        -parenb \
        -ixon \
        -ixoff \
        -crtscts \
        raw

    print_success "Serial port configured"
}

# Capture serial output
capture_serial() {
    print_header "Capturing Serial Output"

    print_info "Serial port: $SERIAL_PORT"
    print_info "Log file: $LOG_FILE"
    print_info "Timeout: ${TIMEOUT}s"
    print_info ""
    print_info "Boot the test machine now..."
    print_info "Press Ctrl+C to stop capture"
    echo ""

    # Create log directory
    mkdir -p "$(dirname "$LOG_FILE")"

    # Capture with timeout
    if [ $MONITOR_ONLY -eq 1 ]; then
        # Monitor mode: live output, no timeout
        cat "$SERIAL_PORT" | tee "$LOG_FILE"
    else
        # Capture mode: timeout, save to file
        timeout "$TIMEOUT" cat "$SERIAL_PORT" | tee "$LOG_FILE" || true
    fi

    echo ""
    print_success "Capture complete"
    print_info "Output saved to: $LOG_FILE"
}

# Validate boot messages
validate_boot() {
    print_header "Validating Boot Messages"

    if [ ! -f "$LOG_FILE" ]; then
        print_failure "Log file not found: $LOG_FILE"
        return 1
    fi

    local output=$(cat "$LOG_FILE")
    local passed=0
    local failed=0

    # Check critical boot messages
    if echo "$output" | grep -q "AutomationOS"; then
        print_success "Kernel banner found"
        ((passed++))
    else
        print_failure "Kernel banner not found"
        ((failed++))
    fi

    if echo "$output" | grep -q "\[BOOT\]"; then
        print_success "Bootloader messages found"
        ((passed++))
    else
        print_failure "Bootloader messages not found"
        ((failed++))
    fi

    if echo "$output" | grep -q "\[PMM\]"; then
        print_success "Physical Memory Manager initialized"
        ((passed++))
    else
        print_failure "PMM not initialized"
        ((failed++))
    fi

    if echo "$output" | grep -q "\[VMM\]"; then
        print_success "Virtual Memory Manager initialized"
        ((passed++))
    else
        print_failure "VMM not initialized"
        ((failed++))
    fi

    if echo "$output" | grep -q "\[HEAP\]"; then
        print_success "Kernel heap initialized"
        ((passed++))
    else
        print_failure "Heap not initialized"
        ((failed++))
    fi

    if echo "$output" | grep -q "\[GDT\]"; then
        print_success "Global Descriptor Table loaded"
        ((passed++))
    else
        print_failure "GDT not loaded"
        ((failed++))
    fi

    if echo "$output" | grep -q "\[IDT\]"; then
        print_success "Interrupt Descriptor Table loaded"
        ((passed++))
    else
        print_failure "IDT not loaded"
        ((failed++))
    fi

    if echo "$output" | grep -q "\[PIT\]"; then
        print_success "Timer initialized"
        ((passed++))
    else
        print_failure "Timer not initialized"
        ((failed++))
    fi

    # Check for errors
    if echo "$output" | grep -iq "panic\|fault\|error"; then
        print_warning "Errors detected in output"
        echo ""
        echo "Error messages:"
        echo "$output" | grep -i "panic\|fault\|error"
    fi

    # Print summary
    echo ""
    print_header "Validation Summary"
    echo "Passed: $passed"
    echo "Failed: $failed"
    echo "Total:  $((passed + failed))"
    echo ""

    if [ $failed -eq 0 ]; then
        print_success "All checks passed!"
        return 0
    else
        print_failure "$failed check(s) failed"
        return 1
    fi
}

# Print hardware info reminder
print_hardware_reminder() {
    print_header "Hardware Testing Checklist"
    echo "After boot, please document:"
    echo ""
    echo "1. Hardware specifications:"
    echo "   - CPU model and features"
    echo "   - RAM size and type"
    echo "   - Storage type (HDD/SSD/NVMe)"
    echo "   - Graphics card"
    echo "   - Motherboard model"
    echo ""
    echo "2. Boot method:"
    echo "   - USB or CD/DVD"
    echo "   - UEFI or legacy BIOS"
    echo ""
    echo "3. Boot result:"
    echo "   - ✅ Works: Boots successfully"
    echo "   - ⚠️ Partial: Boots with issues"
    echo "   - ❌ Broken: Does not boot"
    echo ""
    echo "4. Any error messages or issues"
    echo ""
    echo "Please report results to:"
    echo "  GitHub Issues or egotbrawlter@gmail.com"
    echo ""
}

# Main
main() {
    print_header "AutomationOS Hardware Testing"

    check_tools
    configure_serial

    # Print hardware reminder first
    if [ $MONITOR_ONLY -eq 0 ]; then
        print_hardware_reminder
    fi

    # Capture serial output
    capture_serial

    # Validate if requested
    if [ $VALIDATE -eq 1 ] && [ $MONITOR_ONLY -eq 0 ]; then
        echo ""
        validate_boot
    fi

    echo ""
    print_info "Testing complete"
    print_info "Log saved to: $LOG_FILE"
}

# Handle Ctrl+C gracefully
trap 'echo ""; print_info "Capture interrupted by user"; exit 0' INT

main
