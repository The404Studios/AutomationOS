#!/usr/bin/env bash
#
# AutomationOS Integration Test Runner
#
# Runs all integration tests in QEMU and collects results.
# Tests include:
# - Integration suite (inter-subsystem tests)
# - Stress tests (extreme load)
# - Regression tests (bug prevention)
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
ISO_PATH="$BUILD_DIR/AutomationOS.iso"
TEST_REPORT_DIR="$BUILD_DIR/test-reports"
INTEGRATION_REPORT="$TEST_REPORT_DIR/integration-results.txt"
QEMU_TIMEOUT=120  # 2 minutes

# Options
SKIP_BUILD=0
VERBOSE=0
TEST_SUITE="all"  # all, integration, stress, regression

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --suite)
            TEST_SUITE="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --skip-build       Skip building the ISO"
            echo "  --verbose          Enable verbose output"
            echo "  --suite SUITE      Run specific test suite (all|integration|stress|regression)"
            echo "  --help             Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Run '$0 --help' for usage information"
            exit 1
            ;;
    esac
done

# Functions
log() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

check_prerequisites() {
    log "Checking prerequisites..."

    local missing=0

    # Check for QEMU
    if ! command -v qemu-system-x86_64 &> /dev/null; then
        error "qemu-system-x86_64 not found"
        echo "  Install: sudo apt install qemu-system-x86 (Ubuntu/Debian)"
        echo "          sudo pacman -S qemu (Arch Linux)"
        missing=1
    fi

    # Check for cross-compiler (if not skipping build)
    if [ $SKIP_BUILD -eq 0 ]; then
        if ! command -v x86_64-elf-gcc &> /dev/null; then
            error "x86_64-elf-gcc not found"
            echo "  Run: bash scripts/setup-toolchain.sh"
            missing=1
        fi

        if ! command -v nasm &> /dev/null; then
            error "nasm not found"
            echo "  Install: sudo apt install nasm (Ubuntu/Debian)"
            missing=1
        fi
    fi

    if [ $missing -eq 1 ]; then
        error "Missing required tools. Please install them and try again."
        exit 1
    fi

    success "All prerequisites met"
}

build_iso() {
    if [ $SKIP_BUILD -eq 1 ]; then
        log "Skipping build (--skip-build specified)"

        if [ ! -f "$ISO_PATH" ]; then
            error "ISO not found at $ISO_PATH"
            error "Cannot skip build when ISO doesn't exist"
            exit 1
        fi

        return 0
    fi

    log "Building AutomationOS ISO..."

    cd "$PROJECT_ROOT"

    # Clean previous build
    if [ $VERBOSE -eq 1 ]; then
        make clean
    else
        make clean > /dev/null 2>&1
    fi

    # Build kernel and ISO
    if [ $VERBOSE -eq 1 ]; then
        make all
    else
        make all > "$BUILD_DIR/build.log" 2>&1
    fi

    if [ $? -ne 0 ]; then
        error "Build failed. Check $BUILD_DIR/build.log for details"
        exit 1
    fi

    if [ ! -f "$ISO_PATH" ]; then
        error "Build succeeded but ISO not found at $ISO_PATH"
        exit 1
    fi

    success "Build complete: $ISO_PATH"
}

run_test_suite() {
    local suite_name=$1
    local test_marker=$2

    log "Running $suite_name..."

    # Create report directory
    mkdir -p "$TEST_REPORT_DIR"

    # Run QEMU with serial output
    local output_file="$TEST_REPORT_DIR/${suite_name,,}-output.txt"

    timeout $QEMU_TIMEOUT qemu-system-x86_64 \
        -cdrom "$ISO_PATH" \
        -serial stdio \
        -nographic \
        -m 512M \
        -enable-kvm 2>/dev/null || true \
        > "$output_file" 2>&1 &

    local qemu_pid=$!

    # Wait for test completion or timeout
    local elapsed=0
    local check_interval=2

    while kill -0 $qemu_pid 2>/dev/null; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))

        if [ $elapsed -ge $QEMU_TIMEOUT ]; then
            warning "Test timeout after ${QEMU_TIMEOUT}s"
            kill $qemu_pid 2>/dev/null || true
            break
        fi

        # Check if test completed
        if grep -q "TEST SUITE SUMMARY" "$output_file" 2>/dev/null; then
            log "Test completed, shutting down QEMU..."
            kill $qemu_pid 2>/dev/null || true
            break
        fi
    done

    # Parse results
    if [ -f "$output_file" ]; then
        if grep -q "ALL TESTS PASSED" "$output_file"; then
            success "$suite_name: ALL TESTS PASSED"
            return 0
        elif grep -q "TESTS FAILED" "$output_file"; then
            local failed_count=$(grep "Failed:" "$output_file" | awk '{print $2}')
            error "$suite_name: $failed_count TESTS FAILED"
            return 1
        else
            warning "$suite_name: Could not determine test results"
            return 2
        fi
    else
        error "$suite_name: Output file not found"
        return 1
    fi
}

generate_summary_report() {
    log "Generating summary report..."

    local report_file="$TEST_REPORT_DIR/integration-test-summary.md"

    cat > "$report_file" <<EOF
# AutomationOS Integration Test Results

**Date:** $(date '+%Y-%m-%d %H:%M:%S')
**Test Runner:** run-integration-tests.sh
**ISO:** $ISO_PATH

---

## Test Suites Executed

EOF

    # Check each test suite output
    for suite in integration stress regression; do
        local output_file="$TEST_REPORT_DIR/${suite}-output.txt"

        if [ -f "$output_file" ]; then
            echo "### ${suite^} Test Suite" >> "$report_file"
            echo "" >> "$report_file"

            if grep -q "ALL TESTS PASSED" "$output_file"; then
                echo "**Status:** ✅ PASSED" >> "$report_file"
            elif grep -q "TESTS FAILED" "$output_file"; then
                echo "**Status:** ❌ FAILED" >> "$report_file"
            else
                echo "**Status:** ⚠️ INCOMPLETE" >> "$report_file"
            fi

            # Extract statistics
            if grep -q "Total:" "$output_file"; then
                local total=$(grep "Total:" "$output_file" | tail -1 | awk '{print $2}')
                local passed=$(grep "Passed:" "$output_file" | tail -1 | awk '{print $2}')
                local failed=$(grep "Failed:" "$output_file" | tail -1 | awk '{print $2}')

                echo "" >> "$report_file"
                echo "- Total Tests: $total" >> "$report_file"
                echo "- Passed: $passed" >> "$report_file"
                echo "- Failed: $failed" >> "$report_file"
            fi

            echo "" >> "$report_file"
            echo "**Output:** \`$output_file\`" >> "$report_file"
            echo "" >> "$report_file"
        fi
    done

    cat >> "$report_file" <<EOF

---

## Summary

EOF

    # Overall status
    local all_passed=1
    for suite in integration stress regression; do
        local output_file="$TEST_REPORT_DIR/${suite}-output.txt"
        if [ -f "$output_file" ]; then
            if ! grep -q "ALL TESTS PASSED" "$output_file"; then
                all_passed=0
                break
            fi
        fi
    done

    if [ $all_passed -eq 1 ]; then
        echo "**Overall Status:** ✅ ALL TESTS PASSED" >> "$report_file"
        echo "" >> "$report_file"
        echo "AutomationOS has passed all integration, stress, and regression tests." >> "$report_file"
    else
        echo "**Overall Status:** ❌ SOME TESTS FAILED" >> "$report_file"
        echo "" >> "$report_file"
        echo "Review individual test suite outputs for details." >> "$report_file"
    fi

    success "Summary report generated: $report_file"
}

# Main execution
main() {
    echo ""
    echo "===================================================================="
    echo "  AutomationOS Integration Test Runner"
    echo "===================================================================="
    echo ""

    # Check prerequisites
    check_prerequisites

    # Build ISO
    build_iso

    # Run test suites
    case $TEST_SUITE in
        all)
            run_test_suite "Integration Suite" "INTEGRATION TEST"
            run_test_suite "Stress Test Suite" "STRESS TEST"
            run_test_suite "Regression Suite" "REGRESSION TEST"
            ;;
        integration)
            run_test_suite "Integration Suite" "INTEGRATION TEST"
            ;;
        stress)
            run_test_suite "Stress Test Suite" "STRESS TEST"
            ;;
        regression)
            run_test_suite "Regression Suite" "REGRESSION TEST"
            ;;
        *)
            error "Unknown test suite: $TEST_SUITE"
            exit 1
            ;;
    esac

    # Generate summary report
    generate_summary_report

    echo ""
    echo "===================================================================="
    echo "  Test Results"
    echo "===================================================================="
    cat "$TEST_REPORT_DIR/integration-test-summary.md"
    echo "===================================================================="
    echo ""

    success "Integration testing complete"
}

# Run main function
main
