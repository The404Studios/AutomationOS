#!/bin/bash
#
# AutomationOS CI Hardware Testing Script
#
# Comprehensive hardware compatibility testing for CI/CD pipelines.
#
# Usage:
#   ./scripts/ci-hardware-test.sh              # Run all tests
#   ./scripts/ci-hardware-test.sh --quick      # Quick test (for fast CI)
#   ./scripts/ci-hardware-test.sh --stage boot # Run specific stage

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
ISO="build/AutomationOS.iso"
RESULTS_DIR="build/ci-test-results"
QUICK_MODE=0
STAGE="all"

# Test results
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

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
    ((PASSED_TESTS++))
}

print_failure() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((FAILED_TESTS++))
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_help() {
    cat << EOF
AutomationOS CI Hardware Testing Script

Usage: $0 [OPTIONS]

Options:
    --quick               Quick test mode (for fast CI)
    --stage <stage>       Run specific test stage
    --help                Show this help message

Test Stages:
    all                   All tests (default)
    boot                  Boot test (basic QEMU)
    cpu                   CPU compatibility test
    memory                Memory size tests
    stress                Stress tests

Quick Mode:
    - Runs minimal tests for fast feedback
    - Boot test on default CPU only
    - Single memory configuration
    - Duration: ~2 minutes

Full Mode:
    - Comprehensive hardware testing
    - Multiple CPU models
    - Multiple memory configurations
    - Duration: ~15 minutes

Examples:
    # Quick CI test
    $0 --quick

    # Full test
    $0

    # Boot test only
    $0 --stage boot

    # CPU compatibility only
    $0 --stage cpu

Exit Codes:
    0 - All tests passed
    1 - One or more tests failed
    2 - Setup error (missing ISO, QEMU, etc.)
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)
            QUICK_MODE=1
            shift
            ;;
        --stage)
            STAGE="$2"
            shift 2
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

# Check prerequisites
check_prerequisites() {
    print_header "Checking Prerequisites"

    local missing=0

    # Check for ISO
    if [ ! -f "$ISO" ]; then
        print_failure "ISO not found: $ISO"
        echo "Run 'make iso' first"
        missing=1
    else
        print_success "ISO found"
    fi

    # Check for QEMU
    if ! command -v qemu-system-x86_64 &> /dev/null; then
        print_failure "qemu-system-x86_64 not found"
        missing=1
    else
        print_success "QEMU found"
    fi

    if [ $missing -eq 1 ]; then
        echo ""
        echo "Fix missing prerequisites before running tests"
        exit 2
    fi

    echo ""
}

# Boot test
test_boot() {
    print_header "Boot Test"

    ((TOTAL_TESTS++))

    local serial_log="$RESULTS_DIR/boot-test-serial.log"
    rm -f "$serial_log"

    print_info "Testing basic boot on default CPU..."

    # Run QEMU
    timeout 15 qemu-system-x86_64 \
        -cdrom "$ISO" \
        -m 4G \
        -smp 4 \
        -serial file:"$serial_log" \
        -display none \
        -no-reboot \
        -no-shutdown \
        &> /dev/null || true

    # Wait for file
    sleep 0.5

    # Verify output
    if [ ! -f "$serial_log" ]; then
        print_failure "Boot test failed (no output)"
        return
    fi

    local output=$(cat "$serial_log")

    # Check critical messages
    if echo "$output" | grep -q "AutomationOS" && \
       echo "$output" | grep -q "\[PMM\]" && \
       echo "$output" | grep -q "\[VMM\]" && \
       echo "$output" | grep -q "\[IDT\]"; then
        print_success "Boot test passed"
    else
        print_failure "Boot test failed (incomplete boot)"
    fi

    echo ""
}

# CPU compatibility test
test_cpu_compatibility() {
    print_header "CPU Compatibility Test"

    if [ $QUICK_MODE -eq 1 ]; then
        print_info "Quick mode: Testing qemu64 only"
        ((TOTAL_TESTS++))

        local serial_log="$RESULTS_DIR/cpu-qemu64-serial.log"
        rm -f "$serial_log"

        timeout 15 qemu-system-x86_64 \
            -cdrom "$ISO" \
            -m 4G \
            -cpu qemu64 \
            -serial file:"$serial_log" \
            -display none \
            -no-reboot \
            -no-shutdown \
            &> /dev/null || true

        sleep 0.5

        if [ -f "$serial_log" ] && grep -q "AutomationOS" "$serial_log"; then
            print_success "qemu64 CPU test passed"
        else
            print_failure "qemu64 CPU test failed"
        fi
    else
        print_info "Running comprehensive CPU test..."
        if bash scripts/test-qemu-cpus.sh --quick &> "$RESULTS_DIR/cpu-test.log"; then
            ((TOTAL_TESTS++))
            print_success "CPU compatibility test passed"
        else
            ((TOTAL_TESTS++))
            print_failure "CPU compatibility test failed"
        fi
    fi

    echo ""
}

# Memory size tests
test_memory_sizes() {
    print_header "Memory Size Tests"

    local memory_sizes

    if [ $QUICK_MODE -eq 1 ]; then
        memory_sizes=("1G")
    else
        memory_sizes=("1G" "2G" "4G" "8G")
    fi

    for mem in "${memory_sizes[@]}"; do
        ((TOTAL_TESTS++))

        print_info "Testing with $mem RAM..."

        local serial_log="$RESULTS_DIR/mem-${mem}-serial.log"
        rm -f "$serial_log"

        timeout 15 qemu-system-x86_64 \
            -cdrom "$ISO" \
            -m "$mem" \
            -smp 2 \
            -serial file:"$serial_log" \
            -display none \
            -no-reboot \
            -no-shutdown \
            &> /dev/null || true

        sleep 0.5

        if [ -f "$serial_log" ] && grep -q "\[PMM\]" "$serial_log"; then
            print_success "$mem RAM test passed"
        else
            print_failure "$mem RAM test failed"
        fi
    done

    echo ""
}

# Stress test
test_stress() {
    print_header "Stress Test"

    if [ $QUICK_MODE -eq 1 ]; then
        print_warning "Skipping stress test in quick mode"
        echo ""
        return
    fi

    ((TOTAL_TESTS++))

    print_info "Running extended boot test (30s timeout)..."

    local serial_log="$RESULTS_DIR/stress-test-serial.log"
    rm -f "$serial_log"

    timeout 30 qemu-system-x86_64 \
        -cdrom "$ISO" \
        -m 8G \
        -smp 8 \
        -serial file:"$serial_log" \
        -display none \
        -no-reboot \
        -no-shutdown \
        &> /dev/null || true

    sleep 0.5

    if [ -f "$serial_log" ] && grep -q "AutomationOS" "$serial_log"; then
        print_success "Stress test passed"
    else
        print_failure "Stress test failed"
    fi

    echo ""
}

# Generate CI report
generate_ci_report() {
    local report_file="$RESULTS_DIR/ci-report.txt"

    {
        echo "AutomationOS CI Hardware Test Report"
        echo "====================================="
        echo ""
        echo "Date: $(date)"
        echo "Mode: $([ $QUICK_MODE -eq 1 ] && echo "Quick" || echo "Full")"
        echo "Stage: $STAGE"
        echo ""
        echo "Results:"
        echo "--------"
        echo "Total Tests: $TOTAL_TESTS"
        echo "Passed:      $PASSED_TESTS"
        echo "Failed:      $FAILED_TESTS"
        echo ""

        if [ $FAILED_TESTS -eq 0 ]; then
            echo "Status: ✅ ALL TESTS PASSED"
        else
            echo "Status: ❌ $FAILED_TESTS TEST(S) FAILED"
        fi

        echo ""
        echo "Artifacts:"
        echo "----------"
        echo "Results directory: $RESULTS_DIR"
        echo "Serial logs:       $RESULTS_DIR/*-serial.log"

    } > "$report_file"

    cat "$report_file"
}

# Print summary
print_summary() {
    print_header "Test Summary"

    echo "Total Tests: $TOTAL_TESTS"
    echo "Passed:      $PASSED_TESTS"
    echo "Failed:      $FAILED_TESTS"
    echo ""

    if [ $FAILED_TESTS -eq 0 ]; then
        print_success "All tests passed!"
        return 0
    else
        print_failure "$FAILED_TESTS test(s) failed"
        return 1
    fi
}

# Main
main() {
    print_header "AutomationOS CI Hardware Testing"

    if [ $QUICK_MODE -eq 1 ]; then
        print_info "Running in QUICK mode"
    else
        print_info "Running in FULL mode"
    fi

    # Create results directory
    mkdir -p "$RESULTS_DIR"

    # Check prerequisites
    check_prerequisites

    # Run tests based on stage
    case "$STAGE" in
        all)
            test_boot
            test_cpu_compatibility
            test_memory_sizes
            test_stress
            ;;
        boot)
            test_boot
            ;;
        cpu)
            test_cpu_compatibility
            ;;
        memory)
            test_memory_sizes
            ;;
        stress)
            test_stress
            ;;
        *)
            echo -e "${RED}ERROR: Unknown stage: $STAGE${NC}"
            exit 1
            ;;
    esac

    # Generate report
    echo ""
    generate_ci_report

    # Print summary and exit
    echo ""
    if print_summary; then
        exit 0
    else
        exit 1
    fi
}

main
