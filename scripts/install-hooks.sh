#!/bin/bash
#
# Install Git Hooks for AutomationOS
# ===================================
#
# This script installs pre-commit hooks that enforce code quality standards.
#
# Usage: ./scripts/install-hooks.sh

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo ""
echo "=========================================="
echo "  AutomationOS Git Hooks Installation"
echo "=========================================="
echo ""

# Get project root
PROJECT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"

if [ -z "$PROJECT_ROOT" ]; then
    echo -e "${RED}Error: Not in a git repository${NC}"
    exit 1
fi

cd "$PROJECT_ROOT"

# Check if .git directory exists
if [ ! -d ".git" ]; then
    echo -e "${RED}Error: .git directory not found${NC}"
    exit 1
fi

# Create hooks directory if it doesn't exist
mkdir -p ".git/hooks"

# Install pre-commit hook
echo -e "${YELLOW}Installing pre-commit hook...${NC}"

if [ -f ".git/hooks/pre-commit" ]; then
    echo -e "${YELLOW}  Backing up existing pre-commit hook...${NC}"
    mv ".git/hooks/pre-commit" ".git/hooks/pre-commit.backup.$(date +%Y%m%d_%H%M%S)"
fi

cp "scripts/pre-commit" ".git/hooks/pre-commit"
chmod +x ".git/hooks/pre-commit"

echo -e "${GREEN}  ✓ Pre-commit hook installed${NC}"

# Check for required tools
echo ""
echo -e "${YELLOW}Checking for required tools...${NC}"

MISSING_TOOLS=0

if ! command -v clang-format &> /dev/null; then
    echo -e "${RED}  ✗ clang-format not found${NC}"
    echo -e "    Install: ${YELLOW}apt install clang-format${NC} or ${YELLOW}brew install clang-format${NC}"
    MISSING_TOOLS=1
else
    CLANG_FORMAT_VERSION=$(clang-format --version | grep -oP '\d+\.\d+' | head -1)
    echo -e "${GREEN}  ✓ clang-format ${CLANG_FORMAT_VERSION}${NC}"
fi

if ! command -v clang-tidy &> /dev/null; then
    echo -e "${RED}  ✗ clang-tidy not found${NC}"
    echo -e "    Install: ${YELLOW}apt install clang-tidy${NC} or ${YELLOW}brew install llvm${NC}"
    MISSING_TOOLS=1
else
    CLANG_TIDY_VERSION=$(clang-tidy --version | grep -oP '\d+\.\d+' | head -1)
    echo -e "${GREEN}  ✓ clang-tidy ${CLANG_TIDY_VERSION}${NC}"
fi

if ! command -v git &> /dev/null; then
    echo -e "${RED}  ✗ git not found${NC}"
    MISSING_TOOLS=1
else
    GIT_VERSION=$(git --version | grep -oP '\d+\.\d+\.\d+')
    echo -e "${GREEN}  ✓ git ${GIT_VERSION}${NC}"
fi

echo ""

if [ $MISSING_TOOLS -ne 0 ]; then
    echo -e "${YELLOW}⚠ Some tools are missing. Hooks will work but some checks will be skipped.${NC}"
    echo -e "${YELLOW}  Install the missing tools for full functionality.${NC}"
else
    echo -e "${GREEN}✓ All required tools found${NC}"
fi

echo ""
echo "=========================================="
echo -e "${GREEN}Installation complete!${NC}"
echo "=========================================="
echo ""
echo "The pre-commit hook will now:"
echo "  1. Auto-format code with clang-format"
echo "  2. Remove trailing whitespace"
echo "  3. Check indentation (tabs vs spaces)"
echo "  4. Run static analysis with clang-tidy"
echo "  5. Check for debug markers"
echo "  6. Validate commit message format"
echo ""
echo "To bypass checks for a specific commit:"
echo "  git commit --no-verify"
echo ""
echo "To uninstall hooks:"
echo "  rm .git/hooks/pre-commit"
echo ""

exit 0
