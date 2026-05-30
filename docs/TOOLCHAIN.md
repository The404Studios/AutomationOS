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
# Cross-compiler toolchain
sudo apt install gcc-x86-64-elf binutils-x86-64-elf

# Additional build tools
sudo apt install build-essential nasm python3 qemu-system-x86 xorriso
```

#### Linux (Arch)
```bash
# Cross-compiler toolchain
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils

# Additional build tools
sudo pacman -S base-devel nasm python qemu xorriso
```

#### macOS
```bash
# Cross-compiler toolchain
brew install x86_64-elf-gcc x86_64-elf-binutils

# Additional build tools
brew install nasm python qemu xorriso
```

#### Windows (WSL2)
```bash
# Use Ubuntu instructions in WSL2
sudo apt install gcc-x86-64-elf binutils-x86-64-elf
sudo apt install build-essential nasm python3 qemu-system-x86 xorriso
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

## Troubleshooting

### "x86_64-elf-gcc: command not found"

This means the cross-compiler toolchain is not installed or not in your PATH.

**Solution:**
1. Install the cross-compiler using the instructions above
2. Verify installation: `x86_64-elf-gcc --version`
3. If installed but not found, add to PATH:
   ```bash
   export PATH="/usr/local/cross/bin:$PATH"
   ```

### Package not found in Ubuntu/Debian

If `gcc-x86-64-elf` is not available in your package repository:
1. Try `apt search x86.*elf.*gcc` to find the correct package name
2. Build from source using the instructions below
3. Use a PPA or third-party repository (not recommended)

### Building from Source - Best for OS Development

The most reliable method for OS development is building the toolchain from source. This ensures you have a proper freestanding compiler configured for bare-metal development.

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
