#!/bin/bash
# Font Integration Verification Script
# Checks that all font integration components are in place

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=========================================="
echo "Font Integration Verification"
echo "=========================================="
echo ""

PASS_COUNT=0
FAIL_COUNT=0

check() {
    local name="$1"
    local path="$2"

    if [ -f "$path" ] || [ -d "$path" ]; then
        echo "✓ $name"
        ((PASS_COUNT++))
        return 0
    else
        echo "✗ $name (missing: $path)"
        ((FAIL_COUNT++))
        return 1
    fi
}

# Check font library
echo "=== Font Library ==="
check "Font library source" "$KERNEL_ROOT/userspace/lib/font/ttf_parser.c"
check "Font library header" "$KERNEL_ROOT/userspace/lib/font/font.h"
check "Font cache module" "$KERNEL_ROOT/userspace/lib/font/cache.c"
check "Font rasterizer" "$KERNEL_ROOT/userspace/lib/font/rasterizer.c"
check "Font library Makefile" "$KERNEL_ROOT/userspace/lib/font/Makefile"
echo ""

# Check font library installation
echo "=== Font Library Installation ==="
check "Installed header" "$KERNEL_ROOT/userspace/include/font.h"
check "Installed library" "$KERNEL_ROOT/userspace/lib/libfont.a"
echo ""

# Check fonts
echo "=== Font Files ==="
check "DejaVu Sans" "$KERNEL_ROOT/userspace/lib/font/fonts/DejaVuSans.ttf"
check "DejaVu Sans Mono" "$KERNEL_ROOT/userspace/lib/font/fonts/DejaVuSansMono.ttf"
echo ""

# Check integration modules
echo "=== Compositor Integration ==="
check "Compositor font header" "$KERNEL_ROOT/userspace/compositor/font_integration.h"
check "Compositor font source" "$KERNEL_ROOT/userspace/compositor/font_integration.c"
check "Compositor Makefile (updated)" "$KERNEL_ROOT/userspace/compositor/Makefile.font-integrated"
echo ""

echo "=== Desktop Shell Integration ==="
check "Desktop font header" "$KERNEL_ROOT/userspace/shell/desktop/font_integration.h"
check "Desktop font source" "$KERNEL_ROOT/userspace/shell/desktop/font_integration.c"
echo ""

echo "=== Terminal Integration ==="
check "Terminal font header" "$KERNEL_ROOT/userspace/apps/terminal/font_integration.h"
check "Terminal font source" "$KERNEL_ROOT/userspace/apps/terminal/font_integration.c"
check "Terminal Makefile (updated)" "$KERNEL_ROOT/userspace/apps/terminal/Makefile.font-integrated"
echo ""

echo "=== File Explorer Integration ==="
check "Explorer font header" "$KERNEL_ROOT/userspace/apps/files/font_integration.h"
check "Explorer font source" "$KERNEL_ROOT/userspace/apps/files/font_integration.c"
check "Explorer Makefile (updated)" "$KERNEL_ROOT/userspace/apps/files/Makefile.font-integrated"
echo ""

# Check documentation
echo "=== Documentation ==="
check "Integration deliverables" "$KERNEL_ROOT/FONT_INTEGRATION_DELIVERABLES.md"
check "Quick start guide" "$KERNEL_ROOT/FONT_INTEGRATION_QUICK_START.md"
check "Font library README" "$KERNEL_ROOT/userspace/lib/font/README.md"
check "Integration guide" "$KERNEL_ROOT/userspace/lib/font/INTEGRATION.md"
echo ""

# Check setup scripts
echo "=== Setup Scripts ==="
check "Font setup script" "$KERNEL_ROOT/scripts/setup-fonts.sh"
check "Verification script" "$KERNEL_ROOT/scripts/verify-font-integration.sh"
echo ""

# Summary
echo "=========================================="
echo "Verification Summary"
echo "=========================================="
echo "Passed: $PASS_COUNT"
echo "Failed: $FAIL_COUNT"
echo ""

if [ $FAIL_COUNT -eq 0 ]; then
    echo "✓ All font integration components present!"
    echo ""
    echo "Next steps:"
    echo "  1. Run: bash scripts/setup-fonts.sh"
    echo "  2. Update application Makefiles"
    echo "  3. Build applications with font support"
    echo "  4. Bundle fonts in initrd"
    exit 0
else
    echo "✗ Some components are missing"
    echo ""
    echo "Please ensure all font integration files are in place."
    exit 1
fi
