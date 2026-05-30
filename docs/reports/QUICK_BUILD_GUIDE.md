# Quick Build Guide

This guide shows how to build AutomationOS after the build system integration.

## Prerequisites

### Required Tools

1. **Cross-Compiler Toolchain**
   ```bash
   x86_64-elf-gcc
   x86_64-elf-ld
   nasm
   ```

2. **Build Utilities**
   ```bash
   make
   bash
   python3
   tar
   ```

3. **Optional (for QEMU testing)**
   ```bash
   qemu-system-x86_64
   ```

### Installation

#### Option 1: WSL (Recommended for Windows)

```bash
# Install WSL
wsl --install

# Inside WSL, install build tools
sudo apt update
sudo apt install build-essential nasm python3 qemu-system-x86

# Install cross-compiler (see docs/TOOLCHAIN.md)
./scripts/setup-toolchain.sh
```

#### Option 2: MSYS2 (Windows Native)

```bash
# Install MSYS2 from https://www.msys2.org/

# In MSYS2 terminal
pacman -S base-devel nasm python

# Install cross-compiler manually (see docs/TOOLCHAIN.md)
```

#### Option 3: Docker

```bash
# Build the Docker image
make docker-build

# Run build inside Docker
make docker-test
```

## Build Commands

### Clean Build

```bash
# Remove all build artifacts
make clean

# Build everything
make all
```

This will:
1. Build bootloader (`boot/`)
2. Build kernel (`kernel/`)
3. Build userspace (`userspace/`)
   - libc with IPC support
   - Libraries (font, image)
   - Init process
   - Compositor
   - Window manager
   - Applications
4. Create initrd with fonts/icons/wallpapers
5. Generate bootable ISO

### Validate Build

```bash
# Check that all components built correctly
make validate
```

Expected output:
```
========================================
  AutomationOS Build Validation
========================================

=== Kernel Components ===
[PASS] Kernel ELF: build/kernel.elf (2145728 bytes)
...

=== Userspace Binaries ===
[PASS] Init Process: build/userspace/init/init (52341 bytes)
[PASS] Compositor: build/userspace/compositor/compositor (125678 bytes)
...

========================================
  Validation Summary
========================================
PASS: 25
WARN: 3
FAIL: 0

✓ BUILD VALIDATION PASSED
```

### Run in QEMU

```bash
# Run with graphics
make qemu

# Run with debugging (GDB server on port 1234)
make qemu-debug
```

### Run Tests

```bash
# Integration tests
make test

# Unit tests
make unit-tests

# Full test suite
make test-full
```

## Build Output

After a successful build, you should have:

```
build/
├── kernel.elf              # Kernel binary (~2 MB)
├── initrd.img              # Initial ramdisk (~10-50 MB)
├── automationos.iso        # Bootable ISO (~50-100 MB)
└── ...
```

### Initrd Contents

The initrd contains:

```
/sbin/
  ├── init              # Init process
  ├── compositor        # Display compositor
  ├── wm                # Window manager
  └── desktop-shell     # Desktop shell

/bin/
  ├── sh                # Shell
  ├── terminal          # Terminal emulator
  ├── files             # File manager
  └── taskmanager       # Task manager

/lib/
  └── libc.a            # C library with IPC

/usr/share/
  ├── fonts/
  │   └── DejaVuSans.ttf
  ├── icons/
  └── wallpapers/
      └── default.png

/etc/
  ├── fstab
  └── inittab
```

## Troubleshooting

### Build Errors

#### Error: `make: command not found`

**Solution:**
```bash
# WSL/Linux
sudo apt install build-essential

# MSYS2
pacman -S make
```

#### Error: `x86_64-elf-gcc: command not found`

**Solution:**
```bash
# Install cross-compiler
./scripts/setup-toolchain.sh

# Or see docs/TOOLCHAIN.md for manual installation
```

#### Error: `nasm: command not found`

**Solution:**
```bash
# WSL/Linux
sudo apt install nasm

# MSYS2
pacman -S nasm
```

### Validation Errors

#### Missing Font Library

**Error:** `[WARN] Font library not found (optional)`

**Solution:**
```bash
cd userspace/lib/font
make download-stb
make all
```

#### Missing Init Binary

**Error:** `[FAIL] Init Process: build/userspace/init/init not found (REQUIRED)`

**Solution:**
```bash
# Rebuild userspace
make -C userspace/ clean
make -C userspace/ all
```

### Runtime Issues

#### QEMU Hangs at Boot

**Check:**
1. Verify kernel.elf exists: `ls -lh build/kernel.elf`
2. Verify initrd.img exists: `ls -lh build/initrd.img`
3. Check initrd contents: `tar -tf build/initrd.img | grep sbin/init`

**Debug:**
```bash
# Enable verbose QEMU output
make qemu-debug
```

#### IPC Not Working

**Check:**
1. Verify libc has IPC symbols:
   ```bash
   nm build/userspace/libc/libc.a | grep msgqueue_create
   nm build/userspace/libc/libc.a | grep shm_create
   ```

2. Verify kernel has IPC compiled:
   ```bash
   ls -lh build/kernel/ipc/msgqueue.o
   ls -lh build/kernel/ipc/shm.o
   ```

## Quick Start (Copy-Paste)

```bash
# Prerequisites check
which make && which x86_64-elf-gcc && which nasm && which python3

# If any missing, install toolchain first
./scripts/setup-toolchain.sh

# Clean build
make clean
make all

# Validate
make validate

# Test in QEMU
make qemu
```

## Next Steps

After successful build:

1. **Test boot in QEMU**
   - `make qemu`
   - Verify kernel boots
   - Check init process starts

2. **Test IPC functionality**
   - Write test program using `msgqueue_create()`
   - Test shared memory with `shm_create()`

3. **Test PTY functionality**
   - Open terminal
   - Verify keyboard input works
   - Test multiple terminals

4. **Test VFS operations**
   - Mount filesystems
   - Test file operations
   - Verify directory traversal

5. **Test compositor**
   - Verify framebuffer initializes
   - Test window rendering
   - Verify font rendering works

## CI/CD Integration

### GitHub Actions

```yaml
name: Build AutomationOS

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      
      - name: Install dependencies
        run: |
          sudo apt update
          sudo apt install build-essential nasm python3
      
      - name: Setup toolchain
        run: ./scripts/setup-toolchain.sh
      
      - name: Build
        run: make all
      
      - name: Validate
        run: make validate
      
      - name: Upload ISO
        uses: actions/upload-artifact@v2
        with:
          name: automationos-iso
          path: build/automationos.iso
```

### Docker Build

```dockerfile
FROM ubuntu:22.04

RUN apt update && apt install -y \
    build-essential \
    nasm \
    python3 \
    qemu-system-x86

WORKDIR /workspace
COPY . .

RUN ./scripts/setup-toolchain.sh
RUN make clean && make all

CMD ["make", "qemu"]
```

## Performance Tips

### Parallel Builds

```bash
# Use multiple CPU cores
make -j$(nproc) all
```

### Incremental Builds

```bash
# After editing a single file
make kernel          # Only rebuild kernel
make userspace       # Only rebuild userspace
```

### Fast Testing

```bash
# Skip full build for testing
make test --skip-build
```

## Help

For more information:

- **Toolchain Setup:** `docs/TOOLCHAIN.md`
- **Coding Standards:** `docs/CODING_STANDARDS.md`
- **Full Build System:** `BUILD_SYSTEM_INTEGRATION_REPORT.md`
- **Main Makefile Help:** `make help`

For issues, consult the validation output:
```bash
make validate 2>&1 | tee validation.log
```
