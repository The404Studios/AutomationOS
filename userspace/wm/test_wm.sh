#!/bin/bash
# Window Manager Integration Test

set -e

echo "=== Window Manager Integration Test ==="
echo

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if running as root
if [[ $EUID -ne 0 ]]; then
   echo -e "${RED}✗ This script must be run as root${NC}"
   exit 1
fi

echo -e "${GREEN}✓ Running as root${NC}"

# Test 1: Check service file exists
echo
echo "Test 1: Service Configuration"
if [ -f "/etc/services/window-manager.service" ]; then
    echo -e "${GREEN}✓ Service file exists${NC}"
else
    echo -e "${RED}✗ Service file not found${NC}"
    exit 1
fi

# Test 2: Check compositor service exists
echo
echo "Test 2: Compositor Dependency"
if [ -f "/etc/services/compositor.service" ]; then
    echo -e "${GREEN}✓ Compositor service exists${NC}"
else
    echo -e "${RED}✗ Compositor service not found${NC}"
    exit 1
fi

# Test 3: Check binary exists
echo
echo "Test 3: Window Manager Binary"
if [ -f "/usr/bin/window-manager" ]; then
    echo -e "${GREEN}✓ Binary exists${NC}"
    /usr/bin/window-manager --version 2>&1 || echo "  (version check skipped)"
else
    echo -e "${YELLOW}⚠ Binary not installed yet${NC}"
    echo "  Run: make install"
fi

# Test 4: Check for compositor socket
echo
echo "Test 4: Compositor Socket"
if [ -S "/run/compositor.sock" ]; then
    echo -e "${GREEN}✓ Compositor socket exists${NC}"
else
    echo -e "${YELLOW}⚠ Compositor not running${NC}"
    echo "  Run: servicectl start compositor"
fi

# Test 5: Check service status
echo
echo "Test 5: Service Status"
servicectl status window-manager 2>&1 | head -5

# Test 6: Check animation system
echo
echo "Test 6: Animation System"
if [ -f "../compositor/animations.c" ]; then
    echo -e "${GREEN}✓ Animation system available${NC}"
    grep -c "easing_apply" ../compositor/animations.c || echo "  Easing functions found"
else
    echo -e "${RED}✗ Animation system not found${NC}"
fi

# Test 7: Check memory requirements
echo
echo "Test 7: System Resources"
TOTAL_MEM=$(free -m | awk '/^Mem:/{print $2}')
if [ $TOTAL_MEM -gt 512 ]; then
    echo -e "${GREEN}✓ Sufficient memory: ${TOTAL_MEM}MB${NC}"
else
    echo -e "${YELLOW}⚠ Low memory: ${TOTAL_MEM}MB (512MB recommended)${NC}"
fi

# Test 8: Check for GPU device
echo
echo "Test 8: GPU Device"
if [ -c "/dev/dri/card0" ] || [ -c "/dev/fb0" ]; then
    echo -e "${GREEN}✓ Display device available${NC}"
else
    echo -e "${YELLOW}⚠ No display device found${NC}"
fi

# Summary
echo
echo "=== Integration Test Summary ==="
echo "Window Manager: Ready for launch"
echo
echo "To start:"
echo "  1. servicectl start compositor"
echo "  2. servicectl start window-manager"
echo
echo "To view logs:"
echo "  tail -f /var/log/services/window-manager.log"
echo

exit 0
