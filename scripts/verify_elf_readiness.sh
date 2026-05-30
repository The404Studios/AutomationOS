#!/bin/bash
#
# ELF Loader Readiness Verification Script
# =========================================
#
# Checks if all components needed for ELF loading are present and ready.
#

set -e

KERNEL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$KERNEL_DIR"

echo "========================================"
echo "  ELF Loader Readiness Check"
echo "========================================"
echo ""

PASS=0
FAIL=0
WARN=0

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

check_file() {
    local file="$1"
    local desc="$2"

    if [ -f "$file" ]; then
        echo -e "${GREEN}✓${NC} $desc"
        echo "  → $file"
        ((PASS+=1))
        return 0
    else
        echo -e "${RED}✗${NC} $desc"
        echo "  → Missing: $file"
        ((FAIL+=1))
        return 0
    fi
}

check_function() {
    local file="$1"
    local func="$2"
    local desc="$3"

    if [ -f "$file" ] && grep -q "$func" "$file"; then
        echo -e "${GREEN}✓${NC} $desc"
        echo "  → Found in $file"
        ((PASS+=1))
        return 0
    else
        echo -e "${RED}✗${NC} $desc"
        echo "  → Not found in $file"
        ((FAIL+=1))
        return 0
    fi
}

warn_if_missing() {
    local file="$1"
    local desc="$2"

    if [ -f "$file" ]; then
        echo -e "${GREEN}✓${NC} $desc"
        echo "  → $file"
        ((PASS+=1))
        return 0
    else
        echo -e "${YELLOW}⚠${NC} $desc"
        echo "  → Missing: $file (optional)"
        ((WARN+=1))
        return 0
    fi
}

echo "1. Core ELF Loader Components"
echo "------------------------------"
check_file "kernel/fs/elf_loader.c" "ELF loader implementation"
check_file "kernel/include/elf.h" "ELF header definitions"
check_function "kernel/fs/elf_loader.c" "elf_validate_header" "ELF header validation"
check_function "kernel/fs/elf_loader.c" "elf_load_segment" "ELF segment loading"
check_function "kernel/fs/elf_loader.c" "elf_load" "Main ELF load function"
echo ""

echo "2. Usermode Transition Components"
echo "----------------------------------"
check_file "kernel/arch/x86_64/usermode.asm" "Assembly usermode transition"
check_file "kernel/core/usermode.c" "C usermode wrapper"
check_file "kernel/include/usermode.h" "Usermode API header"
check_function "kernel/arch/x86_64/usermode.asm" "enter_usermode" "IRETQ transition code"
check_function "kernel/core/usermode.c" "start_usermode" "Usermode setup function"
echo ""

echo "3. GDT and TSS Components"
echo "-------------------------"
check_file "kernel/arch/x86_64/gdt.c" "GDT implementation"
check_file "kernel/include/tss.h" "TSS header"
check_function "kernel/arch/x86_64/gdt.c" "gdt_init" "GDT initialization"
check_function "kernel/arch/x86_64/gdt.c" "tss_init" "TSS initialization"
check_function "kernel/arch/x86_64/gdt.c" "tss_set_kernel_stack" "TSS stack setter"
echo ""

echo "4. Memory Management Components"
echo "--------------------------------"
check_file "kernel/core/mem/heap.c" "Kernel heap"
check_file "kernel/core/mem/vmm.c" "Virtual memory manager"
check_function "kernel/core/mem/heap.c" "heap_init" "Heap initialization"
check_function "kernel/core/mem/heap.c" "kmalloc" "Heap allocation"
check_function "kernel/core/mem/vmm.c" "vmm_init" "VMM initialization"
check_function "kernel/core/mem/vmm.c" "vmm_map_page" "Page mapping"
echo ""

echo "5. Test Suite"
echo "-------------"
check_file "kernel/fs/elf_loader_test.c" "Test suite implementation"
check_file "kernel/include/elf_loader_test.h" "Test suite header"
check_function "kernel/fs/elf_loader_test.c" "elf_loader_test_suite" "Test runner"
echo ""

echo "6. Userspace Test Programs"
echo "---------------------------"
check_file "userspace/test_minimal.c" "Minimal test program"
check_file "userspace/test_program.ld" "Userspace linker script"
warn_if_missing "build/userspace/tests/test_minimal" "Compiled test binary"
echo ""

echo "7. Documentation"
echo "----------------"
check_file "ELF_USERMODE_IMPLEMENTATION_GUIDE.md" "Implementation guide"
echo ""

echo "========================================"
echo "Summary"
echo "========================================"
echo -e "${GREEN}Passed:${NC} $PASS"
echo -e "${YELLOW}Warnings:${NC} $WARN"
echo -e "${RED}Failed:${NC} $FAIL"
echo ""

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}✓ All critical components present!${NC}"
    echo ""
echo "Next Steps:"
echo "1. Build userspace test program:"
echo "   cd userspace && make tests"
echo "   (output: build/userspace/tests/test_minimal)"
    echo ""
    echo "2. Add test_minimal to initrd"
    echo ""
    echo "3. In kernel init, add:"
    echo "   #include <elf_loader_test.h>"
    echo "   elf_loader_test_suite(0);  // Run all safe tests"
    echo ""
    echo "4. Once tests pass, run:"
    echo "   elf_loader_test_suite(6);  // Enter ring 3"
    echo ""
    exit 0
else
    echo -e "${RED}✗ Missing critical components!${NC}"
    echo ""
    echo "Please review the implementation guide:"
    echo "  ELF_USERMODE_IMPLEMENTATION_GUIDE.md"
    echo ""
    exit 1
fi
