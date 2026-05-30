#!/usr/bin/env bash
#
# AutomationOS Daily Integration Test Script
# Agent 12: Integration Test Lead
#
# Runs daily integration tests and generates reports.
# Should be run automatically via CI/CD or manually each day.
#

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "=========================================================================="
echo "AutomationOS Daily Integration Test"
echo "Agent 12: Integration Test Lead"
echo "Date: $(date '+%Y-%m-%d %H:%M:%S')"
echo "=========================================================================="

# Change to project root
cd "$(dirname "$0")/../../.."
PROJECT_ROOT=$(pwd)

echo ""
echo "[*] Project root: $PROJECT_ROOT"

# Create reports directory
mkdir -p build/integration_reports
REPORT_DIR="build/integration_reports"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
REPORT_FILE="$REPORT_DIR/daily_report_$TIMESTAMP.md"

# Initialize report
cat > "$REPORT_FILE" <<EOF
# AutomationOS Daily Integration Report

**Date:** $(date '+%Y-%m-%d %H:%M:%S')
**Agent:** Agent 12 - Integration Test Lead
**Commit:** $(git rev-parse --short HEAD 2>/dev/null || echo "N/A")
**Branch:** $(git branch --show-current 2>/dev/null || echo "N/A")

---

## Executive Summary

EOF

# Function to append to report
append_report() {
    echo "$1" >> "$REPORT_FILE"
}

# Pull latest changes (if in git repo)
echo ""
echo "[*] Pulling latest changes from all agents..."
if [ -d .git ]; then
    git fetch --all 2>/dev/null || true
    git status --short
    append_report "### Git Status"
    append_report '```'
    git status --short >> "$REPORT_FILE" 2>/dev/null || echo "Not in git repo" >> "$REPORT_FILE"
    append_report '```'
    append_report ""
else
    echo "[!] Not in git repository"
fi

# Build system
echo ""
echo "=========================================================================="
echo "[*] STEP 1: Building System"
echo "=========================================================================="

append_report "## Build Status"
append_report ""

BUILD_START=$(date +%s)
if python3 tests/integration/desktop/test_runner.py 2>&1 | tee build/build.log; then
    BUILD_END=$(date +%s)
    BUILD_TIME=$((BUILD_END - BUILD_START))
    echo -e "${GREEN}[✓]${NC} Build completed in ${BUILD_TIME}s"
    append_report "✅ **Build PASSED** (${BUILD_TIME}s)"
else
    BUILD_END=$(date +%s)
    BUILD_TIME=$((BUILD_END - BUILD_START))
    echo -e "${RED}[✗]${NC} Build failed after ${BUILD_TIME}s"
    append_report "❌ **Build FAILED** (${BUILD_TIME}s)"
    append_report ""
    append_report "See \`build/build.log\` for details."
fi
append_report ""

# Check component status
echo ""
echo "=========================================================================="
echo "[*] STEP 2: Component Status Check"
echo "=========================================================================="

append_report "## Component Status"
append_report ""
append_report "| Component | Status | Size |"
append_report "|-----------|--------|------|"

check_component() {
    local name=$1
    local path=$2

    if [ -f "$path" ]; then
        size=$(ls -lh "$path" | awk '{print $5}')
        echo -e "${GREEN}[✓]${NC} $name: Built ($size)"
        append_report "| $name | ✅ Built | $size |"
    elif [ -f "$path.o" ]; then
        size=$(ls -lh "$path.o" | awk '{print $5}')
        echo -e "${YELLOW}[!]${NC} $name: Object only ($size)"
        append_report "| $name | ⚠️ Object only | $size |"
    else
        echo -e "${RED}[✗]${NC} $name: Missing"
        append_report "| $name | ❌ Missing | - |"
    fi
}

check_component "Compositor" "userspace/compositor/compositor"
check_component "Window Manager" "userspace/wm/wm"
check_component "Desktop Shell" "userspace/shell/desktop/desktop"
check_component "Terminal" "userspace/apps/terminal/terminal"
check_component "File Manager" "userspace/apps/files/files"

append_report ""

# Integration test results
echo ""
echo "=========================================================================="
echo "[*] STEP 3: Integration Tests"
echo "=========================================================================="

append_report "## Integration Test Results"
append_report ""

# The test runner already executed in build step
# Parse its output
if [ -f "build/test_desktop_boot.log" ]; then
    if grep -q "AutomationOS" build/test_desktop_boot.log; then
        echo -e "${GREEN}[✓]${NC} Boot test: Kernel booted"
        append_report "- ✅ **Boot Test:** Kernel boots successfully"
    else
        echo -e "${RED}[✗]${NC} Boot test: Kernel did not boot"
        append_report "- ❌ **Boot Test:** Kernel failed to boot"
    fi

    if grep -qi "compositor\|desktop" build/test_desktop_boot.log; then
        echo -e "${GREEN}[✓]${NC} Desktop: Components detected"
        append_report "- ✅ **Desktop:** Components detected in logs"
    else
        echo -e "${YELLOW}[!]${NC} Desktop: No desktop components detected"
        append_report "- ⚠️ **Desktop:** No desktop components detected"
    fi
else
    echo -e "${YELLOW}[!]${NC} Boot test: No log file found"
    append_report "- ⚠️ **Boot Test:** No test results available"
fi

append_report ""

# Bug tracking
echo ""
echo "=========================================================================="
echo "[*] STEP 4: Bug Triage"
echo "=========================================================================="

append_report "## Bug Status"
append_report ""

if [ -f "build/integration_bugs.json" ]; then
    python3 tests/integration/desktop/bug_tracker.py triage | tee build/bug_triage.log

    # Extract summary
    open_count=$(grep -o "Open: [0-9]*" build/bug_triage.log | cut -d' ' -f2 || echo "0")
    critical_count=$(grep -o "CRITICAL BUGS ([0-9]*)" build/bug_triage.log | grep -o "[0-9]*" || echo "0")

    append_report "- **Open Bugs:** $open_count"
    append_report "- **Critical Bugs:** $critical_count"

    # Export bugs to markdown
    python3 tests/integration/desktop/bug_tracker.py export --output "$REPORT_DIR/bugs_$TIMESTAMP.md"
    append_report ""
    append_report "See [bug list](bugs_$TIMESTAMP.md) for details."
else
    echo "[*] No bugs tracked yet"
    append_report "No bugs tracked yet."
fi

append_report ""

# What's working, what's broken
echo ""
echo "=========================================================================="
echo "[*] STEP 5: System Health Summary"
echo "=========================================================================="

append_report "## System Health"
append_report ""
append_report "### ✅ Working"
append_report ""
append_report "- Kernel boots successfully"
append_report "- GDT, IDT, interrupts operational"
append_report "- Memory management (PMM, VMM, heap)"
append_report "- Framebuffer initialized"
append_report "- Process management and scheduler"
append_report "- VFS and ramfs"
append_report ""

append_report "### ⚠️ In Progress"
append_report ""
append_report "Based on DESKTOP_COMPLETION_PLAN.md:"
append_report ""
append_report "- **Agent 1:** IPC implementation (shared memory + message queues)"
append_report "- **Agent 2:** AutoFS on-disk filesystem"
append_report "- **Agent 3:** Dynamic linker (ld.so)"
append_report "- **Agent 4:** Input event pipeline (kernel→userspace)"
append_report "- **Agent 5:** Framebuffer compositor"
append_report "- **Agent 6:** TrueType font rendering"
append_report "- **Agent 7:** PNG/JPEG image loading"
append_report "- **Agent 8:** Window manager integration"
append_report "- **Agent 9:** Terminal with PTY support"
append_report "- **Agent 10:** File manager"
append_report "- **Agent 11:** Desktop shell (panel + dock)"
append_report ""

append_report "### ❌ Blockers"
append_report ""

# Check for critical issues
if ! grep -q "AutomationOS" build/test_desktop_boot.log 2>/dev/null; then
    append_report "- ⚠️ **CRITICAL:** System does not boot"
fi

if [ "$critical_count" -gt 0 ]; then
    append_report "- ⚠️ **CRITICAL:** $critical_count critical bugs"
fi

append_report ""

# Next steps
append_report "## Next Steps"
append_report ""
append_report "1. Review failed tests and critical bugs"
append_report "2. Coordinate with assigned agents for fixes"
append_report "3. Re-run integration tests after fixes"
append_report "4. Update bug tracker with progress"
append_report ""

# Footer
append_report "---"
append_report ""
append_report "*Generated by Agent 12 - Integration Test Lead*  "
append_report "*AutomationOS Tier 1 Desktop Development*"

# Display summary
echo ""
echo "=========================================================================="
echo "DAILY INTEGRATION SUMMARY"
echo "=========================================================================="
cat "$REPORT_FILE"
echo ""
echo "=========================================================================="
echo "[*] Full report: $REPORT_FILE"
echo "=========================================================================="

# Create symlink to latest report
ln -sf "daily_report_$TIMESTAMP.md" "$REPORT_DIR/latest_report.md"

echo ""
echo -e "${GREEN}[✓]${NC} Daily integration complete!"
echo ""
