#!/bin/bash
#
# SMP Test Suite Runner
# =====================
#
# Automated test runner for SMP validation with multiple CPU configurations.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
KERNEL="../kernel.bin"
QEMU="qemu-system-x86_64"
TIMEOUT=60  # seconds

# Test configurations
declare -a CPU_CONFIGS=(2 4 8 16)
declare -a MEM_CONFIGS=(256 512 1024 2048)

echo -e "${BLUE}================================${NC}"
echo -e "${BLUE}  SMP Test Suite Runner${NC}"
echo -e "${BLUE}================================${NC}"
echo ""

# Check if kernel exists
if [ ! -f "$KERNEL" ]; then
    echo -e "${RED}ERROR: Kernel not found at $KERNEL${NC}"
    echo "Please build the kernel first."
    exit 1
fi

# Check if QEMU is available
if ! command -v $QEMU &> /dev/null; then
    echo -e "${RED}ERROR: QEMU not found${NC}"
    echo "Please install qemu-system-x86_64"
    exit 1
fi

# Function to run test with specific configuration
run_test() {
    local cpus=$1
    local mem=$2
    local log="smp_test_${cpus}cpu_${mem}mb.log"

    echo -e "${YELLOW}Testing: ${cpus} CPUs, ${mem} MB RAM${NC}"
    echo "  Log: $log"

    # Run QEMU with timeout
    timeout $TIMEOUT $QEMU \
        -kernel $KERNEL \
        -smp cpus=$cpus \
        -m ${mem}M \
        -serial file:$log \
        -display none \
        -append "test_smp" \
        2>&1 > /dev/null &

    local qemu_pid=$!

    # Wait for QEMU to finish
    if wait $qemu_pid; then
        # Check for test success in log
        if grep -q "ALL TESTS PASSED" $log; then
            echo -e "  ${GREEN}✓ PASSED${NC}"
            return 0
        else
            echo -e "  ${RED}✗ FAILED${NC}"
            echo "  Check $log for details"
            return 1
        fi
    else
        echo -e "  ${RED}✗ TIMEOUT or CRASH${NC}"
        return 1
    fi
}

# Function to run all tests
run_all_tests() {
    local total=0
    local passed=0
    local failed=0

    for i in "${!CPU_CONFIGS[@]}"; do
        local cpus=${CPU_CONFIGS[$i]}
        local mem=${MEM_CONFIGS[$i]}

        echo ""
        total=$((total + 1))

        if run_test $cpus $mem; then
            passed=$((passed + 1))
        else
            failed=$((failed + 1))
        fi
    done

    # Print summary
    echo ""
    echo -e "${BLUE}================================${NC}"
    echo -e "${BLUE}  Test Summary${NC}"
    echo -e "${BLUE}================================${NC}"
    echo "  Total configurations: $total"
    echo -e "  Passed: ${GREEN}$passed${NC}"
    echo -e "  Failed: ${RED}$failed${NC}"
    echo ""

    if [ $failed -eq 0 ]; then
        echo -e "${GREEN}SUCCESS: All tests passed!${NC}"
        return 0
    else
        echo -e "${RED}FAILURE: $failed test(s) failed${NC}"
        return 1
    fi
}

# Function to run single test
run_single_test() {
    local cpus=${1:-4}
    local mem=${2:-512}

    echo ""
    run_test $cpus $mem
}

# Function to extract results from logs
extract_results() {
    echo ""
    echo -e "${BLUE}================================${NC}"
    echo -e "${BLUE}  Performance Results${NC}"
    echo -e "${BLUE}================================${NC}"
    echo ""

    printf "%-10s %-10s %-15s %-15s\n" "CPUs" "Memory" "Speedup" "Efficiency"
    echo "------------------------------------------------------------"

    for log in smp_test_*.log; do
        if [ -f "$log" ]; then
            # Extract configuration from filename
            cpus=$(echo $log | grep -oP '(?<=_)\d+(?=cpu)')
            mem=$(echo $log | grep -oP '(?<=_)\d+(?=mb)')

            # Extract results from log
            speedup=$(grep "Speedup:" $log | grep -oP '\d+\.\d+' || echo "N/A")
            efficiency=$(grep "Efficiency:" $log | grep -oP '\d+\.\d+' || echo "N/A")

            printf "%-10s %-10s %-15s %-15s\n" "$cpus" "${mem}MB" "${speedup}x" "${efficiency}%"
        fi
    done

    echo ""
}

# Function to clean logs
clean_logs() {
    echo "Cleaning test logs..."
    rm -f smp_test_*.log
    echo "Done."
}

# Main script
case "${1:-all}" in
    all)
        run_all_tests
        extract_results
        ;;
    single)
        run_single_test ${2:-4} ${3:-512}
        ;;
    results)
        extract_results
        ;;
    clean)
        clean_logs
        ;;
    help)
        echo "Usage: $0 [command] [args]"
        echo ""
        echo "Commands:"
        echo "  all              Run all test configurations (default)"
        echo "  single [c] [m]   Run single test with c CPUs and m MB RAM"
        echo "  results          Extract results from existing logs"
        echo "  clean            Clean test logs"
        echo "  help             Show this help"
        echo ""
        echo "Examples:"
        echo "  $0               # Run all tests"
        echo "  $0 single 8 1024 # Run test with 8 CPUs, 1GB RAM"
        echo "  $0 results       # Show results"
        ;;
    *)
        echo "Unknown command: $1"
        echo "Run '$0 help' for usage information"
        exit 1
        ;;
esac
