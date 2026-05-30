# AutomationOS Build Guide

**Version:** 0.1.0  
**Phase:** 1 - Core Foundation  
**Last Updated:** 2026-05-26

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Toolchain Setup](#toolchain-setup)
3. [Building the System](#building-the-system)
4. [Build System Architecture](#build-system-architecture)
5. [Build Targets](#build-targets)
6. [Customizing the Build](#customizing-the-build)
7. [Troubleshooting](#troubleshooting)
8. [Cross-Platform Notes](#cross-platform-notes)

---

## Prerequisites

### Supported Host Systems

- **Linux:** Ubuntu 20.04+, Debian 11+, Arch Linux, Fedora 35+
- **macOS:** macOS 11+ (Big Sur or later) with Homebrew
- **Windows:** WSL2 (Ubuntu 20.04+)

### Required Tools

| Tool | Version | Purpose |
|------|---------|---------|
| GCC (x86_64-elf) | 11.0+ | Cross-compiler for kernel |
| Binutils (x86_64-elf) | 2.37+ | Linker and assembler |
| NASM | 2.15+ | x86_64 assembly |
| Python | 3.8+ | Build scripts |
| QEMU | 6.0+ | Emulator for testing |
| xorriso | 1.5+ | ISO generation |
| Make | 4.0+ | Build automation |

### Optional Tools

| Tool | Purpose |
|------|---------|
| GDB | Debugging |
| Bochs | Alternative emulator |
| GNU tar | Extracting toolchain |
| curl/wget | Downloading toolchain |

---

## Toolchain Setup

AutomationOS requires a cross-compiler targeting `x86_64-elf`. We provide an automated setup script.

### Automated Setup (Recommended)

```bash
cd AutomationOS
bash scripts/setup-toolchain.sh
```

This script will:
1. Detect your OS
2. Install system dependencies
3. Download and build x86_64-elf-gcc
4. Add toolchain to PATH
5. Verify installation

**Estimated time:** 20-40 minutes (depending on CPU)

### Manual Setup

If the automated script fails, follow the manual steps below.

#### Step 1: Install System Dependencies

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install -y build-essential nasm python3 qemu-system-x86 \
                    xorriso curl git libgmp-dev libmpfr-dev libmpc-dev \
                    texinfo bison flex
```

**Arch Linux:**
```bash
sudo pacman -Syu --noconfirm base-devel nasm python qemu xorriso \
                             gmp mpfr libmpc
```

**macOS:**
```bash
brew install nasm python qemu xorriso gmp mpfr libmpc texinfo
```

#### Step 2: Build Cross-Compiler

```bash
# Set install directory
export PREFIX="$HOME/opt/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

# Download sources
mkdir -p ~/src
cd ~/src
wget https://ftp.gnu.org/gnu/binutils/binutils-2.39.tar.xz
wget https://ftp.gnu.org/gnu/gcc/gcc-12.2.0/gcc-12.2.0.tar.xz
tar xf binutils-2.39.tar.xz
tar xf gcc-12.2.0.tar.xz

# Build binutils
mkdir build-binutils
cd build-binutils
../binutils-2.39/configure --target=$TARGET --prefix="$PREFIX" \
                            --with-sysroot --disable-nls --disable-werror
make -j$(nproc)
make install

# Build GCC
cd ~/src
mkdir build-gcc
cd build-gcc
../gcc-12.2.0/configure --target=$TARGET --prefix="$PREFIX" \
                         --disable-nls --enable-languages=c,c++ \
                         --without-headers
make -j$(nproc) all-gcc
make -j$(nproc) all-target-libgcc
make install-gcc
make install-target-libgcc
```

#### Step 3: Add to PATH

Add to `~/.bashrc` or `~/.zshrc`:

```bash
export PATH="$HOME/opt/cross/bin:$PATH"
```

Reload shell:
```bash
source ~/.bashrc
```

#### Step 4: Verify Installation

```bash
x86_64-elf-gcc --version
x86_64-elf-ld --version
nasm -version
python3 --version
qemu-system-x86_64 --version
xorriso --version
```

All commands should succeed and show version numbers.

---

## Building the System

### Quick Build

```bash
# Clone repository
git clone https://github.com/yourusername/AutomationOS.git
cd AutomationOS

# Build everything
make all

# Run in QEMU
make qemu
```

### Step-by-Step Build

#### 1. Build Bootloader

```bash
make bootloader
```

**Output:** `build/BOOTX64.EFI`

**Components:**
- `boot/boot.asm` → `build/boot.o`
- `boot/loader.c` → `build/loader.o`
- Linked into UEFI binary

#### 2. Build Kernel

```bash
make kernel
```

**Output:** `build/kernel.elf`

**Components:**
- Architecture-specific code (`kernel/arch/x86_64/*.{c,asm}`)
- Core subsystems (`kernel/core/**/*.c`)
- Drivers (`kernel/drivers/*.c`)
- Kernel library (`kernel/lib/*.c`)
- Main kernel (`kernel/kernel.c`)

#### 3. Build Userspace

```bash
make userspace
```

**Outputs:**
- `build/init` - Init process
- `build/shell` - Shell
- `build/libc.a` - C library

#### 4. Generate ISO

```bash
make iso
```

**Output:** `build/AutomationOS.iso`

**ISO Structure:**
```
AutomationOS.iso
├── EFI/
│   └── BOOT/
│       └── BOOTX64.EFI
└── boot/
    ├── kernel.elf
    ├── init
    └── shell
```

#### 5. Run in QEMU

```bash
make qemu
```

Boots the ISO in QEMU with:
- 256 MB RAM
- Serial console redirected to terminal
- UEFI firmware (OVMF)

---

## Build System Architecture

### Makefile Hierarchy

```
Makefile (root)
├── boot/Makefile
├── kernel/Makefile
│   └── kernel/lib/Makefile
└── userspace/Makefile
    ├── userspace/libc/Makefile
    ├── userspace/init/Makefile
    └── userspace/shell/Makefile
```

### Build Phases

```
┌─────────────────┐
│  make all       │
└────┬────────────┘
     │
     ├─────> make bootloader
     │       └─> build/BOOTX64.EFI
     │
     ├─────> make kernel
     │       ├─> kernel/lib/*.c → build/lib/*.o
     │       ├─> kernel/arch/x86_64/*.{c,asm} → build/arch/*.o
     │       ├─> kernel/core/**/*.c → build/core/*.o
     │       ├─> kernel/drivers/*.c → build/drivers/*.o
     │       └─> Link → build/kernel.elf
     │
     ├─────> make userspace
     │       ├─> userspace/libc/*.c → build/libc.a
     │       ├─> userspace/init/init.c → build/init
     │       └─> userspace/shell/*.c → build/shell
     │
     └─────> make iso
             └─> scripts/build-iso.py → build/AutomationOS.iso
```

### Compiler Flags

**Kernel C Flags:**
```makefile
CFLAGS = -std=c11 -ffreestanding -nostdlib -mcmodel=large \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -Wall -Wextra -Werror -O2 -g
```

**Explanation:**
- `-ffreestanding`: Freestanding environment (no hosted features)
- `-nostdlib`: Don't link against standard library
- `-mcmodel=large`: Support 64-bit addressing
- `-mno-red-zone`: Disable red zone (required for interrupts)
- `-mno-mmx/sse/sse2`: Disable SSE (requires context saving)
- `-Wall -Wextra -Werror`: Enable all warnings, treat as errors
- `-O2`: Optimization level 2
- `-g`: Debug symbols

**Linker Flags:**
```makefile
LDFLAGS = -T kernel.ld -nostdlib -zmax-page-size=0x1000
```

**Linker Script:** `kernel/kernel.ld`

```ld
ENTRY(_start)

SECTIONS {
    . = 0xFFFFFFFF80000000;
    
    .text : ALIGN(4K) {
        *(.text.boot)
        *(.text)
    }
    
    .rodata : ALIGN(4K) {
        *(.rodata)
    }
    
    .data : ALIGN(4K) {
        *(.data)
    }
    
    .bss : ALIGN(4K) {
        *(.bss)
    }
}
```

---

## Build Targets

### Primary Targets

| Target | Description | Output |
|--------|-------------|--------|
| `make all` | Build everything | ISO + all components |
| `make bootloader` | Build UEFI bootloader | `build/BOOTX64.EFI` |
| `make kernel` | Build kernel | `build/kernel.elf` |
| `make userspace` | Build userspace | `build/init`, `build/shell` |
| `make iso` | Generate bootable ISO | `build/AutomationOS.iso` |
| `make clean` | Remove build artifacts | (deletes `build/`) |

### Testing Targets

| Target | Description |
|--------|-------------|
| `make qemu` | Run in QEMU |
| `make qemu-debug` | Run with GDB server |
| `make test` | Run integration tests |
| `make test-full` | Full build + test |
| `make test-unit` | Run unit tests |

### Utility Targets

| Target | Description |
|--------|-------------|
| `make help` | Show available targets |
| `make check-toolchain` | Verify toolchain |
| `make distclean` | Deep clean (including downloaded files) |

---

## Customizing the Build

### Changing Optimization Level

Edit `kernel/Makefile`:

```makefile
# Debug build (no optimization)
CFLAGS += -O0

# Optimize for size
CFLAGS += -Os

# Maximum optimization (may break debugging)
CFLAGS += -O3
```

### Enabling/Disabling Features

Edit `kernel/include/config.h`:

```c
// Enable debug output
#define CONFIG_DEBUG 1

// Enable assertions
#define CONFIG_ASSERT 1

// Scheduler time slice (ms)
#define CONFIG_TIME_SLICE 10

// Maximum processes
#define CONFIG_MAX_PROCESSES 256
```

### Changing Memory Layout

Edit `kernel/kernel.ld` to adjust kernel load address or section layout.

**Example:** Move kernel to different address
```ld
. = 0xFFFFFFFF90000000;  // New base address
```

### Custom QEMU Options

Edit `scripts/run-qemu.sh`:

```bash
# Increase RAM
QEMU_ARGS="-m 512"

# Enable KVM acceleration (Linux only)
QEMU_ARGS="-enable-kvm"

# Change CPU model
QEMU_ARGS="-cpu Haswell"
```

---

## Troubleshooting

### Common Build Errors

#### Error: `x86_64-elf-gcc: command not found`

**Cause:** Cross-compiler not in PATH

**Fix:**
```bash
export PATH="$HOME/opt/cross/bin:$PATH"
# Or re-run setup script
bash scripts/setup-toolchain.sh
```

#### Error: `kernel.ld: No such file or directory`

**Cause:** Running make from wrong directory

**Fix:**
```bash
cd AutomationOS  # Must be in root directory
make kernel
```

#### Error: `undefined reference to '__udivdi3'`

**Cause:** 64-bit division without compiler support

**Fix:** Implement division helpers or avoid 64-bit division in kernel.

#### Error: `xorriso: command not found`

**Cause:** ISO generation tool not installed

**Fix:**
```bash
# Ubuntu/Debian
sudo apt install xorriso

# macOS
brew install xorriso
```

#### Error: `OVMF not found`

**Cause:** UEFI firmware for QEMU not installed

**Fix:**
```bash
# Ubuntu/Debian
sudo apt install ovmf

# Arch
sudo pacman -S edk2-ovmf

# macOS - already included with qemu
```

### Build Performance

#### Slow Compilation

**Solution:** Use parallel builds
```bash
make -j$(nproc) all
```

#### Out of Disk Space

**Solution:** Clean build directory
```bash
make clean
```

Check ISO size:
```bash
du -h build/AutomationOS.iso
```

### Debug Build Issues

#### Enable Verbose Output

```bash
make V=1 all
```

This shows full compiler commands.

#### Check Individual Components

```bash
# Test bootloader build
make bootloader
ls -lh build/BOOTX64.EFI

# Test kernel build
make kernel
file build/kernel.elf
readelf -h build/kernel.elf
```

#### Verify ISO Structure

```bash
# Mount ISO (Linux)
mkdir -p /tmp/iso
sudo mount -o loop build/AutomationOS.iso /tmp/iso
ls -R /tmp/iso
sudo umount /tmp/iso

# Extract ISO (cross-platform)
xorriso -osirrox on -indev build/AutomationOS.iso -extract / /tmp/iso
```

---

## Cross-Platform Notes

### Linux

**Preferred Platform** - All features fully supported.

**Known Issues:**
- Some distros may require additional packages for UEFI testing

### macOS

**Status:** Fully supported with some caveats.

**Issues:**
- QEMU UEFI support may require manual OVMF installation
- Cross-compiler build may be slower

**macOS-Specific Setup:**
```bash
# Install OVMF firmware manually if needed
mkdir -p ~/.qemu
wget https://github.com/clearlinux/common/raw/master/OVMF.fd \
     -O ~/.qemu/OVMF.fd
```

### Windows (WSL2)

**Status:** Supported via WSL2.

**Setup:**
```powershell
# Install WSL2
wsl --install -d Ubuntu-20.04

# Enter WSL
wsl

# Follow Linux setup instructions
```

**Known Issues:**
- Serial console may have display issues
- USB boot creation more complex (use Rufus on Windows)

**WSL Performance Tips:**
- Store code in WSL filesystem (`~/AutomationOS`), not `/mnt/c/`
- Disable Windows Defender scanning for WSL directory

---

## Build Artifacts

### Output Directory Structure

```
build/
├── BOOTX64.EFI          # UEFI bootloader
├── kernel.elf           # Kernel executable
├── kernel.map           # Symbol map
├── init                 # Init process
├── shell                # Shell program
├── libc.a               # C library archive
├── AutomationOS.iso     # Bootable ISO
│
├── boot/                # Bootloader objects
│   ├── boot.o
│   └── loader.o
│
├── kernel/              # Kernel objects
│   ├── kernel.o
│   ├── arch/*.o
│   ├── core/**/*.o
│   ├── drivers/*.o
│   └── lib/*.o
│
└── userspace/           # Userspace objects
    ├── init/*.o
    ├── shell/*.o
    └── libc/*.o
```

### Artifact Sizes

Typical sizes (debug build):

- BOOTX64.EFI: ~50 KB
- kernel.elf: ~200 KB
- init: ~10 KB
- shell: ~20 KB
- AutomationOS.iso: ~2 MB

Release builds (with `-Os`) are ~30% smaller.

---

## Advanced Topics

### Creating a USB Boot Drive

#### Linux
```bash
# Find USB device (e.g., /dev/sdb)
lsblk

# Write ISO (DANGEROUS - verify device!)
sudo dd if=build/AutomationOS.iso of=/dev/sdb bs=4M status=progress
sudo sync
```

#### macOS
```bash
# Find USB device
diskutil list

# Unmount (replace diskN with your device)
diskutil unmountDisk /dev/diskN

# Write ISO
sudo dd if=build/AutomationOS.iso of=/dev/rdiskN bs=4m
```

#### Windows
Use Rufus:
1. Download Rufus from https://rufus.ie/
2. Select AutomationOS.iso
3. Choose "DD Image" mode
4. Click "Start"

### Network Boot (PXE)

Coming in Phase 2.

### Custom Bootloader

Replace AutoBoot with GRUB:

1. Install GRUB EFI binary in `iso/EFI/BOOT/BOOTX64.EFI`
2. Create `grub.cfg`:

```
menuentry "AutomationOS" {
    multiboot2 /boot/kernel.elf
    boot
}
```

---

## Build System Reference

### Environment Variables

| Variable | Default | Purpose |
|----------|---------|---------|
| `CC` | `x86_64-elf-gcc` | C compiler |
| `LD` | `x86_64-elf-ld` | Linker |
| `AS` | `nasm` | Assembler |
| `PYTHON` | `python3` | Python interpreter |
| `V` | `0` | Verbose mode (1=enabled) |

**Example:**
```bash
CC=clang LD=ld.lld make kernel
```

### Parallel Builds

```bash
# Auto-detect cores
make -j$(nproc) all

# Explicit core count
make -j8 all
```

### Incremental Builds

The build system automatically tracks dependencies. Only changed files are recompiled.

**Force Rebuild:**
```bash
make clean && make all
```

---

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Build AutomationOS

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Setup toolchain
      run: bash scripts/setup-toolchain.sh
      
    - name: Build
      run: make all
      
    - name: Test
      run: make test
      
    - name: Upload ISO
      uses: actions/upload-artifact@v3
      with:
        name: AutomationOS.iso
        path: build/AutomationOS.iso
```

---

## Next Steps

After successfully building AutomationOS:

1. **Run in QEMU:** `make qemu`
2. **Read Development Guide:** [DEVELOPMENT_GUIDE.md](DEVELOPMENT_GUIDE.md)
3. **Explore the Code:** Start with `kernel/kernel.c`
4. **Run Tests:** `make test`

---

**End of Build Guide**
