# AutomationOS Boot Test Execution Guide
## Step-by-Step Instructions for First Boot

**Date:** 2026-05-26  
**Purpose:** Execute the first complete boot test of AutomationOS  
**Estimated Time:** 1-2 hours (including toolchain setup)

---

## Prerequisites Checklist

Before running the boot test, ensure you have:

- [ ] Windows WSL2 or native Linux environment
- [ ] At least 10GB free disk space
- [ ] Internet connection (for downloading toolchain)
- [ ] Administrator/sudo access

---

## Phase 1: Install Build Toolchain (30 minutes)

### Option A: MSys2/MinGW (Windows)

```bash
# Update package database
pacman -Syu

# Install build essentials
pacman -S base-devel

# Install cross-compiler
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-binutils

# Install assembler
pacman -S nasm

# Install QEMU
pacman -S mingw-w64-x86_64-qemu

# Install build tools
pacman -S python3
pacman -S make

# Verify installation
x86_64-w64-mingw32-gcc --version
nasm -version
qemu-system-x86_64 --version
```

### Option B: Use Setup Script (Linux/WSL)

```bash
cd /c/Users/wilde/Desktop/Kernel

# Check what's needed
bash scripts/setup-toolchain.sh --check

# Install/build toolchain
bash scripts/setup-toolchain.sh --build --prefix=$HOME/cross

# Add to PATH
export PATH="$HOME/cross/bin:$PATH"
echo 'export PATH="$HOME/cross/bin:$PATH"' >> ~/.bashrc

# Verify
x86_64-elf-gcc --version
nasm -version
qemu-system-x86_64 --version
```

### Option C: Native Linux Package Manager

**Ubuntu/Debian:**
```bash
sudo apt update
sudo apt install build-essential nasm python3 qemu-system-x86 xorriso
sudo apt install gcc-x86-64-elf binutils-x86-64-elf
```

**Arch Linux:**
```bash
sudo pacman -Syu
sudo pacman -S base-devel nasm python qemu xorriso
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils
```

### Verification Test

Run this to confirm everything is installed:

```bash
cd /c/Users/wilde/Desktop/Kernel

# Check for required tools
which x86_64-elf-gcc || which x86_64-w64-mingw32-gcc
which nasm
which qemu-system-x86_64
which python3
which make
which xorriso

# If all commands return paths, you're ready!
```

---

## Phase 2: Build AutomationOS (5-10 minutes)

### Step 1: Clean Previous Builds

```bash
cd /c/Users/wilde/Desktop/Kernel

# Remove any old build artifacts
make clean

# Verify clean
ls build/  # Should be empty or not exist
```

### Step 2: Configure Build (if needed)

Check `Makefile` to ensure correct compiler:

```bash
# If using x86_64-w64-mingw32-gcc instead of x86_64-elf-gcc:
# Edit Makefile line 8:
# CC = x86_64-w64-mingw32-gcc

# Or use environment variable:
export CC=x86_64-w64-mingw32-gcc
```

### Step 3: Build Everything

```bash
# Full build (bootloader + kernel + userspace + ISO)
make all

# Expected output:
# [CC] kernel/kernel.c
# [CC] kernel/arch/x86_64/gdt.c
# [CC] kernel/core/mem/pmm.c
# ... (many files) ...
# [LD] kernel.elf
# [ISO] AutomationOS.iso

# Build should complete in 20-60 seconds
```

### Step 4: Verify Build Artifacts

```bash
# Check that all files were created
ls -lh build/BOOTX64.EFI     # Bootloader
ls -lh build/kernel.elf      # Kernel
ls -lh build/AutomationOS.iso # Bootable ISO

# Check ISO size (should be 10-20 MB)
du -h build/AutomationOS.iso
```

### Troubleshooting Build Issues

**Problem: Compiler not found**
```bash
# Check PATH
echo $PATH | grep cross

# Verify compiler exists
which x86_64-elf-gcc

# If not found, repeat Phase 1
```

**Problem: Missing dependencies**
```bash
# Check what failed
make clean
make all 2>&1 | tee build.log

# Look for "error:" messages
grep "error:" build.log

# Common fixes:
# - Missing headers: Check toolchain installation
# - Wrong compiler: Update Makefile CC variable
# - Permissions: Run as admin/sudo
```

**Problem: Linker errors**
```bash
# Verify linker exists
which x86_64-elf-ld

# Check linker script
cat boot/boot.ld
cat kernel/arch/x86_64/linker.ld
```

---

## Phase 3: Run Boot Test (5 minutes)

### Test 1: Automated Integration Test

```bash
cd /c/Users/wilde/Desktop/Kernel

# Run full integration test suite
make test

# This will:
# 1. Boot AutomationOS in QEMU (headless)
# 2. Capture serial output
# 3. Validate boot messages
# 4. Check for subsystem initialization
# 5. Report pass/fail

# Expected output:
# ==================================================
# AutomationOS Boot Integration Test
# ==================================================
# 
# Checking prerequisites...
#   ✓ All prerequisites met
# Running boot tests...
#   ✓ Kernel banner printed
#   ✓ Physical Memory Manager initialized
#   ✓ Virtual Memory Manager initialized
#   ✓ Kernel heap initialized
#   ✓ Global Descriptor Table loaded
#   ? Init process started  # May fail (known issue)
# 
# ==================================================
#   Results: X/Y tests passed
# ==================================================
```

### Test 2: Interactive QEMU Boot

```bash
# Boot with graphics
make qemu

# This opens QEMU window showing:
# - Boot menu (5 second timeout)
# - Kernel boot messages
# - Console output

# Use keyboard to interact:
# - Arrow keys: Navigate boot menu
# - Enter: Boot immediately
# - Esc: Boot options

# Press Ctrl-Alt-Q to exit QEMU
```

### Test 3: Boot with Serial Console

```bash
# Boot with serial output to terminal
bash scripts/run-qemu.sh --no-display

# Serial output appears in terminal
# Press Ctrl-A then X to exit
```

### Test 4: Debug Mode Boot

```bash
# Start with GDB support
make qemu-debug

# In another terminal, attach GDB:
gdb build/kernel.elf

# GDB commands available:
# (gdb) target remote :1234
# (gdb) break kernel_main
# (gdb) continue
# (gdb) backtrace
# (gdb) info registers
```

---

## Phase 4: Analyze Boot Results (10 minutes)

### Check Serial Log

```bash
# View complete boot log
cat build/serial.log

# Expected messages (in order):
# 1. Kernel banner
# 2. Serial init
# 3. GDT init
# 4. Memory init (PMM, VMM, Heap)
# 5. Interrupt init (IDT)
# 6. Syscall init
# 7. Timer init
# 8. Drivers init (Framebuffer, PS/2)
# 9. Process management init
# 10. Scheduler init
# 11. Boot time report
# 12. "Starting init process..."
# 13. EITHER: Desktop appears OR "Init not yet implemented"
```

### Measure Boot Time

```bash
# Extract boot time from log
grep "Total boot time" build/serial.log

# Should show something like:
# [BOOT] Total boot time: 4824000000 cycles (2.01 ms)

# This is KERNEL boot time only
# Total boot time = UEFI (~1s) + Bootloader (~0.3s) + Kernel (~2s) + Init (~0.5s)
```

### Check for Errors

```bash
# Search for error messages
grep -i "error\|fail\|panic" build/serial.log

# Search for warnings
grep -i "warn" build/serial.log

# Check for known blockers
grep "Init not yet implemented" build/serial.log  # Known issue
```

### Verify Subsystems

Check that all subsystems initialized:

```bash
# Create verification script
cat > verify_boot.sh << 'EOF'
#!/bin/bash
log="build/serial.log"

echo "Verifying AutomationOS Boot..."
echo "================================"

check() {
    if grep -q "$1" "$log"; then
        echo "✓ $2"
        return 0
    else
        echo "✗ $2"
        return 1
    fi
}

check "AutomationOS v" "Kernel banner"
check "Serial.*initialized" "Serial driver"
check "GDT" "Global Descriptor Table"
check "PMM.*initialized" "Physical Memory Manager"
check "VMM.*initialized" "Virtual Memory Manager"
check "Heap.*initialized" "Kernel heap"
check "IDT" "Interrupt Descriptor Table"
check "SYSCALL.*initialized" "System calls"
check "Timer.*initialized" "PIT timer"
check "Framebuffer" "Graphics driver"
check "Keyboard.*initialized" "PS/2 keyboard"
check "Scheduler.*initialized" "Process scheduler"
check "Free memory" "Memory report"
check "Total boot time" "Boot time measurement"

echo "================================"
EOF

chmod +x verify_boot.sh
./verify_boot.sh
```

---

## Phase 5: Document Results (15 minutes)

### Create Boot Report

```bash
cat > FIRST_BOOT_RESULTS.md << EOF
# AutomationOS First Boot Results

**Date:** $(date)
**Tester:** $(whoami)
**Platform:** $(uname -a)

## Build Information

- **Compiler:** $(x86_64-elf-gcc --version | head -1)
- **NASM:** $(nasm -version)
- **Build Time:** X seconds
- **ISO Size:** $(du -h build/AutomationOS.iso)

## Boot Test Results

### Test Environment
- **Emulator:** QEMU $(qemu-system-x86_64 --version | head -1)
- **RAM:** 4GB
- **CPUs:** 4 cores
- **Display:** [Graphics/VNC/Headless]

### Boot Success Checklist

- [ ] Bootloader appears
- [ ] Boot menu displays
- [ ] Kernel loads
- [ ] Kernel banner shows
- [ ] All subsystems initialize
- [ ] No panic/crash
- [ ] Init process spawns
- [ ] Desktop appears

### Boot Time Measurements

- **UEFI to Bootloader:** X.Xs
- **Bootloader to Kernel:** X.Xs
- **Kernel Initialization:** X.Xs
- **Total Boot Time:** X.Xs
- **Target:** <5.0s
- **Status:** [PASS/FAIL]

### Issues Found

[List any errors, warnings, or blockers]

### Serial Log Excerpt

\`\`\`
[Paste relevant boot messages]
\`\`\`

## Conclusion

[Summary of test results]

## Next Steps

[What needs to be fixed or tested next]
EOF

# Edit with actual results
nano FIRST_BOOT_RESULTS.md
```

### Take Screenshots

If booting with graphics:

```bash
# QEMU screenshot
# While QEMU is running, press:
# Ctrl-Alt-Shift-3  (saves to screenshots/)

# Or use VNC and take screenshot externally
```

### Save Logs

```bash
# Create logs directory
mkdir -p test-results/boot-$(date +%Y%m%d-%H%M%S)

# Copy all relevant files
cp build/serial.log test-results/boot-$(date +%Y%m%d-%H%M%S)/
cp FIRST_BOOT_RESULTS.md test-results/boot-$(date +%Y%m%d-%H%M%S)/
cp build.log test-results/boot-$(date +%Y%m%d-%H%M%S)/ 2>/dev/null || true

# Archive
tar czf boot-test-$(date +%Y%m%d-%H%M%S).tar.gz test-results/boot-*
```

---

## Expected Results

### Success Scenario

**What You Should See:**

1. **Boot Menu (1-2 seconds)**
   ```
   =====================================
       AutomationOS Boot Menu
   =====================================
   
   > AutomationOS v0.1.0 (default)
     AutomationOS v0.1.0 (recovery mode)
     Boot Options
   
   Press Enter to boot, or wait 5 seconds...
   ```

2. **Kernel Boot Messages (2-3 seconds)**
   ```
   =====================================
      AutomationOS v0.1.0
   =====================================
   
   [SERIAL] COM1 initialized
   [GDT] Global Descriptor Table loaded
   [PMM] Physical Memory Manager initialized
   [PMM] Total memory: 4096 MB
   [VMM] Virtual Memory Manager initialized
   [HEAP] Kernel heap initialized
   [IDT] Interrupt Descriptor Table loaded
   [SYSCALL] System call interface initialized
   [PIT] Timer initialized at 100Hz
   [FRAMEBUFFER] Graphics mode: 1024x768
   [PS2] Keyboard initialized
   [SCHEDULER] Scheduler initialized
   [KERNEL] All subsystems initialized
   [KERNEL] Free memory: 4080 MB
   [BOOT] Total boot time: 4824000000 cycles (2.01 ms)
   ```

3. **Init Launch (Expected - May Not Happen Yet)**
   ```
   [KERNEL] Starting init process...
   [INIT] Init v0.1.0 (PID 1)
   [INIT] Mounting filesystems...
   [INIT] Starting service manager...
   ```

4. **Desktop (Expected - May Not Happen Yet)**
   - Compositor initializes
   - Window manager starts
   - Desktop shell appears
   - Login screen or default desktop

**Boot Time:** Should complete in **<5 seconds** from UEFI to desktop

### Known Issues Scenario

**What Might Happen Instead:**

1. **Kernel Boots, Then Hangs**
   ```
   [KERNEL] Starting init process...
   [KERNEL] Init not yet implemented
   [KERNEL] Entering idle loop
   *cursor blinks forever*
   ```
   
   **This is OK!** It means kernel works, just needs init integration.

2. **No Desktop Appears**
   - Init may start but not launch compositor
   - This is expected (BLOCKER #3 from E2E report)

3. **Build Fails**
   - Missing dependencies
   - Wrong compiler
   - See troubleshooting sections above

---

## Troubleshooting Boot Issues

### Issue: QEMU Won't Start

**Symptoms:**
```
qemu-system-x86_64: command not found
```

**Fix:**
```bash
# Install QEMU
pacman -S mingw-w64-x86_64-qemu  # MSys2
sudo apt install qemu-system-x86 # Ubuntu
sudo pacman -S qemu              # Arch
```

### Issue: Black Screen in QEMU

**Symptoms:**
- QEMU window opens
- Screen is completely black
- No output

**Fix:**
```bash
# Try headless mode with serial
bash scripts/run-qemu.sh --no-display

# Check serial log
cat build/serial.log

# If log is empty, bootloader didn't run
ls -lh build/BOOTX64.EFI  # Check exists
ls -lh build/kernel.elf   # Check exists
```

### Issue: Kernel Panic

**Symptoms:**
```
[PANIC] Kernel panic: [some error]
```

**Fix:**
```bash
# Read full panic message
cat build/serial.log | grep -A 10 PANIC

# Common causes:
# - Memory corruption: Check PMM/VMM init
# - Null pointer: Check all kmalloc() calls
# - Stack overflow: Check thread stack sizes
# - Invalid opcode: Check GDT/IDT setup

# Run with GDB to debug:
make qemu-debug
# (in another terminal)
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) bt
```

### Issue: "No bootable device"

**Symptoms:**
- QEMU/UEFI says "No bootable device"
- Or "Operating System not found"

**Fix:**
```bash
# ISO might be corrupted or invalid
make clean
make iso

# Verify ISO structure
xorriso -indev build/AutomationOS.iso -find

# Should show:
# /EFI
# /EFI/BOOT
# /EFI/BOOT/BOOTX64.EFI
# /kernel.elf
# /initrd.img
```

### Issue: Boot Loops

**Symptoms:**
- System keeps rebooting
- Never gets past bootloader

**Fix:**
```bash
# Check for triple fault
bash scripts/run-qemu.sh --no-display 2>&1 | tee boot_debug.log

# Look for:
# - "Triple fault" message
# - Repeated "Loading kernel..." messages

# Common causes:
# - Stack overflow in bootloader
# - Invalid page tables
# - Wrong kernel entry point

# Verify entry point:
readelf -h build/kernel.elf | grep Entry
```

---

## Success Criteria

You have successfully completed the boot test when:

- [x] Build completes without errors
- [x] ISO file is created (10-20 MB)
- [x] QEMU boots the system
- [x] Bootloader menu appears
- [x] Kernel loads and initializes
- [x] All subsystems report "initialized"
- [x] No kernel panic
- [x] Boot completes in <10 seconds (kernel only)
- [ ] Init process spawns (may not happen yet - see E2E report)
- [ ] Desktop appears (may not happen yet - see E2E report)

**Minimum Success:** First 8 items ✅  
**Full Success:** All 10 items ✅

---

## Next Steps After Boot Test

### If Boot Succeeds Completely (Desktop Appears)

🎉 **CONGRATULATIONS!** You have a working OS!

Next:
1. Test desktop interaction (mouse, keyboard)
2. Launch applications (Terminal, Task Manager)
3. Performance benchmarking
4. Real hardware testing (USB boot)

### If Boot Succeeds Partially (Kernel Only)

✅ **GREAT PROGRESS!** Kernel works!

Next:
1. Fix init process spawning (see E2E report BLOCKER #1)
2. Integrate service manager (see E2E report BLOCKER #2)
3. Complete desktop launch sequence (see E2E report BLOCKER #3)
4. Re-test after fixes

### If Boot Fails

📋 **DEBUGGING NEEDED**

Next:
1. Save full serial log
2. Identify error message
3. Use GDB to debug
4. Check E2E_BOOT_TEST_REPORT.md for known issues
5. Create bug report with logs

---

## Quick Reference Commands

```bash
# Build
make clean && make all

# Test
make test                    # Automated tests
make qemu                    # Interactive boot
make qemu-debug              # Debug mode

# View logs
cat build/serial.log         # Boot messages
cat build.log                # Build messages

# Verify build
ls -lh build/BOOTX64.EFI     # Bootloader
ls -lh build/kernel.elf      # Kernel
ls -lh build/AutomationOS.iso # ISO

# Clean
make clean                   # Remove build artifacts
```

---

## Contact / Help

If you encounter issues not covered here:

1. Check `E2E_BOOT_TEST_REPORT.md` for detailed analysis
2. Check `FINAL_VALIDATION_REPORT.md` for known bugs
3. Check `docs/` directory for subsystem-specific docs
4. Review test infrastructure in `tests/integration/`

---

**Good luck with the first boot! 🚀**
