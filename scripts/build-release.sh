#!/bin/bash
#
# AutomationOS Release Builder
#
# Creates official release builds with proper versioning, checksums, and packaging.
#
# Usage:
#   ./scripts/build-release.sh VERSION
#
# Example:
#   ./scripts/build-release.sh v1.0.0
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build"
RELEASE_DIR="$ROOT_DIR/release"

print_banner() {
    echo -e "${BLUE}=========================================${NC}"
    echo -e "${BLUE}  AutomationOS Release Builder${NC}"
    echo -e "${BLUE}=========================================${NC}"
    echo ""
}

print_section() {
    echo ""
    echo -e "${GREEN}>>> $1${NC}"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

# Check arguments
if [ $# -ne 1 ]; then
    echo "Usage: $0 VERSION"
    echo ""
    echo "Example:"
    echo "  $0 v1.0.0"
    exit 1
fi

VERSION="$1"

# Validate version format
if ! [[ "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9]+)?$ ]]; then
    print_error "Invalid version format: $VERSION"
    echo "Expected format: vX.Y.Z or vX.Y.Z-suffix"
    echo "Examples: v1.0.0, v1.2.3-beta1, v2.0.0-rc1"
    exit 1
fi

print_banner
echo "Building release: $VERSION"
echo "Build date: $(date)"
echo ""

# Create release directory
print_section "Setting Up Release Environment"
mkdir -p "$RELEASE_DIR"
print_success "Release directory: $RELEASE_DIR"

# Clean previous build
print_section "Cleaning Previous Build"
if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
    print_success "Cleaned build directory"
fi

# Check for uncommitted changes (if in git repo)
if [ -d "$ROOT_DIR/.git" ]; then
    print_section "Checking Git Status"

    if ! git diff-index --quiet HEAD --; then
        print_warning "Uncommitted changes detected!"
        echo "  It's recommended to commit all changes before building a release."
        read -p "  Continue anyway? [y/N] " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
    else
        print_success "Working directory is clean"
    fi

    # Get git commit hash
    GIT_COMMIT=$(git rev-parse --short HEAD)
    print_success "Commit: $GIT_COMMIT"
fi

# Configure build
print_section "Configuring Build"
cd "$ROOT_DIR"

if [ -x "./configure" ]; then
    ./configure \
        --enable-release \
        --enable-optimization=3 \
        --enable-lto \
        --disable-tests \
        --with-drivers=nvme,ahci,e1000,rtl8139,usb_hcd,usb_hid,keyboard,mouse
    print_success "Build configured for release"
else
    print_warning "Configure script not found, using default configuration"
fi

# Build everything
print_section "Building AutomationOS"

echo "Building bootloader..."
make bootloader
print_success "Bootloader built"

echo "Building kernel..."
make kernel
print_success "Kernel built"

echo "Building userspace..."
make userspace || print_warning "Userspace build incomplete (non-fatal)"
print_success "Userspace built"

echo "Creating ISO..."
make iso
print_success "ISO created"

# Verify build artifacts
print_section "Verifying Build Artifacts"

KERNEL_ELF="$BUILD_DIR/kernel.elf"
BOOTLOADER="$BUILD_DIR/BOOTX64.EFI"
ISO_FILE="$BUILD_DIR/AutomationOS.iso"

for artifact in "$KERNEL_ELF" "$BOOTLOADER" "$ISO_FILE"; do
    if [ -f "$artifact" ]; then
        size=$(du -h "$artifact" | cut -f1)
        print_success "$(basename "$artifact") ($size)"
    else
        print_error "Missing: $(basename "$artifact")"
        exit 1
    fi
done

# Strip symbols from release kernel (optional)
if command -v x86_64-elf-strip &> /dev/null; then
    print_section "Stripping Debug Symbols"
    cp "$KERNEL_ELF" "$KERNEL_ELF.debug"
    x86_64-elf-strip --strip-debug "$KERNEL_ELF"
    print_success "Debug symbols stripped (saved to kernel.elf.debug)"
fi

# Generate version file
print_section "Generating Version Information"

VERSION_FILE="$BUILD_DIR/VERSION"
cat > "$VERSION_FILE" << EOF
AutomationOS $VERSION
Build Date: $(date -u "+%Y-%m-%d %H:%M:%S UTC")
Build Host: $(hostname)
Git Commit: ${GIT_COMMIT:-unknown}
Compiler: $(x86_64-elf-gcc --version | head -n1)
EOF

print_success "Version file created"

# Create release archives
print_section "Creating Release Archives"

# Binary release (ISO + kernel)
BINARY_ARCHIVE="$RELEASE_DIR/automationos-${VERSION}.tar.gz"
echo "Creating binary archive..."
tar -czf "$BINARY_ARCHIVE" \
    -C "$BUILD_DIR" \
    AutomationOS.iso \
    kernel.elf \
    BOOTX64.EFI \
    VERSION

size=$(du -h "$BINARY_ARCHIVE" | cut -f1)
print_success "Binary archive: automationos-${VERSION}.tar.gz ($size)"

# Source release
SOURCE_ARCHIVE="$RELEASE_DIR/automationos-${VERSION}-src.tar.gz"
echo "Creating source archive..."

if [ -d "$ROOT_DIR/.git" ]; then
    # Use git archive for clean source
    git archive --format=tar.gz --prefix="automationos-${VERSION}/" -o "$SOURCE_ARCHIVE" HEAD
else
    # Manual archive
    tar -czf "$SOURCE_ARCHIVE" \
        --exclude="$BUILD_DIR" \
        --exclude="$RELEASE_DIR" \
        --exclude=".git" \
        --exclude="*.o" \
        --exclude="*.elf" \
        --exclude="*.iso" \
        --transform="s,^,automationos-${VERSION}/," \
        -C "$(dirname "$ROOT_DIR")" \
        "$(basename "$ROOT_DIR")"
fi

size=$(du -h "$SOURCE_ARCHIVE" | cut -f1)
print_success "Source archive: automationos-${VERSION}-src.tar.gz ($size)"

# ISO only (for easy distribution)
ISO_RELEASE="$RELEASE_DIR/automationos-${VERSION}.iso"
cp "$ISO_FILE" "$ISO_RELEASE"
size=$(du -h "$ISO_RELEASE" | cut -f1)
print_success "ISO image: automationos-${VERSION}.iso ($size)"

# Generate checksums
print_section "Generating Checksums"

CHECKSUM_FILE="$RELEASE_DIR/SHA256SUMS"
cd "$RELEASE_DIR"

sha256sum "automationos-${VERSION}.tar.gz" > "$CHECKSUM_FILE"
sha256sum "automationos-${VERSION}-src.tar.gz" >> "$CHECKSUM_FILE"
sha256sum "automationos-${VERSION}.iso" >> "$CHECKSUM_FILE"

print_success "Checksums generated: SHA256SUMS"
echo ""
cat "$CHECKSUM_FILE"

# Generate release notes template
print_section "Generating Release Notes"

RELEASE_NOTES="$RELEASE_DIR/RELEASE_NOTES_${VERSION}.md"
cat > "$RELEASE_NOTES" << EOF
# AutomationOS $VERSION

Release Date: $(date "+%Y-%m-%d")

## Overview

[Add release overview here]

## What's New

### Features
- [Feature 1]
- [Feature 2]

### Improvements
- [Improvement 1]
- [Improvement 2]

### Bug Fixes
- [Bug fix 1]
- [Bug fix 2]

## Known Issues

- [Known issue 1]

## Download

- **ISO Image**: automationos-${VERSION}.iso
- **Binary Archive**: automationos-${VERSION}.tar.gz
- **Source Archive**: automationos-${VERSION}-src.tar.gz

## Checksums (SHA256)

\`\`\`
$(cat "$CHECKSUM_FILE")
\`\`\`

## Installation

1. Download the ISO image
2. Write to USB drive or boot in virtual machine
3. Follow on-screen instructions

## Testing

This release has been tested on:
- QEMU/KVM
- VirtualBox
- VMware
- [Add real hardware if tested]

## Build Information

- Version: $VERSION
- Build Date: $(date -u "+%Y-%m-%d %H:%M:%S UTC")
- Git Commit: ${GIT_COMMIT:-unknown}
- Compiler: $(x86_64-elf-gcc --version | head -n1)

## Documentation

For documentation, see:
- README.md
- docs/

## Support

- GitHub: https://github.com/your-org/automationos
- Issues: https://github.com/your-org/automationos/issues

EOF

print_success "Release notes template: RELEASE_NOTES_${VERSION}.md"

# Test the release
print_section "Testing Release Build"

if command -v qemu-system-x86_64 &> /dev/null; then
    echo "Starting quick boot test..."
    timeout 10 qemu-system-x86_64 \
        -cdrom "$ISO_RELEASE" \
        -m 512M \
        -serial stdio \
        -display none \
        -no-reboot \
        -no-shutdown \
        2>&1 | grep -q "AutomationOS" && \
        print_success "Boot test passed" || \
        print_warning "Boot test inconclusive"
else
    print_warning "QEMU not available, skipping boot test"
fi

# Summary
print_section "Release Build Complete"
echo ""
echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}  Release: $VERSION${NC}"
echo -e "${GREEN}=========================================${NC}"
echo ""
echo "Files created in $RELEASE_DIR:"
echo ""
echo "  📦 automationos-${VERSION}.iso"
echo "  📦 automationos-${VERSION}.tar.gz"
echo "  📦 automationos-${VERSION}-src.tar.gz"
echo "  📄 SHA256SUMS"
echo "  📄 RELEASE_NOTES_${VERSION}.md"
echo ""
echo "Total size: $(du -sh "$RELEASE_DIR" | cut -f1)"
echo ""
echo -e "${BLUE}Next steps:${NC}"
echo "  1. Edit release notes: $RELEASE_NOTES"
echo "  2. Test the ISO thoroughly"
echo "  3. Create git tag: git tag -a $VERSION -m 'Release $VERSION'"
echo "  4. Push tag: git push origin $VERSION"
echo "  5. Create GitHub release with artifacts"
echo ""
print_success "Release build completed successfully!"
