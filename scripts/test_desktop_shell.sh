#!/bin/bash
#
# Desktop Shell Test Script
#
# Tests the desktop shell integration with compositor and window manager
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$KERNEL_DIR/build"

echo "=========================================="
echo "  Desktop Shell Integration Test"
echo "=========================================="
echo

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Step 1: Build desktop shell
info "Building desktop shell..."
cd "$KERNEL_DIR/userspace/shell/desktop"
make clean
make

if [ ! -f "$BUILD_DIR/shell/desktop/desktop_shell" ]; then
    error "Desktop shell binary not found!"
    exit 1
fi

info "Desktop shell built successfully"
echo

# Step 2: Check for running compositor
info "Checking for compositor..."
if [ ! -S "/run/compositor.sock" ]; then
    warn "Compositor socket not found at /run/compositor.sock"
    warn "Desktop shell will fail to create windows"
    echo
    echo "To start compositor:"
    echo "  sudo systemctl start compositor"
    echo
fi

# Step 3: Check for running window manager
info "Checking for window manager..."
if [ ! -S "/run/wm.sock" ]; then
    warn "Window manager socket not found at /run/wm.sock"
    warn "Desktop shell will fail to connect"
    echo
    echo "To start window manager:"
    echo "  sudo systemctl start window-manager"
    echo
fi

# Step 4: Run desktop shell in test mode
echo "=========================================="
echo "  Running Desktop Shell"
echo "=========================================="
echo
echo "Expected behavior:"
echo "  1. Desktop shell initializes"
echo "  2. Creates panel window (1920x32)"
echo "  3. Creates dock window (bottom)"
echo "  4. Creates desktop window (fullscreen)"
echo "  5. Panel displays with clock"
echo "  6. Dock shows app icons"
echo "  7. Click dock icon to launch app"
echo
echo "Press Ctrl+C to exit"
echo

# Set environment variables
export DISPLAY=:0
export WM_SOCKET=/run/wm.sock

# Run desktop shell
"$BUILD_DIR/shell/desktop/desktop_shell" --dark

echo
info "Desktop shell exited"
