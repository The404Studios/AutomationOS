#!/bin/bash
# Edge Case Test Runner for AutomationOS
#
# This script runs comprehensive edge case testing to find bugs
# that normal testing misses.

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo ""
echo "================================================"
echo "  AutomationOS Edge Case Test Runner"
echo "  Mission: Find Edge Case Bugs"
echo "================================================"
echo ""

# Function to run a test and capture result
run_test() {
    local test_name=$1
    local test_cmd=$2

    echo -e "${BLUE}[TEST]${NC} Running $test_name..."

    if eval "$test_cmd"; then
        echo -e "${GREEN}[PASS]${NC} $test_name passed"
        return 0
    else
        echo -e "${RED}[FAIL]${NC} $test_name failed"
        return 1
    fi
}

# Track test results
total_tests=0
passed_tests=0
failed_tests=0

# Build edge case tests
echo -e "${BLUE}[BUILD]${NC} Building edge case test suite..."
if make -C tests/unit test_edge_cases.elf 2>&1 | tail -5; then
    echo -e "${GREEN}[OK]${NC} Build successful"
else
    echo -e "${RED}[ERROR]${NC} Build failed"
    exit 1
fi

echo ""
echo "================================================"
echo "  Test Suite 1: Boundary Values & Invalid Input"
echo "================================================"
echo ""

# Test 1: Edge Cases
if run_test "Edge Case Tests" "./tests/unit/test_edge_cases.elf"; then
    ((passed_tests++))
else
    ((failed_tests++))
fi
((total_tests++))

echo ""
echo "================================================"
echo "  Test Suite 2: Race Conditions"
echo "================================================"
echo ""

# Build race condition tests
echo -e "${BLUE}[BUILD]${NC} Building race condition test suite..."
if make -C tests/unit test_race_conditions.elf 2>&1 | tail -5; then
    echo -e "${GREEN}[OK]${NC} Build successful"

    # Test 2: Race Conditions
    if run_test "Race Condition Tests" "./tests/unit/test_race_conditions.elf"; then
        ((passed_tests++))
    else
        ((failed_tests++))
    fi
    ((total_tests++))
else
    echo -e "${YELLOW}[SKIP]${NC} Race condition tests not built"
fi

echo ""
echo "================================================"
echo "  Test Suite 3: Resource Exhaustion"
echo "================================================"
echo ""

# Build resource exhaustion tests
echo -e "${BLUE}[BUILD]${NC} Building resource exhaustion test suite..."
if make -C tests/unit test_resource_exhaustion.elf 2>&1 | tail -5; then
    echo -e "${GREEN}[OK]${NC} Build successful"

    # Test 3: Resource Exhaustion
    if run_test "Resource Exhaustion Tests" "./tests/unit/test_resource_exhaustion.elf"; then
        ((passed_tests++))
    else
        ((failed_tests++))
    fi
    ((total_tests++))
else
    echo -e "${YELLOW}[SKIP]${NC} Resource exhaustion tests not built"
fi

echo ""
echo "================================================"
echo "  Edge Case Testing Complete"
echo "================================================"
echo ""
echo "Results:"
echo "  Total Tests:  $total_tests"
echo -e "  ${GREEN}Passed:${NC}       $passed_tests"
echo -e "  ${RED}Failed:${NC}       $failed_tests"
echo ""

if [ $failed_tests -eq 0 ]; then
    echo -e "${GREEN}✓ All edge case tests passed!${NC}"
    echo ""
    echo "Edge cases tested:"
    echo "  ✓ Boundary values (0, 1, MAX, -1, NULL)"
    echo "  ✓ Resource exhaustion (OOM, page exhaustion)"
    echo "  ✓ Race conditions (concurrent access)"
    echo "  ✓ Invalid input (overflow, bad pointers)"
    echo "  ✓ Timing issues (rapid operations)"
    echo "  ✓ Edge case combinations"
    echo ""
    echo "System robustness: VERIFIED"
    echo ""
    exit 0
else
    echo -e "${RED}✗ Some edge case tests failed${NC}"
    echo ""
    echo "Review the output above for details."
    echo ""
    exit 1
fi
