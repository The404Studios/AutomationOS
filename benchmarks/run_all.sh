#!/bin/bash
#
# Run All Performance Benchmarks
#
# This script runs the complete benchmark suite and generates reports.

set -e

RESULTS_DIR="results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_FILE="${RESULTS_DIR}/benchmark_${TIMESTAMP}.txt"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================"
echo "AutomationOS Performance Benchmark Suite"
echo -e "========================================${NC}"
echo ""

# Create results directory
mkdir -p "${RESULTS_DIR}"

# Check if benchmarks are built
if [ ! -f "micro/context_switch_bench" ]; then
    echo -e "${YELLOW}Building benchmarks...${NC}"
    make clean
    make all
    echo ""
fi

# System information
echo -e "${BLUE}=== System Information ===${NC}"
echo "Date: $(date)"
echo "Hostname: $(hostname)"
echo "Kernel: $(uname -r)"
echo "CPU: $(lscpu | grep 'Model name' | cut -d: -f2 | xargs)"
echo "CPU Cores: $(nproc)"
echo "Memory: $(free -h | grep Mem | awk '{print $2}')"
echo "Governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'N/A')"
echo ""

# Check CPU governor (should be performance for benchmarks)
GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'N/A')
if [ "$GOVERNOR" != "performance" ] && [ "$GOVERNOR" != "N/A" ]; then
    echo -e "${YELLOW}⚠ Warning: CPU governor is '$GOVERNOR', not 'performance'${NC}"
    echo "For accurate results, set governor to 'performance':"
    echo "  sudo cpupower frequency-set -g performance"
    echo ""
    read -p "Continue anyway? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Function to run a benchmark and capture output
run_benchmark() {
    local bench=$1
    local name=$(basename $bench)

    echo -e "${GREEN}Running: ${name}${NC}"
    echo "========================================"
    echo ""

    # Run benchmark and tee to result file
    if ./"$bench" 2>&1 | tee -a "${RESULT_FILE}"; then
        echo -e "${GREEN}✓ ${name} completed${NC}"
    else
        echo -e "${RED}✗ ${name} failed${NC}"
        return 1
    fi

    echo ""
    echo ""
}

# Start benchmarking
{
    echo "========================================"
    echo "AutomationOS Performance Benchmark Suite"
    echo "========================================"
    echo ""
    echo "Timestamp: $(date)"
    echo "System: $(uname -a)"
    echo ""
} > "${RESULT_FILE}"

# Micro-benchmarks
echo -e "${BLUE}=== Micro-Benchmarks ===${NC}"
echo ""

if [ -f "micro/context_switch_bench" ]; then
    run_benchmark "micro/context_switch_bench"
fi

if [ -f "micro/allocation_bench" ]; then
    run_benchmark "micro/allocation_bench"
fi

if [ -f "micro/syscall_bench" ]; then
    run_benchmark "micro/syscall_bench"
fi

# Macro-benchmarks
echo -e "${BLUE}=== Macro-Benchmarks ===${NC}"
echo ""

if [ -f "macro/boot_time_bench" ]; then
    run_benchmark "macro/boot_time_bench"
fi

if [ -f "macro/process_bench" ]; then
    run_benchmark "macro/process_bench"
fi

# Workload benchmarks
echo -e "${BLUE}=== Workload Benchmarks ===${NC}"
echo ""

if [ -f "workloads/compile_bench" ]; then
    run_benchmark "workloads/compile_bench"
fi

# Regression baseline
echo -e "${BLUE}=== Regression Check ===${NC}"
echo ""

if [ -f "regression/baseline" ]; then
    run_benchmark "regression/baseline"
fi

# Stress tests (optional)
echo ""
echo -e "${YELLOW}=== Stress Tests (optional) ===${NC}"
echo "Stress tests can be resource-intensive and take a long time."
read -p "Run stress tests? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    if [ -f "stress/memory_stress" ]; then
        run_benchmark "stress/memory_stress"
    fi
fi

# Summary
echo ""
echo -e "${BLUE}========================================"
echo "Benchmark Suite Complete"
echo -e "========================================${NC}"
echo ""
echo "Results saved to: ${RESULT_FILE}"
echo ""

# Parse results and generate summary
echo -e "${BLUE}=== Performance Summary ===${NC}"
echo ""

# Extract key metrics
if grep -q "Context Switch" "${RESULT_FILE}"; then
    CONTEXT_SWITCH=$(grep "Median:" "${RESULT_FILE}" | head -1 | awk '{print $2}')
    echo "Context Switch (median): ${CONTEXT_SWITCH} ns"
fi

if grep -q "Allocation.*Fast Path" "${RESULT_FILE}"; then
    ALLOCATION=$(grep "Median:" "${RESULT_FILE}" | grep -A 10 "Fast Path" | head -1 | awk '{print $2}')
    echo "Page Allocation (median): ${ALLOCATION} ns"
fi

if grep -q "getpid.*Syscall" "${RESULT_FILE}"; then
    SYSCALL=$(grep "Median:" "${RESULT_FILE}" | grep -A 10 "getpid" | head -1 | awk '{print $2}')
    echo "Syscall getpid (median): ${SYSCALL} ns"
fi

echo ""

# Check regression report
if [ -f "regression_report.json" ]; then
    echo -e "${BLUE}=== Regression Report ===${NC}"
    if command -v jq &> /dev/null; then
        jq -r '.metrics[] | "\(.name): \(.change_percent)%"' regression_report.json
    else
        echo "Install 'jq' to parse JSON report"
        cat regression_report.json
    fi
    echo ""
fi

# Recommendations
echo -e "${BLUE}=== Recommendations ===${NC}"
echo ""
echo "1. Review detailed results in: ${RESULT_FILE}"
echo "2. Update baseline if this is a new baseline build:"
echo "   ./regression/baseline --update"
echo "3. Generate flamegraphs for bottleneck analysis:"
echo "   perf record -g -- ./micro/context_switch_bench"
echo "   perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg"
echo "4. Compare with previous results in ${RESULTS_DIR}/"
echo ""

# Archive results
ARCHIVE_FILE="${RESULTS_DIR}/benchmarks_${TIMESTAMP}.tar.gz"
tar -czf "${ARCHIVE_FILE}" "${RESULT_FILE}" regression_report.json 2>/dev/null || true
echo "Results archived to: ${ARCHIVE_FILE}"
echo ""

echo -e "${GREEN}✓ All benchmarks complete${NC}"
