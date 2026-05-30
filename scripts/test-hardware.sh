#!/bin/bash
#
# AutomationOS Hardware Testing Script
#
# Test AutomationOS on different virtual machines and hardware platforms.
#
# Usage:
#   ./scripts/test-hardware.sh --vm qemu                    # Test on QEMU (default)
#   ./scripts/test-hardware.sh --vm qemu --cpu-model Haswell # Test specific CPU
#   ./scripts/test-hardware.sh --vm virtualbox              # Test on VirtualBox
#   ./scripts/test-hardware.sh --vm vmware                  # Test on VMware
#   ./scripts/test-hardware.sh --vm hyperv                  # Test on Hyper-V
#   ./scripts/test-hardware.sh --vm kvm                     # Test on KVM
#   ./scripts/test-hardware.sh --all                        # Test all platforms

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
ISO="build/AutomationOS.iso"
SERIAL_LOG="build/serial-test.log"
TIMEOUT=15
MEMORY="4G"
CPUS="4"
VM_PLATFORM="qemu"
CPU_MODEL="qemu64"
TEST_ALL=0

# Test results
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

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
    ((TESTS_PASSED++))
}

print_failure() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

print_skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
    ((TESTS_SKIPPED++))
}

print_help() {
    cat << EOF
AutomationOS Hardware Testing Script

Usage: $0 [OPTIONS]

Options:
    --vm <platform>       Virtual machine platform to test
                          (qemu, virtualbox, vmware, hyperv, kvm)
    --cpu-model <model>   QEMU CPU model to test (default: qemu64)
    --memory <size>       Memory size (default: 4G)
    --cpus <count>        CPU count (default: 4)
    --timeout <seconds>   Boot timeout (default: 15)
    --all                 Test all platforms
    --help                Show this help message

Platforms:
    qemu          QEMU emulator (default)
    virtualbox    Oracle VirtualBox
    vmware        VMware Workstation
    hyperv        Microsoft Hyper-V (Windows only)
    kvm           Linux KVM (virt-manager/virsh)

QEMU CPU Models:
    qemu64        Default QEMU CPU (generic x86_64)
    host          Host CPU passthrough (KVM)
    IvyBridge     Intel Ivy Bridge
    Haswell       Intel Haswell
    Broadwell     Intel Broadwell
    Skylake-Client    Intel Skylake (client)
    Cascadelake-Server Intel Cascade Lake (server)
    EPYC          AMD EPYC (1st gen)
    EPYC-Rome     AMD EPYC Rome (2nd gen)

Examples:
    $0 --vm qemu
    $0 --vm qemu --cpu-model Haswell
    $0 --vm virtualbox
    $0 --all

Notes:
    - Requires ISO to be built first: make iso
    - Some platforms require specific software installed
    - Use --all to test all available platforms
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --vm)
            VM_PLATFORM="$2"
            shift 2
            ;;
        --cpu-model)
            CPU_MODEL="$2"
            shift 2
            ;;
        --memory)
            MEMORY="$2"
            shift 2
            ;;
        --cpus)
            CPUS="$2"
            shift 2
            ;;
        --timeout)
            TIMEOUT="$2"
            shift 2
            ;;
        --all)
            TEST_ALL=1
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

# Check for ISO
check_iso() {
    if [ ! -f "$ISO" ]; then
        echo -e "${RED}ERROR: ISO not found: $ISO${NC}"
        echo "Run 'make iso' first to build the ISO image"
        exit 1
    fi
    print_info "ISO found: $ISO"
}

# Test on QEMU
test_qemu() {
    local cpu_model=$1
    print_header "Testing on QEMU (CPU: $cpu_model)"

    # Check for QEMU
    if ! command -v qemu-system-x86_64 &> /dev/null; then
        print_skip "QEMU not installed"
        return
    fi

    print_info "Starting QEMU with $cpu_model CPU model..."
    print_info "Memory: $MEMORY, CPUs: $CPUS, Timeout: ${TIMEOUT}s"

    # Remove old serial log
    rm -f "$SERIAL_LOG"

    # Build QEMU command
    local cmd=(
        qemu-system-x86_64
        -cdrom "$ISO"
        -m "$MEMORY"
        -smp "$CPUS"
        -cpu "$cpu_model"
        -serial file:"$SERIAL_LOG"
        -display none
        -no-reboot
        -no-shutdown
    )

    # Add KVM if host CPU
    if [ "$cpu_model" = "host" ]; then
        cmd+=(-enable-kvm)
    fi

    print_info "Command: ${cmd[*]}"

    # Run QEMU with timeout
    timeout $TIMEOUT "${cmd[@]}" &> /dev/null || true

    # Wait a moment for file to be written
    sleep 1

    # Check serial output
    if [ ! -f "$SERIAL_LOG" ]; then
        print_failure "Serial log not created"
        return
    fi

    # Verify boot messages
    local output=$(cat "$SERIAL_LOG")

    if echo "$output" | grep -q "AutomationOS"; then
        print_success "Kernel banner found"
    else
        print_failure "Kernel banner not found"
    fi

    if echo "$output" | grep -q "\[PMM\]"; then
        print_success "Physical Memory Manager initialized"
    else
        print_failure "PMM not initialized"
    fi

    if echo "$output" | grep -q "\[VMM\]"; then
        print_success "Virtual Memory Manager initialized"
    else
        print_failure "VMM not initialized"
    fi

    if echo "$output" | grep -q "\[HEAP\]"; then
        print_success "Kernel heap initialized"
    else
        print_failure "Heap not initialized"
    fi

    if echo "$output" | grep -q "\[IDT\]"; then
        print_success "Interrupt Descriptor Table loaded"
    else
        print_failure "IDT not loaded"
    fi

    # Save detailed log
    local log_file="build/qemu-${cpu_model}-test.log"
    cp "$SERIAL_LOG" "$log_file"
    print_info "Detailed log saved to: $log_file"

    echo ""
}

# Test multiple QEMU CPU models
test_qemu_all_cpus() {
    local cpu_models=(
        "qemu64"
        "IvyBridge"
        "Haswell"
        "Broadwell"
        "Skylake-Client"
        "Cascadelake-Server"
        "EPYC"
        "EPYC-Rome"
    )

    for cpu in "${cpu_models[@]}"; do
        test_qemu "$cpu"
    done
}

# Test on VirtualBox
test_virtualbox() {
    print_header "Testing on VirtualBox"

    # Check for VirtualBox
    if ! command -v VBoxManage &> /dev/null; then
        print_skip "VirtualBox not installed"
        return
    fi

    print_info "VirtualBox testing not yet automated"
    print_skip "Manual testing required"
    print_info "See docs/PLATFORM_SUPPORT.md for manual setup"

    echo ""
}

# Test on VMware
test_vmware() {
    print_header "Testing on VMware Workstation"

    # Check for VMware
    if ! command -v vmrun &> /dev/null; then
        print_skip "VMware not installed or vmrun not in PATH"
        return
    fi

    print_info "VMware testing not yet automated"
    print_skip "Manual testing required"
    print_info "See docs/PLATFORM_SUPPORT.md for manual setup"

    echo ""
}

# Test on Hyper-V
test_hyperv() {
    print_header "Testing on Microsoft Hyper-V"

    # Hyper-V is Windows-only
    if [[ "$OSTYPE" != "msys" && "$OSTYPE" != "win32" ]]; then
        print_skip "Hyper-V requires Windows"
        return
    fi

    print_info "Hyper-V testing not yet automated"
    print_skip "Manual testing required"
    print_info "See docs/PLATFORM_SUPPORT.md for manual setup"

    echo ""
}

# Test on KVM
test_kvm() {
    print_header "Testing on Linux KVM"

    # Check for KVM
    if [ ! -e /dev/kvm ]; then
        print_skip "KVM not available"
        return
    fi

    # Check for virt-manager/virsh
    if ! command -v virsh &> /dev/null; then
        print_skip "libvirt/virsh not installed"
        return
    fi

    print_info "KVM testing via QEMU with host CPU..."
    test_qemu "host"

    echo ""
}

# Test all platforms
test_all_platforms() {
    print_header "Testing All Platforms"

    test_qemu_all_cpus
    test_virtualbox
    test_vmware
    test_hyperv
    test_kvm
}

# Print summary
print_summary() {
    echo ""
    print_header "Test Summary"

    echo "Passed:  $TESTS_PASSED"
    echo "Failed:  $TESTS_FAILED"
    echo "Skipped: $TESTS_SKIPPED"
    echo "Total:   $((TESTS_PASSED + TESTS_FAILED + TESTS_SKIPPED))"

    echo ""

    if [ $TESTS_FAILED -eq 0 ]; then
        print_success "All tests passed!"
        return 0
    else
        print_failure "$TESTS_FAILED test(s) failed"
        return 1
    fi
}

# Main
main() {
    print_header "AutomationOS Hardware Testing"

    check_iso

    if [ $TEST_ALL -eq 1 ]; then
        test_all_platforms
    else
        case "$VM_PLATFORM" in
            qemu)
                test_qemu "$CPU_MODEL"
                ;;
            virtualbox)
                test_virtualbox
                ;;
            vmware)
                test_vmware
                ;;
            hyperv)
                test_hyperv
                ;;
            kvm)
                test_kvm
                ;;
            *)
                echo -e "${RED}ERROR: Unknown platform: $VM_PLATFORM${NC}"
                echo "Use --help for usage information"
                exit 1
                ;;
        esac
    fi

    print_summary
}

main
