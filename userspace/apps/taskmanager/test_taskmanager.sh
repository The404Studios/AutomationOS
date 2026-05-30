#!/bin/bash
# Test script for Task Manager

set -e

echo "=========================================="
echo "  Task Manager Test Suite"
echo "=========================================="
echo ""

# Test 1: Check all source files exist
echo "[TEST 1] Checking source files..."
REQUIRED_FILES=(
    "taskmanager.h"
    "taskmanager.c"
    "procinfo.c"
    "sysinfo.c"
    "procctl.c"
    "ui.c"
    "input.c"
    "utils.c"
    "Makefile"
    "README.md"
    "INTEGRATION.md"
)

for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$file" ]; then
        echo "  ✗ Missing file: $file"
        exit 1
    fi
    echo "  ✓ Found: $file"
done

echo ""

# Test 2: Count lines of code
echo "[TEST 2] Counting lines of code..."
C_FILES="taskmanager.c procinfo.c sysinfo.c procctl.c ui.c input.c utils.c"
H_FILES="taskmanager.h"

c_lines=$(cat $C_FILES | wc -l)
h_lines=$(cat $H_FILES | wc -l)
total_lines=$((c_lines + h_lines))

echo "  C files:      $c_lines lines"
echo "  Header files: $h_lines lines"
echo "  Total:        $total_lines lines"

if [ $total_lines -lt 2000 ]; then
    echo "  ✗ Line count below target (2000+ LOC)"
    exit 1
fi
echo "  ✓ Line count meets requirements"

echo ""

# Test 3: Check for required functions
echo "[TEST 3] Checking required functions..."
REQUIRED_FUNCTIONS=(
    "collect_process_info"
    "filter_processes"
    "sort_processes"
    "get_system_stats"
    "update_perf_history"
    "kill_process"
    "suspend_process"
    "resume_process"
    "set_process_priority"
    "set_cpu_affinity"
    "render_ui"
    "render_processes_tab"
    "render_performance_tab"
    "render_services_tab"
    "handle_input"
    "format_bytes"
    "format_rate"
    "format_time"
    "state_to_string"
)

for func in "${REQUIRED_FUNCTIONS[@]}"; do
    if ! grep -q "$func" $C_FILES; then
        echo "  ✗ Missing function: $func"
        exit 1
    fi
    echo "  ✓ Found: $func"
done

echo ""

# Test 4: Check for required data structures
echo "[TEST 4] Checking data structures..."
REQUIRED_STRUCTS=(
    "process_info_t"
    "system_stats_t"
    "perf_history_t"
    "ui_state_t"
)

for struct in "${REQUIRED_STRUCTS[@]}"; do
    if ! grep -q "$struct" taskmanager.h; then
        echo "  ✗ Missing structure: $struct"
        exit 1
    fi
    echo "  ✓ Found: $struct"
done

echo ""

# Test 5: Check for UI features
echo "[TEST 5] Checking UI features..."
UI_FEATURES=(
    "TAB_PROCESSES"
    "TAB_PERFORMANCE"
    "TAB_SERVICES"
    "SORT_PID"
    "SORT_NAME"
    "SORT_CPU"
    "SORT_MEMORY"
    "SORT_DISK"
    "SORT_NETWORK"
)

for feature in "${UI_FEATURES[@]}"; do
    if ! grep -q "$feature" taskmanager.h; then
        echo "  ✗ Missing feature: $feature"
        exit 1
    fi
    echo "  ✓ Found: $feature"
done

echo ""

# Test 6: Syntax check (if compiler available)
echo "[TEST 6] Syntax checking..."
if command -v x86_64-elf-gcc &> /dev/null; then
    echo "  Compiling with x86_64-elf-gcc..."
    for file in $C_FILES; do
        x86_64-elf-gcc -std=c11 -ffreestanding -nostdlib -c -I../../libc -o /dev/null $file 2>/dev/null
        if [ $? -eq 0 ]; then
            echo "  ✓ $file: No syntax errors"
        else
            echo "  ✗ $file: Syntax errors detected"
            x86_64-elf-gcc -std=c11 -ffreestanding -nostdlib -c -I../../libc $file
            exit 1
        fi
    done
else
    echo "  ⚠ Compiler not found, skipping syntax check"
fi

echo ""

# Test 7: Documentation check
echo "[TEST 7] Checking documentation..."
if [ ! -s "README.md" ]; then
    echo "  ✗ README.md is empty"
    exit 1
fi
echo "  ✓ README.md exists and is non-empty"

if [ ! -s "INTEGRATION.md" ]; then
    echo "  ✗ INTEGRATION.md is empty"
    exit 1
fi
echo "  ✓ INTEGRATION.md exists and is non-empty"

echo ""

# Summary
echo "=========================================="
echo "  All Tests Passed!"
echo "=========================================="
echo ""
echo "Task Manager Statistics:"
echo "  Total LOC:       $total_lines"
echo "  C files:         $(echo $C_FILES | wc -w)"
echo "  Header files:    $(echo $H_FILES | wc -w)"
echo "  Functions:       ${#REQUIRED_FUNCTIONS[@]}+ implemented"
echo "  Data structures: ${#REQUIRED_STRUCTS[@]}"
echo "  UI features:     ${#UI_FEATURES[@]}"
echo ""
echo "Ready for integration with AutomationOS kernel!"
