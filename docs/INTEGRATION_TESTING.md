# AutomationOS Integration Testing Guide

This document describes the integration testing process for AutomationOS Phase 1.

## Overview

Integration testing verifies that all components (bootloader, kernel, drivers, userspace) work together correctly. The test suite boots AutomationOS in QEMU and validates subsystem initialization.

## Prerequisites

### Required Tools

- **QEMU** (`qemu-system-x86_64`) - Virtual machine for testing
- **Python 3.11+** - Test script runtime
- **xorriso** - ISO generation tool
- **GCC cross-compiler** - Building kernel and bootloader
- **NASM** - Assembler
- **Make** - Build automation

### Installation

#### Ubuntu/Debian
```bash
sudo apt update
sudo apt install qemu-system-x86 python3 xorriso gcc nasm make
```

#### Arch Linux
```bash
sudo pacman -S qemu python xorriso gcc nasm make
```

#### macOS
```bash
brew install qemu python xorriso nasm
# For cross-compiler, see docs/TOOLCHAIN.md
```

## Test Suite Structure

```
tests/
├── integration/
│   ├── test_boot.py          # Boot integration test
│   └── test_shell.py         # Shell interaction test (future)
└── unit/
    ├── test_pmm.c            # Physical memory manager tests
    ├── test_vmm.c            # Virtual memory manager tests
    ├── test_heap.c           # Heap allocator tests
    └── test_scheduler.c      # Scheduler tests
```

## Running Tests

### Full Build and Test

Build everything from scratch and run tests:

```bash
./scripts/test-boot.sh
```

### Quick Test (Skip Build)

Test using existing build artifacts:

```bash
./scripts/test-boot.sh --skip-build
```

### Individual Test

Run boot test only:

```bash
python3 tests/integration/test_boot.py
```

With verbose output:

```bash
python3 tests/integration/test_boot.py --verbose
```

With custom timeout:

```bash
python3 tests/integration/test_boot.py --timeout 30
```

## Boot Test Checklist

The boot test validates the following subsystems:

### Critical Tests (Must Pass)

- [ ] **Bootloader loads kernel** - AutoBoot UEFI bootloader successfully loads
- [ ] **Kernel banner printed** - "AutomationOS" banner appears
- [ ] **Serial console works** - Output visible on serial port
- [ ] **PMM initialized** - `[PMM]` message present
- [ ] **VMM initialized** - `[VMM]` message present
- [ ] **Heap initialized** - `[HEAP]` message present
- [ ] **GDT loaded** - `[GDT]` message present
- [ ] **IDT loaded** - `[IDT]` message present
- [ ] **Timer initialized** - `[PIT]` message present
- [ ] **Kernel main runs** - `[KERNEL]` message present

### Optional Tests (Nice to Have)

- [ ] **PS/2 keyboard** - `[PS2]` message present
- [ ] **Framebuffer** - `[FB]` message present
- [ ] **Scheduler** - `[SCHED]` message present
- [ ] **Init process** - `[INIT]` message present
- [ ] **Shell** - `[SHELL]` message present

## Manual Testing

### Boot in QEMU

```bash
make qemu
```

Expected output on serial console:

```
=====================================
   AutomationOS v0.1.0
=====================================

[GDT] Initializing Global Descriptor Table...
[GDT] GDT loaded
[PMM] Initializing physical memory manager...
[PMM] Total memory: 4096 MB
[PMM] Memory range: 0x100000 - 0x100000000
[VMM] Initializing virtual memory manager...
[VMM] Paging enabled
[HEAP] Initializing kernel heap...
[HEAP] Kernel heap initialized at 0xFFFFFFFF90000000
[IDT] Initializing Interrupt Descriptor Table...
[IDT] IDT loaded
[PIT] Initializing timer (100Hz)...
[PIT] Timer initialized
[KERNEL] All subsystems initialized
[KERNEL] Free memory: 4080 MB
[KERNEL] Starting init process...
```

### Debug with GDB

Start QEMU in debug mode:

```bash
make qemu-debug
```

In another terminal, attach GDB:

```bash
gdb build/kernel.elf
```

The `.gdbinit` file is automatically loaded with useful breakpoints and commands.

### Test on Real Hardware (USB Boot)

1. Build ISO:
   ```bash
   make iso
   ```

2. Write to USB drive (replace `/dev/sdX` with your USB device):
   ```bash
   sudo dd if=build/AutomationOS.iso of=/dev/sdX bs=4M status=progress
   sudo sync
   ```

3. Boot from USB drive

4. Connect serial cable to capture boot log (optional)

## Debugging Failed Tests

### Test Fails: ISO Not Found

**Error:**
```
ERROR: ISO not found: build/AutomationOS.iso
```

**Solution:**
```bash
make iso
```

### Test Fails: QEMU Not Found

**Error:**
```
ERROR: qemu-system-x86_64 not found
```

**Solution:**
Install QEMU (see Prerequisites above)

### Test Fails: Missing Boot Messages

**Error:**
```
✗ Physical Memory Manager initialized
  Expected: '[PMM]'
```

**Solution:**
1. Check serial output: `cat build/serial.log`
2. Look for error messages before the expected message
3. If kernel panics, debug with `make qemu-debug` and GDB

### Kernel Panic

If the kernel panics during boot:

1. Check serial log:
   ```bash
   cat build/serial.log
   ```

2. Look for panic message:
   ```
   ====================================
          KERNEL PANIC
   ====================================
   PMM: Out of memory
   ====================================
   ```

3. Debug with GDB:
   ```bash
   make qemu-debug
   # In another terminal:
   gdb build/kernel.elf
   (gdb) continue
   # When panic occurs, examine state:
   (gdb) backtrace
   (gdb) info registers
   ```

### No Output on Serial Console

If no output appears:

1. Check bootloader built correctly:
   ```bash
   ls -lh build/BOOTX64.EFI
   ```

2. Check kernel built correctly:
   ```bash
   ls -lh build/kernel.elf
   ```

3. Check serial driver initialized:
   - Serial driver should be initialized first in `kernel_main()`
   - Verify `serial_init()` is called before any `kprintf()`

## Continuous Integration

The test suite is designed to run in CI environments:

### GitHub Actions Example

```yaml
name: Boot Test

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2

      - name: Install dependencies
        run: |
          sudo apt update
          sudo apt install qemu-system-x86 python3 xorriso nasm

      - name: Build and test
        run: ./scripts/test-boot.sh
```

## Test Coverage

### Current Coverage (Phase 1)

- ✅ Boot sequence
- ✅ Memory management initialization
- ✅ CPU setup (GDT, IDT)
- ✅ Timer initialization
- ✅ Serial console
- ⏳ Interrupts (partially - timer only)
- ⏳ Process management (structure defined, not tested)
- ⏳ Scheduler (not yet implemented)
- ⏳ Syscalls (not yet implemented)
- ⏳ Keyboard input (not yet implemented)
- ⏳ Shell (not yet implemented)

### Future Tests (Phase 2+)

- File system operations
- Network stack
- Multi-process execution
- Inter-process communication
- Advanced driver testing
- Stress testing
- Performance benchmarks

## Reporting Issues

When reporting test failures, include:

1. **System information:**
   - OS and version
   - QEMU version: `qemu-system-x86_64 --version`
   - Python version: `python3 --version`

2. **Build output:**
   ```bash
   make clean
   make all 2>&1 | tee build.log
   ```

3. **Test output:**
   ```bash
   ./scripts/test-boot.sh 2>&1 | tee test.log
   ```

4. **Serial console log:**
   ```bash
   cat build/serial.log
   ```

5. **Steps to reproduce**

## Performance Benchmarks

### Boot Time

Target boot times:

- QEMU boot to kernel main: < 1 second
- Full boot to shell: < 2 seconds
- Real hardware boot: < 5 seconds

### Memory Usage

- Kernel size: < 1 MB
- Boot-time memory usage: < 10 MB
- Heap overhead: < 5%

## References

- [QEMU Documentation](https://www.qemu.org/documentation/)
- [GDB Debugging Guide](https://sourceware.org/gdb/current/onlinedocs/gdb/)
- [OSDev Testing](https://wiki.osdev.org/Testing)
