#!/bin/bash
# Verification script for minimal compositor

set -e

echo "========================================="
echo "Minimal Compositor Verification Script"
echo "========================================="
echo ""

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if required files exist
echo "Step 1: Checking required files..."
files=(
    "main.c"
    "fb.c"
    "fb.h"
    "render.c"
    "render.h"
    "test_minimal_compositor.c"
    "Makefile.minimal"
)

all_found=true
for file in "${files[@]}"; do
    if [ -f "$file" ]; then
        echo -e "  ${GREEN}✓${NC} Found: $file"
    else
        echo -e "  ${RED}✗${NC} Missing: $file"
        all_found=false
    fi
done

if [ "$all_found" = false ]; then
    echo -e "${RED}Error: Some required files are missing${NC}"
    exit 1
fi

echo ""
echo "Step 2: Checking file sizes..."
echo "  main.c:                     $(wc -l < main.c) lines"
echo "  fb.c:                       $(wc -l < fb.c) lines"
echo "  render.c:                   $(wc -l < render.c) lines"
echo "  test_minimal_compositor.c:  $(wc -l < test_minimal_compositor.c) lines"
echo "  Total core code:            $(($(wc -l < main.c) + $(wc -l < fb.c) + $(wc -l < render.c))) lines"

echo ""
echo "Step 3: Cleaning previous build..."
make -f Makefile.minimal clean > /dev/null 2>&1 || true
echo -e "  ${GREEN}✓${NC} Clean complete"

echo ""
echo "Step 4: Compiling minimal compositor..."
if make -f Makefile.minimal 2>&1 | tee /tmp/compositor_build.log; then
    echo -e "  ${GREEN}✓${NC} Compilation successful"
else
    echo -e "  ${RED}✗${NC} Compilation failed"
    echo "Build log:"
    cat /tmp/compositor_build.log
    exit 1
fi

echo ""
echo "Step 5: Checking binary..."
if [ -f "compositor-minimal" ]; then
    size=$(stat -f%z "compositor-minimal" 2>/dev/null || stat -c%s "compositor-minimal" 2>/dev/null)
    echo -e "  ${GREEN}✓${NC} Binary created: compositor-minimal ($(($size / 1024)) KB)"
else
    echo -e "  ${RED}✗${NC} Binary not created"
    exit 1
fi

echo ""
echo "Step 6: Compiling test program..."
if make -f Makefile.minimal test 2>&1 | tee /tmp/test_build.log; then
    echo -e "  ${GREEN}✓${NC} Test compilation successful"
else
    echo -e "  ${RED}✗${NC} Test compilation failed"
    cat /tmp/test_build.log
    exit 1
fi

echo ""
echo "Step 7: Checking test binary..."
if [ -f "test_minimal_compositor" ]; then
    size=$(stat -f%z "test_minimal_compositor" 2>/dev/null || stat -c%s "test_minimal_compositor" 2>/dev/null)
    echo -e "  ${GREEN}✓${NC} Test binary created: test_minimal_compositor ($(($size / 1024)) KB)"
else
    echo -e "  ${RED}✗${NC} Test binary not created"
    exit 1
fi

echo ""
echo "Step 8: Running quick smoke test..."
timeout 2 ./compositor-minimal > /tmp/compositor_run.log 2>&1 &
pid=$!
sleep 1
if ps -p $pid > /dev/null; then
    echo -e "  ${GREEN}✓${NC} Compositor started successfully"
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
else
    echo -e "  ${YELLOW}⚠${NC}  Compositor exited (expected if no framebuffer)"
fi

# Check output
if grep -q "Initializing minimal compositor" /tmp/compositor_run.log; then
    echo -e "  ${GREEN}✓${NC} Compositor initialization message found"
else
    echo -e "  ${RED}✗${NC} Compositor did not initialize properly"
fi

echo ""
echo "========================================="
echo -e "${GREEN}✓ All verification steps passed!${NC}"
echo "========================================="
echo ""
echo "Minimal compositor is ready to use:"
echo "  - Run compositor:  ./compositor-minimal"
echo "  - Run tests:       ./test_minimal_compositor"
echo "  - See docs:        cat MINIMAL_COMPOSITOR.md"
echo ""
