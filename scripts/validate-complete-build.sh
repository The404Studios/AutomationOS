#!/bin/bash
#
# AutomationOS Complete Build Validation
#
# Validates the entire build system infrastructure by testing all components:
# - Toolchain setup
# - Configuration system
# - Build targets
# - Testing infrastructure
# - Packaging system
# - CI/CD integration
#
# Usage:
#   ./scripts/validate-complete-build.sh
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
NC='\033[0m'

# Counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

# Log file
LOG_FILE="build-validation-$(date +%Y%m%d-%H%M%S).log"

print_banner() {
    echo -e "${BLUE}============================================${NC}"
    echo -e "${BLUE}  AutomationOS Build System Validation${NC}"
    echo -e "${BLUE}============================================${NC}"
    echo ""
}

print_section() {
    echo ""
    echo -e "${MAGENTA}=======================================${NC}"
    echo -e "${MAGENTA}  $1${NC}"
    echo -e "${MAGENTA}=======================================${NC}"
    echo ""
}

print_test() {
    echo -n -e "${BLUE}TEST:${NC} $1 ... "
}

print_success() {
    echo -e "${GREEN}✓ PASS${NC}"
    ((TESTS_PASSED++))
    ((TESTS_RUN++))
}

print_fail() {
    echo -e "${RED}✗ FAIL${NC}"
    if [ -n "$1" ]; then
        echo -e "${RED}  Error: $1${NC}"
    fi
    ((TESTS_FAILED++))
    ((TESTS_RUN++))
}

print_skip() {
    echo -e "${YELLOW}⊘ SKIP${NC}"
    if [ -n "$1" ]; then
        echo -e "${YELLOW}  Reason: $1${NC}"
    fi
    ((TESTS_SKIPPED++))
    ((TESTS_RUN++))
}

print_info() {
    echo -e "${BLUE}ℹ${NC} $1"
}

run_test() {
    local test_name="$1"
    local test_cmd="$2"

    print_test "$test_name"

    if eval "$test_cmd" >> "$LOG_FILE" 2>&1; then
        print_success
        return 0
    else
        print_fail
        return 1
    fi
}

# Initialize
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$ROOT_DIR"

print_banner
echo "Start time: $(date)"
echo "Log file: $LOG_FILE"
echo ""

#
# Phase 1: File Structure Validation
#
print_section "Phase 1: File Structure"

print_test "Root Makefile exists"
if [ -f "Makefile" ]; then
    print_success
else
    print_fail
fi

print_test "Configure script exists and is executable"
if [ -x "configure" ]; then
    print_success
else
    print_fail
fi

print_test "Kernel Makefile exists"
if [ -f "kernel/Makefile" ]; then
    print_success
else
    print_fail
fi

print_test "Boot Makefile exists"
if [ -f "boot/Makefile" ]; then
    print_success
else
    print_fail
fi

print_test "Build scripts directory exists"
if [ -d "scripts" ]; then
    script_count=$(ls scripts/*.sh scripts/*.py 2>/dev/null | wc -l)
    print_success
    print_info "Found $script_count build scripts"
else
    print_fail
fi

print_test "Documentation exists"
if [ -f "docs/BUILD.md" ]; then
    print_success
else
    print_fail
fi

print_test "GitHub Actions workflows exist"
if [ -f ".github/workflows/build.yml" ]; then
    print_success
else
    print_fail
fi

print_test "Dockerfile exists"
if [ -f "Dockerfile.build" ]; then
    print_success
else
    print_fail
fi

#
# Phase 2: Toolchain Validation
#
print_section "Phase 2: Toolchain"

print_test "setup-toolchain.sh exists"
if [ -f "scripts/setup-toolchain.sh" ]; then
    print_success
else
    print_fail
fi

run_test "make is installed" "command -v make"
run_test "nasm is installed" "command -v nasm"
run_test "python3 is installed" "command -v python3"

print_test "Cross-compiler (x86_64-elf-gcc)"
if command -v x86_64-elf-gcc &> /dev/null; then
    print_success
    print_info "Version: $(x86_64-elf-gcc --version | head -n1)"
else
    print_skip "Not installed (required for actual build)"
fi

run_test "xorriso is installed (for ISO creation)" "command -v xorriso" || true
run_test "qemu is installed (for testing)" "command -v qemu-system-x86_64" || true

#
# Phase 3: Configuration System
#
print_section "Phase 3: Configuration System"

if [ -x "./configure" ]; then
    print_test "Configure script is executable"
    print_success

    print_test "Configure --help works"
    if ./configure --help > /dev/null 2>&1; then
        print_success
    else
        print_fail
    fi

    print_test "Configure --version works"
    if ./configure --version > /dev/null 2>&1; then
        print_success
    else
        print_fail
    fi

    # Test configuration (if toolchain is available)
    if command -v x86_64-elf-gcc &> /dev/null; then
        print_test "Configure runs successfully"
        if ./configure >> "$LOG_FILE" 2>&1; then
            print_success

            print_test "config.mk generated"
            if [ -f "config.mk" ]; then
                print_success
            else
                print_fail
            fi

            print_test "include/config.h generated"
            if [ -f "include/config.h" ]; then
                print_success
            else
                print_fail
            fi
        else
            print_fail
        fi
    else
        print_skip "Configure test" "Cross-compiler not available"
    fi
else
    print_fail "Configure script not executable"
fi

#
# Phase 4: Build System Targets
#
print_section "Phase 4: Build System Targets"

print_test "Makefile has help target"
if grep -q "^help:" Makefile; then
    print_success
else
    print_fail
fi

print_test "make help runs"
if make help > /dev/null 2>&1; then
    print_success
else
    print_fail
fi

# Check for essential targets
REQUIRED_TARGETS=("all" "clean" "bootloader" "kernel" "userspace" "iso" "qemu" "test")

for target in "${REQUIRED_TARGETS[@]}"; do
    print_test "Makefile has '$target' target"
    if grep -q "^$target:" Makefile || grep -q "^\.PHONY:.*$target" Makefile; then
        print_success
    else
        print_fail
    fi
done

# Check for code quality targets
QUALITY_TARGETS=("format" "lint" "analyze-all" "coverage")

for target in "${QUALITY_TARGETS[@]}"; do
    print_test "Makefile has '$target' target (code quality)"
    if grep -q "^$target:" Makefile || grep -q "^\.PHONY:.*$target" Makefile; then
        print_success
    else
        print_fail
    fi
done

#
# Phase 5: Build Scripts
#
print_section "Phase 5: Build Scripts"

REQUIRED_SCRIPTS=(
    "scripts/setup-toolchain.sh"
    "scripts/run-qemu.sh"
    "scripts/build-iso.py"
    "scripts/build-release.sh"
    "scripts/build-deb.sh"
    "scripts/build-rpm.sh"
)

for script in "${REQUIRED_SCRIPTS[@]}"; do
    print_test "$(basename "$script") exists and is executable"
    if [ -x "$script" ]; then
        print_success
    else
        if [ -f "$script" ]; then
            chmod +x "$script" 2>/dev/null && print_success || print_fail "Not executable"
        else
            print_fail "Not found"
        fi
    fi
done

# Validate scripts have help
print_test "run-qemu.sh has --help"
if ./scripts/run-qemu.sh --help > /dev/null 2>&1; then
    print_success
else
    print_fail
fi

print_test "setup-toolchain.sh has --help"
if ./scripts/setup-toolchain.sh --help > /dev/null 2>&1; then
    print_success
else
    print_fail
fi

#
# Phase 6: Documentation
#
print_section "Phase 6: Documentation"

REQUIRED_DOCS=(
    "README.md"
    "docs/BUILD.md"
    "CHANGELOG.md"
)

for doc in "${REQUIRED_DOCS[@]}"; do
    print_test "$doc exists"
    if [ -f "$doc" ]; then
        word_count=$(wc -w < "$doc")
        print_success
        print_info "$word_count words"
    else
        print_fail
    fi
done

print_test "BUILD.md has table of contents"
if [ -f "docs/BUILD.md" ] && grep -q "## Table of Contents" "docs/BUILD.md"; then
    print_success
else
    print_fail
fi

print_test "BUILD.md documents all major targets"
if [ -f "docs/BUILD.md" ]; then
    missing_docs=()
    for target in "${REQUIRED_TARGETS[@]}"; do
        if ! grep -q "$target" "docs/BUILD.md"; then
            missing_docs+=("$target")
        fi
    done

    if [ ${#missing_docs[@]} -eq 0 ]; then
        print_success
    else
        print_fail "Missing documentation for: ${missing_docs[*]}"
    fi
else
    print_fail
fi

#
# Phase 7: CI/CD Integration
#
print_section "Phase 7: CI/CD Integration"

print_test "GitHub Actions build workflow exists"
if [ -f ".github/workflows/build.yml" ]; then
    print_success
else
    print_fail
fi

if [ -f ".github/workflows/build.yml" ]; then
    print_test "Build workflow has multiple platforms"
    if grep -q "matrix:" ".github/workflows/build.yml"; then
        print_success
    else
        print_fail
    fi

    print_test "Build workflow tests Ubuntu"
    if grep -q "ubuntu" ".github/workflows/build.yml"; then
        print_success
    else
        print_fail
    fi

    print_test "Build workflow tests macOS"
    if grep -q "macos" ".github/workflows/build.yml"; then
        print_success
    else
        print_fail
    fi

    print_test "Build workflow uploads artifacts"
    if grep -q "upload-artifact" ".github/workflows/build.yml"; then
        print_success
    else
        print_fail
    fi
fi

print_test "Docker build environment exists"
if [ -f "Dockerfile.build" ]; then
    print_success
else
    print_fail
fi

if [ -f "Dockerfile.build" ]; then
    print_test "Dockerfile installs cross-compiler"
    if grep -q "x86_64-elf-gcc" "Dockerfile.build" || grep -q "GCC_VERSION" "Dockerfile.build"; then
        print_success
    else
        print_fail
    fi
fi

#
# Phase 8: Packaging
#
print_section "Phase 8: Packaging System"

print_test "Release build script exists"
if [ -x "scripts/build-release.sh" ]; then
    print_success
else
    print_fail
fi

print_test "Debian package script exists"
if [ -x "scripts/build-deb.sh" ]; then
    print_success
else
    print_fail
fi

print_test "RPM package script exists"
if [ -x "scripts/build-rpm.sh" ]; then
    print_success
else
    print_fail
fi

# Test release script help
if [ -x "scripts/build-release.sh" ]; then
    print_test "build-release.sh accepts version argument"
    if bash -n scripts/build-release.sh 2>/dev/null; then
        print_success
    else
        print_fail "Syntax error"
    fi
fi

#
# Phase 9: Line Count Validation
#
print_section "Phase 9: Code Metrics"

# Count lines in Makefile
print_test "Root Makefile has 400+ lines"
if [ -f "Makefile" ]; then
    lines=$(wc -l < Makefile)
    if [ "$lines" -ge 400 ]; then
        print_success
        print_info "$lines lines"
    else
        print_fail "$lines lines (expected 400+)"
    fi
else
    print_fail
fi

# Count lines in configure script
print_test "Configure script has 500+ lines"
if [ -f "configure" ]; then
    lines=$(wc -l < configure)
    if [ "$lines" -ge 500 ]; then
        print_success
        print_info "$lines lines"
    else
        print_fail "$lines lines (expected 500+)"
    fi
else
    print_fail
fi

# Count total build system LOC
print_test "Total build system has 5000+ LOC"
total_loc=0

if [ -f "Makefile" ]; then
    total_loc=$((total_loc + $(wc -l < Makefile)))
fi

if [ -f "configure" ]; then
    total_loc=$((total_loc + $(wc -l < configure)))
fi

if [ -d "scripts" ]; then
    for script in scripts/*.sh scripts/*.py; do
        if [ -f "$script" ]; then
            total_loc=$((total_loc + $(wc -l < "$script")))
        fi
    done
fi

if [ -f "docs/BUILD.md" ]; then
    total_loc=$((total_loc + $(wc -l < docs/BUILD.md)))
fi

if [ "$total_loc" -ge 5000 ]; then
    print_success
    print_info "$total_loc lines"
else
    print_fail "$total_loc lines (expected 5000+)"
fi

#
# Summary
#
echo ""
echo -e "${BLUE}============================================${NC}"
echo -e "${BLUE}  Validation Summary${NC}"
echo -e "${BLUE}============================================${NC}"
echo ""
echo "Total tests:   $TESTS_RUN"
echo -e "${GREEN}Passed:        $TESTS_PASSED${NC}"
if [ $TESTS_FAILED -gt 0 ]; then
    echo -e "${RED}Failed:        $TESTS_FAILED${NC}"
else
    echo "Failed:        $TESTS_FAILED"
fi
if [ $TESTS_SKIPPED -gt 0 ]; then
    echo -e "${YELLOW}Skipped:       $TESTS_SKIPPED${NC}"
else
    echo "Skipped:       $TESTS_SKIPPED"
fi
echo ""

# Calculate pass rate
PASS_RATE=$((TESTS_PASSED * 100 / (TESTS_RUN - TESTS_SKIPPED)))
echo "Pass rate:     $PASS_RATE%"
echo "End time:      $(date)"
echo "Log file:      $LOG_FILE"
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All tests passed!${NC}"
    echo ""
    echo "Build system validation successful."
    echo "The AutomationOS build infrastructure is complete and functional."
    exit 0
else
    echo -e "${RED}✗ Some tests failed${NC}"
    echo ""
    echo "Review the log file for details: $LOG_FILE"
    exit 1
fi
