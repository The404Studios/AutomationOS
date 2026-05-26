# AutomationOS Build Toolchain Setup

This document explains how to set up the build toolchain for AutomationOS.

## Required Tools

### 1. Cross-Compiler (x86_64-elf)

A freestanding cross-compiler is required to build the kernel and bootloader.

**Why cross-compiler?**
- Standard gcc targets your host OS (Windows/Linux)
- We need a compiler that targets bare metal (no OS)
- x86_64-elf-gcc is configured for freestanding environments

**Installation:**

#### Linux (Ubuntu/Debian)
```bash
sudo apt-get install build-essential gcc-x86-64-linux-gnu nasm
# Or build from source (recommended for OS development)
```

#### macOS
```bash
brew install x86_64-elf-gcc nasm
```

#### Windows (WSL2 or MSYS2)
```bash
# WSL2 (Ubuntu)
sudo apt-get install build-essential nasm

# MSYS2
pacman -S mingw-w64-x86_64-gcc nasm
```

#### Building from Source (All platforms)
```bash
# Download binutils and gcc
export PREFIX="/usr/local/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

# Build binutils
wget https://ftp.gnu.org/gnu/binutils/binutils-2.40.tar.gz
tar xf binutils-2.40.tar.gz
mkdir build-binutils
cd build-binutils
../binutils-2.40/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
make
make install

# Build gcc
wget https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.gz
tar xf gcc-13.2.0.tar.gz
cd gcc-13.2.0
./contrib/download_prerequisites
cd ..
mkdir build-gcc
cd build-gcc
../gcc-13.2.0/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headers
make all-gcc
make all-target-libgcc
make install-gcc
make install-target-libgcc
```

### 2. NASM (Netwide Assembler)

Required for assembling bootloader and kernel entry points.

```bash
# Ubuntu/Debian
sudo apt-get install nasm

# macOS
brew install nasm

# Windows
# Download from https://www.nasm.us/
```

### 3. QEMU (for testing)

```bash
# Ubuntu/Debian
sudo apt-get install qemu-system-x86

# macOS
brew install qemu

# Windows
# Download from https://www.qemu.org/download/
```

### 4. xorriso (for ISO generation)

```bash
# Ubuntu/Debian
sudo apt-get install xorriso

# macOS
brew install xorriso

# Windows (WSL2)
sudo apt-get install xorriso
```

### 5. Python 3.11+

Required for build scripts.

```bash
# Most systems have Python 3 installed
python3 --version
```

## Verification

After installation, verify all tools are available:

```bash
x86_64-elf-gcc --version
x86_64-elf-ld --version
nasm --version
qemu-system-x86_64 --version
xorriso --version
python3 --version
```

## Current Status (2026-05-26)

On this Windows 11 system with WSL2:
- ❌ x86_64-elf-gcc - **NOT INSTALLED**
- ❌ nasm - **NOT INSTALLED**
- ❓ QEMU - Not verified
- ❓ xorriso - Not verified
- ✅ Python 3 - Installed

**Action Required:** Install cross-compiler toolchain before building.

## Quick Setup Script

See `scripts/setup-toolchain.sh` for an automated setup script (checks requirements, creates build directories).

## Build Without Toolchain

You can still work on:
- Documentation
- Scripts (Python)
- Project structure
- Design and architecture
- Code review and planning

But you cannot build binaries until the toolchain is installed.

## References

- [OSDev Wiki - GCC Cross-Compiler](https://wiki.osdev.org/GCC_Cross-Compiler)
- [OSDev Wiki - NASM](https://wiki.osdev.org/NASM)
- [NASM Documentation](https://www.nasm.us/docs.php)
- [QEMU Documentation](https://www.qemu.org/docs/master/)
