# AutomationOS Build Scripts

This directory contains build automation and testing scripts for AutomationOS.

## Scripts

### `setup-toolchain.sh`

Sets up the build environment and checks for required tools.

**Usage:**
```bash
./scripts/setup-toolchain.sh
```

**Requirements:**
- gcc
- make
- nasm
- python3
- qemu-system-x86_64
- xorriso

---

### `build-iso.py`

Generates a bootable ISO image from built components.

**Usage:**
```bash
python3 scripts/build-iso.py
```

**Prerequisites:**
- Bootloader must be built (`build/BOOTX64.EFI`)
- Kernel must be built (`build/kernel.elf`)
- xorriso must be installed

**Output:**
- `build/AutomationOS.iso` - Bootable ISO image

**What it does:**
1. Checks for required tools (xorriso)
2. Verifies build artifacts exist
3. Creates ISO directory structure:
   ```
   iso/
   ├── EFI/
   │   └── BOOT/
   │       └── BOOTX64.EFI
   └── boot/
       └── kernel.elf
   ```
4. Generates UEFI-bootable ISO with xorriso

---

### `run-qemu.sh`

Launches AutomationOS in QEMU for testing.

**Usage:**
```bash
# Normal boot
./scripts/run-qemu.sh

# Debug mode (GDB server on port 1234)
./scripts/run-qemu.sh --debug

# Custom memory and CPU count
./scripts/run-qemu.sh -m 8G -smp 8

# Headless mode (no display, serial only)
./scripts/run-qemu.sh --no-display

# VNC display
./scripts/run-qemu.sh --vnc
```

**Options:**
- `--debug` - Start with GDB server on port 1234
- `--help` - Show help message
- `-m, --memory SIZE` - Set RAM size (default: 4G)
- `-smp COUNT` - Set CPU count (default: 4)
- `--vnc` - Use VNC display (connect to localhost:5900)
- `--no-display` - Headless mode

**Debug Mode:**

When started with `--debug`, attach GDB with:
```bash
gdb build/kernel.elf -ex 'target remote :1234'
```

Or simply:
```bash
gdb build/kernel.elf
```

The `.gdbinit` file will automatically connect.

**Serial Output:**

Serial console is redirected to stdout. Boot messages appear in the terminal.

**Keyboard:**
- `Ctrl-A X` - Exit QEMU
- `Ctrl-A C` - Switch to QEMU monitor

---

### `test-boot.sh`

Full build and test automation.

**Usage:**
```bash
# Build everything and run tests
./scripts/test-boot.sh

# Test only (skip build)
./scripts/test-boot.sh --skip-build
```

**What it does:**
1. Cleans previous build
2. Builds bootloader
3. Builds kernel
4. Builds userspace
5. Generates ISO
6. Runs integration tests

**Exit Codes:**
- 0 - All tests passed
- 1 - Tests failed

---

## Makefile Integration

All scripts are integrated into the top-level Makefile:

```bash
# Build ISO
make iso

# Run in QEMU
make qemu

# Run in debug mode
make qemu-debug

# Full build and test
make test
```

## Directory Structure

After building:

```
build/
├── BOOTX64.EFI       # UEFI bootloader
├── kernel.elf        # Kernel binary
├── AutomationOS.iso  # Bootable ISO
└── serial.log        # Serial output from last test

iso/
├── EFI/
│   └── BOOT/
│       └── BOOTX64.EFI
└── boot/
    └── kernel.elf
```

## Troubleshooting

### ISO Generation Fails

**Error:** `xorriso not found`

**Solution:**
```bash
# Ubuntu/Debian
sudo apt install xorriso

# Arch
sudo pacman -S libisoburn

# macOS
brew install xorriso
```

---

**Error:** `Missing required build artifacts`

**Solution:**
Build components first:
```bash
make bootloader
make kernel
```

### QEMU Won't Start

**Error:** `qemu-system-x86_64 not found`

**Solution:**
```bash
# Ubuntu/Debian
sudo apt install qemu-system-x86

# Arch
sudo pacman -S qemu

# macOS
brew install qemu
```

---

**Error:** `ISO not found`

**Solution:**
```bash
make iso
```

### No Serial Output

If QEMU starts but shows no output:

1. Check bootloader built correctly:
   ```bash
   ls -lh build/BOOTX64.EFI
   ```

2. Check kernel built correctly:
   ```bash
   ls -lh build/kernel.elf
   ```

3. Try running with verbose output:
   ```bash
   ./scripts/run-qemu.sh --no-display
   ```

## Performance

### Build Times

- Bootloader: < 5 seconds
- Kernel: < 10 seconds
- ISO generation: < 2 seconds
- Total: < 20 seconds

### ISO Size

- Minimal (Phase 1): ~10-20 MB
- With userspace: ~30-50 MB

---

## Hardware Testing Scripts (New)

### `test-hardware.sh`

Test on different virtual machine platforms.

**Usage:**
```bash
# Test on QEMU
./scripts/test-hardware.sh --vm qemu

# Test with specific CPU model
./scripts/test-hardware.sh --vm qemu --cpu-model Haswell

# Test all platforms
./scripts/test-hardware.sh --all
```

**Platforms:** QEMU, VirtualBox, VMware, Hyper-V, KVM

---

### `test-qemu-cpus.sh`

Test multiple QEMU CPU models for compatibility.

**Usage:**
```bash
# Quick test (4 CPU models)
./scripts/test-qemu-cpus.sh --quick

# Full test (22 CPU models)
./scripts/test-qemu-cpus.sh

# List available CPUs
./scripts/test-qemu-cpus.sh --list
```

**Results:** `build/cpu-test-results/`

---

### `test-on-hardware.sh`

Test on physical hardware with serial console capture.

**Usage:**
```bash
# Capture serial output
./scripts/test-on-hardware.sh --serial /dev/ttyUSB0

# Capture and validate
./scripts/test-on-hardware.sh --serial /dev/ttyUSB0 --validate

# Live monitoring
./scripts/test-on-hardware.sh --serial /dev/ttyUSB0 --monitor
```

**Prerequisites:** Serial cable, bootable USB

---

### `create-usb.sh`

Create bootable USB drive from ISO.

**Usage:**
```bash
# Create USB (with confirmation)
sudo ./scripts/create-usb.sh /dev/sdX

# With verification
sudo ./scripts/create-usb.sh --device /dev/sdX --verify
```

**⚠️ WARNING:** Erases all data on target device!

---

### `ci-hardware-test.sh`

Automated CI/CD testing.

**Usage:**
```bash
# Quick CI test (~2 minutes)
./scripts/ci-hardware-test.sh --quick

# Full CI test (~15 minutes)
./scripts/ci-hardware-test.sh

# Specific stage
./scripts/ci-hardware-test.sh --stage boot
```

**Exit codes:** 0 (pass), 1 (fail), 2 (setup error)

---

## See Also

- **[Hardware Testing Guide](../HARDWARE_TESTING_GUIDE.md)** - Complete hardware testing guide
- **[Platform Support](../docs/PLATFORM_SUPPORT.md)** - Supported platforms
- **[Hardware Compatibility](../docs/HARDWARE_COMPATIBILITY.md)** - Tested hardware list
- **[Troubleshooting Hardware](../docs/TROUBLESHOOTING_HARDWARE.md)** - Hardware issues
- [Integration Testing Guide](../docs/INTEGRATION_TESTING.md)
- [Development Guide](../README.md)
- [Implementation Plan](../docs/superpowers/plans/2026-05-26-phase1-core-foundation.md)
