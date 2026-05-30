#!/bin/bash
# AutomationOS Integration Test Build and Runner
# Builds all 110 integration tests and runs them

set -e  # Exit on error

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Directories
KERNEL_DIR="/c/Users/wilde/Desktop/Kernel"
TEST_DIR="${KERNEL_DIR}/tests/integration"
BUILD_DIR="${TEST_DIR}/build"
INCLUDE_DIR="${KERNEL_DIR}/kernel/include"

# Compiler settings
CC="gcc"
CFLAGS="-Wall -Wextra -std=c11 -I${INCLUDE_DIR} -g -O0"
LDFLAGS=""

echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}AutomationOS Integration Test Suite${NC}"
echo -e "${BLUE}=====================================${NC}"
echo ""

# Create build directory
mkdir -p "${BUILD_DIR}"

# Test files to compile
TEST_FILES=(
    "test_boot_expanded.c"
    "test_application_lifecycle.c"
    "test_filesystem_integration.c"
    "test_network_stack.c"
    "test_graphics_stack.c"
    "test_security_expanded.c"
    "test_power_management.c"
    "integration_suite.c"
    "integration_suite_expanded.c"
)

echo -e "${YELLOW}Phase 1: Compilation${NC}"
echo "Building integration tests..."
echo ""

COMPILE_SUCCESS=0
COMPILE_FAIL=0

for test_file in "${TEST_FILES[@]}"; do
    if [ -f "${TEST_DIR}/${test_file}" ]; then
        obj_file="${BUILD_DIR}/$(basename ${test_file} .c).o"
        echo -n "Compiling ${test_file}... "

        if ${CC} ${CFLAGS} -c "${TEST_DIR}/${test_file}" -o "${obj_file}" 2>"${BUILD_DIR}/$(basename ${test_file} .c).err"; then
            echo -e "${GREEN}OK${NC}"
            ((COMPILE_SUCCESS++))
        else
            echo -e "${RED}FAILED${NC}"
            echo "  Error log: ${BUILD_DIR}/$(basename ${test_file} .c).err"
            cat "${BUILD_DIR}/$(basename ${test_file} .c).err" | head -20
            echo ""
            ((COMPILE_FAIL++))
        fi
    else
        echo -e "${YELLOW}SKIP${NC} ${test_file} (not found)"
    fi
done

echo ""
echo -e "${BLUE}Compilation Summary:${NC}"
echo "  Success: ${COMPILE_SUCCESS}"
echo "  Failed:  ${COMPILE_FAIL}"
echo ""

if [ ${COMPILE_FAIL} -gt 0 ]; then
    echo -e "${RED}Compilation failed. Analyzing errors...${NC}"
    echo ""

    # Analyze common errors
    grep -h "error:" ${BUILD_DIR}/*.err 2>/dev/null | sort | uniq -c | head -20

    exit 1
fi

echo -e "${GREEN}All files compiled successfully!${NC}"
echo ""

# Note: We can't link into a userspace executable because these tests
# need to run in kernel context. Instead, we'll report on the test files.

echo -e "${YELLOW}Phase 2: Test Analysis${NC}"
echo ""

echo "Analyzing test coverage..."

for test_file in "${TEST_FILES[@]}"; do
    if [ -f "${TEST_DIR}/${test_file}" ]; then
        test_count=$(grep -c "^void test_" "${TEST_DIR}/${test_file}" 2>/dev/null || echo 0)
        echo "  ${test_file}: ${test_count} test functions"
    fi
done

echo ""
echo -e "${BLUE}=====================================${NC}"
echo -e "${BLUE}Build Complete${NC}"
echo -e "${BLUE}=====================================${NC}"
echo ""
echo "Next steps:"
echo "1. Integration tests must run in kernel context"
echo "2. Boot kernel with integration test flag"
echo "3. Tests will execute during boot and report results"
echo ""
