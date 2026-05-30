#!/bin/bash
#
# AutomationOS Toolchain Setup Script
#
# Sets up the cross-compiler toolchain required to build AutomationOS.
# Can either verify existing toolchain or build from source.
#
# Usage:
#   ./scripts/setup-toolchain.sh [OPTIONS]
#
# Options:
#   --check           Only check for toolchain, don't install
#   --build           Build toolchain from source
#   --prefix=DIR      Installation prefix (default: /opt/cross)
#   --ci              CI mode (non-interactive, minimal output)
#   --help            Show this help
#

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
CHECK_ONLY=0
BUILD_FROM_SOURCE=0
PREFIX="/opt/cross"
CI_MODE=0
TARGET="x86_64-elf"

# Versions for building from source
BINUTILS_VERSION="2.41"
GCC_VERSION="13.2.0"

print_banner() {
    if [ $CI_MODE -eq 0 ]; then
        echo -e "${BLUE}=========================================${NC}"
        echo -e "${BLUE}  AutomationOS Toolchain Setup${NC}"
        echo -e "${BLUE}=========================================${NC}"
        echo ""
    fi
}

print_section() {
    if [ $CI_MODE -eq 0 ]; then
        echo ""
        echo -e "${GREEN}>>> $1${NC}"
    fi
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_help() {
    cat << EOF
AutomationOS Toolchain Setup

Usage: $0 [OPTIONS]

Options:
  --check           Only check for toolchain, don't install
  --build           Build toolchain from source
  --prefix=DIR      Installation prefix (default: /opt/cross)
  --ci              CI mode (non-interactive, minimal output)
  --help            Show this help

Examples:
  $0                           # Check for existing toolchain
  $0 --check                   # Just verify toolchain exists
  $0 --build                   # Build from source (default prefix)
  $0 --build --prefix=~/cross  # Build from source (custom prefix)
  $0 --ci                      # CI mode

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --check)
            CHECK_ONLY=1
            shift
            ;;
        --build)
            BUILD_FROM_SOURCE=1
            shift
            ;;
        --prefix=*)
            PREFIX="${1#*=}"
            shift
            ;;
        --ci)
            CI_MODE=1
            shift
            ;;
        --help)
            print_help
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            echo "Use --help for usage"
            exit 1
            ;;
    esac
done

print_banner

# Check for required base tools
print_section "Checking Required Tools"

MISSING_TOOLS=()

for tool in make nasm python3 git; do
    if command -v $tool >/dev/null 2>&1; then
        print_success "$tool found"
    else
        print_error "$tool not found"
        MISSING_TOOLS+=("$tool")
    fi
done

# Optional but recommended tools
for tool in qemu-system-x86_64 xorriso; do
    if command -v $tool >/dev/null 2>&1; then
        print_success "$tool found (optional)"
    else
        print_warning "$tool not found (optional, needed for testing/ISO creation)"
    fi
done

if [ ${#MISSING_TOOLS[@]} -ne 0 ]; then
    echo ""
    print_error "Missing required tools:"
    for tool in "${MISSING_TOOLS[@]}"; do
        echo "  - $tool"
    done
    echo ""
    echo "Install with:"
    echo "  Ubuntu/Debian: sudo apt install build-essential nasm python3 git"
    echo "  Arch: sudo pacman -S base-devel nasm python git"
    echo "  macOS: brew install nasm python git"
    exit 1
fi

# Check for cross-compiler
print_section "Checking Cross-Compiler Toolchain"

# Add prefix to PATH if specified
if [ -n "$PREFIX" ] && [ -d "$PREFIX/bin" ]; then
    export PATH="$PREFIX/bin:$PATH"
fi

CC_FOUND=0
LD_FOUND=0

if command -v ${TARGET}-gcc >/dev/null 2>&1; then
    print_success "${TARGET}-gcc found: $(command -v ${TARGET}-gcc)"
    echo "         Version: $(${TARGET}-gcc --version | head -n1)"
    CC_FOUND=1
else
    print_warning "${TARGET}-gcc not found"
fi

if command -v ${TARGET}-ld >/dev/null 2>&1; then
    print_success "${TARGET}-ld found: $(command -v ${TARGET}-ld)"
    echo "         Version: $(${TARGET}-ld --version | head -n1)"
    LD_FOUND=1
else
    print_warning "${TARGET}-ld not found"
fi

# If just checking, exit here
if [ $CHECK_ONLY -eq 1 ]; then
    if [ $CC_FOUND -eq 1 ] && [ $LD_FOUND -eq 1 ]; then
        print_success "Toolchain verification passed"
        exit 0
    else
        print_error "Toolchain verification failed"
        exit 1
    fi
fi

# If toolchain not found and not building from source, show instructions
if [ $CC_FOUND -eq 0 ] || [ $LD_FOUND -eq 0 ]; then
    if [ $BUILD_FROM_SOURCE -eq 0 ]; then
        echo ""
        print_error "Cross-compiler toolchain not found"
        echo ""
        echo "AutomationOS requires a cross-compiler for x86_64 bare-metal development."
        echo ""
        echo "Option 1: Install from package manager (recommended)"
        echo ""
        echo "  Ubuntu/Debian:"
        echo "    sudo apt install gcc-x86-64-elf binutils-x86-64-elf"
        echo ""
        echo "  Arch Linux:"
        echo "    sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils"
        echo ""
        echo "  macOS:"
        echo "    brew tap messense/macos-cross-toolchains"
        echo "    brew install x86_64-elf-gcc x86_64-elf-binutils"
        echo ""
        echo "Option 2: Build from source"
        echo ""
        echo "    $0 --build [--prefix=/custom/path]"
        echo ""
        echo "Option 3: Manual build (see docs/TOOLCHAIN.md)"
        echo ""
        exit 1
    else
        # Build from source
        print_section "Building Toolchain from Source"

        # Check build dependencies
        BUILD_DEPS=()
        for dep in gcc g++ make bison flex texinfo; do
            if ! command -v $dep >/dev/null 2>&1; then
                BUILD_DEPS+=("$dep")
            fi
        done

        if [ ${#BUILD_DEPS[@]} -ne 0 ]; then
            print_error "Missing build dependencies:"
            for dep in "${BUILD_DEPS[@]}"; do
                echo "  - $dep"
            done
            echo ""
            echo "Install with:"
            echo "  Ubuntu/Debian: sudo apt install build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev"
            echo "  Arch: sudo pacman -S base-devel gmp mpfr libmpc"
            echo "  macOS: brew install gmp mpfr libmpc"
            exit 1
        fi

        # Create build directory
        BUILD_DIR=$(mktemp -d)
        trap "rm -rf $BUILD_DIR" EXIT

        echo "Build directory: $BUILD_DIR"
        echo "Install prefix: $PREFIX"
        echo ""

        cd "$BUILD_DIR"

        # Download and build binutils
        print_section "Building binutils ${BINUTILS_VERSION}"
        echo "Downloading..."
        wget -q --show-progress "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz"
        tar -xf "binutils-${BINUTILS_VERSION}.tar.xz"

        mkdir binutils-build
        cd binutils-build

        echo "Configuring..."
        ../binutils-${BINUTILS_VERSION}/configure \
            --target=$TARGET \
            --prefix=$PREFIX \
            --with-sysroot \
            --disable-nls \
            --disable-werror

        echo "Building..."
        make -j$(nproc)

        echo "Installing..."
        if [ -w "$PREFIX" ]; then
            make install
        else
            sudo make install
        fi

        print_success "binutils installed"

        cd "$BUILD_DIR"

        # Download and build GCC
        print_section "Building GCC ${GCC_VERSION}"
        echo "Downloading..."
        wget -q --show-progress "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz"
        tar -xf "gcc-${GCC_VERSION}.tar.xz"

        mkdir gcc-build
        cd gcc-build

        export PATH="$PREFIX/bin:$PATH"

        echo "Configuring..."
        ../gcc-${GCC_VERSION}/configure \
            --target=$TARGET \
            --prefix=$PREFIX \
            --disable-nls \
            --enable-languages=c,c++ \
            --without-headers

        echo "Building..."
        make -j$(nproc) all-gcc
        make -j$(nproc) all-target-libgcc

        echo "Installing..."
        if [ -w "$PREFIX" ]; then
            make install-gcc
            make install-target-libgcc
        else
            sudo make install-gcc
            sudo make install-target-libgcc
        fi

        print_success "GCC installed"

        # Verify installation
        print_section "Verifying Installation"

        export PATH="$PREFIX/bin:$PATH"

        ${TARGET}-gcc --version | head -n1
        ${TARGET}-ld --version | head -n1

        print_success "Toolchain built successfully"

        echo ""
        echo "Add to PATH:"
        echo "  export PATH=\"$PREFIX/bin:\$PATH\""
        echo ""
        echo "Or add to ~/.bashrc:"
        echo "  echo 'export PATH=\"$PREFIX/bin:\$PATH\"' >> ~/.bashrc"
        echo ""
    fi
fi

# Create build directories
print_section "Creating Build Directories"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$ROOT_DIR"

mkdir -p build
mkdir -p iso/EFI/BOOT
mkdir -p iso/boot

print_success "Build directories created"

# Summary
echo ""
echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}  Toolchain Setup Complete${NC}"
echo -e "${GREEN}=========================================${NC}"
echo ""
echo "Toolchain:"
echo "  Compiler: ${TARGET}-gcc"
echo "  Linker:   ${TARGET}-ld"
echo "  Target:   $TARGET"
if [ -n "$PREFIX" ] && [ "$PREFIX" != "/usr" ]; then
    echo "  Location: $PREFIX/bin"
fi
echo ""
echo "Next steps:"
echo "  ./configure    # Configure build"
echo "  make all       # Build everything"
echo "  make qemu      # Test in QEMU"
echo ""

print_success "Setup complete!"
