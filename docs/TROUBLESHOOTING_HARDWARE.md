# AutomationOS Hardware Troubleshooting Guide

**Last Updated:** 2026-05-26  
**Version:** 0.1.0

---

## Table of Contents

1. [Quick Diagnostics](#quick-diagnostics)
2. [Boot Issues](#boot-issues)
3. [Virtual Machine Issues](#virtual-machine-issues)
4. [Physical Hardware Issues](#physical-hardware-issues)
5. [Memory Issues](#memory-issues)
6. [CPU Issues](#cpu-issues)
7. [Display Issues](#display-issues)
8. [Serial Console Issues](#serial-console-issues)
9. [Boot Media Issues](#boot-media-issues)
10. [Advanced Debugging](#advanced-debugging)

---

## Quick Diagnostics

### Step 1: Check Prerequisites

```bash
# Verify ISO exists
ls -lh build/AutomationOS.iso

# Verify QEMU installation
qemu-system-x86_64 --version

# Verify cross-compiler
x86_64-elf-gcc --version
```

### Step 2: Test in QEMU First

```bash
# Minimal test (fastest)
make qemu

# With serial output capture
qemu-system-x86_64 \
    -cdrom build/AutomationOS.iso \
    -m 1G \
    -serial file:serial.log \
    -display none
    
# Check serial output
cat serial.log
```

### Step 3: Check Boot Messages

Look for these critical messages in serial output:

```
✅ Expected:
[BOOT] AutomationOS Bootloader
[BOOT] Loading kernel...
[KERNEL] AutomationOS kernel starting...
[PMM] Physical Memory Manager initialized
[VMM] Virtual Memory Manager initialized
[HEAP] Kernel heap initialized
[GDT] Global Descriptor Table loaded
[IDT] Interrupt Descriptor Table loaded
[PIT] Timer initialized

❌ Bad Signs:
- Triple fault (CPU resets)
- Page fault (memory access error)
- General protection fault (segmentation error)
- No output (bootloader failure)
```

---

## Boot Issues

### Issue: System Won't Boot

**Symptoms:**
- Black screen
- No serial output
- VM/system resets immediately

**Diagnosis:**

1. **Check firmware mode:**
   ```bash
   # UEFI required, not legacy BIOS
   # In VM settings, ensure UEFI/EFI is enabled
   ```

2. **Verify ISO integrity:**
   ```bash
   # Check ISO size (should be > 100MB)
   ls -lh build/AutomationOS.iso
   
   # Rebuild if needed
   make clean && make iso
   ```

3. **Check boot order:**
   - Ensure CD/DVD is first boot device
   - Or ensure USB is first boot device

**Solutions:**

- **Enable UEFI boot** in VM/BIOS settings
- **Disable Secure Boot** (not supported yet)
- **Increase timeout** for slow systems
- **Use QEMU first** to validate ISO

---

### Issue: Bootloader Starts But Kernel Doesn't Load

**Symptoms:**
- See "AutomationOS Bootloader" message
- No kernel messages
- System hangs or reboots

**Diagnosis:**

```bash
# Check serial output
cat serial.log | grep -i "boot\|load\|kernel"

# Expected messages:
# [BOOT] AutomationOS Bootloader
# [BOOT] Loading kernel...
# [BOOT] Kernel loaded at 0xFFFFFFFF80000000
# [KERNEL] AutomationOS kernel starting...
```

**Solutions:**

1. **Check kernel file:**
   ```bash
   # Verify kernel exists
   ls -lh build/kernel.elf
   
   # Check kernel is 64-bit ELF
   file build/kernel.elf
   # Should say: ELF 64-bit LSB executable, x86-64
   ```

2. **Rebuild bootloader and kernel:**
   ```bash
   make clean
   make bootloader
   make kernel
   make iso
   ```

3. **Check memory size:**
   ```bash
   # Try with more memory
   qemu-system-x86_64 -cdrom build/AutomationOS.iso -m 4G
   ```

---

### Issue: Kernel Panic on Boot

**Symptoms:**
- Kernel starts but crashes
- "KERNEL PANIC" message
- System halts

**Diagnosis:**

```bash
# Check panic message
cat serial.log | grep -i "panic\|fault\|error"
```

**Common Panics:**

1. **Page Fault:**
   ```
   [PANIC] Page fault at 0xXXXXXXXX
   ```
   - Cause: Invalid memory access
   - Solution: Check memory initialization, verify page tables

2. **Double Fault:**
   ```
   [PANIC] Double fault
   ```
   - Cause: Exception handler caused another exception
   - Solution: Check stack setup, verify GDT/IDT

3. **Triple Fault:**
   - System just resets, no message
   - Cause: Double fault handler failed
   - Solution: Check early kernel initialization

**Solutions:**

1. **Enable debug symbols:**
   ```bash
   # Rebuild with debugging
   make clean
   make kernel
   
   # Run with GDB
   make qemu-debug
   ```

2. **Increase memory:**
   ```bash
   # Try with more RAM
   qemu-system-x86_64 -cdrom build/AutomationOS.iso -m 8G
   ```

3. **Check prerequisites:**
   - CPU must support x86_64 (64-bit mode)
   - CPU must support SSE2 (required by GCC)
   - UEFI firmware must be 64-bit

---

## Virtual Machine Issues

### QEMU Issues

**Issue: QEMU won't start**

```bash
# Check QEMU installation
qemu-system-x86_64 --version

# Install if missing
# Ubuntu/Debian:
sudo apt install qemu-system-x86

# Arch Linux:
sudo pacman -S qemu

# macOS:
brew install qemu
```

**Issue: KVM acceleration error**

```
Could not access KVM kernel module: Permission denied
```

**Solution:**
```bash
# Add user to kvm group (Linux)
sudo usermod -a -G kvm $USER

# Reload groups (or logout/login)
newgrp kvm

# Verify KVM access
ls -l /dev/kvm
```

---

### VirtualBox Issues

**Issue: EFI boot not available**

**Solution:**
1. Enable EFI in VM settings:
   - System → Motherboard → Enable EFI (Special OSes)

2. Ensure 64-bit OS type selected:
   - General → Basic → Version: Other/Unknown (64-bit)

**Issue: Serial port not working**

**Solution:**
1. Add serial port:
   - Settings → Serial Ports → Port 1
   - Enable Serial Port
   - Port Mode: File
   - Path/Address: /path/to/serial.log

2. Restart VM

---

### VMware Issues

**Issue: BIOS boot instead of UEFI**

**Solution:**
1. Edit VM settings:
   - Options → Advanced → Firmware Type: **UEFI**

2. Restart VM

**Issue: Black screen on boot**

**Solution:**
1. Check serial output (if configured)
2. Try changing graphics mode:
   - Settings → Display → Graphics Memory: 128MB+
   - Settings → Display → 3D Acceleration: Off

---

### Hyper-V Issues

**Issue: Generation 1 VM not supported**

**Solution:**
- AutomationOS requires UEFI
- Use **Generation 2 VM** only
- Cannot convert Gen 1 to Gen 2 - must create new VM

**Issue: Secure Boot error**

```
Secure Boot Violation
```

**Solution:**
1. Disable Secure Boot:
   - VM Settings → Security → Secure Boot: **Disabled**

2. Restart VM

**Issue: Serial port not available**

**Solution:**
1. Add COM port:
   - VM Settings → Hardware → Add Hardware → COM 1
   - Configure as file or named pipe

---

### KVM Issues

**Issue: OVMF firmware not found**

```
Could not find UEFI firmware
```

**Solution:**
```bash
# Install OVMF (UEFI firmware)
# Ubuntu/Debian:
sudo apt install ovmf

# Arch Linux:
sudo pacman -S edk2-ovmf

# Fedora:
sudo dnf install edk2-ovmf
```

**Issue: virt-manager won't boot ISO**

**Solution:**
1. Create VM with these settings:
   - Architecture: x86_64
   - Firmware: UEFI x86_64 (select OVMF firmware)
   - Boot: CD-ROM (AutomationOS.iso)

2. Before first boot:
   - Add Hardware → Console → Serial

---

## Physical Hardware Issues

### Issue: USB Boot Not Working

**Diagnosis:**
1. Verify USB device:
   ```bash
   # List USB devices
   lsblk
   
   # Check USB is detected
   sudo fdisk -l /dev/sdX
   ```

2. Verify UEFI boot mode:
   - Enter BIOS/UEFI settings (usually F2, Del, F12)
   - Check boot mode: UEFI (not Legacy/CSM)
   - Disable Secure Boot

**Solutions:**

1. **Recreate USB:**
   ```bash
   # Ensure USB is unmounted
   sudo umount /dev/sdX*
   
   # Write ISO
   sudo dd if=build/AutomationOS.iso of=/dev/sdX bs=4M status=progress oflag=sync
   
   # Sync filesystem
   sudo sync
   ```

2. **Try different USB port:**
   - Use USB 2.0 port (more compatible)
   - Try front panel USB
   - Try rear panel USB

3. **Update BIOS/UEFI firmware:**
   - Check manufacturer website
   - Flash latest firmware

---

### Issue: Serial Console Not Working

**Diagnosis:**
1. Verify serial cable:
   - Check physical connection
   - Try different cable
   - Test with another device

2. Check serial port settings:
   ```bash
   # List serial devices
   ls -l /dev/ttyS* /dev/ttyUSB*
   
   # Test serial port
   sudo minicom -D /dev/ttyUSB0 -b 115200
   ```

**Solutions:**

1. **Configure serial port:**
   ```bash
   # Set baud rate (115200)
   stty -F /dev/ttyUSB0 115200
   
   # Read from serial port
   cat /dev/ttyUSB0
   ```

2. **Use screen/minicom:**
   ```bash
   # Using screen
   sudo screen /dev/ttyUSB0 115200
   
   # Using minicom
   sudo minicom -D /dev/ttyUSB0 -b 115200
   ```

3. **Check BIOS settings:**
   - Enable serial port in BIOS
   - Set to COM1, 115200 baud
   - Disable flow control

---

### Issue: No Display Output

**Diagnosis:**
- System boots (fans spin, disk activity)
- No video output
- Monitor shows "No signal"

**Solutions:**

1. **Use serial console:**
   - Connect serial cable
   - Capture boot messages
   - Debug via serial only

2. **Check graphics card:**
   - Try different output (HDMI, DisplayPort, VGA)
   - Try integrated graphics (if available)
   - Remove discrete GPU, use onboard

3. **Check BIOS settings:**
   - Set primary display
   - Disable fast boot
   - Enable legacy option ROM

---

## Memory Issues

### Issue: Out of Memory

**Symptoms:**
```
[PANIC] Out of memory
[PMM] Failed to allocate physical page
[HEAP] Allocation failed
```

**Solutions:**

1. **Increase RAM:**
   ```bash
   # QEMU
   qemu-system-x86_64 -m 4G ...
   
   # Physical: Add more RAM
   ```

2. **Check minimum requirements:**
   - Minimum: 1GB
   - Recommended: 4GB

3. **Reduce memory usage:**
   - Single CPU core
   - Minimal boot

---

### Issue: Memory Detection Failure

**Symptoms:**
```
[PMM] No usable memory found
[BOOT] Memory map empty
```

**Solutions:**

1. **Check UEFI memory map:**
   - Bootloader issue
   - Rebuild bootloader

2. **Try different VM:**
   - Test in QEMU first
   - Check firmware version

---

## CPU Issues

### Issue: Unsupported CPU

**Symptoms:**
```
[PANIC] CPU does not support x86_64
[PANIC] Long mode not supported
```

**Solutions:**

1. **Verify CPU:**
   ```bash
   # Linux
   lscpu | grep -E "Architecture|op-mode"
   
   # Should show: x86_64, 64-bit
   ```

2. **Check virtualization:**
   - QEMU: Use `-cpu qemu64` (default)
   - VirtualBox: Enable PAE/NX, 64-bit guest

3. **Enable 64-bit mode:**
   - BIOS: Enable VT-x (Intel) or AMD-V (AMD)
   - BIOS: Enable 64-bit support

---

### Issue: CPU Feature Not Supported

**Symptoms:**
```
[WARNING] SSE3 not supported
[WARNING] AVX not supported
```

**Solutions:**

- AutomationOS requires only x86_64 and SSE2
- Warnings are informational only
- Optional features (AVX, etc.) are not required

---

## Display Issues

### Issue: Framebuffer Not Initialized

**Symptoms:**
```
[WARNING] GOP framebuffer not available
[FB] No display found
```

**Solutions:**

1. **Use serial console:**
   - Framebuffer is optional
   - Serial console always works

2. **Check UEFI GOP support:**
   - Modern UEFI has GOP
   - Legacy BIOS does not (not supported)

3. **Enable graphics in VM:**
   - VirtualBox: Graphics Controller: VMSVGA
   - QEMU: `-vga std`

---

## Serial Console Issues

### Issue: No Serial Output

**Solutions:**

1. **QEMU:**
   ```bash
   # Use stdio (console)
   qemu-system-x86_64 ... -serial stdio
   
   # Use file
   qemu-system-x86_64 ... -serial file:serial.log
   
   # Check output
   cat serial.log
   ```

2. **VirtualBox:**
   - Settings → Serial Ports → Port 1
   - Enable Serial Port
   - Port Mode: File
   - Path: /path/to/serial.log

3. **VMware:**
   - Add Hardware → Serial Port
   - Use output file

4. **Physical hardware:**
   - Connect null-modem cable
   - Use USB-to-serial adapter
   - Configure BIOS serial settings

---

### Issue: Garbage Characters in Serial Output

**Symptoms:**
- Unreadable characters
- Corrupted text
- Random symbols

**Solutions:**

1. **Check baud rate:**
   ```bash
   # Must be 115200 (default)
   stty -F /dev/ttyUSB0 115200
   ```

2. **Check parity/stop bits:**
   - 8N1 (8 data bits, no parity, 1 stop bit)
   - No flow control

3. **Check cable:**
   - Use null-modem cable for crossover
   - Or use USB-to-serial adapter

---

## Boot Media Issues

### Issue: ISO Too Large for CD

**Symptoms:**
- ISO > 700MB (CD-R size)

**Solutions:**

1. **Use DVD:**
   - DVD-R: 4.7GB capacity
   - Burn to DVD instead

2. **Use USB:**
   - Larger capacity
   - Faster boot

3. **Optimize ISO:**
   - Remove debug symbols
   - Reduce kernel size

---

### Issue: USB Write Failed

**Symptoms:**
```bash
dd: error writing '/dev/sdX': No space left on device
```

**Solutions:**

1. **Check USB size:**
   ```bash
   # Must be larger than ISO
   ls -lh build/AutomationOS.iso
   lsblk | grep sdX
   ```

2. **Use larger USB:**
   - Minimum 1GB
   - Recommended 4GB+

3. **Verify USB device:**
   ```bash
   # Double-check device name!
   lsblk
   
   # Use correct device (e.g., /dev/sdb, not /dev/sdb1)
   ```

---

## Advanced Debugging

### Enable Kernel Debug Output

**Rebuild with debug symbols:**
```bash
# Add to kernel Makefile
CFLAGS += -g -O0 -DDEBUG

# Rebuild
make clean && make kernel
```

### GDB Debugging

**Start QEMU with GDB server:**
```bash
make qemu-debug

# Or manually:
qemu-system-x86_64 \
    -cdrom build/AutomationOS.iso \
    -m 4G \
    -s -S \
    -serial stdio
```

**Attach GDB:**
```bash
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) continue
```

**Set breakpoints:**
```bash
(gdb) break kernel_main
(gdb) break page_fault_handler
(gdb) continue
```

### Capture Serial Output

**QEMU:**
```bash
qemu-system-x86_64 \
    -cdrom build/AutomationOS.iso \
    -serial file:serial.log
    
# View output
tail -f serial.log
```

**Physical hardware:**
```bash
# Capture with screen
sudo screen -L /dev/ttyUSB0 115200

# Or with cat
sudo cat /dev/ttyUSB0 > serial.log
```

### Check Hardware Detection

**In QEMU:**
```bash
# List detected devices
qemu-system-x86_64 -cdrom build/AutomationOS.iso -device help

# Check CPU model
qemu-system-x86_64 -cpu help
```

**Physical hardware:**
```bash
# Check UEFI settings
# - Enter BIOS/UEFI setup (F2, Del, F12)
# - View hardware information
# - Check CPU, RAM, devices
```

---

## Getting Help

If you've tried these solutions and still have issues:

1. **Capture diagnostics:**
   ```bash
   # Build logs
   make clean && make all 2>&1 | tee build.log
   
   # Serial output
   cat serial.log > diagnostics.log
   
   # System info
   uname -a >> diagnostics.log
   qemu-system-x86_64 --version >> diagnostics.log
   ```

2. **Report issue:**
   - GitHub Issues: [AutomationOS Issues](https://github.com/yourusername/AutomationOS/issues)
   - Include: Hardware specs, serial log, error messages
   - Email: egotbrawlter@gmail.com

3. **Provide details:**
   - Hardware: CPU, RAM, firmware version
   - VM: Platform, version, settings
   - Boot method: USB, CD, network
   - Error messages: Full serial output
   - Steps to reproduce

---

## Additional Resources

- [PLATFORM_SUPPORT.md](PLATFORM_SUPPORT.md) - Supported platforms
- [HARDWARE_COMPATIBILITY.md](HARDWARE_COMPATIBILITY.md) - Tested hardware
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) - General troubleshooting
- [BUILD_GUIDE.md](BUILD_GUIDE.md) - Build system guide
- [ARCHITECTURE.md](ARCHITECTURE.md) - System architecture

---

**Last Updated:** 2026-05-26  
**Status:** ✅ Complete
