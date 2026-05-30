# AutomationOS Build Instructions

**Version:** v0.1.0 "Foundation"  
**Last Updated:** 2026-05-27  
**Agent:** Integration Agent 15

---

## Quick Start (5 Minutes)

For experienced developers who want to build and run AutomationOS immediately:

```bash
# 1. Clone repository
cd AutomationOS/

# 2. Setup cross-compiler toolchain (automated)
./scripts/setup-toolchain.sh

# 3. Build everything
make all

# 4. Run in QEMU
make qemu

# Expected: Desktop boots in 1-2 seconds
```

**That's it!** If this works, you're done. Read on for detailed instructions.

---

## Prerequisites

### Required Tools

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| **make** | 3.81+ | Build automation |
| **x86_64-elf-gcc** | 9.0+ | Cross-compiler for kernel |
| **x86_64-elf-ld** | 2.30+ | Linker for kernel |
| **nasm** | 2.14+ | x86_64 assembler |
| **python3** | 3.7+ | Build scripts |
| **xorriso** | 1.4+ | ISO image creation |
| **mtools** | 4.0+ | FAT filesystem tools |
| **git** | 2.20+ | Version control |

### Optional Tools (Recommended)

| Tool | Purpose |
|------|---------|
| **qemu-system-x86_64** | Testing and emulation |
| **clang-format** | Code formatting |
| **clang-tidy** | Static analysis |
| **gdb** | Kernel debugging |

---

## Platform-Specific Setup

### Ubuntu / Debian

```bash
# Essential build tools
sudo apt update
sudo apt install build-essential nasm python3 git xorriso mtools

# Cross-compiler toolchain
# Try package manager first:
sudo apt install gcc-x86-64-elf binutils-x86-64-elf

# If not available, use automated setup script:
./scripts/setup-toolchain.sh

# Optional: QEMU for testing
sudo apt install qemu-system-x86

# Optional: Development tools
sudo apt install clang-format clang-tidy cppcheck lcov gdb
```

### Arch Linux

```bash
# Essential build tools
sudo pacman -S base-devel nasm python git xorriso mtools

# Cross-compiler toolchain
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils

# Optional: QEMU and dev tools
sudo pacman -S qemu clang gdb
```

### macOS

```bash
# Install Homebrew (if not already installed)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Essential build tools
brew install nasm python git xorriso mtools

# Cross-compiler toolchain
brew install x86_64-elf-gcc x86_64-elf-binutils

# Optional: QEMU
brew install qemu
```

### Windows (WSL2)

```bash
# Install WSL2 with Ubuntu
wsl --install

# Inside WSL2, follow Ubuntu instructions above
sudo apt update
sudo apt install build-essential nasm python3 git xorriso mtools qemu-system-x86

# Cross-compiler setup
./scripts/setup-toolchain.sh
```

---

## Build Process

### Step 1: Toolchain Setup

The automated setup script downloads and builds the cross-compiler toolchain:

```bash
./scripts/setup-toolchain.sh
```

**What it does:**
- Downloads GCC 11.2.0 and Binutils 2.37 source
- Builds x86_64-elf cross-compiler
- Installs to `$HOME/opt/cross` by default
- Adds toolchain to PATH

**Time:** 15-30 minutes (one-time setup)

**Verify installation:**
```bash
x86_64-elf-gcc --version
x86_64-elf-ld --version
```

### Step 2: Build Configuration

AutomationOS uses a simple Makefile-based build system. No separate configure step is required for basic builds.

For custom configuration, edit `Makefile` variables:
```makefile
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
AS = nasm
CFLAGS = -O2 -Wall -Wextra ...
```

### Step 3: Build Components

#### Build Everything (Recommended)

```bash
make all
```

**This builds:**
1. Bootloader (`build/BOOTX64.EFI`)
2. Kernel (`build/kernel.elf`)
3. Userspace applications (`build/userspace/*`)
4. InitRD (`build/initrd.img`)
5. Bootable ISO (`build/automationos.iso`)

**Time:** 2-3 minutes on modern hardware

#### Build Individual Components

```bash
# Bootloader only
make bootloader

# Kernel only
make kernel

# Userspace only
make userspace

# InitRD only
make initrd

# ISO image only
make iso
```

### Step 4: Verify Build

Check that the ISO was created:

```bash
ls -lh build/automationos.iso
```

**Expected output:**
```
-rw-r--r-- 1 user user 12M May 27 01:40 build/automationos.iso
```

**Expected size:** ~12 MB (less than 50 MB target)

---

## Running AutomationOS

### QEMU Emulation (Recommended for Testing)

```bash
# Standard boot
make qemu

# Debug mode (with GDB support)
make qemu-debug

# Custom QEMU options
qemu-system-x86_64 -cdrom build/automationos.iso -m 512M -enable-kvm -serial stdio
```

**QEMU Options Explained:**
- `-cdrom build/automationos.iso` - Boot from ISO
- `-m 512M` - Allocate 512 MB RAM
- `-enable-kvm` - Enable hardware virtualization (Linux only, much faster)
- `-serial stdio` - Serial output to terminal

**Expected behavior:**
- QEMU window opens
- AutoBoot bootloader appears
- Kernel boots in ~1-2 seconds
- Desktop environment appears
- Applications can be launched

### Real Hardware (Advanced)

⚠️ **WARNING:** Only attempt on dedicated test hardware. Backup important data first.

#### Create Bootable USB

```bash
# Identify USB device (CAREFUL!)
lsblk

# Write ISO to USB (replace /dev/sdX with your USB device)
sudo dd if=build/automationos.iso of=/dev/sdX bs=4M status=progress
sudo sync

# Or use the automated script
./scripts/create-bootable-usb.sh /dev/sdX
```

#### BIOS/UEFI Configuration

1. Insert USB drive
2. Boot into BIOS/UEFI settings (F2, F12, or DEL key)
3. Enable UEFI boot mode (disable Legacy/CSM)
4. Set USB drive as first boot device
5. Save and reboot

**Compatibility Notes:**
- Requires UEFI firmware (not Legacy BIOS)
- x86_64 CPU required (Intel/AMD 64-bit)
- Minimum 512 MB RAM recommended

---

## Testing

### Unit Tests

```bash
# Run all unit tests
make unit-tests

# Expected: 32/32 tests pass
```

### Integration Tests

```bash
# Run integration test suite
make test

# Or run manually
bash scripts/run-integration-tests.sh
```

### Boot Test

```bash
# Automated boot validation
bash scripts/test-boot.sh

# Expected: Desktop boots successfully
```

### Manual Testing Checklist

After booting in QEMU:

- [ ] Desktop appears
- [ ] Terminal can be launched
- [ ] Terminal accepts keyboard input
- [ ] File manager opens
- [ ] Settings app launches
- [ ] Task manager shows processes
- [ ] No kernel panics
- [ ] Shutdown works (Ctrl+Alt+F10 in QEMU)

---

## Build Targets Reference

### Standard Targets

| Target | Description |
|--------|-------------|
| `make all` | Build everything (bootloader + kernel + userspace + ISO) |
| `make clean` | Remove all build artifacts |
| `make bootloader` | Build UEFI bootloader only |
| `make kernel` | Build kernel only |
| `make userspace` | Build userspace applications only |
| `make initrd` | Generate initial RAM disk |
| `make iso` | Create bootable ISO image |
| `make qemu` | Build and run in QEMU |
| `make test` | Run test suite |

### Development Targets

| Target | Description |
|--------|-------------|
| `make qemu-debug` | Run QEMU with GDB debugging |
| `make unit-tests` | Run unit tests |
| `make format` | Auto-format code (clang-format) |
| `make lint` | Run static analysis (clang-tidy, cppcheck) |
| `make coverage` | Generate code coverage report |

### Advanced Targets

| Target | Description |
|--------|-------------|
| `make analyze` | Run comprehensive static analysis |
| `make benchmark` | Run performance benchmarks |
| `make install` | Install to /boot (Linux hosts only) |
| `make deb` | Create Debian package |
| `make rpm` | Create RPM package |

---

## Troubleshooting

### Build Errors

#### "x86_64-elf-gcc: command not found"

**Cause:** Cross-compiler toolchain not installed or not in PATH

**Solution:**
```bash
# Run setup script
./scripts/setup-toolchain.sh

# Add to PATH manually
export PATH="$HOME/opt/cross/bin:$PATH"

# Make permanent (add to ~/.bashrc)
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc
```

#### "nasm: command not found"

**Cause:** NASM assembler not installed

**Solution:**
```bash
# Ubuntu/Debian
sudo apt install nasm

# Arch
sudo pacman -S nasm

# macOS
brew install nasm
```

#### "xorriso: command not found"

**Cause:** ISO creation tool not installed

**Solution:**
```bash
# Ubuntu/Debian
sudo apt install xorriso

# Arch
sudo pacman -S libisoburn

# macOS
brew install xorriso
```

#### Build fails with "Permission denied"

**Cause:** Build directory not writable

**Solution:**
```bash
# Make build directory writable
chmod -R u+w build/

# Or clean and rebuild
make clean
make all
```

### Runtime Errors

#### QEMU fails to start

**Cause:** QEMU not installed or ISO not built

**Solution:**
```bash
# Install QEMU
sudo apt install qemu-system-x86  # Ubuntu
brew install qemu                  # macOS

# Ensure ISO is built
make iso
```

#### Kernel panic on boot

**Cause:** Hardware incompatibility or corrupted build

**Solution:**
```bash
# Clean rebuild
make clean
make all

# Try in QEMU first
make qemu

# Check serial output for error messages
```

#### Desktop doesn't appear

**Cause:** Compositor/window manager not starting

**Solution:**
```bash
# Check boot logs (serial output)
make qemu 2>&1 | tee boot.log

# Look for errors in boot.log
grep -i error boot.log
grep -i panic boot.log
```

---

## Performance Tips

### Faster Builds

```bash
# Parallel compilation (use all CPU cores)
make -j$(nproc) all

# Example for 8-core system
make -j8 all
```

### Faster QEMU Execution

```bash
# Enable KVM hardware virtualization (Linux only)
make qemu  # KVM auto-enabled if available

# Or manually
qemu-system-x86_64 -cdrom build/automationos.iso -enable-kvm -cpu host
```

### Incremental Builds

After making changes to a single file, just run:
```bash
make all
```

The build system automatically detects changes and only rebuilds modified components.

---

## Advanced Configuration

### Custom Build Flags

Edit `Makefile` to customize compilation:

```makefile
# Optimization level
CFLAGS += -O3              # Maximum optimization
CFLAGS += -O0 -g           # No optimization, debug symbols

# Architecture tuning
CFLAGS += -march=native    # Optimize for build machine CPU
CFLAGS += -mtune=generic   # Generic x86_64 optimization

# Warnings
CFLAGS += -Wall -Wextra -Werror  # Treat warnings as errors
```

### Debug Build

```bash
# Build with debug symbols
make clean
make CFLAGS="-O0 -g" all

# Debug with GDB
make qemu-debug

# In another terminal
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) continue
```

### Release Build

```bash
# Optimized release build
make clean
make CFLAGS="-O3 -DNDEBUG" all

# Or use release script
./scripts/build-release.sh
```

---

## Directory Structure

```
AutomationOS/
├── boot/                   # Bootloader source
│   ├── boot.asm           # UEFI entry point
│   ├── loader.c           # Kernel loading logic
│   └── Makefile
├── kernel/                 # Kernel source
│   ├── arch/x86_64/       # x86_64 architecture code
│   ├── core/              # Memory, scheduler, syscalls
│   ├── drivers/           # Device drivers
│   ├── lib/               # Kernel library
│   └── Makefile
├── userspace/              # Userspace programs
│   ├── apps/              # GUI applications
│   ├── compositor/        # Desktop compositor
│   ├── init/              # Init process (PID 1)
│   ├── libc/              # C standard library
│   └── Makefile
├── build/                  # Build artifacts (generated)
│   ├── BOOTX64.EFI        # UEFI bootloader
│   ├── kernel.elf         # Kernel binary
│   ├── initrd.img         # Initial RAM disk
│   └── automationos.iso   # Bootable ISO image
├── scripts/                # Build and test scripts
├── docs/                   # Documentation
└── Makefile               # Main build script
```

---

## Build System Details

### Build Flow

```
make all
    ├─→ make bootloader
    │       └─→ Builds boot/boot.asm + boot/loader.c
    │           Output: build/BOOTX64.EFI
    │
    ├─→ make kernel
    │       ├─→ Compiles kernel C files
    │       ├─→ Assembles kernel ASM files
    │       └─→ Links into kernel.elf
    │           Output: build/kernel.elf
    │
    ├─→ make userspace
    │       ├─→ Builds libc
    │       ├─→ Builds compositor
    │       ├─→ Builds applications
    │       └─→ Installs to build/userspace/
    │           Output: build/userspace/{bin,sbin,lib}/
    │
    ├─→ make initrd
    │       └─→ Packages userspace into initrd.img
    │           Output: build/initrd.img
    │
    └─→ make iso
            └─→ Creates bootable ISO with:
                - BOOTX64.EFI (EFI/BOOT/)
                - kernel.elf (/)
                - initrd.img (/)
                Output: build/automationos.iso
```

### Build Artifacts

| Artifact | Size | Purpose |
|----------|------|---------|
| `BOOTX64.EFI` | 228 KB | UEFI bootloader |
| `kernel.elf` | ~2 MB | Kernel binary |
| `initrd.img` | ~8 MB | Initial RAM disk with userspace |
| `automationos.iso` | ~12 MB | Bootable ISO image |

---

## Next Steps

After successfully building:

1. **Run in QEMU:** `make qemu` to test the desktop environment
2. **Read Documentation:** See `docs/` for architecture and API details
3. **Explore Source:** Browse `kernel/` and `userspace/` to understand the code
4. **Run Tests:** `make test` to validate functionality
5. **Contribute:** See `CONTRIBUTING.md` for development guidelines

---

## Support & Documentation

- **Architecture:** `docs/ARCHITECTURE.md`
- **API Reference:** `docs/API_REFERENCE.md`
- **Development Guide:** `docs/DEVELOPMENT_GUIDE.md`
- **Troubleshooting:** `docs/TROUBLESHOOTING.md`
- **FAQ:** `docs/FAQ.md`
- **Agent Reports:** See `AGENT*.md` files for subsystem details

---

## Build System Validation

Verify your build system is correctly configured:

```bash
# Run validation script
./scripts/validate-build-system.sh

# Expected output:
# ✓ Cross-compiler found
# ✓ NASM assembler found
# ✓ Python 3 found
# ✓ xorriso found
# ✓ All dependencies satisfied
```

---

**AutomationOS v0.1.0 "Foundation"**  
**Build System: Validated and Production-Ready**

**Last Updated:** 2026-05-27  
**Agent 15: Integration & Polish**

---

**Co-Authored-By:** Claude Sonnet 4.5 (1M context) <noreply@anthropic.com>
