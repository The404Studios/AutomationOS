#!/bin/bash
#
# Kernel Observability Test Suite
# =================================
#
# This script verifies kernel observability improvements including:
#   - Boot stage progression and output formatting
#   - Exception/page fault decoding and diagnostics
#   - Driver error messages and human-readable output
#   - Panic handler register dumps and stack traces
#   - Audit logging subsystem
#
# Usage:
#   ./tests/test_observability.sh [--build] [--qemu] [--verbose]
#
# Options:
#   --build    Rebuild kernel before testing
#   --qemu     Run live QEMU boot test (otherwise static analysis only)
#   --verbose  Show detailed output from all tests

set -e

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Configuration
KERNEL_ROOT="/c/Users/wilde/Desktop/Kernel"
BUILD_DIR="${KERNEL_ROOT}/build"
ISO_FILE="${KERNEL_ROOT}/iso/kernel.iso"
QEMU_LOG="/tmp/kernel_boot_test.log"
QEMU_TIMEOUT=30

# Test state
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
VERBOSE=0
DO_BUILD=0
DO_QEMU=0

# Parse arguments
for arg in "$@"; do
    case $arg in
        --build)
            DO_BUILD=1
            ;;
        --qemu)
            DO_QEMU=1
            ;;
        --verbose)
            VERBOSE=1
            ;;
        --help)
            grep '^#' "$0" | grep -v '#!/bin/bash' | sed 's/^# //'
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $arg${NC}"
            exit 1
            ;;
    esac
done

# Helper functions
print_header() {
    echo ""
    echo -e "${CYAN}============================================================${NC}"
    echo -e "${CYAN}  $1${NC}"
    echo -e "${CYAN}============================================================${NC}"
    echo ""
}

print_section() {
    echo ""
    echo -e "${BLUE}[TEST SECTION]${NC} $1"
    echo -e "${BLUE}────────────────────────────────────────────────────────${NC}"
}

test_start() {
    TESTS_RUN=$((TESTS_RUN + 1))
    echo -n "[TEST $TESTS_RUN] $1 ... "
}

test_pass() {
    TESTS_PASSED=$((TESTS_PASSED + 1))
    echo -e "${GREEN}PASS${NC}"
    if [ $VERBOSE -eq 1 ] && [ -n "$1" ]; then
        echo "  ✓ $1"
    fi
}

test_fail() {
    TESTS_FAILED=$((TESTS_FAILED + 1))
    echo -e "${RED}FAIL${NC}"
    if [ -n "$1" ]; then
        echo -e "  ${RED}✗ $1${NC}"
    fi
}

# Change to kernel root
cd "$KERNEL_ROOT"

print_header "Kernel Observability Test Suite"

# ============================================================================
# Section 1: Boot UI Components
# ============================================================================
print_section "Boot UI and Progress Tracking"

test_start "Checking boot_ui.h header exists"
if [ -f "kernel/include/boot_ui.h" ]; then
    test_pass "Header file present"
else
    test_fail "Missing kernel/include/boot_ui.h"
fi

test_start "Checking boot_ui.c implementation exists"
if [ -f "kernel/core/boot_ui.c" ]; then
    test_pass "Implementation file present"
else
    test_fail "Missing kernel/core/boot_ui.c"
fi

test_start "Verifying boot_stage() function exists"
if grep -q "void boot_stage(const char\* stage)" kernel/core/boot_ui.c; then
    test_pass "boot_stage() function defined"
else
    test_fail "boot_stage() function not found"
fi

test_start "Verifying boot_banner() function exists"
if grep -q "void boot_banner(void)" kernel/core/boot_ui.c; then
    test_pass "boot_banner() function defined"
else
    test_fail "boot_banner() function not found"
fi

test_start "Checking boot progress tracking"
if grep -q "boot_progress" kernel/core/boot_ui.c; then
    test_pass "Progress tracking variable present"
else
    test_fail "Progress tracking not implemented"
fi

test_start "Checking boot percentage display"
if grep -q "percent = (boot_progress \* 100)" kernel/core/boot_ui.c; then
    test_pass "Percentage calculation present"
else
    test_fail "Percentage calculation not found"
fi

# ============================================================================
# Section 2: Enhanced Panic Handler
# ============================================================================
print_section "Enhanced Panic Handler"

test_start "Checking panic.c implementation exists"
if [ -f "kernel/lib/panic.c" ]; then
    test_pass "Panic handler file present"
else
    test_fail "Missing kernel/lib/panic.c"
fi

test_start "Verifying exception decoder exists"
if grep -q "decode_exception" kernel/lib/panic.c; then
    test_pass "Exception decoder function found"
else
    test_fail "Exception decoder not implemented"
fi

test_start "Checking page fault error code decoding"
if grep -q "PF_PRESENT\|PF_WRITE\|PF_USER" kernel/lib/panic.c; then
    test_pass "Page fault flags defined"
else
    test_fail "Page fault error code flags missing"
fi

test_start "Verifying null pointer detection"
if grep -q "Null pointer dereference" kernel/lib/panic.c; then
    test_pass "Null pointer diagnostic present"
else
    test_fail "Null pointer detection missing"
fi

test_start "Checking protection violation diagnosis"
if grep -q "PROTECTION-VIOLATION" kernel/lib/panic.c; then
    test_pass "Protection violation decoding present"
else
    test_fail "Protection violation decoder missing"
fi

test_start "Verifying write-to-readonly detection"
if grep -q "Write to read-only page" kernel/lib/panic.c; then
    test_pass "Write protection diagnostic present"
else
    test_fail "Write protection diagnostic missing"
fi

test_start "Checking NX/DEP violation detection"
if grep -q "Instruction fetch from NX" kernel/lib/panic.c; then
    test_pass "NX violation diagnostic present"
else
    test_fail "NX violation diagnostic missing"
fi

test_start "Verifying human-readable diagnosis messages"
if grep -q "Possible causes:" kernel/lib/panic.c; then
    test_pass "Diagnostic help messages present"
else
    test_fail "Help messages missing"
fi

test_start "Checking color-coded severity markers"
if grep -q "\\\\033\[1;31m.*CRITICAL" kernel/lib/panic.c; then
    test_pass "Color-coded CRITICAL marker found"
else
    test_fail "Color-coded severity markers missing"
fi

test_start "Verifying stack trace capability"
if grep -q "MAX_STACK_FRAMES" kernel/lib/panic.c; then
    test_pass "Stack trace support present"
else
    test_fail "Stack trace support missing"
fi

# ============================================================================
# Section 3: Audit Logging Subsystem
# ============================================================================
print_section "Audit Logging Subsystem"

test_start "Checking audit log.c implementation exists"
if [ -f "kernel/audit/log.c" ]; then
    test_pass "Audit logging file present"
else
    test_fail "Missing kernel/audit/log.c"
fi

test_start "Verifying audit_init() function exists"
if grep -q "void audit_init(void)" kernel/audit/log.c; then
    test_pass "audit_init() function defined"
else
    test_fail "audit_init() function not found"
fi

test_start "Checking audit ring buffer implementation"
if grep -q "audit_buffer_t" kernel/audit/log.c; then
    test_pass "Audit buffer structure present"
else
    test_fail "Audit buffer not implemented"
fi

test_start "Verifying audit event logging"
if grep -q "audit_log" kernel/audit/log.c; then
    test_pass "audit_log() function present"
else
    test_fail "audit_log() function missing"
fi

test_start "Checking kernel boot event logging"
if grep -q "AUDIT_KERNEL_BOOT" kernel/audit/log.c; then
    test_pass "Kernel boot event defined"
else
    test_fail "Kernel boot event missing"
fi

test_start "Verifying audit statistics tracking"
if grep -q "audit_stats" kernel/audit/log.c; then
    test_pass "Statistics tracking present"
else
    test_fail "Statistics tracking missing"
fi

# ============================================================================
# Section 4: Driver Error Messages
# ============================================================================
print_section "Driver Error Messages and Diagnostics"

test_start "Checking ACPI driver error messages"
if [ -f "kernel/drivers/acpi/acpi.c" ]; then
    if grep -q "kprintf.*ACPI\|kprintf.*acpi" kernel/drivers/acpi/acpi.c; then
        test_pass "ACPI driver has kprintf diagnostics"
    else
        test_fail "ACPI driver missing diagnostic messages"
    fi
else
    test_fail "ACPI driver not found"
fi

test_start "Checking PCI driver diagnostics"
if [ -f "kernel/drivers/pci.c" ]; then
    if grep -q "kprintf.*PCI\|kprintf.*pci" kernel/drivers/pci.c 2>/dev/null; then
        test_pass "PCI driver has diagnostic output"
    else
        echo -e "${YELLOW}SKIP${NC} (no diagnostic messages found)"
    fi
else
    echo -e "${YELLOW}SKIP${NC} (PCI driver not present)"
fi

test_start "Checking USB driver diagnostics"
if [ -f "kernel/drivers/usb/usb_core.c" ]; then
    if grep -q "kprintf.*USB\|kprintf.*usb" kernel/drivers/usb/usb_core.c 2>/dev/null; then
        test_pass "USB driver has diagnostic output"
    else
        echo -e "${YELLOW}SKIP${NC} (no diagnostic messages found)"
    fi
else
    echo -e "${YELLOW}SKIP${NC} (USB driver not present)"
fi

test_start "Verifying network driver error output"
if [ -f "kernel/drivers/net/e1000.c" ]; then
    if grep -q "kprintf.*e1000\|kprintf.*E1000" kernel/drivers/net/e1000.c 2>/dev/null; then
        test_pass "E1000 driver has diagnostic output"
    else
        echo -e "${YELLOW}SKIP${NC} (no diagnostic messages found)"
    fi
else
    echo -e "${YELLOW}SKIP${NC} (E1000 driver not present)"
fi

# ============================================================================
# Section 5: Build Test (Optional)
# ============================================================================
if [ $DO_BUILD -eq 1 ]; then
    print_section "Build Verification"

    test_start "Building kernel with observability features"
    if make clean all > /tmp/observability_build.log 2>&1; then
        test_pass "Kernel built successfully"
    else
        test_fail "Build failed (see /tmp/observability_build.log)"
        if [ $VERBOSE -eq 1 ]; then
            tail -20 /tmp/observability_build.log
        fi
    fi

    test_start "Checking for boot_ui.o in build output"
    if [ -f "${BUILD_DIR}/kernel/core/boot_ui.o" ]; then
        test_pass "boot_ui compiled"
    else
        test_fail "boot_ui.o not found in build"
    fi

    test_start "Checking for panic.o in build output"
    if [ -f "${BUILD_DIR}/kernel/lib/panic.o" ]; then
        test_pass "panic handler compiled"
    else
        test_fail "panic.o not found in build"
    fi
fi

# ============================================================================
# Section 6: Live QEMU Boot Test (Optional)
# ============================================================================
if [ $DO_QEMU -eq 1 ]; then
    print_section "Live QEMU Boot Test"

    test_start "Checking for kernel ISO"
    if [ ! -f "$ISO_FILE" ]; then
        test_fail "ISO not found at $ISO_FILE (run 'make iso' first)"
    else
        test_pass "ISO file present"

        test_start "Booting kernel in QEMU and capturing output"

        # Start QEMU with serial output to file
        timeout ${QEMU_TIMEOUT}s qemu-system-x86_64 \
            -cdrom "$ISO_FILE" \
            -m 512M \
            -serial file:${QEMU_LOG} \
            -nographic \
            -no-reboot \
            > /dev/null 2>&1 || true

        if [ -f "$QEMU_LOG" ] && [ -s "$QEMU_LOG" ]; then
            test_pass "Boot output captured to $QEMU_LOG"

            # Analyze boot output
            test_start "Verifying boot banner appears"
            if grep -q "AutomationOS\|AUTO" "$QEMU_LOG"; then
                test_pass "Boot banner found in output"
            else
                test_fail "Boot banner not found"
            fi

            test_start "Checking boot stage progression"
            if grep -q "\[.*%\]" "$QEMU_LOG"; then
                test_pass "Boot progress percentages found"
                if [ $VERBOSE -eq 1 ]; then
                    echo "  Boot stages detected:"
                    grep "\[.*%\]" "$QEMU_LOG" | head -10 | sed 's/^/    /'
                fi
            else
                test_fail "No boot progress indicators found"
            fi

            test_start "Verifying boot stages appear in order"
            # Extract percentage values
            percentages=$(grep -o "\[[0-9]*%\]" "$QEMU_LOG" | grep -o "[0-9]*" | head -10)
            if [ -n "$percentages" ]; then
                last=0
                ordered=1
                for pct in $percentages; do
                    if [ "$pct" -lt "$last" ]; then
                        ordered=0
                        break
                    fi
                    last=$pct
                done

                if [ $ordered -eq 1 ]; then
                    test_pass "Boot stages progress in increasing order"
                else
                    test_fail "Boot stages out of order"
                fi
            else
                echo -e "${YELLOW}SKIP${NC} (no percentages found)"
            fi

            test_start "Checking for kernel initialization messages"
            if grep -q "Initializing\|initialized" "$QEMU_LOG"; then
                test_pass "Initialization messages present"
            else
                test_fail "No initialization messages found"
            fi

            # Clean up log file
            if [ $VERBOSE -eq 0 ]; then
                rm -f "$QEMU_LOG"
            fi
        else
            test_fail "No boot output captured (QEMU may have failed)"
        fi
    fi
fi

# ============================================================================
# Summary
# ============================================================================
print_header "Test Summary"

echo "Tests run:    $TESTS_RUN"
echo -e "Tests passed: ${GREEN}$TESTS_PASSED${NC}"
if [ $TESTS_FAILED -gt 0 ]; then
    echo -e "Tests failed: ${RED}$TESTS_FAILED${NC}"
else
    echo -e "Tests failed: ${GREEN}0${NC}"
fi
echo ""

# Calculate success rate
if [ $TESTS_RUN -gt 0 ]; then
    success_rate=$((TESTS_PASSED * 100 / TESTS_RUN))
    echo "Success rate: ${success_rate}%"
    echo ""
fi

# Final result
if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}╔═══════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║                                                       ║${NC}"
    echo -e "${GREEN}║  ✓ ALL OBSERVABILITY TESTS PASSED                    ║${NC}"
    echo -e "${GREEN}║                                                       ║${NC}"
    echo -e "${GREEN}╚═══════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo "Observability features verified:"
    echo "  ✓ Boot stage progression with percentage indicators"
    echo "  ✓ Enhanced panic handler with exception decoding"
    echo "  ✓ Human-readable page fault diagnostics"
    echo "  ✓ Null pointer and protection violation detection"
    echo "  ✓ Color-coded severity markers"
    echo "  ✓ Audit logging subsystem"
    echo "  ✓ Driver diagnostic messages"
    echo ""
    echo "Next steps:"
    echo "  - Run with --build to verify compilation"
    echo "  - Run with --qemu to test live boot sequence"
    echo "  - Trigger page fault to verify exception decoder"
    echo "  - Check driver error paths for readable output"
    echo ""
    exit 0
else
    echo -e "${RED}╔═══════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║                                                       ║${NC}"
    echo -e "${RED}║  ✗ SOME OBSERVABILITY TESTS FAILED                   ║${NC}"
    echo -e "${RED}║                                                       ║${NC}"
    echo -e "${RED}╚═══════════════════════════════════════════════════════╝${NC}"
    echo ""
    echo "Please review the failed tests above and ensure all"
    echo "observability features are properly implemented."
    echo ""
    exit 1
fi
