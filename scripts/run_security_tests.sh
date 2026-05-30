#!/bin/bash
# ELF Loader Security Test Runner
# ================================
#
# Compiles and runs security validation tests for the ELF loader.
# Tests malformed binaries, boundary conditions, and attack vectors.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"
TEST_DIR="$ROOT_DIR/tests/unit"

echo "=========================================="
echo "ELF Loader Security Test Runner"
echo "=========================================="
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if source files exist
if [ ! -f "$TEST_DIR/test_elf_security.c" ]; then
    echo -e "${RED}ERROR: test_elf_security.c not found${NC}"
    exit 1
fi

if [ ! -f "$ROOT_DIR/kernel/fs/elf_loader.c" ]; then
    echo -e "${RED}ERROR: elf_loader.c not found${NC}"
    exit 1
fi

# Create build directory
mkdir -p "$BUILD_DIR"

echo "Building security test suite..."
echo ""

# Compile test suite (standalone mode)
# Note: This builds a minimal test harness
gcc -Wall -Wextra -O2 \
    -I"$ROOT_DIR/kernel/include" \
    -DSTANDALONE_TEST \
    -DUSER_SPACE_END=0x0000800000000000ULL \
    -DKERNEL_SPACE_START=0xFFFF800000000000ULL \
    -DPAGE_SIZE=4096 \
    -o "$BUILD_DIR/test_elf_security" \
    "$TEST_DIR/test_elf_security.c" \
    -lm

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Compilation successful${NC}"
else
    echo -e "${RED}✗ Compilation failed${NC}"
    exit 1
fi

echo ""
echo "Running security tests..."
echo ""

# Run tests
"$BUILD_DIR/test_elf_security"

TEST_RESULT=$?

echo ""
if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}=========================================="
    echo -e "✓ ALL SECURITY TESTS PASSED"
    echo -e "==========================================${NC}"
else
    echo -e "${RED}=========================================="
    echo -e "✗ SOME TESTS FAILED"
    echo -e "==========================================${NC}"
fi

exit $TEST_RESULT
