#!/usr/bin/env bash
# =============================================================================
# verify_hda_yield.sh - Quick HDA Yield Verification Script
# =============================================================================
#
# Quick check that hda_msleep() is using sys_yield() correctly.
# This script validates the implementation without requiring a full boot.
#
# Usage:
#   bash scripts/verify_hda_yield.sh
#
# Exit codes:
#   0 = Implementation looks correct
#   1 = Implementation has issues
#
# =============================================================================

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

echo "╔═══════════════════════════════════════════════════════════╗"
echo "║   HDA Yield Implementation Verification                  ║"
echo "╚═══════════════════════════════════════════════════════════╝"
echo ""

ISSUES=0

# Check 1: Verify hda_msleep exists and calls sys_yield
echo "[CHECK 1] Verifying hda_msleep implementation..."
if grep -q "void hda_msleep(uint32_t ms)" kernel/drivers/hda.c; then
    echo "  ✓ hda_msleep() function found"

    if grep -A 5 "void hda_msleep" kernel/drivers/hda.c | grep -q "sys_yield"; then
        echo "  ✓ hda_msleep() calls sys_yield()"
    else
        echo "  ✗ hda_msleep() does NOT call sys_yield()"
        echo "    Expected: sys_yield(0, 0, 0, 0, 0, 0) in the sleep loop"
        ISSUES=$((ISSUES + 1))
    fi

    if grep -A 5 "void hda_msleep" kernel/drivers/hda.c | grep -q "while.*timer_get_ticks"; then
        echo "  ✓ hda_msleep() uses timer-based loop"
    else
        echo "  ✗ hda_msleep() does NOT use timer-based loop"
        echo "    Expected: while (timer_get_ticks() < end) loop"
        ISSUES=$((ISSUES + 1))
    fi
else
    echo "  ✗ hda_msleep() function NOT found"
    ISSUES=$((ISSUES + 1))
fi
echo ""

# Check 2: Verify hda_msleep is actually used (not bypassed)
echo "[CHECK 2] Verifying hda_msleep is used for delays..."
MSLEEP_CALLS=$(grep -c "hda_msleep(" kernel/drivers/hda.c || true)
if [ "$MSLEEP_CALLS" -gt 5 ]; then
    echo "  ✓ hda_msleep() called $MSLEEP_CALLS times in hda.c"
else
    echo "  ✗ hda_msleep() only called $MSLEEP_CALLS times"
    echo "    Expected at least 6 calls (reset, CORB/RIRB init, codec enum)"
    ISSUES=$((ISSUES + 1))
fi
echo ""

# Check 3: Verify critical waits use hda_msleep
echo "[CHECK 3] Verifying critical HDA waits use hda_msleep..."

if grep -A 20 "int hda_reset_controller" kernel/drivers/hda.c | grep -q "hda_msleep(10)"; then
    echo "  ✓ Reset controller uses hda_msleep(10) for 10ms hold"
else
    echo "  ✗ Reset controller missing hda_msleep(10)"
    ISSUES=$((ISSUES + 1))
fi

if grep -A 30 "int hda_reset_controller" kernel/drivers/hda.c | grep -q "hda_msleep(100)"; then
    echo "  ✓ Reset controller uses hda_msleep(100) for codec enum"
else
    echo "  ✗ Reset controller missing hda_msleep(100)"
    ISSUES=$((ISSUES + 1))
fi

if grep -A 10 "int hda_wait_for_response" kernel/drivers/hda.c | grep -q "hda_msleep(1)"; then
    echo "  ✓ Response wait uses hda_msleep(1) for polling"
else
    echo "  ✗ Response wait missing hda_msleep(1)"
    ISSUES=$((ISSUES + 1))
fi
echo ""

# Check 4: Verify sys_yield is implemented
echo "[CHECK 4] Verifying sys_yield syscall implementation..."
if grep -q "int64_t sys_yield" kernel/core/syscall/handlers.c; then
    echo "  ✓ sys_yield() syscall handler found"

    if grep -A 10 "int64_t sys_yield" kernel/core/syscall/handlers.c | grep -q "scheduler_yield_requeue"; then
        echo "  ✓ sys_yield() calls scheduler_yield_requeue()"
    else
        echo "  ✗ sys_yield() does NOT call scheduler_yield_requeue()"
        ISSUES=$((ISSUES + 1))
    fi

    if grep -A 15 "int64_t sys_yield" kernel/core/syscall/handlers.c | grep -q "scheduler_pick_next"; then
        echo "  ✓ sys_yield() calls scheduler_pick_next()"
    else
        echo "  ✗ sys_yield() does NOT call scheduler_pick_next()"
        ISSUES=$((ISSUES + 1))
    fi
else
    echo "  ✗ sys_yield() syscall handler NOT found"
    ISSUES=$((ISSUES + 1))
fi
echo ""

# Check 5: Verify timer support
echo "[CHECK 5] Verifying timer support..."
if grep -q "uint64_t timer_get_ticks" kernel/include/drivers.h || \
   grep -q "uint64_t timer_get_ticks" kernel/drivers/timer.c; then
    echo "  ✓ timer_get_ticks() function found"
else
    echo "  ✗ timer_get_ticks() function NOT found"
    ISSUES=$((ISSUES + 1))
fi

if grep -q "uint64_t timer_get_frequency" kernel/include/drivers.h || \
   grep -q "uint64_t timer_get_frequency" kernel/drivers/timer.c; then
    echo "  ✓ timer_get_frequency() function found"
else
    echo "  ✗ timer_get_frequency() function NOT found"
    ISSUES=$((ISSUES + 1))
fi
echo ""

# Check 6: Look for potential issues
echo "[CHECK 6] Checking for potential implementation issues..."

if grep -q "asm.*hlt" kernel/drivers/hda.c; then
    echo "  ⚠ WARNING: hda.c contains HLT instruction"
    echo "    This may cause system hangs during audio init"
fi

if grep "hda_msleep" kernel/drivers/hda.c | grep -q "//.*hda_msleep"; then
    echo "  ⚠ WARNING: Some hda_msleep calls are commented out"
    echo "    This may indicate incomplete implementation"
fi

BUSY_LOOPS=$(grep -c "while.*{.*}" kernel/drivers/hda.c || true)
if [ "$BUSY_LOOPS" -gt "$MSLEEP_CALLS" ]; then
    echo "  ⚠ WARNING: More while-loops ($BUSY_LOOPS) than hda_msleep calls ($MSLEEP_CALLS)"
    echo "    Some busy-waits may not be yielding to scheduler"
fi

echo "  ✓ No critical issues detected"
echo ""

# Summary
echo "═══════════════════════════════════════════════════════════"
if [ $ISSUES -eq 0 ]; then
    echo "✓ VERIFICATION PASSED"
    echo ""
    echo "Implementation appears correct:"
    echo "  • hda_msleep() properly uses sys_yield()"
    echo "  • Critical HDA waits call hda_msleep()"
    echo "  • sys_yield() properly requeues and switches tasks"
    echo "  • Timer functions available for timing"
    echo ""
    echo "Next steps:"
    echo "  1. Build the test: bash userspace/apps/hda_yield_test/build.sh"
    echo "  2. Add to initrd and run boot test"
    echo "  3. Verify no boot freeze and concurrent execution works"
    echo ""
    exit 0
else
    echo "✗ VERIFICATION FAILED ($ISSUES issues found)"
    echo ""
    echo "Please fix the issues above before running the full test."
    echo "Refer to HDA_YIELD_TEST_GUIDE.md for troubleshooting."
    echo ""
    exit 1
fi
