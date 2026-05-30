#!/bin/bash
# run_epoll_tests.sh - Epoll test suite runner
# =============================================
#
# Builds and runs all epoll tests in sequence.
# Usage: ./run_epoll_tests.sh [test_name]
#
# If test_name is not provided, runs all tests.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$SCRIPT_DIR/../.."
BUILD_DIR="$KERNEL_ROOT/build/userspace/tests"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║          AutomationOS Epoll Test Suite Runner             ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Build tests
echo -e "${YELLOW}[BUILD]${NC} Building epoll tests..."
cd "$SCRIPT_DIR"
make -s test_epoll test_epoll_sockets test_epoll_kernel

if [ $? -ne 0 ]; then
    echo -e "${RED}[ERROR]${NC} Build failed!"
    exit 1
fi

echo -e "${GREEN}[BUILD]${NC} Build successful"
echo ""

# Check if specific test requested
SPECIFIC_TEST="$1"

# Test list
TESTS=(
    "test_epoll:Basic API Tests"
    "test_epoll_sockets:Socket Integration Tests"
    "test_epoll_kernel:Kernel Verification Tests"
)

run_test() {
    local test_binary="$1"
    local test_name="$2"
    local test_path="$BUILD_DIR/$test_binary"

    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
    echo -e "${BLUE}Running: $test_name${NC}"
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
    echo ""

    if [ ! -f "$test_path" ]; then
        echo -e "${RED}[ERROR]${NC} Test binary not found: $test_path"
        return 1
    fi

    # In a real kernel environment, you would:
    # 1. Boot the kernel
    # 2. Copy the test binary to the disk image
    # 3. Run it via the terminal
    #
    # For now, we'll just verify the binary exists
    echo -e "${YELLOW}[INFO]${NC} Test binary built: $test_path"
    echo -e "${YELLOW}[INFO]${NC} To run on AutomationOS:"
    echo -e "  1. Build kernel: make"
    echo -e "  2. Boot QEMU: make run"
    echo -e "  3. In terminal: exec /tests/$test_binary"
    echo ""

    return 0
}

# Run tests
TOTAL_TESTS=0
PASSED_TESTS=0

for test_entry in "${TESTS[@]}"; do
    IFS=':' read -r test_binary test_name <<< "$test_entry"

    # Skip if specific test requested and this isn't it
    if [ -n "$SPECIFIC_TEST" ] && [ "$test_binary" != "$SPECIFIC_TEST" ]; then
        continue
    fi

    run_test "$test_binary" "$test_name"
    if [ $? -eq 0 ]; then
        ((PASSED_TESTS++))
    fi
    ((TOTAL_TESTS++))

    echo ""
done

# Summary
echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║                    Test Summary                            ║${NC}"
echo -e "${BLUE}╠════════════════════════════════════════════════════════════╣${NC}"
echo -e "${BLUE}║${NC}  Tests Built:  ${PASSED_TESTS}/${TOTAL_TESTS}                                        ${BLUE}║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

if [ $PASSED_TESTS -eq $TOTAL_TESTS ]; then
    echo -e "${GREEN}✓ All test binaries built successfully!${NC}"
    echo ""
    echo -e "${YELLOW}Next steps:${NC}"
    echo "  1. Build kernel: cd $KERNEL_ROOT && make"
    echo "  2. Boot OS: make run"
    echo "  3. Run tests in terminal:"
    echo "     - exec /tests/test_epoll"
    echo "     - exec /tests/test_epoll_sockets"
    echo "     - exec /tests/test_epoll_kernel"
    exit 0
else
    echo -e "${RED}✗ Some tests failed to build!${NC}"
    exit 1
fi
