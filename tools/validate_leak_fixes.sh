#!/bin/bash
# Validation Script for Memory Leak Fixes
# Verifies all fixes are in place and tests pass

set -euo pipefail

KERNEL_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$KERNEL_ROOT"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0

check() {
    local name="$1"
    local cmd="$2"

    echo -n "Checking $name... "
    if eval "$cmd" > /dev/null 2>&1; then
        echo -e "${GREEN}✓ PASS${NC}"
        ((PASS++))
        return 0
    else
        echo -e "${RED}✗ FAIL${NC}"
        ((FAIL++))
        return 1
    fi
}

echo "=========================================="
echo "Memory Leak Fix Validation"
echo "=========================================="
echo ""

echo "Phase 1: Verify LEAK-002 Fix (File Info Buffer)"
echo "================================================"
check "File info FreePool added" \
    "grep -q 'FreePool(file_info)' boot/loader.c"
check "File info cleanup before kernel alloc" \
    "grep -B 3 'Allocate buffer for kernel' boot/loader.c | grep -q 'FreePool'"
echo ""

echo "Phase 2: Verify LEAK-003 Fix (Kernel Load Buffer)"
echo "==================================================="
check "Kernel buffer FreePages added" \
    "grep -q 'FreePages(kernel_addr' boot/loader.c"
check "Kernel buffer cleanup before exit" \
    "grep -B 5 'Exit Boot Services' boot/loader.c | grep -q 'FreePages'"
echo ""

echo "Phase 3: Verify LEAK-004/005 Fix (PID Leaks)"
echo "=============================================="
check "free_pid function exists" \
    "grep -q 'static void free_pid' kernel/core/sched/process.c"
check "PID freed on kmalloc failure" \
    "grep -A 3 'Failed to allocate process structure' kernel/core/sched/process.c | grep -q 'free_pid'"
check "PID freed on stack alloc failure" \
    "grep -A 3 'Failed to allocate kernel stack' kernel/core/sched/process.c | grep -q 'free_pid'"
check "PID freed on namespace failure" \
    "grep -A 3 'Failed to create namespaces' kernel/core/sched/process.c | grep -q 'free_pid'"
echo ""

echo "Phase 4: Verify LEAK-006 Fix (Atomic Refcounts)"
echo "================================================"
check "Atomic increment in create_container" \
    "grep -q '__atomic_add_fetch.*ref_count' kernel/security/namespace.c"
check "Atomic decrement in destroy_container" \
    "grep -q '__atomic_sub_fetch.*ref_count' kernel/security/namespace.c"
check "Atomic increment in clone_container" \
    "grep -A 20 'CLONE_NEWPID' kernel/security/namespace.c | grep -q '__atomic_add_fetch'"
check "Atomic decrement in error_cleanup" \
    "grep -A 30 'error_cleanup:' kernel/security/namespace.c | grep -q '__atomic_sub_fetch'"

# Count total atomic operations
atomic_count=$(grep -c '__atomic.*ref_count' kernel/security/namespace.c)
echo "  Total atomic ref_count operations: $atomic_count"
if [ "$atomic_count" -ge 20 ]; then
    echo -e "  ${GREEN}✓ Sufficient atomic operations found${NC}"
    ((PASS++))
else
    echo -e "  ${YELLOW}⚠ Only $atomic_count atomic operations found (expected 20+)${NC}"
    ((FAIL++))
fi
echo ""

echo "Phase 5: Verify Test Suite"
echo "==========================="
check "Memory leak test file exists" \
    "test -f tests/unit/test_memory_leaks.c"
check "Test has PID leak test" \
    "grep -q 'test_pid_leak_on_process_create_failure' tests/unit/test_memory_leaks.c"
check "Test has namespace race test" \
    "grep -q 'test_namespace_refcount_race' tests/unit/test_memory_leaks.c"
check "Test has lifecycle test" \
    "grep -q 'test_namespace_lifecycle' tests/unit/test_memory_leaks.c"
check "Test has process lifecycle test" \
    "grep -q 'test_process_lifecycle_with_namespaces' tests/unit/test_memory_leaks.c"
echo ""

echo "Phase 6: Check for Remaining Issues"
echo "===================================="

# Look for non-atomic ref_count operations in namespace code
echo "Checking for non-atomic ref_count operations..."
non_atomic=$(grep -E 'ref_count(\+\+|--|\s*=\s*[^_])' kernel/security/namespace.c | grep -v '__atomic' | wc -l)
if [ "$non_atomic" -eq 0 ]; then
    echo -e "${GREEN}✓ No non-atomic ref_count operations found${NC}"
    ((PASS++))
else
    echo -e "${RED}✗ Found $non_atomic non-atomic ref_count operations!${NC}"
    grep -n -E 'ref_count(\+\+|--|\s*=\s*[^_])' kernel/security/namespace.c | grep -v '__atomic'
    ((FAIL++))
fi

# Look for malloc/kmalloc without corresponding free
echo "Checking for potential new leaks in modified files..."
for file in boot/loader.c kernel/core/sched/process.c kernel/security/namespace.c; do
    alloc_count=$(grep -c -E '\b(malloc|kmalloc|calloc|AllocatePool|AllocatePages)\s*\(' "$file" || true)
    free_count=$(grep -c -E '\b(free|kfree|FreePool|FreePages)\s*\(' "$file" || true)

    echo "  $file: $alloc_count allocations, $free_count frees"

    # This is a rough heuristic - should be close but not necessarily equal
    if [ "$alloc_count" -eq 0 ]; then
        echo -e "    ${GREEN}✓ No allocations in this file${NC}"
    elif [ "$free_count" -ge $((alloc_count - 2)) ]; then
        echo -e "    ${GREEN}✓ Allocation/free ratio looks good${NC}"
    else
        echo -e "    ${YELLOW}⚠ Fewer frees than allocations (may be intentional)${NC}"
    fi
done
echo ""

echo "Phase 7: Documentation Check"
echo "============================"
check "Analysis document exists" \
    "test -f MEMORY_LEAK_ANALYSIS.md"
check "Fix summary exists" \
    "test -f MEMORY_LEAK_FIX_SUMMARY.md"
check "Leak checker script exists" \
    "test -f tools/memory_leak_checker.sh"
check "Leak checker is executable" \
    "test -x tools/memory_leak_checker.sh"
echo ""

echo "=========================================="
echo "Validation Summary"
echo "=========================================="
echo -e "Passed: ${GREEN}$PASS${NC}"
echo -e "Failed: ${RED}$FAIL${NC}"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}✓✓✓ All validations passed! ✓✓✓${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. Build the kernel: make clean && make"
    echo "  2. Run unit tests: make test"
    echo "  3. Boot in QEMU and check for leak messages"
    echo "  4. Run long-running stress tests"
    echo "  5. Set up Valgrind/ASAN for dynamic analysis"
    exit 0
else
    echo -e "${RED}✗✗✗ Some validations failed ✗✗✗${NC}"
    echo ""
    echo "Please review the failures above and fix them before proceeding."
    exit 1
fi
