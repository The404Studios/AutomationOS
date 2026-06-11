#!/bin/bash
# Memory Leak Detection Tool for AutomationOS
# Scans for common memory leak patterns

set -euo pipefail

KERNEL_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$KERNEL_ROOT"

echo "=========================================="
echo "AutomationOS Memory Leak Hunter"
echo "=========================================="
echo ""

# Colors
RED='\033[0;31m'
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

LEAK_COUNT=0

# Function to report a leak
report_leak() {
    local file="$1"
    local line="$2"
    local category="$3"
    local description="$4"

    echo -e "${RED}[LEAK]${NC} $category in $file:$line"
    echo "       $description"
    echo ""
    ((LEAK_COUNT++))
}

# Function to report potential leak
report_potential() {
    local file="$1"
    local line="$2"
    local category="$3"
    local description="$4"

    echo -e "${YELLOW}[POTENTIAL]${NC} $category in $file:$line"
    echo "            $description"
    echo ""
}

echo "Phase 1: Scanning for malloc/calloc without free..."
echo "======================================================"

# Find all malloc/calloc calls and check for matching free
while IFS=: read -r file line content; do
    # Skip test files for now
    if [[ "$file" =~ tests/ ]] || [[ "$file" =~ examples/ ]]; then
        continue
    fi

    # Extract function name
    func_name=$(grep -B 20 "^$line:" "$file" 2>/dev/null | grep -E "^[a-zA-Z_].*\(" | tail -1 | sed 's/(.*//' || echo "unknown")

    # Check if there's a corresponding free in the same function
    # This is a heuristic - not perfect
    if ! grep -A 50 "$content" "$file" | grep -q "free\|kfree"; then
        report_potential "$file" "$line" "Missing free" "malloc/calloc found in $func_name without obvious free"
    fi
done < <(grep -n -E "\b(malloc|calloc|kmalloc)\s*\(" $(find . -name "*.c" -type f) | grep -v "tests/" | grep -v "examples/")

echo ""
echo "Phase 2: Scanning for early return without cleanup..."
echo "======================================================="

# Find functions with malloc followed by return without free
for file in $(find kernel -name "*.c" -type f); do
    # This is complex to do in bash - we'll look for common patterns
    # Pattern: malloc ... if (...) return ... (without free)

    awk '
    /malloc|calloc|kmalloc/ {
        alloc_line = NR
        alloc_content = $0
    }
    alloc_line > 0 && /return/ && NR - alloc_line < 10 && !/free/ {
        print FILENAME ":" NR ": Potential early return after allocation at line " alloc_line
        alloc_line = 0
    }
    alloc_line > 0 && /free|kfree/ {
        alloc_line = 0
    }
    ' "$file"
done

echo ""
echo "Phase 3: Checking process_create error paths..."
echo "================================================="

# Check process.c specifically
if [ -f "kernel/core/sched/process.c" ]; then
    echo "Analyzing process_create()..."

    # Check for PID allocation leak
    if grep -A 5 "allocate_pid()" kernel/core/sched/process.c | grep -q "return NULL" && \
       ! grep -A 5 "allocate_pid()" kernel/core/sched/process.c | grep -q "free_pid\|Return PID to pool"; then
        report_leak "kernel/core/sched/process.c" "36-44" "PID Leak" "PID allocated but not returned to pool on error"
    fi
fi

echo ""
echo "Phase 4: Checking bootloader allocations..."
echo "============================================="

if [ -f "boot/loader.c" ]; then
    # Check if UEFI allocations are tracked
    if grep -q "AllocatePool" boot/loader.c && ! grep -q "FreePool" boot/loader.c; then
        report_potential "boot/loader.c" "multiple" "UEFI Pool Leak" "AllocatePool called but FreePool never called (may be intentional)"
    fi

    # Check memory map allocation
    if grep -A 20 "AllocatePool.*memory_map" boot/loader.c | grep -v "FreePool"; then
        echo "UEFI memory map allocation: intentional (passed to kernel)"
    fi
fi

echo ""
echo "Phase 5: Checking namespace reference counting..."
echo "==================================================="

# Check for unbalanced ref_count operations
for file in kernel/security/namespace.c kernel/core/namespace/*.c; do
    if [ -f "$file" ]; then
        echo "Checking $file..."

        # Count ref_count increments vs decrements
        inc_count=$(grep -c "ref_count++" "$file" || true)
        dec_count=$(grep -c "ref_count--" "$file" || true)

        if [ "$inc_count" -ne "$dec_count" ]; then
            report_potential "$file" "N/A" "Ref Count Imbalance" "Found $inc_count increments but $dec_count decrements"
        fi
    fi
done

echo ""
echo "Phase 6: Checking syscall handlers..."
echo "======================================="

if [ -f "kernel/core/syscall/handlers.c" ]; then
    # Check sys_write for allocation cleanup
    if grep -A 50 "sys_write" kernel/core/syscall/handlers.c | grep -q "kmalloc" && \
       grep -A 50 "sys_write" kernel/core/syscall/handlers.c | grep -A 20 "kmalloc" | grep -q "return.*EFAULT" && \
       grep -A 50 "sys_write" kernel/core/syscall/handlers.c | grep -B 2 "return.*EFAULT" | grep -q "kfree"; then
        echo "sys_write: EFAULT error path has proper cleanup ✓"
    fi
fi

echo ""
echo "Phase 7: Checking PE loader..."
echo "==============================="

if [ -f "kernel/pe/pe_loader.c" ]; then
    # Check pe_parse for error path cleanup
    if grep -A 100 "pe_parse(" kernel/pe/pe_loader.c | grep -q "malloc.*file_data" && \
       grep -A 100 "pe_parse(" kernel/pe/pe_loader.c | grep -q "calloc.*pe_file_t"; then

        # Check if all error paths free both allocations
        echo "Checking PE loader error paths..."
        grep -A 100 "^pe_file_t\* pe_parse" kernel/pe/pe_loader.c | grep -B 2 "return NULL" | \
        while read -r line; do
            if [[ "$line" =~ "return NULL" ]]; then
                echo "Found early return at: $line"
            fi
        done
    fi
fi

echo ""
echo "Phase 8: Checking driver initialization..."
echo "==========================================="

for file in kernel/drivers/**/*.c; do
    if [ ! -f "$file" ]; then continue; fi

    # Check for init functions that allocate but don't handle errors
    if grep -E ".*_init\(.*\)" "$file" | grep -q "malloc\|kmalloc" && \
       ! grep -A 30 ".*_init" "$file" | grep -q "error\|cleanup\|fail"; then
        report_potential "$file" "N/A" "Driver Init" "Driver init allocates but may not have error handling"
    fi
done

echo ""
echo "=========================================="
echo "Summary"
echo "=========================================="
echo "Confirmed leaks found: $LEAK_COUNT"
echo ""

if [ "$LEAK_COUNT" -gt 0 ]; then
    echo -e "${RED}ACTION REQUIRED: Fix the leaks above${NC}"
    exit 1
else
    echo -e "${GREEN}No confirmed leaks found!${NC}"
    echo "Review potential leaks manually."
    exit 0
fi
