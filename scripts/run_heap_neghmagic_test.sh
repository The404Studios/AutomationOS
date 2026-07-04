#!/bin/bash
# NEGHMAGIC-0 Regression Test Runner
# ===================================
#
# Bug-specific NEGATIVE regression test for the forged-heap-magic type-confusion
# HIGH fixed in kernel/core/mem/heap.c (KERNEL-ROBUST-0, commit 6aacaeb).
#
# Compiles tests/unit/test_heap_neghmagic.c, which pulls the REAL
# kernel/core/mem/heap.c source directly into its translation unit (the
# "#include the .c" host-harness idiom, same as tests/unit/test_heap_segregated.c)
# under -DHEAP_TEST_HOST, then forges a page-aligned heap block's data to match
# the slab allocator's SLAB_MAGIC sentinel and proves kfree()/krealloc() route it
# through the heap free path (not the slab path) regardless of those forged bytes.
#
# See tests/unit/test_heap_neghmagic.c's header comment for the full writeup of
# why this discriminates fixed vs. pre-fix (parent commit 55926d4) code.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"
TEST_DIR="$ROOT_DIR/tests/unit"
TEST_SRC="$TEST_DIR/test_heap_neghmagic.c"
HEAP_SRC="$ROOT_DIR/kernel/core/mem/heap.c"
BIN="$BUILD_DIR/test_heap_neghmagic"

echo "=========================================="
echo "NEGHMAGIC-0 Heap Regression Test Runner"
echo "=========================================="
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

if [ ! -f "$TEST_SRC" ]; then
    echo -e "${RED}ERROR: test_heap_neghmagic.c not found at $TEST_SRC${NC}"
    exit 1
fi

if [ ! -f "$HEAP_SRC" ]; then
    echo -e "${RED}ERROR: heap.c not found at $HEAP_SRC${NC}"
    exit 1
fi

mkdir -p "$BUILD_DIR"

echo "Building NEGHMAGIC-0 regression test (host harness, real heap.c source)..."
echo ""

# test_heap_neghmagic.c #includes ../../kernel/core/mem/heap.c directly (a
# self-contained host test, like tests/unit/test_heap_segregated.c) -- it takes
# NO other source files on the command line, unlike run_security_tests.sh's
# STANDALONE_TEST mode.
#
# -fsanitize=address,undefined catches any real out-of-bounds/UB the forged
# pointer might trigger on either code path; ASAN_OPTIONS=detect_leaks=0 at run
# time is required because the pre-fix (buggy) path deliberately orphans the
# victim block (never returns it to any heap bin) -- that orphaned block is the
# bug's signature, not an unrelated test-harness leak, so leak detection would
# misreport the very condition this test exists to catch.
gcc -std=c11 -O0 -g -Wall -Wextra \
    -fsanitize=address,undefined \
    -DHEAP_TEST_HOST \
    -o "$BIN" \
    "$TEST_SRC"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Compilation successful${NC}"
else
    echo -e "${RED}✗ Compilation failed${NC}"
    exit 1
fi

echo ""
echo "Running NEGHMAGIC-0 regression test..."
echo ""

set +e
ASAN_OPTIONS=detect_leaks=0 "$BIN"
TEST_RESULT=$?
set -e

echo ""
if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}=========================================="
    echo -e "✓ NEGHMAGIC-0: PASS -- forged SLAB_MAGIC cannot reach the unsafe slab-free/realloc path"
    echo -e "==========================================${NC}"
else
    echo -e "${RED}=========================================="
    echo -e "✗ NEGHMAGIC-0: FAIL -- forged heap magic was misrouted (type-confusion regression present)"
    echo -e "==========================================${NC}"
fi

exit $TEST_RESULT
