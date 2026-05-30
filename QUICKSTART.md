# AutomationOS Quick Start Guide

Get AutomationOS running in under 5 minutes.

## Prerequisites

Install required tools:

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install build-essential nasm python3 qemu-system-x86 xorriso

# Arch Linux
sudo pacman -S base-devel nasm python qemu xorriso

# macOS
brew install nasm python qemu xorriso
# For GCC cross-compiler, see docs/TOOLCHAIN.md
```

## Quick Start

```bash
# Clone repository
git clone <repository-url>
cd AutomationOS

# Setup build environment
bash scripts/setup-toolchain.sh

# Build everything
make all

# Run in QEMU
make qemu
```

That's it! AutomationOS will boot in QEMU.

## Common Commands

```bash
# Build specific components
make bootloader    # Build AutoBoot UEFI bootloader
make kernel        # Build kernel
make userspace     # Build userspace programs
make iso           # Generate bootable ISO

# Testing
make qemu          # Run in QEMU
make qemu-debug    # Run with GDB debugging
make test          # Run integration tests
make test-full     # Full build + test

# Cleanup
make clean         # Remove build artifacts
```

## Expected Output

When AutomationOS boots, you should see:

```
=====================================
   AutomationOS v0.1.0
=====================================

[GDT] Initializing Global Descriptor Table...
[GDT] GDT loaded
[PMM] Initializing physical memory manager...
[PMM] Total memory: 4096 MB
[VMM] Initializing virtual memory manager...
[VMM] Paging enabled
[HEAP] Initializing kernel heap...
[HEAP] Kernel heap initialized
[IDT] Initializing Interrupt Descriptor Table...
[IDT] IDT loaded
[PIT] Initializing timer (100Hz)...
[PIT] Timer initialized
[KERNEL] All subsystems initialized
[KERNEL] Free memory: 4080 MB
```

## Debugging

Start QEMU in debug mode:

```bash
make qemu-debug
```

In another terminal, attach GDB:

```bash
gdb build/kernel.elf
```

The `.gdbinit` file is automatically loaded with useful commands.

## Testing

Run automated boot tests:

```bash
make test
```

Expected output:

```
==================================================
AutomationOS Boot Integration Test
==================================================

Checking prerequisites...
  ✓ All prerequisites met
Running boot tests...
  ✓ Kernel banner printed
  ✓ Physical Memory Manager initialized
  ✓ Virtual Memory Manager initialized
  ✓ Kernel heap initialized
  ✓ Global Descriptor Table loaded

==================================================
  ✓ ALL TESTS PASSED
==================================================
```

## Real Hardware (USB Boot)

Create bootable USB drive:

```bash
# Build ISO
make iso

# Write to USB (replace /dev/sdX with your USB device)
sudo dd if=build/AutomationOS.iso of=/dev/sdX bs=4M status=progress
sudo sync
```

Boot from the USB drive.

## Project Structure

```
AutomationOS/
├── boot/          # AutoBoot UEFI bootloader
├── kernel/        # Kernel core
├── userspace/     # Init, shell, utilities
├── scripts/       # Build and test scripts
├── tests/         # Integration and unit tests
├── docs/          # Documentation
├── build/         # Build artifacts (generated)
└── iso/           # ISO structure (generated)
```

## Next Steps

- Read [Integration Testing Guide](docs/INTEGRATION_TESTING.md)
- Read [Implementation Plan](docs/superpowers/plans/2026-05-26-phase1-core-foundation.md)
- See [README.md](README.md) for detailed information

## Troubleshooting

### Problem: `xorriso not found`

Install xorriso:
```bash
sudo apt install xorriso        # Ubuntu/Debian
sudo pacman -S libisoburn       # Arch
brew install xorriso            # macOS
```

### Problem: `qemu-system-x86_64 not found`

Install QEMU:
```bash
sudo apt install qemu-system-x86    # Ubuntu/Debian
sudo pacman -S qemu                 # Arch
brew install qemu                   # macOS
```

### Problem: No output in QEMU

Check build artifacts:
```bash
ls -lh build/BOOTX64.EFI    # Bootloader
ls -lh build/kernel.elf     # Kernel
```

If missing:
```bash
make clean
make all
```

### Problem: Build fails

Check toolchain:
```bash
bash scripts/setup-toolchain.sh
```

## Help

Get help:
```bash
make help
./scripts/run-qemu.sh --help
python3 tests/integration/test_boot.py --help
```

## Performance

- Build time: ~20 seconds
- Boot time (QEMU): <1 second
- ISO size: ~10-20 MB (Phase 1)

Enjoy AutomationOS! 🚀
