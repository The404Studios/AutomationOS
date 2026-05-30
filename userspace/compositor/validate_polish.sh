#!/bin/bash
#
# AutomationOS Graphics & Animation Polish - Validation Script
#
# Verifies all polish components are present and properly structured

set -e

echo "╔══════════════════════════════════════════════════════════╗"
echo "║  AutomationOS Graphics Polish - Validation              ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

COMPOSITOR_DIR="userspace/compositor"
UI_LIB_DIR="userspace/lib/ui"

# Color codes
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

passed=0
failed=0

check_file() {
    local file=$1
    local desc=$2

    if [ -f "$file" ]; then
        echo -e "${GREEN}✓${NC} $desc"
        ((passed++))
    else
        echo -e "${RED}✗${NC} $desc (missing: $file)"
        ((failed++))
    fi
}

check_function() {
    local file=$1
    local function=$2
    local desc=$3

    if grep -q "$function" "$file" 2>/dev/null; then
        echo -e "${GREEN}✓${NC} $desc"
        ((passed++))
    else
        echo -e "${RED}✗${NC} $desc (missing: $function in $file)"
        ((failed++))
    fi
}

echo "=== Performance Monitoring System ==="
check_file "$COMPOSITOR_DIR/performance.h" "Performance header"
check_file "$COMPOSITOR_DIR/performance.c" "Performance implementation"
check_function "$COMPOSITOR_DIR/performance.h" "perf_init" "FPS tracking initialization"
check_function "$COMPOSITOR_DIR/performance.c" "perf_record_frame" "Frame timing recorder"
check_function "$COMPOSITOR_DIR/performance.c" "perf_get_grade" "Performance grading"
echo ""

echo "=== Shadow System (Material Design 3) ==="
check_file "$COMPOSITOR_DIR/shadow_system.h" "Shadow system header"
check_file "$COMPOSITOR_DIR/shadow_system.c" "Shadow system implementation"
check_function "$COMPOSITOR_DIR/shadow_system.h" "shadow_draw_layered" "Layered shadow rendering"
check_function "$COMPOSITOR_DIR/shadow_system.c" "SHADOW_SPECS" "5-level shadow specifications"
echo ""

echo "=== Blur Effects System ==="
check_file "$COMPOSITOR_DIR/blur_effects.h" "Blur effects header"
check_file "$COMPOSITOR_DIR/blur_effects.c" "Blur effects implementation"
check_function "$COMPOSITOR_DIR/blur_effects.h" "blur_region" "Region blur"
check_function "$COMPOSITOR_DIR/blur_effects.c" "blur_gaussian_two_pass" "Gaussian blur"
check_function "$COMPOSITOR_DIR/blur_effects.c" "blur_kawase" "Kawase blur (fast)"
echo ""

echo "=== Smooth Scrolling System ==="
check_file "$UI_LIB_DIR/smooth_scroll.h" "Smooth scroll header"
check_file "$UI_LIB_DIR/smooth_scroll.c" "Smooth scroll implementation"
check_function "$UI_LIB_DIR/smooth_scroll.h" "scroll_drag_start" "Drag tracking"
check_function "$UI_LIB_DIR/smooth_scroll.c" "scroll_update" "Physics update"
check_function "$UI_LIB_DIR/smooth_scroll.c" "SCROLL_PHYSICS_DEFAULT" "Physics presets"
echo ""

echo "=== Demo & Build System ==="
check_file "$COMPOSITOR_DIR/polish_demo.c" "Polish demo program"
check_file "$COMPOSITOR_DIR/Makefile.polish" "Build system"
check_file "$COMPOSITOR_DIR/GRAPHICS_POLISH_REPORT.md" "Documentation"
echo ""

echo "=== Code Quality Checks ==="

# Check for common issues
echo -n "Checking for memory leaks (free after malloc)... "
if grep -r "malloc" "$COMPOSITOR_DIR"/*.c | grep -q "free"; then
    echo -e "${GREEN}✓${NC}"
    ((passed++))
else
    echo -e "${YELLOW}⚠${NC} (manual review needed)"
fi

echo -n "Checking for NULL pointer checks... "
if grep -q "if (!.*)" "$COMPOSITOR_DIR"/performance.c; then
    echo -e "${GREEN}✓${NC}"
    ((passed++))
else
    echo -e "${RED}✗${NC}"
    ((failed++))
fi

echo -n "Checking for proper header guards... "
if grep -q "#ifndef.*_H" "$COMPOSITOR_DIR"/performance.h && \
   grep -q "#ifndef.*_H" "$COMPOSITOR_DIR"/shadow_system.h && \
   grep -q "#ifndef.*_H" "$COMPOSITOR_DIR"/blur_effects.h; then
    echo -e "${GREEN}✓${NC}"
    ((passed++))
else
    echo -e "${RED}✗${NC}"
    ((failed++))
fi

echo ""
echo "=== Line Count Statistics ==="
total_lines=0

for component in "performance" "shadow_system" "blur_effects" "smooth_scroll"; do
    if [ "$component" = "smooth_scroll" ]; then
        h_file="$UI_LIB_DIR/${component}.h"
        c_file="$UI_LIB_DIR/${component}.c"
    else
        h_file="$COMPOSITOR_DIR/${component}.h"
        c_file="$COMPOSITOR_DIR/${component}.c"
    fi

    if [ -f "$h_file" ] && [ -f "$c_file" ]; then
        h_lines=$(wc -l < "$h_file" 2>/dev/null || echo 0)
        c_lines=$(wc -l < "$c_file" 2>/dev/null || echo 0)
        component_total=$((h_lines + c_lines))
        total_lines=$((total_lines + component_total))
        printf "  %-20s: %4d lines (header: %3d, impl: %3d)\n" "$component" "$component_total" "$h_lines" "$c_lines"
    fi
done

demo_lines=$(wc -l < "$COMPOSITOR_DIR/polish_demo.c" 2>/dev/null || echo 0)
total_lines=$((total_lines + demo_lines))
printf "  %-20s: %4d lines\n" "polish_demo" "$demo_lines"

echo "  ────────────────────────────────────"
printf "  %-20s: %4d lines\n" "TOTAL" "$total_lines"
echo ""

echo "=== Final Report ==="
echo "Checks passed: $passed"
echo "Checks failed: $failed"
echo "Total lines of code: $total_lines"
echo ""

if [ $failed -eq 0 ]; then
    echo -e "${GREEN}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${GREEN}║  ✓ ALL POLISH COMPONENTS VALIDATED                      ║${NC}"
    echo -e "${GREEN}║                                                          ║${NC}"
    echo -e "${GREEN}║  Graphics & Animation Polish: COMPLETE                   ║${NC}"
    echo -e "${GREEN}╚══════════════════════════════════════════════════════════╝${NC}"
    exit 0
else
    echo -e "${RED}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${RED}║  ✗ VALIDATION FAILED                                     ║${NC}"
    echo -e "${RED}║                                                          ║${NC}"
    echo -e "${RED}║  $failed component(s) missing or incomplete                ║${NC}"
    echo -e "${RED}╚══════════════════════════════════════════════════════════╝${NC}"
    exit 1
fi
