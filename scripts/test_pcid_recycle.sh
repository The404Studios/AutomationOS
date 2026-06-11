#!/usr/bin/env bash
# =============================================================================
# test_pcid_recycle.sh -- PCID Recycling IPI Broadcast Test Runner
# =============================================================================
#
# Builds and runs the PCID recycling IPI broadcast validation test.
# Verifies BUG-013 fix: TLB flush IPI sent to all CPUs when next_pcid >= 4096.
#
# Usage (from project root inside WSL Arch):
#   bash scripts/test_pcid_recycle.sh [--cpus N] [--timeout SECONDS]
#
# Requirements:
#   - At least 2 CPUs (--cpus 2 or higher)
#   - SMP_FOUNDATION enabled in build
#   - TEST_PCID_RECYCLE enabled in build
#
# Exit codes:
#   0  All tests passed
#   1  One or more tests failed
#   2  Setup error or insufficient CPUs
#
# =============================================================================

set -euo pipefail

# ── Configuration ───────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
CPUS=4
TIMEOUT=60
LOG="/tmp/pcid_recycle_test.log"

# ── Argument parsing ────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cpus)     CPUS="$2";     shift 2 ;;
        --timeout)  TIMEOUT="$2";  shift 2 ;;
        -h|--help)
            sed -n '2,24p' "$0" | grep '^#' | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "ERROR: Unknown option: $1" >&2
            exit 2
            ;;
    esac
done

# ── Validation ──────────────────────────────────────────────────────────────
if [[ $CPUS -lt 2 ]]; then
    echo "ERROR: Test requires at least 2 CPUs (got --cpus $CPUS)" >&2
    exit 2
fi

# ── Helper functions ────────────────────────────────────────────────────────
log() {
    echo "[$(date '+%H:%M:%S')] $*"
}

err() {
    echo "[$(date '+%H:%M:%S')] ERROR: $*" >&2
}

# ── Step 1: Build test object ──────────────────────────────────────────────
log "Building PCID recycle IPI test..."

cd "$KERNEL_ROOT"

gcc -c tests/test_pcid_recycle_ipi.c -o build/test_pcid_recycle_ipi.o \
    -I kernel/include -std=gnu11 -O2 -ffreestanding -nostdlib \
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie \
    -DTEST_PCID_RECYCLE -DSMP_FOUNDATION

if [[ ! -f build/test_pcid_recycle_ipi.o ]]; then
    err "Failed to compile test object"
    exit 2
fi

log "Test object compiled: build/test_pcid_recycle_ipi.o"

# ── Step 2: Build kernel with test enabled ─────────────────────────────────
log "Building kernel with TEST_PCID_RECYCLE enabled..."

# Add test flags to environment
export CFLAGS="${CFLAGS:-} -DTEST_PCID_RECYCLE -DSMP_FOUNDATION"

bash scripts/quick_build.sh

if [[ ! -f build/kernel.elf ]]; then
    err "Kernel build failed"
    exit 2
fi

log "Kernel built with test support"

# ── Step 3: Build ISO ───────────────────────────────────────────────────────
log "Building bootable ISO..."

if ! command -v grub-mkrescue &>/dev/null; then
    err "grub-mkrescue not found in PATH"
    exit 2
fi

grub-mkrescue -o build/automationos.iso iso/ 2>&1 | grep -v "warning:" || true

if [[ ! -f build/automationos.iso ]]; then
    err "Failed to create ISO"
    exit 2
fi

log "ISO created: build/automationos.iso"

# ── Step 4: Run test in QEMU ────────────────────────────────────────────────
log "Launching QEMU with $CPUS CPUs (timeout: ${TIMEOUT}s)..."

rm -f "$LOG"

timeout "${TIMEOUT}s" qemu-system-x86_64 \
    -cdrom build/automationos.iso \
    -m 512M \
    -smp "$CPUS" \
    -serial file:"$LOG" \
    -display none \
    -enable-kvm \
    -cpu host \
    || true

# ── Step 5: Parse results ───────────────────────────────────────────────────
log "Analyzing test results..."

if [[ ! -f "$LOG" ]]; then
    err "Serial log not found: $LOG"
    exit 2
fi

# Check for test completion marker
if ! grep -q "PCID Recycle IPI Test Summary" "$LOG"; then
    err "Test did not complete (no summary found)"
    echo ""
    echo "Last 50 lines of serial log:"
    tail -50 "$LOG"
    exit 1
fi

# Count passed tests
TOTAL=$(grep -oP 'Total:\s+\K\d+' "$LOG" || echo "0")
PASSED=$(grep -oP 'Passed:\s+\K\d+' "$LOG" || echo "0")
FAILED=$(grep -oP 'Failed:\s+\K\d+' "$LOG" || echo "0")

log "Test results: $PASSED/$TOTAL passed, $FAILED failed"

# Check for overall pass
if grep -q "✓ ALL TESTS PASSED" "$LOG"; then
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║                  ✓ ALL TESTS PASSED                        ║"
    echo "╟────────────────────────────────────────────────────────────╢"
    echo "║  BUG-013 Fix Verified                                      ║"
    echo "║  PCID recycling correctly broadcasts TLB flush IPI         ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""

    # Show test summary from log
    grep -A 10 "PCID Recycle IPI Test Summary" "$LOG"

    echo ""
    log "Full test log: $LOG"
    exit 0
else
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║                  ✗ SOME TESTS FAILED                       ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""

    # Show failures
    grep "FAIL:" "$LOG" || echo "(No explicit FAIL markers found)"

    echo ""
    log "Failed tests: $FAILED/$TOTAL"
    log "Full test log: $LOG"

    # Show last 100 lines for debugging
    echo ""
    echo "Last 100 lines of serial log:"
    echo "========================================"
    tail -100 "$LOG"

    exit 1
fi
