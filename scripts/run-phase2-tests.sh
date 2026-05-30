#!/bin/bash
#
# AutomationOS Phase 2 Integration Test Runner
#
# Runs all Phase 2 security integration tests and generates a comprehensive report.
#
# Usage:
#   ./scripts/run-phase2-tests.sh              # Run all tests
#   ./scripts/run-phase2-tests.sh --verbose    # Verbose output
#   ./scripts/run-phase2-tests.sh --scenario 1 # Run specific scenario only
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
TEST_DIR="$PROJECT_ROOT/tests/integration/phase2"
BUILD_DIR="$PROJECT_ROOT/build"
REPORT_DIR="$BUILD_DIR/test-reports"

# Configuration
VERBOSE=0
RUN_ALL=1
SCENARIO=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -s|--scenario)
            SCENARIO="$2"
            RUN_ALL=0
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -v, --verbose      Verbose output"
            echo "  -s, --scenario N   Run specific scenario (1-4)"
            echo "  -h, --help         Show this help"
            echo ""
            echo "Scenarios:"
            echo "  1 - Web Server Sandbox"
            echo "  2 - Untrusted Binary Sandbox"
            echo "  3 - Container-Like Isolation"
            echo "  4 - Full Stack Security Integration"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Create report directory
mkdir -p "$REPORT_DIR"

# Log functions
log() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

# Check prerequisites
check_prerequisites() {
    log "Checking prerequisites..."

    # Check Python 3
    if ! command -v python3 &> /dev/null; then
        log_error "python3 not found. Install Python 3.8 or later."
        exit 1
    fi

    # Check QEMU
    if ! command -v qemu-system-x86_64 &> /dev/null; then
        log_error "qemu-system-x86_64 not found. Install QEMU."
        exit 1
    fi

    # Check ISO exists
    if [[ ! -f "$BUILD_DIR/AutomationOS.iso" ]]; then
        log_error "AutomationOS.iso not found. Run 'make iso' first."
        exit 1
    fi

    log_success "All prerequisites met"
}

# Run a test scenario
run_test() {
    local test_file=$1
    local test_name=$2
    local log_file="$REPORT_DIR/${test_name}.log"

    log "Running: $test_name"

    local args=""
    if [[ $VERBOSE -eq 1 ]]; then
        args="--verbose"
    fi

    if python3 "$test_file" $args > "$log_file" 2>&1; then
        log_success "$test_name - PASSED"
        return 0
    else
        log_error "$test_name - FAILED"
        log_error "See log: $log_file"
        return 1
    fi
}

# Main test execution
main() {
    echo "========================================================================"
    echo "AutomationOS Phase 2 Integration Test Suite"
    echo "========================================================================"
    echo ""

    check_prerequisites

    # Test results tracking
    local total=0
    local passed=0
    local failed=0

    # Run tests based on configuration
    if [[ $RUN_ALL -eq 1 ]]; then
        log "Running all test scenarios..."
        echo ""

        # Scenario 1: Web Server Sandbox
        log "=== Scenario 1: Web Server Sandbox ==="
        if run_test "$TEST_DIR/test_scenario_web_server.py" "scenario1-web-server"; then
            ((passed++))
        else
            ((failed++))
        fi
        ((total++))
        echo ""

        # Scenario 2: Untrusted Binary Sandbox
        log "=== Scenario 2: Untrusted Binary Sandbox ==="
        if run_test "$TEST_DIR/test_scenario_untrusted_binary.py" "scenario2-untrusted-binary"; then
            ((passed++))
        else
            ((failed++))
        fi
        ((total++))
        echo ""

        # Scenario 3: Container-Like Isolation
        log "=== Scenario 3: Container-Like Isolation ==="
        if run_test "$TEST_DIR/test_scenario_container.py" "scenario3-container"; then
            ((passed++))
        else
            ((failed++))
        fi
        ((total++))
        echo ""

        # Scenario 4: Full Stack Security Integration
        log "=== Scenario 4: Full Stack Security Integration ==="
        if run_test "$TEST_DIR/test_full_stack_security.py" "scenario4-full-stack"; then
            ((passed++))
        else
            ((failed++))
        fi
        ((total++))
        echo ""

    else
        # Run specific scenario
        case $SCENARIO in
            1)
                log "Running Scenario 1: Web Server Sandbox"
                if run_test "$TEST_DIR/test_scenario_web_server.py" "scenario1-web-server"; then
                    ((passed++))
                else
                    ((failed++))
                fi
                ((total++))
                ;;
            2)
                log "Running Scenario 2: Untrusted Binary Sandbox"
                if run_test "$TEST_DIR/test_scenario_untrusted_binary.py" "scenario2-untrusted-binary"; then
                    ((passed++))
                else
                    ((failed++))
                fi
                ((total++))
                ;;
            3)
                log "Running Scenario 3: Container-Like Isolation"
                if run_test "$TEST_DIR/test_scenario_container.py" "scenario3-container"; then
                    ((passed++))
                else
                    ((failed++))
                fi
                ((total++))
                ;;
            4)
                log "Running Scenario 4: Full Stack Security Integration"
                if run_test "$TEST_DIR/test_full_stack_security.py" "scenario4-full-stack"; then
                    ((passed++))
                else
                    ((failed++))
                fi
                ((total++))
                ;;
            *)
                log_error "Invalid scenario: $SCENARIO"
                exit 1
                ;;
        esac
    fi

    # Print summary
    echo ""
    echo "========================================================================"
    echo "Test Results Summary"
    echo "========================================================================"
    echo "Total:  $total"
    echo "Passed: $passed"
    echo "Failed: $failed"
    echo "========================================================================"
    echo ""

    if [[ $failed -eq 0 ]]; then
        log_success "All tests passed!"
        echo ""
        log "Generating test report..."
        generate_report $total $passed $failed
        exit 0
    else
        log_error "$failed test(s) failed"
        echo ""
        log "Check logs in: $REPORT_DIR"
        generate_report $total $passed $failed
        exit 1
    fi
}

# Generate test report
generate_report() {
    local total=$1
    local passed=$2
    local failed=$3

    local report_file="$REPORT_DIR/phase2-test-report.md"

    cat > "$report_file" << EOF
# AutomationOS Phase 2 Integration Test Report

**Date:** $(date)
**Version:** Phase 2 Pre-Release
**Total Tests:** $total
**Passed:** $passed
**Failed:** $failed

## Test Summary

| Scenario | Status | Log File |
|----------|--------|----------|
| 1. Web Server Sandbox | $(test_status "scenario1-web-server.log") | scenario1-web-server.log |
| 2. Untrusted Binary | $(test_status "scenario2-untrusted-binary.log") | scenario2-untrusted-binary.log |
| 3. Container Isolation | $(test_status "scenario3-container.log") | scenario3-container.log |
| 4. Full Stack Security | $(test_status "scenario4-full-stack.log") | scenario4-full-stack.log |

## Test Coverage

### Security Mechanisms Tested

- ✓ Capability-based security
- ✓ Namespace isolation (PID, Mount, Network, IPC, UTS)
- ✓ Mandatory Access Control (MAC)
- ✓ Resource limits (rlimit)
- ✓ Audit logging
- ✓ Syscall filtering
- ✓ Security boundary enforcement

### Test Scenarios

1. **Web Server Sandbox**: Tests realistic web server with MAC label, capabilities, network namespace, and syscall filter.

2. **Untrusted Binary Sandbox**: Tests maximum security isolation for untrusted executables with minimal capabilities and strict MAC policy.

3. **Container-Like Isolation**: Tests Docker-like container isolation with all 5 namespace types and per-container policies.

4. **Full Stack Security**: Comprehensive integration test covering all mechanisms working together, performance overhead, and stress testing.

## Detailed Results

See individual log files in \`build/test-reports/\` for detailed output.

## Next Steps

$(if [[ $failed -eq 0 ]]; then
    echo "- All tests passed! Phase 2 security implementation is ready."
    echo "- Proceed to performance profiling"
    echo "- Begin Phase 3 (Networking & AI) development"
else
    echo "- Fix failed tests"
    echo "- Re-run test suite"
    echo "- Review test logs for failure details"
fi)

---

Generated by AutomationOS Test Suite
EOF

    log_success "Test report generated: $report_file"
}

# Helper function to determine test status
test_status() {
    local log_file="$REPORT_DIR/$1"
    if [[ -f "$log_file" ]]; then
        if grep -q "All tests passed" "$log_file" 2>/dev/null || \
           grep -q "PASS" "$log_file" 2>/dev/null; then
            echo "✓ PASS"
        else
            echo "✗ FAIL"
        fi
    else
        echo "- NOT RUN"
    fi
}

# Run main
main
