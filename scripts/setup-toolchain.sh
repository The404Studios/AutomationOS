#!/bin/bash
set -e

echo "Setting up AutomationOS build toolchain..."

# Check for required tools
command -v gcc >/dev/null 2>&1 || { echo "gcc required"; exit 1; }
command -v make >/dev/null 2>&1 || { echo "make required"; exit 1; }
command -v nasm >/dev/null 2>&1 || { echo "nasm required"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 required"; exit 1; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "qemu-system-x86_64 required"; exit 1; }
command -v xorriso >/dev/null 2>&1 || { echo "xorriso required"; exit 1; }

# Create build directories
mkdir -p build
mkdir -p iso/EFI/BOOT
mkdir -p iso/boot

echo "✅ Toolchain setup complete"
echo ""
echo "Next steps:"
echo "  make all       # Build bootloader, kernel, userspace, and ISO"
echo "  make qemu      # Run in QEMU"
