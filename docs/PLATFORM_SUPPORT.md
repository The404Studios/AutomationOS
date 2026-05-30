# AutomationOS Platform Support

**Last Updated:** 2026-05-26  
**Version:** 0.1.0  
**Phase:** 1 - Core Foundation

---

## Table of Contents

1. [Supported Platforms](#supported-platforms)
2. [Virtual Machines](#virtual-machines)
3. [Physical Hardware](#physical-hardware)
4. [Architecture Support](#architecture-support)
5. [Boot Methods](#boot-methods)
6. [Platform Requirements](#platform-requirements)
7. [Platform-Specific Notes](#platform-specific-notes)

---

## Supported Platforms

### Current Support Matrix

| Platform Type | Status | Notes |
|---------------|--------|-------|
| **QEMU (x86_64)** | ✅ Full Support | Primary development platform |
| **VirtualBox** | 🔄 Testing | Generation 2 VM (UEFI) required |
| **VMware Workstation** | 🔄 Testing | UEFI support required |
| **Microsoft Hyper-V** | 🔄 Testing | Generation 2 VM (UEFI) required |
| **Linux KVM** | 🔄 Testing | Standard libvirt/virt-manager |
| **Physical x86_64** | ⏳ Planned | UEFI-capable systems |

---

## Virtual Machines

### QEMU ✅

**Status:** Fully Supported (Primary Platform)

**Versions:**
- QEMU 8.0+ (recommended)
- QEMU 7.x (compatible)

**Quick Start:**
```bash
# Build and run
make qemu

# With custom settings
qemu-system-x86_64 \
    -cdrom build/AutomationOS.iso \
    -m 4G \
    -smp 4 \
    -serial stdio \
    -no-reboot \
    -no-shutdown
```

**Tested CPU Models:**
- `qemu64` (default) - ✅
- `host` (KVM) - 🔄
- `IvyBridge` - ⏳
- `Haswell` - ⏳
- `Skylake-Client` - ⏳
- `Cascadelake-Server` - ⏳
- `EPYC` - ⏳
- `EPYC-Rome` - ⏳

**Features:**
- Serial console support ✅
- Graphics output ✅
- Multi-core support ✅
- High memory support (64GB+) ✅
- KVM acceleration ✅
- GDB debugging ✅

**Known Issues:**
- None

**Recommended Settings:**
```bash
# Minimum settings
-m 1G -smp 1

# Recommended settings
-m 4G -smp 4

# High-performance settings
-m 8G -smp 8 -enable-kvm
```

---

### VirtualBox 🔄

**Status:** Testing In Progress

**Versions:**
- VirtualBox 7.0+ (recommended)
- VirtualBox 6.1 (compatible)

**Setup Instructions:**

1. Create a new VM:
   - Type: Other
   - Version: Other/Unknown (64-bit)
   - Enable EFI (Special OSes)

2. Configure settings:
   - **System:**
     - Base Memory: 4096 MB (minimum 1024 MB)
     - Processor: 4 CPUs (minimum 1)
     - Enable PAE/NX
     - Enable Nested Paging
   - **Storage:**
     - Attach AutomationOS.iso to IDE controller
   - **Serial Ports:**
     - Enable Serial Port 1
     - Port Mode: File
     - Path: /path/to/serial.log

3. Boot from ISO

**Known Issues:**
- Not yet fully tested
- EFI support required

**Workarounds:**
- Use Generation 2 VM settings
- Enable EFI in VM settings

---

### VMware Workstation 🔄

**Status:** Testing In Progress

**Versions:**
- VMware Workstation 17.x (recommended)
- VMware Workstation 16.x (compatible)

**Setup Instructions:**

1. Create a new VM:
   - Guest OS: Other Linux 5.x kernel 64-bit
   - Firmware: UEFI

2. Configure settings:
   - **Memory:** 4096 MB (minimum 1024 MB)
   - **Processors:** 4 cores (minimum 1)
   - **CD/DVD:** Use ISO image (AutomationOS.iso)
   - **Serial Port:** Add serial port → Use output file

3. Boot from ISO

**Known Issues:**
- Not yet fully tested
- UEFI firmware required

**Workarounds:**
- Use UEFI firmware mode (not legacy BIOS)

---

### Microsoft Hyper-V 🔄

**Status:** Testing In Progress

**Versions:**
- Windows 11 Hyper-V (recommended)
- Windows 10 Hyper-V (compatible)
- Windows Server 2019/2022 (compatible)

**Setup Instructions:**

1. Open Hyper-V Manager

2. Create a new VM:
   - Generation: **Generation 2** (UEFI required)
   - Memory: 4096 MB (minimum 1024 MB)
   - Virtual Processor: 4 (minimum 1)

3. Configure settings:
   - **SCSI Controller:** Add DVD Drive → Insert AutomationOS.iso
   - **Security:** Disable Secure Boot
   - **COM 1:** Add serial port → Named pipe or file

4. Boot from ISO

**Known Issues:**
- Generation 1 VMs (BIOS) not supported
- Secure Boot must be disabled

**Workarounds:**
- Use Generation 2 VMs only
- Disable Secure Boot in VM settings

---

### Linux KVM 🔄

**Status:** Testing In Progress

**Recommended Interface:**
- virt-manager (GUI)
- virsh (CLI)

**Setup with virt-manager:**

1. Create a new VM:
   - Architecture: x86_64
   - Firmware: UEFI x86_64

2. Configure:
   - **Memory:** 4096 MB
   - **CPUs:** 4
   - **Boot media:** AutomationOS.iso

3. Before installation:
   - Add hardware → Serial console
   - Boot options: Enable UEFI

4. Boot from ISO

**Setup with virsh:**

```bash
# Create VM domain XML
virt-install \
    --name AutomationOS \
    --memory 4096 \
    --vcpus 4 \
    --cdrom /path/to/AutomationOS.iso \
    --os-variant generic \
    --boot uefi \
    --graphics spice \
    --console pty,target_type=serial
```

**Known Issues:**
- Not yet fully tested

**Workarounds:**
- Use UEFI firmware (OVMF)

---

## Physical Hardware

### Supported Systems ⏳

**Status:** Testing Planned

**Requirements:**
- x86_64 CPU (Intel or AMD)
- UEFI firmware
- 1GB+ RAM (4GB recommended)
- Serial port (optional, for debugging)

**Tested Hardware:**
- None yet (pending hardware access)

**Installation Methods:**

1. **USB Boot (UEFI):**
   ```bash
   # Create bootable USB
   sudo dd if=build/AutomationOS.iso of=/dev/sdX bs=4M status=progress
   sudo sync
   ```

2. **CD/DVD Boot:**
   - Burn ISO to disc
   - Boot from disc

**Known Issues:**
- Not yet tested on physical hardware

**Workarounds:**
- Test in VM first

---

## Architecture Support

### x86_64 (Intel/AMD) ✅

**Status:** Fully Supported

**Minimum CPU Requirements:**
- x86_64 instruction set (64-bit mode)
- Long mode support
- SSE2 (required by GCC)

**Optional Features:**
- SSE3, SSE4.1, SSE4.2 (enhanced performance)
- AVX, AVX2 (SIMD acceleration)
- AVX-512 (AI workloads, future)
- VT-x / AMD-V (virtualization support)

**Tested CPUs:**
- QEMU qemu64 emulation ✅
- Intel Core series (pending)
- AMD Ryzen series (pending)
- Intel Xeon (pending)
- AMD EPYC (pending)

---

### ARM64 (AArch64) ❌

**Status:** Not Supported

**Reason:** Phase 1 focuses on x86_64 only

**Future Plans:** Phase 3+ may add ARM64 support for embedded/edge devices

---

### RISC-V ❌

**Status:** Not Supported

**Reason:** Phase 1 focuses on x86_64 only

**Future Plans:** Possible Phase 4+ for experimental builds

---

## Boot Methods

### UEFI Boot ✅

**Status:** Fully Supported (Primary Method)

**Features:**
- Custom UEFI bootloader (AutoBoot)
- GOP (Graphics Output Protocol) support
- Large memory support
- Secure Boot compatible (not enforced)

**Requirements:**
- UEFI firmware 2.0+
- 64-bit UEFI only (no 32-bit UEFI)

---

### Legacy BIOS (MBR) ❌

**Status:** Not Supported

**Reason:** AutomationOS is designed for modern UEFI systems only

**Workarounds:**
- Use UEFI boot mode
- Update system firmware to UEFI if possible

---

### Network Boot (PXE) ⏳

**Status:** Planned (Phase 2)

**Features (Planned):**
- TFTP boot
- HTTP boot
- iSCSI root

**Timeline:** Q3 2026

---

## Platform Requirements

### Minimum Requirements

| Component | Requirement |
|-----------|-------------|
| CPU | x86_64 with SSE2 |
| RAM | 1GB |
| Storage | 100MB (for ISO) |
| Firmware | UEFI 2.0+ |
| Display | Optional (serial console supported) |

### Recommended Requirements

| Component | Requirement |
|-----------|-------------|
| CPU | x86_64 with AVX2, 4+ cores |
| RAM | 4GB |
| Storage | 1GB (for future expansion) |
| Firmware | UEFI 2.3+ |
| Display | 1024x768 or higher |

### Optimal Requirements (AI Workloads)

| Component | Requirement |
|-----------|-------------|
| CPU | x86_64 with AVX-512, 8+ cores |
| RAM | 16GB+ |
| GPU | CUDA-capable GPU (Phase 3) |
| Storage | 10GB+ NVMe SSD |
| Network | 1Gbps+ |

---

## Platform-Specific Notes

### QEMU Notes

**Performance Tips:**
- Use KVM acceleration: `-enable-kvm`
- Use virtio devices: `-device virtio-blk-pci`
- Use host CPU model: `-cpu host`

**Debugging:**
- GDB server: `-s -S`
- Serial logging: `-serial file:serial.log`
- Monitor: `-monitor stdio`

---

### VirtualBox Notes

**Known Limitations:**
- EFI support can be quirky
- Serial port configuration requires manual setup

**Tips:**
- Enable PAE/NX
- Enable Nested Paging
- Use VBoxManage for advanced settings

---

### VMware Notes

**Known Limitations:**
- UEFI firmware must be explicitly enabled

**Tips:**
- Use VMXNET3 for network (Phase 2)
- Enable nested virtualization for testing

---

### Hyper-V Notes

**Known Limitations:**
- Generation 1 VMs not supported (BIOS only)
- Secure Boot must be disabled

**Tips:**
- Use Generation 2 VMs
- Enable dynamic memory (optional)
- Use Enhanced Session Mode for better performance

---

### KVM Notes

**Known Limitations:**
- OVMF (UEFI firmware) must be installed

**Tips:**
- Install ovmf package: `apt install ovmf` or `pacman -S edk2-ovmf`
- Use virtio devices for best performance
- Enable KVM acceleration

---

## Testing Tools

### Automated Testing

```bash
# Test on QEMU
make qemu

# Run integration tests
make test

# Test with specific settings
./scripts/run-qemu.sh -m 8G -smp 8
```

### Manual Testing

```bash
# Boot in VirtualBox
./scripts/test-hardware.sh --vm virtualbox

# Boot in VMware
./scripts/test-hardware.sh --vm vmware

# Boot in Hyper-V (PowerShell)
./scripts/test-hardware.ps1 -VM hyperv

# Create bootable USB
./scripts/create-usb.sh /dev/sdX
```

### Hardware Testing

```bash
# Test on real hardware with serial capture
./scripts/test-on-hardware.sh --serial /dev/ttyUSB0 --log hardware-test.log
```

---

## Troubleshooting

### Common Issues

**Problem:** VM won't boot  
**Solution:** Ensure UEFI firmware is enabled (not legacy BIOS)

**Problem:** Black screen, no output  
**Solution:** Check serial console output for errors

**Problem:** Kernel panic on boot  
**Solution:** Check RAM size (minimum 1GB), review serial log

**Problem:** Secure Boot error  
**Solution:** Disable Secure Boot in VM/BIOS settings

**Problem:** ISO not found  
**Solution:** Run `make iso` first to build the ISO image

For more troubleshooting, see [TROUBLESHOOTING.md](TROUBLESHOOTING.md) and [TROUBLESHOOTING_HARDWARE.md](TROUBLESHOOTING_HARDWARE.md).

---

## Contributing Hardware Test Results

We welcome hardware test results! To contribute:

1. Test AutomationOS on your hardware
2. Capture serial console output
3. Document hardware specs (CPU, RAM, etc.)
4. Report results via GitHub issue or email

**Template:**

```markdown
## Hardware Test Report

**Hardware:**
- CPU: [model]
- RAM: [size]
- Storage: [type]
- Graphics: [model]
- Firmware: [UEFI version]

**VM/Physical:** [VM or physical]
**Boot Method:** [USB/CD/Network]

**Result:** ✅ Works / ⚠️ Partial / ❌ Broken

**Notes:**
[Additional details]

**Serial Log:**
[Attach serial.log]
```

---

## Future Platform Support

### Phase 2 (Q3 2026)
- [ ] Expanded VM testing (VirtualBox, VMware, Hyper-V, KVM)
- [ ] Physical hardware testing (10+ machines)
- [ ] Network boot (PXE)
- [ ] USB boot refinement

### Phase 3 (Q4 2026)
- [ ] ARM64 support (Raspberry Pi, embedded)
- [ ] GPU acceleration (NVIDIA/AMD)
- [ ] High-end server support
- [ ] Cluster/distributed testing

### Phase 4 (2027)
- [ ] RISC-V experimental support
- [ ] Cloud instance images (AWS, Azure, GCP)
- [ ] Container support (Docker, Kubernetes)

---

## Contact

For platform support questions:

- GitHub Issues: [AutomationOS Issues](https://github.com/yourusername/AutomationOS/issues)
- Email: egotbrawlter@gmail.com

---

**Documentation Status:** ✅ Complete  
**Last Reviewed:** 2026-05-26
