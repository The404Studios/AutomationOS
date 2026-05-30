#!/bin/bash
#
# run_leak_tests.sh - Build and run memory leak detection tests
#
# Usage:
#   ./scripts/run_leak_tests.sh [--rebuild]
#
# Options:
#   --rebuild    Clean rebuild with MEM_DEBUG enabled
#

set -e

KERNEL_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$KERNEL_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=== AutomationOS Memory Leak Detection ===${NC}"
echo ""

# Check if rebuild requested
if [ "$1" = "--rebuild" ] || [ ! -f kernel.bin ]; then
    echo -e "${YELLOW}Building kernel with MEM_DEBUG enabled...${NC}"
    make clean
    make EXTRA_CFLAGS=-DMEM_DEBUG
    if [ $? -ne 0 ]; then
        echo -e "${RED}Build failed!${NC}"
        exit 1
    fi
    echo -e "${GREEN}Build successful${NC}"
    echo ""
fi

# Check if MEM_DEBUG is enabled in the binary
if ! grep -q "kmalloc_stats_print" kernel.bin 2>/dev/null; then
    echo -e "${RED}ERROR: kernel.bin not built with MEM_DEBUG${NC}"
    echo "Rebuild with: make clean && make EXTRA_CFLAGS=-DMEM_DEBUG"
    exit 1
fi

echo -e "${YELLOW}Starting QEMU with leak detection tests...${NC}"
echo "Tests will run automatically on boot"
echo "Press Ctrl+C to exit QEMU"
echo ""

# Create temporary expect script to run tests
EXPECT_SCRIPT=$(mktemp)
cat > "$EXPECT_SCRIPT" << 'EOF'
#!/usr/bin/expect -f
set timeout 300

spawn qemu-system-x86_64 \
    -cdrom kernel.iso \
    -serial stdio \
    -m 512M \
    -no-reboot \
    -no-shutdown

# Wait for kernel to boot
expect "Kernel initialized" {
    send_user "\n=== Kernel booted, running leak tests ===\n"
}

# Run leak detection tests
send "leak_test\r"

# Wait for test completion
expect {
    "ALL LEAK TESTS PASSED" {
        send_user "\n=== TESTS PASSED ===\n"
        exit 0
    }
    "SOME TESTS FAILED" {
        send_user "\n=== TESTS FAILED ===\n"
        exit 1
    }
    timeout {
        send_user "\n=== TIMEOUT ===\n"
        exit 2
    }
}
EOF

chmod +x "$EXPECT_SCRIPT"

# Run QEMU with expect (if available)
if command -v expect &> /dev/null; then
    expect "$EXPECT_SCRIPT"
    RESULT=$?
    rm -f "$EXPECT_SCRIPT"

    if [ $RESULT -eq 0 ]; then
        echo -e "${GREEN}All leak tests passed!${NC}"
        exit 0
    elif [ $RESULT -eq 1 ]; then
        echo -e "${RED}Leak tests failed!${NC}"
        exit 1
    else
        echo -e "${YELLOW}Tests timed out or did not complete${NC}"
        exit 2
    fi
else
    # Fallback: run QEMU without expect
    echo -e "${YELLOW}expect not found, running QEMU directly${NC}"
    echo "You'll need to manually check for 'ALL LEAK TESTS PASSED' in output"
    qemu-system-x86_64 \
        -cdrom kernel.iso \
        -serial stdio \
        -m 512M \
        -no-reboot \
        -no-shutdown
fi
