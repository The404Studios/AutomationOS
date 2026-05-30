#!/bin/bash
#
# Check fuzzing status and report findings
#

set -e

FUZZ_DIR="$(dirname "$0")/../tests/fuzz"
OUTPUT_DIR="$FUZZ_DIR/output"
CRASH_DIR="$FUZZ_DIR/crashes"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "======================================================================"
echo "                  AutomationOS Fuzzer Status"
echo "======================================================================"
echo ""

# Check if fuzzers are running
echo "Running Fuzzers:"
if pgrep -f "afl-fuzz|syscall_fuzzer|heap_fuzzer|driver_fuzzer" > /dev/null; then
    pgrep -af "afl-fuzz|syscall_fuzzer|heap_fuzzer|driver_fuzzer"
else
    echo "  No fuzzers currently running"
fi
echo ""

# Check for crashes
echo "Crash Summary:"
if [ -d "$CRASH_DIR" ]; then
    CRASH_COUNT=$(find "$CRASH_DIR" -type f 2>/dev/null | wc -l)
    if [ "$CRASH_COUNT" -gt 0 ]; then
        echo -e "  ${RED}Found $CRASH_COUNT crashes!${NC}"
        echo ""
        echo "  Recent crashes:"
        find "$CRASH_DIR" -type f -printf "    %f (%s bytes, %TY-%Tm-%Td %TH:%TM)\n" | head -10
    else
        echo -e "  ${GREEN}No crashes found${NC}"
    fi
else
    echo "  Crash directory not found"
fi
echo ""

# AFL++ Statistics
echo "AFL++ Statistics:"
for FUZZER_DIR in "$OUTPUT_DIR"/*/; do
    if [ -f "$FUZZER_DIR/default/fuzzer_stats" ]; then
        FUZZER_NAME=$(basename "$FUZZER_DIR")
        echo "  Fuzzer: $FUZZER_NAME"

        EXECS=$(grep "^execs_done" "$FUZZER_DIR/default/fuzzer_stats" | cut -d: -f2 | tr -d ' ')
        PATHS=$(grep "^paths_total" "$FUZZER_DIR/default/fuzzer_stats" | cut -d: -f2 | tr -d ' ')
        CRASHES=$(grep "^unique_crashes" "$FUZZER_DIR/default/fuzzer_stats" | cut -d: -f2 | tr -d ' ')
        HANGS=$(grep "^unique_hangs" "$FUZZER_DIR/default/fuzzer_stats" | cut -d: -f2 | tr -d ' ')

        echo "    Execs: $EXECS"
        echo "    Paths: $PATHS"
        echo "    Crashes: $CRASHES"
        echo "    Hangs: $HANGS"
        echo ""
    fi
done

if [ ! -d "$OUTPUT_DIR" ] || [ -z "$(ls -A "$OUTPUT_DIR" 2>/dev/null)" ]; then
    echo "  No AFL++ output found"
    echo ""
fi

echo "======================================================================"
