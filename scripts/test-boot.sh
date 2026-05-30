#!/bin/bash
#
# AutomationOS Boot Test Runner
#
# Builds the entire system and runs boot tests.
#
# Usage:
#   ./scripts/test-boot.sh              # Full build and test
#   ./scripts/test-boot.sh --skip-build # Test only (don't rebuild)

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Configuration
SKIP_BUILD=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        *)
            echo -e "${RED}ERROR: Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

echo -e "${GREEN}==========================================${NC}"
echo -e "${GREEN}  AutomationOS Boot Test Runner${NC}"
echo -e "${GREEN}==========================================${NC}"
echo ""

# Build system
if [ "$SKIP_BUILD" -eq 0 ]; then
    echo -e "${YELLOW}Step 1: Building system...${NC}"
    echo ""

    # Clean previous build
    echo "Cleaning previous build..."
    make clean || true

    # Build all components
    echo "Building bootloader..."
    make bootloader

    echo "Building kernel..."
    make kernel

    echo "Building userspace..."
    make userspace || true  # May not be implemented yet

    echo "Building ISO..."
    make iso

    echo ""
    echo -e "${GREEN}✓ Build complete${NC}"
    echo ""
else
    echo -e "${YELLOW}Skipping build (using existing artifacts)${NC}"
    echo ""
fi

# Run boot test
echo -e "${YELLOW}Step 2: Running boot test...${NC}"
echo ""

python3 tests/integration/test_boot.py --verbose

# Check exit code
if [ $? -eq 0 ]; then
    echo ""
    echo -e "${GREEN}==========================================${NC}"
    echo -e "${GREEN}  ✓ ALL TESTS PASSED${NC}"
    echo -e "${GREEN}==========================================${NC}"
    exit 0
else
    echo ""
    echo -e "${RED}==========================================${NC}"
    echo -e "${RED}  ✗ TESTS FAILED${NC}"
    echo -e "${RED}==========================================${NC}"
    exit 1
fi
