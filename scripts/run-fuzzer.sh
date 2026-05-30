#!/bin/bash
#
# AutomationOS Fuzzing Orchestration Script
#
# This script coordinates fuzzing campaigns across multiple fuzzers.
# It supports parallel fuzzing, continuous fuzzing, and crash management.
#
# Usage:
#   ./run-fuzzer.sh --all --time 1h
#   ./run-fuzzer.sh --syscall --time 30m
#   ./run-fuzzer.sh --continuous

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
FUZZ_DIR="$ROOT_DIR/tests/fuzz"
OUTPUT_DIR="$FUZZ_DIR/output"
CRASH_DIR="$FUZZ_DIR/crashes"
CORPUS_DIR="$FUZZ_DIR/corpus"

# Default configuration
RUN_SYSCALL=false
RUN_HEAP=false
RUN_DRIVER=false
RUN_ALL=false
RUN_CONTINUOUS=false
FUZZ_TIME="1h"
AFL_MODE=false
NUM_CORES=4

# Check if AFL++ is available
if command -v afl-fuzz &> /dev/null; then
    AFL_AVAILABLE=true
else
    AFL_AVAILABLE=false
fi

# ============================================================================
# Helper Functions
# ============================================================================

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_prerequisites() {
    log_info "Checking prerequisites..."

    # Check if fuzzers are built
    if [ ! -f "$FUZZ_DIR/syscall_fuzzer" ]; then
        log_error "Fuzzers not built. Run 'make -C tests/fuzz' first."
        exit 1
    fi

    # Check if corpus exists
    if [ ! -d "$CORPUS_DIR" ]; then
        log_error "Corpus not found. Run 'make -C tests/fuzz corpus' first."
        exit 1
    fi

    # Create output directories
    mkdir -p "$OUTPUT_DIR" "$CRASH_DIR"

    # Check AFL++ availability
    if [ "$AFL_AVAILABLE" = true ]; then
        log_success "AFL++ is available"
        AFL_MODE=true
    else
        log_warning "AFL++ not found. Running in standalone mode."
        AFL_MODE=false
    fi

    # Detect number of cores
    if command -v nproc &> /dev/null; then
        NUM_CORES=$(nproc)
    elif [ -f /proc/cpuinfo ]; then
        NUM_CORES=$(grep -c ^processor /proc/cpuinfo)
    else
        NUM_CORES=4
    fi
    log_info "Detected $NUM_CORES CPU cores"

    log_success "Prerequisites check passed"
}

# ============================================================================
# Fuzzer Runners
# ============================================================================

run_syscall_fuzzer() {
    log_info "Starting syscall fuzzer..."

    if [ "$AFL_MODE" = true ]; then
        # AFL++ mode
        log_info "Running with AFL++ for $FUZZ_TIME"
        mkdir -p "$OUTPUT_DIR/syscall"

        timeout "$FUZZ_TIME" afl-fuzz \
            -i "$CORPUS_DIR/syscall_seeds" \
            -o "$OUTPUT_DIR/syscall" \
            -m none \
            -- "$FUZZ_DIR/syscall_fuzzer" @@ || true

        # Copy crashes
        if [ -d "$OUTPUT_DIR/syscall/default/crashes" ]; then
            cp -v "$OUTPUT_DIR/syscall/default/crashes/"* "$CRASH_DIR/" 2>/dev/null || true
        fi
    else
        # Standalone mode
        log_info "Running in standalone mode for $FUZZ_TIME"

        # Convert time to iterations (rough estimate: 1000 exec/sec)
        ITERATIONS=100000
        if [[ "$FUZZ_TIME" =~ ([0-9]+)m ]]; then
            ITERATIONS=$((${BASH_REMATCH[1]} * 60 * 1000))
        elif [[ "$FUZZ_TIME" =~ ([0-9]+)h ]]; then
            ITERATIONS=$((${BASH_REMATCH[1]} * 3600 * 1000))
        fi

        "$FUZZ_DIR/syscall_fuzzer" --iterations "$ITERATIONS" || true
    fi

    log_success "Syscall fuzzer completed"
}

run_heap_fuzzer() {
    log_info "Starting heap fuzzer..."

    if [ "$AFL_MODE" = true ]; then
        log_info "Running with AFL++ for $FUZZ_TIME"
        mkdir -p "$OUTPUT_DIR/heap"

        timeout "$FUZZ_TIME" afl-fuzz \
            -i "$CORPUS_DIR/heap_seeds" \
            -o "$OUTPUT_DIR/heap" \
            -m none \
            -- "$FUZZ_DIR/heap_fuzzer" @@ || true

        if [ -d "$OUTPUT_DIR/heap/default/crashes" ]; then
            cp -v "$OUTPUT_DIR/heap/default/crashes/"* "$CRASH_DIR/" 2>/dev/null || true
        fi
    else
        log_info "Running in stress test mode for $FUZZ_TIME"

        ITERATIONS=100000
        if [[ "$FUZZ_TIME" =~ ([0-9]+)m ]]; then
            ITERATIONS=$((${BASH_REMATCH[1]} * 60 * 100))
        elif [[ "$FUZZ_TIME" =~ ([0-9]+)h ]]; then
            ITERATIONS=$((${BASH_REMATCH[1]} * 3600 * 100))
        fi

        "$FUZZ_DIR/heap_fuzzer" --stress --concurrent "$NUM_CORES" --iterations "$ITERATIONS" || true
    fi

    log_success "Heap fuzzer completed"
}

run_driver_fuzzer() {
    log_info "Starting driver fuzzer..."

    for DRIVER in ps2 serial fb; do
        log_info "Fuzzing $DRIVER driver..."

        if [ "$AFL_MODE" = true ]; then
            mkdir -p "$OUTPUT_DIR/driver_$DRIVER"

            timeout "$FUZZ_TIME" afl-fuzz \
                -i "$CORPUS_DIR/driver_seeds" \
                -o "$OUTPUT_DIR/driver_$DRIVER" \
                -m none \
                -- "$FUZZ_DIR/driver_fuzzer" --driver "$DRIVER" @@ || true

            if [ -d "$OUTPUT_DIR/driver_$DRIVER/default/crashes" ]; then
                cp -v "$OUTPUT_DIR/driver_$DRIVER/default/crashes/"* "$CRASH_DIR/" 2>/dev/null || true
            fi
        else
            ITERATIONS=50000
            if [[ "$FUZZ_TIME" =~ ([0-9]+)m ]]; then
                ITERATIONS=$((${BASH_REMATCH[1]} * 20 * 1000))
            elif [[ "$FUZZ_TIME" =~ ([0-9]+)h ]]; then
                ITERATIONS=$((${BASH_REMATCH[1]} * 3600 * 20))
            fi

            "$FUZZ_DIR/driver_fuzzer" --driver "$DRIVER" --iterations "$ITERATIONS" || true
        fi
    done

    log_success "Driver fuzzer completed"
}

# ============================================================================
# Parallel Fuzzing
# ============================================================================

run_parallel_fuzzing() {
    log_info "Starting parallel fuzzing on $NUM_CORES cores..."

    PIDS=()

    if [ "$RUN_SYSCALL" = true ] || [ "$RUN_ALL" = true ]; then
        run_syscall_fuzzer &
        PIDS+=($!)
    fi

    if [ "$RUN_HEAP" = true ] || [ "$RUN_ALL" = true ]; then
        run_heap_fuzzer &
        PIDS+=($!)
    fi

    if [ "$RUN_DRIVER" = true ] || [ "$RUN_ALL" = true ]; then
        run_driver_fuzzer &
        PIDS+=($!)
    fi

    # Wait for all fuzzers
    for PID in "${PIDS[@]}"; do
        wait "$PID" || true
    done

    log_success "All fuzzers completed"
}

# ============================================================================
# Continuous Fuzzing
# ============================================================================

run_continuous_fuzzing() {
    log_info "Starting continuous fuzzing (Ctrl+C to stop)..."

    while true; do
        log_info "Fuzzing cycle started at $(date)"
        run_parallel_fuzzing

        # Check for crashes
        CRASHES=$(find "$CRASH_DIR" -type f 2>/dev/null | wc -l)
        if [ "$CRASHES" -gt 0 ]; then
            log_warning "Found $CRASHES crashes! Check $CRASH_DIR"
        fi

        log_info "Cycle complete. Sleeping for 60 seconds..."
        sleep 60
    done
}

# ============================================================================
# Crash Reporting
# ============================================================================

report_crashes() {
    log_info "Checking for crashes..."

    CRASH_COUNT=$(find "$CRASH_DIR" -type f 2>/dev/null | wc -l)

    if [ "$CRASH_COUNT" -eq 0 ]; then
        log_success "No crashes found!"
        return
    fi

    log_warning "Found $CRASH_COUNT crash files:"
    find "$CRASH_DIR" -type f -exec ls -lh {} \;

    echo ""
    log_info "To reproduce crashes:"
    echo "  ./tests/fuzz/syscall_fuzzer --input <crash_file>"
    echo ""
    log_info "To analyze with GDB:"
    echo "  gdb ./tests/fuzz/syscall_fuzzer"
    echo "  (gdb) run --input <crash_file>"
    echo ""
    log_info "See docs/CRASH_TRIAGE.md for detailed triage procedures."
}

# ============================================================================
# Usage
# ============================================================================

print_usage() {
    cat << EOF
AutomationOS Fuzzing Orchestration Script

Usage: $0 [OPTIONS]

Options:
  --all               Run all fuzzers
  --syscall           Run syscall fuzzer only
  --heap              Run heap fuzzer only
  --driver            Run driver fuzzer only
  --time DURATION     Fuzzing duration (e.g., 30m, 1h, 24h) [default: 1h]
  --continuous        Run continuous fuzzing (infinite loop)
  --help              Show this help message

Examples:
  # Fuzz everything for 1 hour
  $0 --all --time 1h

  # Fuzz syscalls for 30 minutes
  $0 --syscall --time 30m

  # Continuous fuzzing (24/7)
  $0 --continuous

  # Fuzz syscalls and heap in parallel
  $0 --syscall --heap --time 2h

EOF
}

# ============================================================================
# Main
# ============================================================================

main() {
    # Parse arguments
    if [ $# -eq 0 ]; then
        print_usage
        exit 1
    fi

    while [ $# -gt 0 ]; do
        case "$1" in
            --all)
                RUN_ALL=true
                ;;
            --syscall)
                RUN_SYSCALL=true
                ;;
            --heap)
                RUN_HEAP=true
                ;;
            --driver)
                RUN_DRIVER=true
                ;;
            --time)
                FUZZ_TIME="$2"
                shift
                ;;
            --continuous)
                RUN_CONTINUOUS=true
                ;;
            --help)
                print_usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                print_usage
                exit 1
                ;;
        esac
        shift
    done

    # Check if at least one fuzzer is selected
    if [ "$RUN_ALL" = false ] && [ "$RUN_SYSCALL" = false ] && \
       [ "$RUN_HEAP" = false ] && [ "$RUN_DRIVER" = false ]; then
        log_error "No fuzzer selected!"
        print_usage
        exit 1
    fi

    echo "======================================================================"
    echo "                  AutomationOS Fuzzing Campaign"
    echo "======================================================================"
    echo ""

    check_prerequisites

    echo ""
    echo "Configuration:"
    echo "  Duration: $FUZZ_TIME"
    echo "  Syscall: $RUN_SYSCALL"
    echo "  Heap: $RUN_HEAP"
    echo "  Driver: $RUN_DRIVER"
    echo "  All: $RUN_ALL"
    echo "  Continuous: $RUN_CONTINUOUS"
    echo "  AFL++ Mode: $AFL_MODE"
    echo "  CPU Cores: $NUM_CORES"
    echo ""
    echo "======================================================================"
    echo ""

    # Run fuzzing
    if [ "$RUN_CONTINUOUS" = true ]; then
        run_continuous_fuzzing
    else
        run_parallel_fuzzing
    fi

    echo ""
    echo "======================================================================"
    report_crashes
    echo "======================================================================"
    echo ""

    log_success "Fuzzing campaign complete!"
}

main "$@"
