#!/bin/bash
# AutomationOS IDE - Build Script

set -e

echo "=========================================="
echo "   AutomationOS IDE Build Script"
echo "=========================================="
echo ""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Configuration
CC="${CC:-gcc}"
CFLAGS="-Wall -Wextra -std=c11 -I."
LDFLAGS=""
TARGET="autoos-ide"

SOURCES=(
    "ide_main.c"
    "editor.c"
    "blueprint.c"
    "debugger.c"
    "project.c"
)

OBJECTS=()

# Check for compiler
if ! command -v "$CC" &> /dev/null; then
    echo -e "${RED}[ERROR]${NC} Compiler '$CC' not found!"
    exit 1
fi

echo -e "${CYAN}Compiler:${NC} $CC"
echo -e "${CYAN}Flags:${NC} $CFLAGS"
echo ""

# Clean previous build
if [ -f "$TARGET" ] || ls *.o 1> /dev/null 2>&1; then
    echo -e "${YELLOW}[CLEAN]${NC} Removing previous build artifacts..."
    rm -f "$TARGET" *.o
fi

# Compile each source file
echo -e "${CYAN}[BUILD]${NC} Compiling sources..."
for src in "${SOURCES[@]}"; do
    obj="${src%.c}.o"
    echo -e "  ${GREEN}→${NC} Compiling $src..."

    if ! $CC $CFLAGS -c "$src" -o "$obj"; then
        echo -e "${RED}[ERROR]${NC} Failed to compile $src"
        exit 1
    fi

    OBJECTS+=("$obj")
done

echo ""

# Link
echo -e "${CYAN}[LINK]${NC} Linking $TARGET..."
if ! $CC "${OBJECTS[@]}" -o "$TARGET" $LDFLAGS; then
    echo -e "${RED}[ERROR]${NC} Failed to link $TARGET"
    exit 1
fi

echo ""
echo -e "${GREEN}[SUCCESS]${NC} Build completed successfully!"
echo ""
echo -e "${CYAN}Output:${NC} $TARGET"
echo -e "${CYAN}Size:${NC} $(du -h "$TARGET" | cut -f1)"
echo ""

# Show help
echo "=========================================="
echo "Usage:"
echo "  ./$TARGET              - Start IDE"
echo "  ./$TARGET <project>    - Open project"
echo ""
echo "To install:"
echo "  make install           - Install to ../bin/"
echo "=========================================="
