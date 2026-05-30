#!/bin/bash
#
# AutomationOS QEMU CPU Compatibility Test
#
# Test AutomationOS on multiple QEMU CPU models to validate compatibility.
#
# Usage:
#   ./scripts/test-qemu-cpus.sh              # Test all CPU models
#   ./scripts/test-qemu-cpus.sh --quick      # Test only common CPUs
#   ./scripts/test-qemu-cpus.sh --list       # List available CPU models

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
ISO="build/AutomationOS.iso"
RESULTS_DIR="build/cpu-test-results"
TIMEOUT=15
MEMORY="4G"
CPUS="4"
QUICK_MODE=0

# Test results
declare -A RESULTS
TOTAL_TESTED=0
TOTAL_PASSED=0
TOTAL_FAILED=0

# CPU models to test
declare -a QUICK_CPUS=(
    "qemu64"
    "Haswell"
    "Skylake-Client"
    "EPYC"
)

declare -a ALL_CPUS=(
    "qemu64"
    "Nehalem"
    "Westmere"
    "SandyBridge"
    "IvyBridge"
    "Haswell"
    "Haswell-noTSX"
    "Broadwell"
    "Broadwell-noTSX"
    "Skylake-Client"
    "Skylake-Server"
    "Cascadelake-Server"
    "Cooperlake"
    "Icelake-Server"
    "Opteron_G1"
    "Opteron_G2"
    "Opteron_G3"
    "Opteron_G4"
    "Opteron_G5"
    "EPYC"
    "EPYC-Rome"
    "EPYC-Milan"
)

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
AutomationOS QEMU CPU Compatibility Test

Usage: $0 [OPTIONS]

Options:
    --quick               Test only common CPU models (faster)
    --list                List all available CPU models
    --memory <size>       Memory size (default: 4G)
    --cpus <count>        CPU count (default: 4)
    --timeout <seconds>   Boot timeout (default: 15)
    --help                Show this help message

Test Modes:
    Default mode: Test all CPU models (~22 models, ~10 minutes)
    Quick mode:   Test only common CPUs (4 models, ~2 minutes)

Quick mode CPUs:
    - qemu64 (generic x86_64)
    - Haswell (Intel)
    - Skylake-Client (Intel)
    - EPYC (AMD)

Full mode CPUs:
    - All Intel generations (Nehalem through Icelake)
    - All AMD Opteron and EPYC generations
    - Various feature configurations (with/without TSX)

Results:
    - Results saved to: build/cpu-test-results/
    - Summary report: build/cpu-test-results/summary.txt
    - Individual logs: build/cpu-test-results/<cpu-model>.log

Examples:
    # Quick test (recommended for CI)
    $0 --quick

    # Full compatibility test
    $0

    # List available CPUs
    $0 --list

    # Custom settings
    $0 --memory 8G --cpus 8 --timeout 30
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)
            QUICK_MODE=1
            shift
            ;;
        --list)
            echo "Available QEMU CPU models:"
            qemu-system-x86_64 -cpu help 2>/dev/null | grep "^x86" | awk '{print $2}' | sort
            exit 0
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
if [ ! -f "$ISO" ]; then
    echo -e "${RED}ERROR: ISO not found: $ISO${NC}"
    echo "Run 'make iso' first to build the ISO image"
    exit 1
fi

# Check for QEMU
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo -e "${RED}ERROR: qemu-system-x86_64 not found${NC}"
    echo "Install QEMU to run tests"
    exit 1
fi

# Create results directory
mkdir -p "$RESULTS_DIR"

# Test single CPU model
test_cpu_model() {
    local cpu_model=$1
    local log_file="$RESULTS_DIR/${cpu_model}.log"
    local serial_log="$RESULTS_DIR/${cpu_model}-serial.log"

    echo -ne "Testing ${BLUE}${cpu_model}${NC}..."

    # Remove old logs
    rm -f "$log_file" "$serial_log"

    # Build QEMU command
    local cmd=(
        qemu-system-x86_64
        -cdrom "$ISO"
        -m "$MEMORY"
        -smp "$CPUS"
        -cpu "$cpu_model"
        -serial file:"$serial_log"
        -display none
        -no-reboot
        -no-shutdown
    )

    # Run QEMU with timeout
    if timeout "$TIMEOUT" "${cmd[@]}" &> "$log_file"; then
        local timeout_status=0
    else
        local timeout_status=$?
    fi

    # Wait for file to be written
    sleep 0.5

    # Check if serial log was created
    if [ ! -f "$serial_log" ]; then
        echo -e " ${RED}FAIL${NC} (no output)"
        RESULTS[$cpu_model]="FAIL_NO_OUTPUT"
        ((TOTAL_FAILED++))
        return
    fi

    # Read serial output
    local output=$(cat "$serial_log")

    # Check for critical boot messages
    local checks_passed=0
    local checks_total=5

    if echo "$output" | grep -q "AutomationOS"; then
        ((checks_passed++))
    fi

    if echo "$output" | grep -q "\[PMM\]"; then
        ((checks_passed++))
    fi

    if echo "$output" | grep -q "\[VMM\]"; then
        ((checks_passed++))
    fi

    if echo "$output" | grep -q "\[HEAP\]"; then
        ((checks_passed++))
    fi

    if echo "$output" | grep -q "\[IDT\]"; then
        ((checks_passed++))
    fi

    # Check for errors
    if echo "$output" | grep -iq "panic\|triple fault\|double fault"; then
        echo -e " ${RED}FAIL${NC} (kernel panic)"
        RESULTS[$cpu_model]="FAIL_PANIC"
        ((TOTAL_FAILED++))
        return
    fi

    # Determine result
    if [ $checks_passed -eq $checks_total ]; then
        echo -e " ${GREEN}PASS${NC}"
        RESULTS[$cpu_model]="PASS"
        ((TOTAL_PASSED++))
    elif [ $checks_passed -gt 0 ]; then
        echo -e " ${YELLOW}PARTIAL${NC} ($checks_passed/$checks_total)"
        RESULTS[$cpu_model]="PARTIAL_$checks_passed"
        ((TOTAL_FAILED++))
    else
        echo -e " ${RED}FAIL${NC} (no boot)"
        RESULTS[$cpu_model]="FAIL_NO_BOOT"
        ((TOTAL_FAILED++))
    fi

    ((TOTAL_TESTED++))
}

# Test all CPUs
test_all_cpus() {
    local cpus_to_test

    if [ $QUICK_MODE -eq 1 ]; then
        cpus_to_test=("${QUICK_CPUS[@]}")
        print_header "Quick CPU Compatibility Test"
    else
        cpus_to_test=("${ALL_CPUS[@]}")
        print_header "Full CPU Compatibility Test"
    fi

    print_info "Testing ${#cpus_to_test[@]} CPU models"
    print_info "Memory: $MEMORY, CPUs: $CPUS, Timeout: ${TIMEOUT}s"
    echo ""

    for cpu in "${cpus_to_test[@]}"; do
        test_cpu_model "$cpu"
    done

    echo ""
}

# Generate summary report
generate_summary() {
    local summary_file="$RESULTS_DIR/summary.txt"

    {
        echo "AutomationOS QEMU CPU Compatibility Test Summary"
        echo "================================================"
        echo ""
        echo "Test Date: $(date)"
        echo "ISO: $ISO"
        echo "Mode: $([ $QUICK_MODE -eq 1 ] && echo "Quick" || echo "Full")"
        echo "Settings: Memory=$MEMORY, CPUs=$CPUS, Timeout=${TIMEOUT}s"
        echo ""
        echo "Results:"
        echo "--------"
        echo "Total Tested: $TOTAL_TESTED"
        echo "Passed:       $TOTAL_PASSED"
        echo "Failed:       $TOTAL_FAILED"
        echo "Pass Rate:    $(awk "BEGIN {printf \"%.1f%%\", ($TOTAL_PASSED / $TOTAL_TESTED) * 100}")"
        echo ""
        echo "Individual Results:"
        echo "-------------------"

        # Sort results by status
        for cpu in "${!RESULTS[@]}"; do
            printf "%-25s %s\n" "$cpu" "${RESULTS[$cpu]}"
        done | sort

        echo ""
        echo "Detailed Logs:"
        echo "--------------"
        echo "Results directory: $RESULTS_DIR"
        echo "Individual logs: $RESULTS_DIR/<cpu-model>.log"
        echo "Serial output:   $RESULTS_DIR/<cpu-model>-serial.log"

    } > "$summary_file"

    # Print to console
    cat "$summary_file"
}

# Generate detailed report with boot messages
generate_detailed_report() {
    local report_file="$RESULTS_DIR/detailed-report.txt"

    {
        echo "AutomationOS QEMU CPU Compatibility - Detailed Report"
        echo "======================================================"
        echo ""
        echo "Test Date: $(date)"
        echo ""

        for cpu in "${!RESULTS[@]}"; do
            local status="${RESULTS[$cpu]}"
            local serial_log="$RESULTS_DIR/${cpu}-serial.log"

            echo ""
            echo "=========================================="
            echo "CPU Model: $cpu"
            echo "Status:    $status"
            echo "=========================================="
            echo ""

            if [ -f "$serial_log" ]; then
                echo "Boot messages (first 30 lines):"
                echo "--------------------------------"
                head -30 "$serial_log"
                echo ""
                echo "[Full output in $serial_log]"
            else
                echo "(No serial output captured)"
            fi

            echo ""
        done

    } > "$report_file"

    print_info "Detailed report saved to: $report_file"
}

# Main
main() {
    print_header "AutomationOS QEMU CPU Compatibility Test"

    test_all_cpus

    print_header "Test Complete"

    generate_summary
    echo ""
    generate_detailed_report

    echo ""
    print_info "Results saved to: $RESULTS_DIR"

    # Exit with failure if any tests failed
    if [ $TOTAL_FAILED -gt 0 ]; then
        echo ""
        print_failure "$TOTAL_FAILED CPU model(s) failed"
        exit 1
    else
        echo ""
        print_success "All CPU models passed!"
        exit 0
    fi
}

main
