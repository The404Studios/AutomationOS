# AutomationOS Build Documentation

**Version:** 0.1.0  
**Last Updated:** 2026-05-26  
**Target:** Desktop Release

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Prerequisites](#prerequisites)
3. [Toolchain Setup](#toolchain-setup)
4. [Build Process](#build-process)
5. [Testing](#testing)
6. [Troubleshooting](#troubleshooting)
7. [Advanced Configuration](#advanced-configuration)

---

## Quick Start

For users who just want to build and run AutomationOS:

```bash
# 1. Install prerequisites (Ubuntu/Debian)
sudo apt install build-essential nasm python3 git xorriso mtools qemu-system-x86

# 2. Clone repository (if not already)
cd AutomationOS/

# 3. Setup cross-compiler toolchain
./scripts/setup-toolchain.sh

# 4. Configure build
./configure

# 5. Build everything
make all

# 6. Run in QEMU
make qemu

# 7. Boot to desktop!
```

**Expected Output:** QEMU window opens, AutomationOS boots to desktop in ~1-2 seconds.

---

## Prerequisites

### Required Tools

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| **make** | 3.81+ | Build automation |
| **gcc** or **clang** | 9.0+ | Host compiler (for scripts) |
| **x86_64-elf-gcc** | 9.0+ | Cross-compiler for kernel |
| **x86_64-elf-ld** | 2.30+ | Linker for kernel |
| **nasm** | 2.14+ | x86_64 assembler |
| **python3** | 3.7+ | Build scripts and testing |
| **xorriso** | 1.4+ | ISO image creation |
| **mtools** | 4.0+ | FAT filesystem tools |
| **git** | 2.20+ | Version control |

### Optional Tools

| Tool | Purpose |
|------|---------|
| **qemu-system-x86_64** | Testing and emulation (recommended) |
| **clang-format** | Code formatting |
| **clang-tidy** | Static analysis |
| **cppcheck** | Bug detection |
| **lcov** | Code coverage reports |
| **gdb** | Kernel debugging |

---

## Platform-Specific Installation

### Ubuntu / Debian

```bash
# Essential build tools
sudo apt update
sudo apt install build-essential nasm python3 git xorriso mtools

# Cross-compiler (try package first, fallback to script)
sudo apt install gcc-x86-64-elf binutils-x86-64-elf

# If the above fails, use the setup script
./scripts/setup-toolchain.sh

# Optional: QEMU for testing
sudo apt install qemu-system-x86

# Optional: Development tools
sudo apt install clang-format clang-tidy cppcheck lcov gdb
```

### Arch Linux

```bash
# Essential build tools
sudo pacman -S base-devel nasm python git libisoburn mtools

# Cross-compiler (available in AUR or official repos)
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils

# Optional: QEMU for testing
sudo pacman -S qemu

# Optional: Development tools
sudo pacman -S clang cppcheck lcov gdb
```

### Fedora / RHEL / CentOS

```bash
# Essential build tools
sudo dnf groupinstall "Development Tools"
sudo dnf install nasm python3 git xorriso mtools

# Cross-compiler (build from source using script)
./scripts/setup-toolchain.sh

# Optional: QEMU for testing
sudo dnf install qemu-system-x86

# Optional: Development tools
sudo dnf install clang-tools-extra cppcheck lcov gdb
```

### macOS

```bash
# Install Homebrew if not already installed
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install required packages
brew install nasm python3 git xorriso mtools qemu

# Install cross-compiler
brew install x86_64-elf-gcc x86_64-elf-binutils

# Optional: Development tools
brew install llvm cppcheck lcov
```

### Windows (WSL2)

**Recommended:** Use WSL2 (Windows Subsystem for Linux) with Ubuntu.

```powershell
# Enable WSL2 (PowerShell as Administrator)
wsl --install

# Launch Ubuntu WSL
wsl

# Then follow Ubuntu instructions above
```

**Note:** Native Windows build is not supported. Use WSL2 or a Linux VM.

---

## Toolchain Setup

### Automated Setup (Recommended)

The `setup-toolchain.sh` script automatically downloads, builds, and installs the cross-compiler toolchain.

```bash
# Run setup script
./scripts/setup-toolchain.sh

# Script will:
# 1. Check for existing toolchain
# 2. Download binutils and gcc sources
# 3. Build x86_64-elf-gcc cross-compiler
# 4. Install to ~/opt/cross/bin/
# 5. Add to PATH

# Verify installation
x86_64-elf-gcc --version
x86_64-elf-ld --version
```

**Installation Location:** `~/opt/cross/`  
**Time Required:** 20-40 minutes (one-time)

### Manual Toolchain Build

If the script fails, build manually:

```bash
# Set environment variables
export PREFIX="$HOME/opt/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

# Download sources
cd /tmp
wget https://ftp.gnu.org/gnu/binutils/binutils-2.39.tar.gz
wget https://ftp.gnu.org/gnu/gcc/gcc-12.2.0/gcc-12.2.0.tar.gz

# Extract
tar -xzf binutils-2.39.tar.gz
tar -xzf gcc-12.2.0.tar.gz

# Build binutils
mkdir build-binutils
cd build-binutils
../binutils-2.39/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
make -j$(nproc)
make install

# Build gcc
cd /tmp
mkdir build-gcc
cd build-gcc
../gcc-12.2.0/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c,c++ --without-headers
make all-gcc -j$(nproc)
make all-target-libgcc -j$(nproc)
make install-gcc
make install-target-libgcc

# Add to PATH permanently
echo "export PATH=\"$PREFIX/bin:\$PATH\"" >> ~/.bashrc
source ~/.bashrc
```

### Verify Toolchain

```bash
# Check cross-compiler
x86_64-elf-gcc --version
# Expected: gcc (GCC) 12.2.0 or similar

# Check linker
x86_64-elf-ld --version
# Expected: GNU ld (GNU Binutils) 2.39 or similar

# Check assembler
nasm --version
# Expected: NASM version 2.14 or later
```

---

## Build Process

### Configuration

```bash
# Configure build system (first time only)
./configure

# Options:
#   --prefix=/path/to/install   Installation prefix (default: /usr/local)
#   --debug                     Enable debug symbols
#   --release                   Enable optimizations
#   --enable-tests              Build test suite
#   --enable-coverage           Enable code coverage

# Example: Debug build with tests
./configure --debug --enable-tests
```

The configure script creates `build/config.mk` with build settings.

---

### Build Targets

#### Build Everything (Default)

```bash
make all

# Builds:
# - Bootloader (boot/BOOTX64.EFI)
# - Kernel (kernel/kernel.elf)
# - Userspace programs
# - InitRD (initial ramdisk)
# - Bootable ISO (build/automationos.iso)
```

#### Build Individual Components

```bash
# Build bootloader only
make bootloader

# Build kernel only
make kernel

# Build userspace only
make userspace

# Build initrd only
make initrd

# Create ISO image
make iso
```

#### Clean Builds

```bash
# Clean build artifacts (keep configuration)
make clean

# Clean everything (including configuration)
make distclean

# Clean specific component
make clean-kernel
make clean-userspace
make clean-bootloader
```

---

### Build Output

After a successful build:

```
build/
├── automationos.iso          # Bootable ISO image (31 MB)
├── kernel.elf                # Kernel ELF binary
├── kernel.map                # Kernel symbol map
├── initrd.img                # Initial RAM disk
├── boot/
│   └── BOOTX64.EFI          # UEFI bootloader
├── userspace/
│   ├── init                  # Init process
│   ├── shell                 # Shell
│   └── bin/                  # Utilities
└── *.o                       # Object files
```

**ISO Size:** ~31 MB  
**Kernel Size:** ~2-3 MB (with debug symbols ~15 MB)

---

## Testing

### Run in QEMU (Recommended)

```bash
# Run with default settings
make qemu

# QEMU options:
# - 2 GB RAM
# - 4 CPU cores
# - Serial console redirected to stdio
# - UEFI firmware (OVMF)
# - Hardware acceleration (KVM if available)
```

**QEMU Window:**
- Boots to desktop in ~1-2 seconds
- Use mouse and keyboard normally
- Serial output in terminal

**Keyboard:**
- `Ctrl+Alt+G` - Release mouse grab
- `Ctrl+C` (in terminal) - Shutdown QEMU

---

### Run with Custom QEMU Options

```bash
# More memory
make qemu QEMU_MEMORY=4096

# More CPUs
make qemu QEMU_CPUS=8

# Enable KVM (Linux only, much faster)
make qemu QEMU_KVM=1

# Disable graphics (headless, serial only)
make qemu QEMU_NOGRAPHIC=1

# Debug mode (wait for GDB)
make qemu-debug
```

---

### Run Tests

```bash
# Run all tests
make test

# Run integration tests
make test-integration

# Run unit tests
make test-unit

# Run with verbose output
make test VERBOSE=1

# Generate coverage report (requires --enable-coverage)
make coverage
# Opens build/coverage/index.html in browser
```

---

### Boot on Real Hardware (USB)

**Warning:** Experimental. Only for advanced users.

```bash
# 1. Build ISO
make iso

# 2. Write to USB drive (CAREFUL: WILL ERASE USB!)
sudo dd if=build/automationos.iso of=/dev/sdX bs=4M status=progress
sudo sync

# Replace /dev/sdX with your USB device (e.g., /dev/sdb)
# Use 'lsblk' to identify USB device

# 3. Boot from USB
# - Insert USB into target machine
# - Enter BIOS/UEFI (usually F2, F10, F12, or Del)
# - Set boot order to USB first
# - Save and reboot
```

**Supported Hardware:**
- UEFI firmware required (no legacy BIOS)
- x86_64 CPU (Intel/AMD)
- 512 MB+ RAM
- SATA or NVMe storage (for install)

**Known Issues:**
- Some UEFI implementations may not boot
- Limited hardware driver support
- No install to disk yet (live USB only)

---

## Troubleshooting

### Build Errors

#### "x86_64-elf-gcc: command not found"

**Solution:**
```bash
# Toolchain not installed or not in PATH
./scripts/setup-toolchain.sh

# Verify installation
which x86_64-elf-gcc
```

#### "nasm: command not found"

**Solution:**
```bash
# Install NASM assembler
sudo apt install nasm       # Ubuntu/Debian
sudo pacman -S nasm         # Arch Linux
sudo dnf install nasm       # Fedora
brew install nasm           # macOS
```

#### "xorriso: command not found"

**Solution:**
```bash
# Install xorriso for ISO creation
sudo apt install xorriso    # Ubuntu/Debian
sudo pacman -S libisoburn   # Arch Linux
sudo dnf install xorriso    # Fedora
brew install xorriso        # macOS
```

#### "Makefile:XX: *** missing separator. Stop."

**Solution:**
```bash
# Makefile tabs corrupted (common with copy/paste)
# Re-clone repository or re-download Makefiles

# Ensure tabs are preserved
git config --global core.autocrlf input
```

#### Linker errors: "undefined reference to..."

**Solution:**
```bash
# Missing object files or incorrect link order
# Clean and rebuild
make clean
make all
```

---

### Runtime Errors (QEMU)

#### QEMU won't start: "Could not access KVM kernel module"

**Solution:**
```bash
# KVM not available (non-Linux or VM)
# Disable KVM acceleration
make qemu QEMU_KVM=0
```

#### QEMU hangs at "Loading kernel..."

**Solution:**
```bash
# Kernel or bootloader issue
# Enable verbose boot output
make qemu QEMU_DEBUG=1

# Check serial console for errors
```

#### Kernel panic on boot

**Solution:**
```bash
# Check serial output for panic message
# Common causes:
# - Memory allocation failure (increase QEMU_MEMORY)
# - Driver initialization failure
# - Corrupted initrd

# Rebuild and try again
make clean && make all && make qemu
```

#### Black screen (no output)

**Solution:**
```bash
# Framebuffer issue
# Try serial console instead
make qemu QEMU_NOGRAPHIC=1

# Check if bootloader initialized GOP
```

---

### Performance Issues

#### QEMU is very slow

**Solution:**
```bash
# Enable KVM acceleration (Linux only)
make qemu QEMU_KVM=1

# Ensure KVM kernel module is loaded
lsmod | grep kvm

# Add user to kvm group (may require re-login)
sudo usermod -aG kvm $USER
```

#### Boot takes longer than expected

**Expected Boot Time:** 1-2 seconds in QEMU with KVM, 5-10 seconds without.

**Common Causes:**
- KVM disabled (slow software emulation)
- Slow disk I/O (use SSD for build directory)
- Debug symbols enabled (larger binary = slower load)

**Optimization:**
```bash
# Build optimized release version
./configure --release
make clean && make all
make qemu QEMU_KVM=1
```

---

## Advanced Configuration

### Build System Variables

Override in `build/config.mk` or via environment:

```makefile
# Compiler flags
CFLAGS += -O2 -g                        # Optimization and debug
CFLAGS += -Wall -Wextra -Werror         # Warnings
CFLAGS += -fstack-protector-strong      # Stack protection
CFLAGS += -mno-red-zone                 # Kernel requirement

# Linker flags
LDFLAGS += -nostdlib -static            # Freestanding binary
LDFLAGS += -z max-page-size=0x1000      # 4KB page alignment

# Assembler flags
NASMFLAGS += -f elf64                   # 64-bit ELF output
NASMFLAGS += -g -F dwarf                # Debug symbols

# Cross-compiler prefix
CROSS_COMPILE ?= x86_64-elf-

# Build directories
BUILD_DIR ?= build
ISO_DIR ?= iso

# QEMU configuration
QEMU ?= qemu-system-x86_64
QEMU_MEMORY ?= 2048
QEMU_CPUS ?= 4
QEMU_KVM ?= 1
```

---

### Custom Build Profiles

Create custom profiles in `profiles/`:

```bash
# profiles/debug.mk
CFLAGS += -Og -g3 -DDEBUG
CFLAGS += -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined

# profiles/release.mk
CFLAGS += -O3 -DNDEBUG
CFLAGS += -flto
LDFLAGS += -flto

# profiles/size.mk
CFLAGS += -Os -DNDEBUG
LDFLAGS += -Wl,--gc-sections

# Use profile
./configure --profile=debug
make all
```

---

### Cross-Compilation for Different Targets

AutomationOS currently supports x86_64 only. Future architectures:

```bash
# ARM64 (future)
./configure --target=aarch64-elf

# RISC-V (future)
./configure --target=riscv64-elf
```

---

### Incremental Builds

```bash
# Rebuild only changed files
make

# Force rebuild of specific component
touch kernel/kernel.c
make kernel

# Parallel builds (faster)
make -j$(nproc)

# Verbose build (see commands)
make VERBOSE=1
```

---

### Dependency Tracking

```bash
# Automatic dependency generation (enabled by default)
# .d files generated alongside .o files

# Rebuild everything if header changes
touch kernel/include/types.h
make
# All dependent files automatically rebuilt
```

---

## Build System Architecture

```
AutomationOS/
├── Makefile                  # Top-level build orchestration
├── configure                 # Configuration script
├── scripts/
│   ├── setup-toolchain.sh   # Toolchain installation
│   ├── build-release.sh     # Release packaging
│   └── test-runner.py       # Test harness
├── boot/Makefile            # Bootloader build
├── kernel/Makefile          # Kernel build
├── userspace/Makefile       # Userspace build
└── build/
    ├── config.mk            # Generated configuration
    └── ...                  # Build artifacts
```

**Build Flow:**

1. `./configure` generates `build/config.mk`
2. Top-level `Makefile` includes `config.mk`
3. Recursive make invokes component Makefiles
4. Components build to `build/` directory
5. ISO created from build artifacts
6. QEMU launched with ISO

---

## Continuous Integration

### GitHub Actions

AutomationOS includes CI/CD workflows:

```yaml
# .github/workflows/build.yml
name: Build and Test

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Setup Toolchain
        run: ./scripts/setup-toolchain.sh
      - name: Build
        run: make all
      - name: Test
        run: make test
      - name: Upload ISO
        uses: actions/upload-artifact@v2
        with:
          name: automationos-iso
          path: build/automationos.iso
```

---

## Additional Resources

### Documentation

- **Architecture Guide:** `docs/ARCHITECTURE.md`
- **API Reference:** `docs/API_REFERENCE.md`
- **Features List:** `docs/FEATURES.md`
- **Development Guide:** `docs/DEVELOPMENT_GUIDE.md`
- **Testing Guide:** `docs/TESTING_FRAMEWORK.md`

### Community

- **Issues:** Report bugs and request features on GitHub
- **Discussions:** Join community discussions
- **Contributing:** See `docs/CONTRIBUTING.md`

---

## Conclusion

You should now be able to build AutomationOS from source. If you encounter any issues not covered here, please consult the full documentation or open an issue.

**Happy Building!**

---

**Document Version:** 1.0  
**Last Updated:** 2026-05-26  
**Maintainer:** AutomationOS Team
