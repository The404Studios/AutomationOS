#!/bin/bash
# AutomationOS Compositor Startup Script

set -e

echo "=== AutomationOS Compositor Service ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root"
    echo "Usage: sudo $0"
    exit 1
fi

# Create required directories
echo "[1/5] Creating runtime directories..."
mkdir -p /run
mkdir -p /var/log/services
mkdir -p /dev/dri

echo "[2/5] Checking GPU device..."
if [ -e /dev/dri/card0 ]; then
    echo "  ✓ GPU found: /dev/dri/card0"
    GPU_INFO=$(lspci | grep -i vga | head -n1 || echo "Unknown GPU")
    echo "    $GPU_INFO"
else
    echo "  ⚠ GPU not found: /dev/dri/card0"
    echo "  Compositor will use software rendering fallback"
fi

# Build compositor daemon
echo "[3/5] Building compositor daemon..."
cd "$(dirname "$0")"
if make compositord 2>&1 | grep -q "Built compositord"; then
    echo "  ✓ Compositor daemon built successfully"
else
    echo "  ℹ Compositor daemon already built or build skipped"
fi

# Install compositor daemon
echo "[4/5] Installing compositor daemon..."
if [ -f compositord ]; then
    cp compositord /usr/bin/compositord
    chmod +x /usr/bin/compositord
    echo "  ✓ Installed to /usr/bin/compositord"
else
    echo "  ✗ ERROR: compositord binary not found"
    exit 1
fi

# Start compositor
echo "[5/5] Starting compositor daemon..."
if [ -f /run/compositor.pid ]; then
    OLD_PID=$(cat /run/compositor.pid)
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "  ⚠ Compositor already running (PID: $OLD_PID)"
        echo "  Stopping old instance..."
        kill "$OLD_PID"
        sleep 1
    fi
    rm -f /run/compositor.pid
fi

# Clean up old socket
rm -f /run/compositor.sock

# Start daemon
/usr/bin/compositord --daemon

# Wait for startup
echo ""
echo "Waiting for compositor to start..."
for i in {1..10}; do
    if [ -f /run/compositor.pid ]; then
        PID=$(cat /run/compositor.pid)
        if kill -0 "$PID" 2>/dev/null; then
            echo ""
            echo "✓ Compositor started successfully!"
            echo "  PID: $PID"
            echo "  Socket: /run/compositor.sock"
            echo ""
            echo "Compositor Status:"
            ps aux | grep compositord | grep -v grep
            echo ""
            echo "IPC Socket:"
            ls -lh /run/compositor.sock 2>/dev/null || echo "  (socket not yet created)"
            echo ""
            echo "To stop: kill $PID"
            echo "To view logs: tail -f /var/log/services/compositor.log"
            exit 0
        fi
    fi
    sleep 0.5
done

echo ""
echo "✗ ERROR: Compositor failed to start"
echo "Check logs for details:"
echo "  tail /var/log/services/compositor.log"
exit 1
