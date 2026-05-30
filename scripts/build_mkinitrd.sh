#!/bin/bash
#
# Build the mkinitrd tool
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KERNEL_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$KERNEL_ROOT/build"

echo "Building mkinitrd tool..."

mkdir -p "$BUILD_DIR"

gcc "$KERNEL_ROOT/tools/mkinitrd.c" -o "$BUILD_DIR/mkinitrd" -Wall -Wextra -O2

if [ -f "$BUILD_DIR/mkinitrd" ]; then
    echo "  [OK] mkinitrd built successfully: $BUILD_DIR/mkinitrd"
    ls -lh "$BUILD_DIR/mkinitrd"
else
    echo "  [ERROR] Failed to build mkinitrd"
    exit 1
fi
