# AutomationOS Hardware Testing Guide

**Version:** 1.0  
**Last Updated:** 2026-05-26  
**Status:** Ready for Testing

---

## Overview

This guide provides comprehensive instructions for testing AutomationOS on various hardware platforms and virtual machines. It is designed for QA engineers, hardware testers, and contributors who want to validate AutomationOS compatibility.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Testing Tools](#testing-tools)
3. [Virtual Machine Testing](#virtual-machine-testing)
4. [Physical Hardware Testing](#physical-hardware-testing)
5. [CPU Compatibility Testing](#cpu-compatibility-testing)
6. [Boot Media Creation](#boot-media-creation)
7. [Serial Console Setup](#serial-console-setup)
8. [Test Reporting](#test-reporting)
9. [CI/CD Integration](#cicd-integration)
10. [Troubleshooting](#troubleshooting)

---

## Quick Start

### Prerequisites

```bash
# 1. Build AutomationOS ISO
make clean && make iso

# 2. Verify build
ls -lh build/AutomationOS.iso
```

### Quick Test (5 minutes)

```bash
# Test on QEMU (fastest)
make qemu

# Or use the test script
./scripts/test-hardware.sh --vm qemu
```

### Quick CI Test (2 minutes)

```bash
# Run automated CI tests
./scripts/ci-hardware-test.sh --quick
```

---

## Testing Tools

### Available Scripts

| Script | Purpose | Duration |
|--------|---------|----------|
| `scripts/run-qemu.sh` | Launch QEMU manually | N/A |
| `scripts/test-hardware.sh` | Test on VMs | ~5 min |
| `scripts/test-qemu-cpus.sh` | CPU compatibility | ~10 min |
| `scripts/test-on-hardware.sh` | Physical hardware | ~30 min |
| `scripts/create-usb.sh` | Create bootable USB | ~5 min |
| `scripts/ci-hardware-test.sh` | CI/CD testing | ~2-15 min |

### Script Details

#### `scripts/run-qemu.sh`

Launch QEMU with AutomationOS ISO.

```bash
# Basic usage
./scripts/run-qemu.sh

# With debugging
./scripts/run-qemu.sh --debug

# Custom memory/CPUs
./scripts/run-qemu.sh -m 8G -smp 8

# Headless mode
./scripts/run-qemu.sh --no-display
```

#### `scripts/test-hardware.sh`

Test on different VM platforms.

```bash
# Test on QEMU
./scripts/test-hardware.sh --vm qemu

# Test with specific CPU model
./scripts/test-hardware.sh --vm qemu --cpu-model Haswell

# Test all platforms
./scripts/test-hardware.sh --all
```

#### `scripts/test-qemu-cpus.sh`

Test multiple CPU models for compatibility.

```bash
# Quick test (4 CPU models)
./scripts/test-qemu-cpus.sh --quick

# Full test (22 CPU models)
./scripts/test-qemu-cpus.sh

# List available CPUs
./scripts/test-qemu-cpus.sh --list
```

#### `scripts/test-on-hardware.sh`

Test on physical hardware with serial capture.

```bash
# Capture serial output
./scripts/test-on-hardware.sh --serial /dev/ttyUSB0

# Capture and validate
./scripts/test-on-hardware.sh --serial /dev/ttyUSB0 --validate

# Live monitoring
./scripts/test-on-hardware.sh --serial /dev/ttyUSB0 --monitor
```

#### `scripts/create-usb.sh`

Create bootable USB drive.

```bash
# Create USB (with confirmation)
sudo ./scripts/create-usb.sh /dev/sdX

# With verification
sudo ./scripts/create-usb.sh --device /dev/sdX --verify

# Force mode (skip confirmation)
sudo ./scripts/create-usb.sh --device /dev/sdX --force
```

#### `scripts/ci-hardware-test.sh`

Automated CI/CD testing.

```bash
# Quick CI test (~2 minutes)
./scripts/ci-hardware-test.sh --quick

# Full CI test (~15 minutes)
./scripts/ci-hardware-test.sh

# Specific stage
./scripts/ci-hardware-test.sh --stage boot
```

---

## Virtual Machine Testing

### QEMU (Recommended)

**Setup:**
```bash
# Install QEMU
# Ubuntu/Debian:
sudo apt install qemu-system-x86

# Arch Linux:
sudo pacman -S qemu

# macOS:
brew install qemu
```

**Test:**
```bash
# Basic test
./scripts/test-hardware.sh --vm qemu

# Test specific CPU
./scripts/test-hardware.sh --vm qemu --cpu-model Haswell

# Multiple CPUs
./scripts/test-qemu-cpus.sh --quick
```

---

### VirtualBox

**Setup:**
1. Download and install VirtualBox 7.0+
2. Create new VM:
   - Type: Other
   - Version: Other/Unknown (64-bit)
   - Enable EFI (Special OSes)
3. Configure:
   - Memory: 4096 MB
   - CPUs: 4
   - Storage: Attach AutomationOS.iso
   - Serial Port: Enable, output to file

**Test:**
```bash
# Automated test (manual setup required)
./scripts/test-hardware.sh --vm virtualbox
```

**Manual Test:**
1. Start VM
2. Check serial output file
3. Verify boot messages
4. Fill out [test report](docs/HARDWARE_TEST_REPORT_TEMPLATE.md)

---

### VMware Workstation

**Setup:**
1. Install VMware Workstation 17.x
2. Create new VM:
   - Guest OS: Other Linux 5.x kernel 64-bit
   - Firmware: UEFI
3. Configure:
   - Memory: 4096 MB
   - CPUs: 4
   - CD/DVD: AutomationOS.iso
   - Serial Port: Output to file

**Test:**
```bash
# Automated test (manual setup required)
./scripts/test-hardware.sh --vm vmware
```

---

### Microsoft Hyper-V

**Setup (Windows):**
1. Enable Hyper-V feature
2. Open Hyper-V Manager
3. Create new VM:
   - **Generation 2** (UEFI required)
   - Memory: 4096 MB
   - CPUs: 4
   - Disable Secure Boot
4. Add serial port (COM 1)

**Test:**
```bash
# Automated test (manual setup required)
./scripts/test-hardware.sh --vm hyperv
```

---

### Linux KVM

**Setup:**
```bash
# Install KVM
# Ubuntu/Debian:
sudo apt install qemu-kvm libvirt-daemon-system virt-manager

# Arch Linux:
sudo pacman -S qemu libvirt virt-manager

# Add user to libvirt group
sudo usermod -a -G libvirt $USER
```

**Test:**
```bash
# Test with KVM acceleration
./scripts/test-hardware.sh --vm kvm

# Or manually with virt-manager
virt-manager
```

---

## Physical Hardware Testing

### Prerequisites

1. **Hardware Requirements:**
   - x86_64 CPU (Intel or AMD)
   - 1GB+ RAM (4GB recommended)
   - UEFI firmware
   - USB port or CD/DVD drive

2. **Optional (for debugging):**
   - Serial port or USB-to-serial adapter
   - Null-modem cable
   - Capture machine with serial port

---

### Create Bootable USB

```bash
# 1. Find USB device
lsblk

# 2. Create bootable USB
sudo ./scripts/create-usb.sh /dev/sdX

# 3. Safely eject
sync
sudo eject /dev/sdX
```

---

### Boot from USB

1. **Insert USB** into test machine

2. **Enter BIOS/UEFI:**
   - Press F2, Del, or F12 during boot
   - Configure:
     - Boot mode: UEFI (not legacy BIOS)
     - Secure Boot: Disabled
     - Boot order: USB first

3. **Enable serial console (optional):**
   - Serial port: Enabled
   - Port: COM1
   - Baud rate: 115200
   - Data: 8 bits, Parity: none, Stop: 1 bit

4. **Save and reboot**

---

### Capture Serial Output

**If hardware has serial port:**

```bash
# On capture machine
./scripts/test-on-hardware.sh --serial /dev/ttyUSB0 --validate
```

**Without serial port:**
- Use display output
- Take photos/videos
- Document error messages

---

### Test Checklist

After boot, verify:

- [ ] System boots to kernel
- [ ] Bootloader message displayed
- [ ] Kernel banner shown
- [ ] PMM initialized
- [ ] VMM initialized
- [ ] Heap initialized
- [ ] IDT loaded
- [ ] Timer working
- [ ] No kernel panics
- [ ] System stable

---

## CPU Compatibility Testing

### Quick Test (4 CPU models)

```bash
./scripts/test-qemu-cpus.sh --quick
```

Tests:
- qemu64 (generic)
- Haswell (Intel)
- Skylake-Client (Intel)
- EPYC (AMD)

### Full Test (22 CPU models)

```bash
./scripts/test-qemu-cpus.sh
```

Tests all Intel and AMD CPU generations:
- Intel: Nehalem through Icelake
- AMD: Opteron G1-G5, EPYC generations

### Results

Results saved to:
- `build/cpu-test-results/summary.txt` - Summary
- `build/cpu-test-results/<cpu>-serial.log` - Individual logs
- `build/cpu-test-results/detailed-report.txt` - Full report

---

## Boot Media Creation

### USB Drive (Recommended)

```bash
# Create bootable USB
sudo ./scripts/create-usb.sh /dev/sdX --verify
```

**Requirements:**
- USB 2.0/3.0 flash drive
- 1GB+ capacity
- UEFI support

---

### CD/DVD

```bash
# Burn ISO to disc (Linux)
wodim -v dev=/dev/sr0 build/AutomationOS.iso

# Or use GUI tools
# - Brasero (Linux)
# - ImgBurn (Windows)
# - Disk Utility (macOS)
```

---

### Network Boot (PXE)

**Status:** Not yet implemented (planned for Phase 2)

---

## Serial Console Setup

### USB-to-Serial Adapter

1. **Connect adapter:**
   - Adapter → Test machine serial port
   - USB → Capture machine

2. **Find device:**
   ```bash
   # List serial devices
   ls -l /dev/ttyUSB*
   
   # Usually /dev/ttyUSB0
   ```

3. **Set permissions:**
   ```bash
   sudo chmod 666 /dev/ttyUSB0
   
   # Or add to dialout group
   sudo usermod -a -G dialout $USER
   ```

4. **Test connection:**
   ```bash
   screen /dev/ttyUSB0 115200
   ```

---

### Null-Modem Cable

For direct serial-to-serial connection:

1. **Connect:**
   - Test machine COM1 → Capture machine COM1
   - Use null-modem (crossover) cable

2. **Configure:**
   - Baud: 115200
   - Data: 8 bits
   - Parity: None
   - Stop: 1 bit
   - Flow control: None

3. **Capture:**
   ```bash
   ./scripts/test-on-hardware.sh --serial /dev/ttyS0
   ```

---

## Test Reporting

### Use Template

Download template:
- [docs/HARDWARE_TEST_REPORT_TEMPLATE.md](docs/HARDWARE_TEST_REPORT_TEMPLATE.md)

Fill in:
- Hardware specifications
- Test results
- Boot messages
- Issues encountered
- Serial console log

---

### Submit Report

**GitHub Issues:**
1. Go to: https://github.com/yourusername/AutomationOS/issues
2. Create new issue
3. Title: "Hardware Test: [CPU Model] - [Result]"
4. Attach:
   - Filled test report
   - Serial console log
   - Photos (optional)

**Email:**
- To: egotbrawlter@gmail.com
- Subject: "AutomationOS Hardware Test Report"
- Attach filled template and logs

---

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Hardware Tests

on: [push, pull_request]

jobs:
  hardware-test:
    runs-on: ubuntu-latest
    
    steps:
      - uses: actions/checkout@v2
      
      - name: Install dependencies
        run: |
          sudo apt install -y qemu-system-x86 x86_64-elf-gcc nasm
      
      - name: Build ISO
        run: make iso
      
      - name: Run hardware tests
        run: ./scripts/ci-hardware-test.sh --quick
      
      - name: Upload results
        if: always()
        uses: actions/upload-artifact@v2
        with:
          name: test-results
          path: build/ci-test-results/
```

---

### GitLab CI Example

```yaml
hardware-test:
  stage: test
  image: archlinux:latest
  
  before_script:
    - pacman -Sy --noconfirm qemu x86_64-elf-gcc nasm
  
  script:
    - make iso
    - ./scripts/ci-hardware-test.sh --quick
  
  artifacts:
    paths:
      - build/ci-test-results/
    when: always
```

---

## Troubleshooting

### Common Issues

**Issue: ISO not found**
```bash
# Solution: Build ISO first
make clean && make iso
```

**Issue: QEMU not found**
```bash
# Solution: Install QEMU
sudo apt install qemu-system-x86  # Ubuntu/Debian
sudo pacman -S qemu               # Arch Linux
brew install qemu                 # macOS
```

**Issue: Boot fails in VM**
```bash
# Solution: Check UEFI enabled
# - VirtualBox: Enable EFI (Special OSes)
# - VMware: Firmware → UEFI
# - Hyper-V: Use Generation 2 VM
```

**Issue: No serial output**
```bash
# Solution: Check serial port configuration
# - Verify device exists: ls -l /dev/ttyUSB*
# - Check permissions: sudo chmod 666 /dev/ttyUSB0
# - Verify baud rate: 115200
```

**Issue: USB won't boot**
```bash
# Solution: Verify UEFI settings
# 1. Boot mode: UEFI (not legacy BIOS)
# 2. Secure Boot: Disabled
# 3. Boot order: USB first
```

---

### Get Help

**Documentation:**
- [PLATFORM_SUPPORT.md](docs/PLATFORM_SUPPORT.md)
- [HARDWARE_COMPATIBILITY.md](docs/HARDWARE_COMPATIBILITY.md)
- [TROUBLESHOOTING_HARDWARE.md](docs/TROUBLESHOOTING_HARDWARE.md)
- [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)

**Support:**
- GitHub Issues: https://github.com/yourusername/AutomationOS/issues
- Email: egotbrawlter@gmail.com

---

## Testing Roadmap

### Phase 1 (Current) - Virtual Machines
- [x] QEMU testing
- [x] CPU compatibility testing
- [x] Automated test scripts
- [ ] VirtualBox testing
- [ ] VMware testing
- [ ] Hyper-V testing
- [ ] KVM testing

### Phase 2 (Q3 2026) - Physical Hardware
- [ ] 10+ different machines
- [ ] Intel desktop/laptop testing
- [ ] AMD desktop/laptop testing
- [ ] Server hardware testing
- [ ] Legacy hardware (pre-2015)
- [ ] Modern hardware (2020+)
- [ ] Network boot (PXE)

### Phase 3 (Q4 2026) - Advanced
- [ ] GPU compatibility
- [ ] AI accelerator testing
- [ ] Cluster testing
- [ ] Performance benchmarks
- [ ] Power management testing

---

## Contributing

We welcome hardware test results! To contribute:

1. **Test on your hardware**
2. **Fill out test report template**
3. **Capture serial console output**
4. **Submit via GitHub or email**

Your contributions help improve AutomationOS compatibility!

---

## License

[License TBD]

---

**Document Version:** 1.0  
**Last Updated:** 2026-05-26  
**Maintainer:** egotbrawlter@gmail.com
