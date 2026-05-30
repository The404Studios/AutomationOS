#!/bin/bash
#
# Verification script for font library deliverables
#

set -e

FONT_DIR="$(dirname "$0")"
cd "$FONT_DIR"

echo "Font Rendering Library - Deliverables Verification"
echo "===================================================="
echo ""

# Check files exist
echo "Checking deliverables..."

FILES=(
    "font.h:Public API header"
    "font_internal.h:Internal API header"
    "ttf_parser.c:TrueType parser implementation"
    "rasterizer.c:Glyph rasterization engine"
    "cache.c:LRU cache implementation"
    "test_font.c:Test program"
    "Makefile:Build system"
    "README.md:Library documentation"
    "INTEGRATION.md:Integration guide"
    "DELIVERABLES.md:Deliverables summary"
)

ALL_OK=true

for item in "${FILES[@]}"; do
    file="${item%%:*}"
    desc="${item#*:}"

    if [ -f "$file" ]; then
        SIZE=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file" 2>/dev/null)
        printf "  ✓ %-25s %s (%'d bytes)\n" "$file" "$desc" "$SIZE"
    else
        printf "  ✗ %-25s %s (MISSING)\n" "$file" "$desc"
        ALL_OK=false
    fi
done

echo ""

# Check for stb_truetype.h
echo "Checking dependencies..."

if [ -f "stb_truetype.h" ]; then
    SIZE=$(stat -f%z "stb_truetype.h" 2>/dev/null || stat -c%s "stb_truetype.h" 2>/dev/null)
    echo "  ✓ stb_truetype.h ($SIZE bytes)"
else
    echo "  ⚠ stb_truetype.h not found (run 'make download-stb')"
fi

if [ -d "fonts" ] && [ -f "fonts/DejaVuSans.ttf" ]; then
    SIZE=$(stat -f%z "fonts/DejaVuSans.ttf" 2>/dev/null || stat -c%s "fonts/DejaVuSans.ttf" 2>/dev/null)
    echo "  ✓ DejaVuSans.ttf ($SIZE bytes)"
else
    echo "  ⚠ DejaVuSans.ttf not found (run './DOWNLOAD_FONT.sh')"
fi

echo ""

# Count lines of code
echo "Code statistics..."

C_FILES=(ttf_parser.c rasterizer.c cache.c test_font.c)
H_FILES=(font.h font_internal.h)

C_LINES=0
for file in "${C_FILES[@]}"; do
    if [ -f "$file" ]; then
        LINES=$(wc -l < "$file")
        C_LINES=$((C_LINES + LINES))
        printf "  %-25s %5d lines\n" "$file" "$LINES"
    fi
done

echo "  -------------------------"
printf "  %-25s %5d lines\n" "Total C code" "$C_LINES"

echo ""

H_LINES=0
for file in "${H_FILES[@]}"; do
    if [ -f "$file" ]; then
        LINES=$(wc -l < "$file")
        H_LINES=$((H_LINES + LINES))
        printf "  %-25s %5d lines\n" "$file" "$LINES"
    fi
done

echo "  -------------------------"
printf "  %-25s %5d lines\n" "Total headers" "$H_LINES"

echo ""
printf "  %-25s %5d lines\n" "TOTAL CODE" "$((C_LINES + H_LINES))"

echo ""

# Test compilation (if stb_truetype.h exists)
if [ -f "stb_truetype.h" ]; then
    echo "Testing compilation..."

    if make clean > /dev/null 2>&1 && make > /dev/null 2>&1; then
        echo "  ✓ Library compiles successfully"

        if [ -f "libfont.a" ]; then
            SIZE=$(stat -f%z "libfont.a" 2>/dev/null || stat -c%s "libfont.a" 2>/dev/null)
            echo "  ✓ libfont.a created ($SIZE bytes)"
        fi

        if [ -f "test_font" ]; then
            SIZE=$(stat -f%z "test_font" 2>/dev/null || stat -c%s "test_font" 2>/dev/null)
            echo "  ✓ test_font created ($SIZE bytes)"
        fi
    else
        echo "  ✗ Compilation failed"
        ALL_OK=false
    fi
else
    echo "Skipping compilation test (stb_truetype.h not found)"
fi

echo ""
echo "===================================================="

if $ALL_OK; then
    echo "✓ All deliverables present and verified"
    exit 0
else
    echo "✗ Some deliverables missing or failed"
    exit 1
fi
