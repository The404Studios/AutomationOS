#!/bin/bash
###############################################################################
# AutoFS Filesystem Test Execution Script
#
# This script builds and runs comprehensive tests for the AutoFS filesystem.
# It generates a detailed test report and checks for production readiness.
###############################################################################

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test configuration
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(dirname "$TEST_DIR")"
AUTOFS_DIR="$KERNEL_DIR/kernel/fs/autofs"
BUILD_DIR="$TEST_DIR/build"
REPORT_FILE="$TEST_DIR/autofs_test_report_$(date +%Y%m%d_%H%M%S).txt"
LOG_FILE="$TEST_DIR/autofs_test_$(date +%Y%m%d_%H%M%S).log"

echo -e "${BLUE}╔════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║         AutoFS Filesystem Validation Test Suite               ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Function to print section header
print_section() {
    echo ""
    echo -e "${BLUE}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════════════════${NC}"
}

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to log and display
log() {
    echo -e "$@" | tee -a "$LOG_FILE"
}

###############################################################################
# 1. ENVIRONMENT CHECK
###############################################################################

print_section "1. Environment Check"

log "Checking build environment..."

# Check for required tools
REQUIRED_TOOLS=("gcc" "make")
MISSING_TOOLS=()

for tool in "${REQUIRED_TOOLS[@]}"; do
    if command_exists "$tool"; then
        log "${GREEN}✓${NC} $tool found"
    else
        log "${RED}✗${NC} $tool not found"
        MISSING_TOOLS+=("$tool")
    fi
done

# Check for optional tools
OPTIONAL_TOOLS=("valgrind" "gdb" "perf")
for tool in "${OPTIONAL_TOOLS[@]}"; do
    if command_exists "$tool"; then
        log "${GREEN}✓${NC} $tool found (optional)"
    else
        log "${YELLOW}⊘${NC} $tool not found (optional)"
    fi
done

# Check for required libraries
log "\nChecking for required libraries..."
REQUIRED_LIBS=("zstd" "lz4" "z" "crypto" "ssl" "pthread")
MISSING_LIBS=()

for lib in "${REQUIRED_LIBS[@]}"; do
    if ldconfig -p | grep -q "lib${lib}"; then
        log "${GREEN}✓${NC} lib$lib found"
    else
        log "${YELLOW}⊘${NC} lib$lib not found"
        MISSING_LIBS+=("$lib")
    fi
done

if [ ${#MISSING_TOOLS[@]} -gt 0 ]; then
    log "${RED}ERROR: Missing required tools: ${MISSING_TOOLS[*]}${NC}"
    exit 1
fi

if [ ${#MISSING_LIBS[@]} -gt 0 ]; then
    log "${YELLOW}WARNING: Some libraries are missing: ${MISSING_LIBS[*]}${NC}"
    log "${YELLOW}Tests may be skipped or fail. Install with:${NC}"
    log "  sudo apt-get install libzstd-dev liblz4-dev zlib1g-dev libssl-dev"
fi

###############################################################################
# 2. BUILD AUTOFS LIBRARY
###############################################################################

print_section "2. Building AutoFS Library"

log "Building AutoFS library..."
cd "$AUTOFS_DIR"

if make clean > /dev/null 2>&1 && make > /dev/null 2>&1; then
    log "${GREEN}✓${NC} AutoFS library built successfully"
    if [ -f "libautofs.so" ]; then
        log "  Library: libautofs.so"
        log "  Size: $(stat -f%z libautofs.so 2>/dev/null || stat -c%s libautofs.so) bytes"
    fi
else
    log "${RED}✗${NC} AutoFS library build failed"
    log "Build log available at: $LOG_FILE"
    exit 1
fi

###############################################################################
# 3. BUILD TEST SUITE
###############################################################################

print_section "3. Building Test Suite"

cd "$TEST_DIR"

log "Building comprehensive test suite..."

# Build with detailed flags
gcc -Wall -Wextra -O2 -g \
    -I"$KERNEL_DIR/kernel/include" \
    -pthread \
    -o autofs_comprehensive_test \
    autofs_comprehensive_test.c \
    -L"$AUTOFS_DIR" -lautofs \
    -lzstd -llz4 -lz -lcrypto -lssl \
    -lpthread -lm 2>&1 | tee -a "$LOG_FILE"

if [ -f "autofs_comprehensive_test" ]; then
    log "${GREEN}✓${NC} Test suite built successfully"
    chmod +x autofs_comprehensive_test
else
    log "${RED}✗${NC} Test suite build failed"
    exit 1
fi

# Also build basic test if it exists
if [ -f "test_autofs.c" ]; then
    log "\nBuilding basic test suite..."
    gcc -Wall -Wextra -O2 -g \
        -I"$KERNEL_DIR/kernel/include" \
        -o test_autofs \
        test_autofs.c \
        -L"$AUTOFS_DIR" -lautofs \
        -lzstd -llz4 -lz -lcrypto -lssl \
        -lm 2>&1 | tee -a "$LOG_FILE"

    if [ -f "test_autofs" ]; then
        log "${GREEN}✓${NC} Basic test suite built"
        chmod +x test_autofs
    fi
fi

###############################################################################
# 4. RUN TESTS
###############################################################################

print_section "4. Running Tests"

log "Starting test execution..."
log "Test output will be logged to: $LOG_FILE"
log "Test report will be saved to: $REPORT_FILE"
log ""

# Set library path
export LD_LIBRARY_PATH="$AUTOFS_DIR:$LD_LIBRARY_PATH"

# Run comprehensive tests
log "${BLUE}>>> Running Comprehensive Test Suite <<<${NC}\n"

# Capture start time
START_TIME=$(date +%s)

# Run tests and capture output
if ./autofs_comprehensive_test 2>&1 | tee -a "$LOG_FILE"; then
    TEST_EXIT_CODE=0
    log "\n${GREEN}✓ Test suite completed successfully${NC}"
else
    TEST_EXIT_CODE=$?
    log "\n${RED}✗ Test suite completed with failures (exit code: $TEST_EXIT_CODE)${NC}"
fi

# Capture end time
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

###############################################################################
# 5. ANALYZE RESULTS
###############################################################################

print_section "5. Test Results Analysis"

log "Analyzing test results..."

# Extract test statistics from log
TOTAL_TESTS=$(grep -c "^\[TEST" "$LOG_FILE" || echo "0")
PASSED_TESTS=$(grep -c "✓ PASSED" "$LOG_FILE" || echo "0")
FAILED_TESTS=$(grep -c "✗ FAILED" "$LOG_FILE" || echo "0")
SKIPPED_TESTS=$(grep -c "⊘ SKIPPED" "$LOG_FILE" || echo "0")

log "\nTest Statistics:"
log "  Total tests:   $TOTAL_TESTS"
log "  Passed:        ${GREEN}$PASSED_TESTS${NC}"
log "  Failed:        ${RED}$FAILED_TESTS${NC}"
log "  Skipped:       ${YELLOW}$SKIPPED_TESTS${NC}"
log "  Duration:      ${DURATION}s"

if [ "$TOTAL_TESTS" -gt 0 ]; then
    PASS_RATE=$((PASSED_TESTS * 100 / TOTAL_TESTS))
    log "  Pass rate:     ${PASS_RATE}%"
fi

# Extract performance numbers if available
log "\nPerformance Results:"
if grep -q "Read speed" "$LOG_FILE"; then
    READ_SPEED=$(grep "Read speed:" "$LOG_FILE" | tail -1 | awk '{print $3 " " $4}')
    log "  Sequential read:  $READ_SPEED"
fi
if grep -q "Write speed" "$LOG_FILE"; then
    WRITE_SPEED=$(grep "Write speed:" "$LOG_FILE" | tail -1 | awk '{print $3 " " $4}')
    log "  Sequential write: $WRITE_SPEED"
fi
if grep -q "Cache hit rate" "$LOG_FILE"; then
    CACHE_RATE=$(grep "Cache hit rate:" "$LOG_FILE" | tail -1 | awk '{print $4}')
    log "  Cache hit rate:   $CACHE_RATE"
fi

###############################################################################
# 6. GENERATE REPORT
###############################################################################

print_section "6. Generating Test Report"

cat > "$REPORT_FILE" << EOF
================================================================================
                AutoFS Filesystem Validation Report
================================================================================

Date:              $(date)
Test Duration:     ${DURATION}s
Environment:       $(uname -s) $(uname -r) $(uname -m)
Compiler:          $(gcc --version | head -1)

================================================================================
                          TEST RESULTS SUMMARY
================================================================================

Total Tests:       $TOTAL_TESTS
Passed:            $PASSED_TESTS
Failed:            $FAILED_TESTS
Skipped:           $SKIPPED_TESTS

Pass Rate:         ${PASS_RATE}%

================================================================================
                        PERFORMANCE RESULTS
================================================================================

Sequential Read:   $(grep "Read speed:" "$LOG_FILE" | tail -1 | awk '{print $3 " " $4}' || echo "N/A")
Sequential Write:  $(grep "Write speed:" "$LOG_FILE" | tail -1 | awk '{print $3 " " $4}' || echo "N/A")
Cache Hit Rate:    $(grep "Cache hit rate:" "$LOG_FILE" | tail -1 | awk '{print $4}' || echo "N/A")

Target Performance:
  Sequential Read:  2.5 GB/s (minimum: 1.0 GB/s)
  Sequential Write: 1.8 GB/s (minimum: 800 MB/s)
  Cache Hit Rate:   90% (minimum: 70%)

================================================================================
                        PRODUCTION READINESS
================================================================================

EOF

# Determine production readiness
CRITICAL_PASSED=true
PROD_READY=true

# Check critical tests
if [ "$FAILED_TESTS" -gt 0 ]; then
    if grep -q "FAILED.*Basic:" "$LOG_FILE"; then
        echo "CRITICAL FAILURE: Basic operations failed" >> "$REPORT_FILE"
        CRITICAL_PASSED=false
        PROD_READY=false
    fi
    if grep -q "FAILED.*Journaling:" "$LOG_FILE"; then
        echo "CRITICAL FAILURE: Journaling tests failed" >> "$REPORT_FILE"
        CRITICAL_PASSED=false
        PROD_READY=false
    fi
fi

if [ "$CRITICAL_PASSED" = true ]; then
    echo "✓ All critical tests passed" >> "$REPORT_FILE"
else
    echo "✗ Critical tests failed - NOT PRODUCTION READY" >> "$REPORT_FILE"
fi

# Overall assessment
echo "" >> "$REPORT_FILE"
echo "Production Readiness Assessment:" >> "$REPORT_FILE"

if [ "$FAILED_TESTS" -eq 0 ] && [ "$PASSED_TESTS" -gt 20 ]; then
    echo "  STATUS: ✓✓✓ PRODUCTION READY ✓✓✓" >> "$REPORT_FILE"
    echo "  All tests passed. Filesystem is ready for production use." >> "$REPORT_FILE"
    log "${GREEN}✓✓✓ PRODUCTION READY ✓✓✓${NC}"
elif [ "$CRITICAL_PASSED" = true ] && [ "$FAILED_TESTS" -lt 5 ]; then
    echo "  STATUS: ⚠ CONDITIONALLY READY" >> "$REPORT_FILE"
    echo "  Critical tests passed but some features need work." >> "$REPORT_FILE"
    log "${YELLOW}⚠ CONDITIONALLY READY${NC}"
else
    echo "  STATUS: ✗✗✗ NOT PRODUCTION READY ✗✗✗" >> "$REPORT_FILE"
    echo "  Critical failures detected. Requires fixes before use." >> "$REPORT_FILE"
    log "${RED}✗✗✗ NOT PRODUCTION READY ✗✗✗${NC}"
fi

# Add failed test details if any
if [ "$FAILED_TESTS" -gt 0 ]; then
    echo "" >> "$REPORT_FILE"
    echo "================================================================================
                            FAILED TESTS
================================================================================" >> "$REPORT_FILE"
    grep "✗ FAILED" "$LOG_FILE" >> "$REPORT_FILE" || true
fi

echo "" >> "$REPORT_FILE"
echo "Full test log available at: $LOG_FILE" >> "$REPORT_FILE"
echo "================================================================================
                              END OF REPORT
================================================================================" >> "$REPORT_FILE"

log "\n${GREEN}✓${NC} Test report generated: $REPORT_FILE"

###############################################################################
# 7. OPTIONAL: RUN VALGRIND
###############################################################################

if command_exists valgrind; then
    print_section "7. Memory Leak Detection (Optional)"

    log "Running valgrind memory leak detection..."
    log "This may take several minutes..."

    VALGRIND_LOG="$TEST_DIR/valgrind_autofs_$(date +%Y%m%d_%H%M%S).log"

    valgrind --leak-check=full \
             --show-leak-kinds=all \
             --track-origins=yes \
             --verbose \
             --log-file="$VALGRIND_LOG" \
             ./autofs_comprehensive_test > /dev/null 2>&1 || true

    if [ -f "$VALGRIND_LOG" ]; then
        LEAK_COUNT=$(grep -c "definitely lost" "$VALGRIND_LOG" || echo "0")
        ERROR_COUNT=$(grep -c "ERROR SUMMARY" "$VALGRIND_LOG" || echo "0")

        log "\nValgrind Results:"
        log "  Memory leaks detected: $LEAK_COUNT"
        log "  Error summary count:   $ERROR_COUNT"
        log "  Full report: $VALGRIND_LOG"

        if [ "$LEAK_COUNT" -eq 0 ]; then
            log "${GREEN}✓${NC} No memory leaks detected"
        else
            log "${RED}✗${NC} Memory leaks detected - review valgrind log"
        fi
    fi
else
    log "\n${YELLOW}⊘${NC} Valgrind not available - skipping memory leak detection"
fi

###############################################################################
# 8. SUMMARY
###############################################################################

print_section "8. Summary"

log "\nTest execution complete!"
log ""
log "Key outputs:"
log "  - Test report:  $REPORT_FILE"
log "  - Full log:     $LOG_FILE"
if [ -f "$VALGRIND_LOG" ]; then
    log "  - Valgrind log: $VALGRIND_LOG"
fi
log ""

if [ "$TEST_EXIT_CODE" -eq 0 ] && [ "$FAILED_TESTS" -eq 0 ]; then
    log "${GREEN}╔════════════════════════════════════════════════════════════════╗${NC}"
    log "${GREEN}║                  ALL TESTS PASSED ✓✓✓                         ║${NC}"
    log "${GREEN}║           AutoFS is ready for production use!                  ║${NC}"
    log "${GREEN}╚════════════════════════════════════════════════════════════════╝${NC}"
    exit 0
else
    log "${RED}╔════════════════════════════════════════════════════════════════╗${NC}"
    log "${RED}║                  SOME TESTS FAILED ✗✗✗                        ║${NC}"
    log "${RED}║        AutoFS requires fixes before production use.            ║${NC}"
    log "${RED}╚════════════════════════════════════════════════════════════════╝${NC}"
    exit 1
fi
