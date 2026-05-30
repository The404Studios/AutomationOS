#!/bin/bash
#
# Verify Code Quality Setup
# ==========================
#
# This script verifies that all code quality components are installed correctly.

set -e

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo ""
echo "=========================================="
echo "  Code Quality Setup Verification"
echo "=========================================="
echo ""

FAILED=0

# Check configuration files
echo -e "${BLUE}Checking configuration files...${NC}"

if [ -f ".clang-format" ]; then
    echo -e "${GREEN}  ✓ .clang-format${NC}"
else
    echo -e "${RED}  ✗ .clang-format missing${NC}"
    FAILED=1
fi

if [ -f ".clang-tidy" ]; then
    echo -e "${GREEN}  ✓ .clang-tidy${NC}"
else
    echo -e "${RED}  ✗ .clang-tidy missing${NC}"
    FAILED=1
fi

# Check scripts
echo ""
echo -e "${BLUE}Checking scripts...${NC}"

if [ -f "scripts/pre-commit" ] && [ -x "scripts/pre-commit" ]; then
    echo -e "${GREEN}  ✓ scripts/pre-commit (executable)${NC}"
else
    echo -e "${RED}  ✗ scripts/pre-commit missing or not executable${NC}"
    FAILED=1
fi

if [ -f "scripts/install-hooks.sh" ] && [ -x "scripts/install-hooks.sh" ]; then
    echo -e "${GREEN}  ✓ scripts/install-hooks.sh (executable)${NC}"
else
    echo -e "${RED}  ✗ scripts/install-hooks.sh missing or not executable${NC}"
    FAILED=1
fi

# Check documentation
echo ""
echo -e "${BLUE}Checking documentation...${NC}"

DOCS=(
    "docs/CODING_STANDARDS.md"
    "docs/CODE_REVIEW_CHECKLIST.md"
    "docs/CODE_QUALITY_IMPLEMENTATION.md"
    "docs/QUICK_START_CODE_QUALITY.md"
)

for doc in "${DOCS[@]}"; do
    if [ -f "$doc" ]; then
        SIZE=$(stat -c%s "$doc" 2>/dev/null || stat -f%z "$doc" 2>/dev/null)
        SIZE_KB=$((SIZE / 1024))
        echo -e "${GREEN}  ✓ $doc (${SIZE_KB} KB)${NC}"
    else
        echo -e "${RED}  ✗ $doc missing${NC}"
        FAILED=1
    fi
done

# Check GitHub Actions
echo ""
echo -e "${BLUE}Checking GitHub Actions workflow...${NC}"

if [ -f ".github/workflows/code-quality.yml" ]; then
    echo -e "${GREEN}  ✓ .github/workflows/code-quality.yml${NC}"
else
    echo -e "${RED}  ✗ .github/workflows/code-quality.yml missing${NC}"
    FAILED=1
fi

# Check Makefile targets
echo ""
echo -e "${BLUE}Checking Makefile targets...${NC}"

TARGETS=("format" "check-format" "lint" "install-hooks")

for target in "${TARGETS[@]}"; do
    if grep -q "^${target}:" Makefile; then
        echo -e "${GREEN}  ✓ make ${target}${NC}"
    else
        echo -e "${RED}  ✗ make ${target} not found${NC}"
        FAILED=1
    fi
done

# Check for required tools
echo ""
echo -e "${BLUE}Checking required tools...${NC}"

if command -v clang-format &> /dev/null; then
    VERSION=$(clang-format --version | grep -oP '\d+\.\d+' | head -1)
    echo -e "${GREEN}  ✓ clang-format ${VERSION}${NC}"
else
    echo -e "${YELLOW}  ⚠ clang-format not found (install: apt install clang-format)${NC}"
fi

if command -v clang-tidy &> /dev/null; then
    VERSION=$(clang-tidy --version | grep -oP '\d+\.\d+' | head -1)
    echo -e "${GREEN}  ✓ clang-tidy ${VERSION}${NC}"
else
    echo -e "${YELLOW}  ⚠ clang-tidy not found (install: apt install clang-tidy)${NC}"
fi

if command -v git &> /dev/null; then
    VERSION=$(git --version | grep -oP '\d+\.\d+\.\d+')
    echo -e "${GREEN}  ✓ git ${VERSION}${NC}"
else
    echo -e "${RED}  ✗ git not found${NC}"
    FAILED=1
fi

# Check git hooks installation
echo ""
echo -e "${BLUE}Checking git hooks installation...${NC}"

if [ -f ".git/hooks/pre-commit" ]; then
    echo -e "${GREEN}  ✓ Pre-commit hook installed${NC}"
else
    echo -e "${YELLOW}  ⚠ Pre-commit hook not installed (run: make install-hooks)${NC}"
fi

# Summary
echo ""
echo "=========================================="
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All checks passed!${NC}"
    echo ""
    echo "Code quality system is ready to use."
    echo ""
    echo "Next steps:"
    echo "  1. Install hooks: make install-hooks"
    echo "  2. Format code: make format"
    echo "  3. Run linter: make lint"
else
    echo -e "${RED}✗ Some checks failed${NC}"
    echo ""
    echo "Fix the issues above and run again."
    exit 1
fi
echo "=========================================="
echo ""

exit 0
