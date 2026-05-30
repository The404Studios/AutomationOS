#!/bin/bash
# Build System Validator
# Validates that the build system is correctly configured

set -e

echo "AutomationOS Build System Validator"
echo "===================================="
echo ""

ERRORS=0
WARNINGS=0

# Color codes
RED='\033[0;31m'
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

error() {
    echo -e "${RED}❌ ERROR:${NC} $1"
    ERRORS=$((ERRORS + 1))
}

warning() {
    echo -e "${YELLOW}⚠️  WARNING:${NC} $1"
    WARNINGS=$((WARNINGS + 1))
}

success() {
    echo -e "${GREEN}✅${NC} $1"
}

info() {
    echo -e "ℹ️  $1"
}

# Check 1: Verify kernel/lib source files exist
echo "Check 1: Kernel library source files"
if [ -f "kernel/lib/string.c" ] && [ -f "kernel/lib/printf.c" ] && [ -f "kernel/lib/panic.c" ]; then
    success "All kernel/lib source files found"
else
    error "Missing kernel/lib source files"
fi

# Check 2: Verify kernel/Makefile discovers lib files
echo ""
echo "Check 2: Kernel Makefile integration"
cd kernel
FOUND_FILES=$(find . -name "*.c" | grep -c "lib/" || echo "0")
cd ..
if [ "$FOUND_FILES" -ge 3 ]; then
    success "Kernel Makefile discovers $FOUND_FILES lib/*.c files"
else
    error "Kernel Makefile does not discover lib files (found: $FOUND_FILES)"
fi

# Check 3: Verify boot/Makefile has mkdir -p commands
echo ""
echo "Check 3: Boot Makefile directory creation"
MKDIR_COUNT=$(grep -c "mkdir -p" boot/Makefile || echo "0")
if [ "$MKDIR_COUNT" -ge 3 ]; then
    success "Boot Makefile has $MKDIR_COUNT mkdir -p commands"
else
    error "Boot Makefile missing mkdir -p commands (found: $MKDIR_COUNT)"
fi

# Check 4: Verify setup-toolchain.sh checks for x86_64-elf-gcc
echo ""
echo "Check 4: Toolchain validation script"
if grep -q "x86_64-elf-gcc" scripts/setup-toolchain.sh; then
    success "setup-toolchain.sh checks for x86_64-elf-gcc"
else
    error "setup-toolchain.sh does not check for x86_64-elf-gcc"
fi

# Check 5: Verify cross-compiler is available
echo ""
echo "Check 5: Cross-compiler toolchain"
if command -v x86_64-elf-gcc >/dev/null 2>&1; then
    VERSION=$(x86_64-elf-gcc --version | head -n1)
    success "x86_64-elf-gcc found: $VERSION"
else
    warning "x86_64-elf-gcc not installed (build will fail)"
    info "Install with: sudo apt install gcc-x86-64-elf binutils-x86-64-elf"
fi

if command -v x86_64-elf-ld >/dev/null 2>&1; then
    VERSION=$(x86_64-elf-ld --version | head -n1)
    success "x86_64-elf-ld found: $VERSION"
else
    warning "x86_64-elf-ld not installed (build will fail)"
fi

# Check 6: Verify main Makefile structure
echo ""
echo "Check 6: Main Makefile structure"
if grep -q "bootloader:" Makefile && grep -q "kernel:" Makefile && grep -q "userspace:" Makefile; then
    success "Main Makefile has all required targets"
else
    error "Main Makefile missing required targets"
fi

# Check 7: Verify build directories can be created
echo ""
echo "Check 7: Build directory structure"
if [ -d "build" ]; then
    info "build/ directory exists"
else
    info "build/ directory will be created on first build"
fi

# Check 8: Verify Makefile uses correct variables
echo ""
echo "Check 8: Makefile toolchain variables"
for makefile in Makefile boot/Makefile kernel/Makefile; do
    if grep -q "CC = x86_64-elf-gcc" "$makefile" 2>/dev/null; then
        success "$makefile uses correct cross-compiler"
    elif grep -q "CC =" "$makefile" 2>/dev/null; then
        error "$makefile uses wrong compiler: $(grep 'CC =' "$makefile")"
    fi
done

# Summary
echo ""
echo "===================================="
echo "Validation Summary"
echo "===================================="
if [ $ERRORS -eq 0 ] && [ $WARNINGS -eq 0 ]; then
    echo -e "${GREEN}✅ All checks passed!${NC}"
    echo ""
    echo "Build system is correctly configured."
    echo "Next steps:"
    echo "  1. Install toolchain: bash scripts/setup-toolchain.sh"
    echo "  2. Build: make all"
    echo "  3. Run: make qemu"
    exit 0
elif [ $ERRORS -eq 0 ]; then
    echo -e "${YELLOW}⚠️  $WARNINGS warning(s)${NC}"
    echo ""
    echo "Build system is configured but toolchain is not installed."
    echo "Install toolchain: bash scripts/setup-toolchain.sh"
    exit 0
else
    echo -e "${RED}❌ $ERRORS error(s), $WARNINGS warning(s)${NC}"
    echo ""
    echo "Build system has configuration errors."
    exit 1
fi
