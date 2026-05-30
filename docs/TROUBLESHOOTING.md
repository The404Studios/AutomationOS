# AutomationOS Troubleshooting Guide

**Version:** 0.1.0  
**Phase:** 1 - Core Foundation  
**Last Updated:** 2026-05-26

---

## Table of Contents

1. [Build Issues](#build-issues)
2. [Boot Issues](#boot-issues)
3. [Runtime Issues](#runtime-issues)
4. [Hardware Issues](#hardware-issues)
5. [QEMU Issues](#qemu-issues)
6. [Development Issues](#development-issues)
7. [Diagnostic Tools](#diagnostic-tools)
8. [Error Messages Reference](#error-messages-reference)

---

## Build Issues

### Cross-Compiler Not Found

**Symptom:**
```
make: x86_64-elf-gcc: Command not found
```

**Cause:** Cross-compiler not installed or not in PATH

**Solution:**
```bash
# Check if installed
which x86_64-elf-gcc

# If not found, run setup script
bash scripts/setup-toolchain.sh

# Or add to PATH manually
export PATH="$HOME/opt/cross/bin:$PATH"
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

---

### NASM Not Found

**Symptom:**
```
make: nasm: Command not found
```

**Solution:**
```bash
# Ubuntu/Debian
sudo apt install nasm

# Arch Linux
sudo pacman -S nasm

# macOS
brew install nasm
```

---

### xorriso Not Found

**Symptom:**
```
scripts/build-iso.py: ERROR: xorriso not found
```

**Solution:**
```bash
# Ubuntu/Debian
sudo apt install xorriso

# Arch Linux
sudo pacman -S libisoburn

# macOS
brew install xorriso
```

---

### Linker Errors

**Symptom:**
```
ld: undefined reference to `__udivdi3'
```

**Cause:** 64-bit division without compiler support

**Solution:**
Avoid 64-bit division in kernel code, or implement division helpers:

```c
// Use shifts for powers of 2
uint64_t result = value >> 10;  // Divide by 1024

// Or implement helper
uint64_t udiv64(uint64_t dividend, uint64_t divisor) {
    // Software division implementation
}
```

---

### Multiple Definition Errors

**Symptom:**
```
ld: multiple definition of `function'
```

**Cause:** Function defined in header file without `static` or `inline`

**Solution:**
```c
// In header (.h)
static inline int helper_function(void) {
    // Implementation
}

// Or declare only
extern int function(void);

// And define in .c file
int function(void) {
    // Implementation
}
```

---

### Missing Symbols

**Symptom:**
```
ld: undefined reference to `symbol_name'
```

**Debugging Steps:**
```bash
# Check if object file contains symbol
nm build/file.o | grep symbol_name

# Check if symbol is undefined in .o files
nm build/*.o | grep " U symbol_name"

# Verify linking order
cat kernel/Makefile | grep LINK
```

**Common Causes:**
- Missing source file in Makefile
- Wrong linking order
- Typo in function name

---

## Boot Issues

### QEMU Shows Black Screen

**Symptom:** QEMU window opens but shows nothing

**Debugging:**
```bash
# Check serial output
make qemu 2>&1 | tee boot.log

# Enable QEMU logging
qemu-system-x86_64 -d int,cpu_reset -D qemu.log -no-reboot build/AutomationOS.iso
```

**Common Causes:**
1. **Bootloader not loaded:** Check `iso/EFI/BOOT/BOOTX64.EFI` exists
2. **Kernel not found:** Check `iso/boot/kernel.elf` exists
3. **Early boot crash:** Check QEMU logs

---

### Triple Fault at Boot

**Symptom:**
```
qemu: warning: triple fault
```
QEMU restarts immediately.

**Causes:**
- Stack overflow during initialization
- Invalid page tables
- Exception during exception handling (no IDT)
- Corrupted GDT/IDT

**Debugging:**
```bash
# Run with QEMU debug
qemu-system-x86_64 -d int,cpu_reset -no-reboot build/AutomationOS.iso

# Check QEMU log for fault address
cat qemu.log
```

**Solutions:**

1. **Stack Overflow:**
```c
// Increase stack size in linker script
kernel_stack:
    .space 16384  ; 16 KB stack
```

2. **Invalid Page Tables:**
```c
// Verify page table setup
kprintf("[DEBUG] PML4 at %p\n", pml4);
kprintf("[DEBUG] Mapping %p -> %p\n", virt, phys);
```

3. **GDT/IDT Issues:**
```c
// Verify tables are set up before use
gdt_init();  // MUST come before any interrupts
idt_init();  // MUST come before sti()
```

---

### Kernel Panic During Boot

**Symptom:**
```
[PANIC] Out of memory
```

**Causes:**
- PMM initialization failed
- Memory map from bootloader is incorrect
- Trying to allocate before PMM init

**Debugging:**
```c
// Add debug output in pmm_init()
kprintf("[PMM] Memory region %d: %p - %p (type %d)\n",
        i, mmap[i].base, mmap[i].base + mmap[i].length, mmap[i].type);
```

**Solution:**
Check memory map passed from bootloader:
```c
// In loader.c
kprintf("[BOOT] Memory map entries: %d\n", boot_info.memory_map_count);
for (int i = 0; i < boot_info.memory_map_count; i++) {
    kprintf("  [%d] %p - %p\n", i, 
            mmap[i].base, mmap[i].base + mmap[i].length);
}
```

---

### Hangs After "Starting init process..."

**Symptom:** Kernel prints initialization messages, then hangs

**Causes:**
1. Init process not implemented
2. Scheduler not starting
3. Infinite loop in init

**Debugging:**
```bash
# Attach GDB
make qemu-debug

# In GDB
(gdb) interrupt
(gdb) backtrace
(gdb) info threads
```

**Solution:**
Check if init process is actually created:
```c
// In kernel_main()
kprintf("[KERNEL] Creating init process...\n");
process_t* init = process_create("init", &init_main);
if (init == NULL) {
    kernel_panic("Failed to create init process");
}
scheduler_add_process(init);
kprintf("[KERNEL] Init process created (PID %d)\n", init->pid);
```

---

## Runtime Issues

### Kernel Panic: Page Fault

**Symptom:**
```
[PANIC] Page fault at 0xDEADBEEF
RIP: 0xFFFFFFFF80123456
Error code: 0x0002
```

**Error Code Meanings:**
- Bit 0: 0 = Page not present, 1 = Protection violation
- Bit 1: 0 = Read, 1 = Write
- Bit 2: 0 = Kernel mode, 1 = User mode

**Common Causes:**

1. **NULL Pointer Dereference:**
```c
// Bad
process_t* proc = NULL;
proc->pid = 1;  // CRASH

// Good
if (proc != NULL) {
    proc->pid = 1;
}
```

2. **Stack Overflow:**
```c
// Bad: Large local array
void function(void) {
    char buffer[10000000];  // Too large!
}

// Good: Use heap
void function(void) {
    char* buffer = kmalloc(10000000);
    // ... use buffer ...
    kfree(buffer);
}
```

3. **Unmapped Memory Access:**
```c
// Ensure memory is mapped
void* page = pmm_alloc_page();
vmm_map_page(virt_addr, page, PAGE_PRESENT | PAGE_WRITE);
// Now safe to access virt_addr
```

**Debugging:**
```bash
# Find source location of crash
(gdb) list *0xFFFFFFFF80123456
```

---

### Kernel Panic: Out of Memory

**Symptom:**
```
[PANIC] PMM: Out of memory
```

**Causes:**
- Memory leak (not freeing pages)
- Allocating too much memory
- Insufficient physical memory

**Debugging:**
```c
// Track memory usage
kprintf("[MEM] Free: %llu MB\n", pmm_get_free_memory() / 1024 / 1024);
kprintf("[MEM] Used: %llu MB\n", pmm_get_used_memory() / 1024 / 1024);

// Find leaks by logging all allocations
void* page = pmm_alloc_page();
kprintf("[MEM] Allocated page at %p\n", page);
```

**Solutions:**
1. Free unused memory
2. Increase QEMU RAM: `qemu-system-x86_64 -m 512 ...`
3. Check for infinite allocation loops

---

### General Protection Fault (GPF)

**Symptom:**
```
[EXCEPTION] General Protection Fault
RIP: 0xFFFFFFFF80ABCDEF
Error code: 0x0000
```

**Common Causes:**

1. **Invalid Segment Access:**
```c
// Check GDT is properly initialized
gdt_init();
```

2. **NULL Function Pointer:**
```c
// Bad
void (*func)(void) = NULL;
func();  // GPF

// Good
if (func != NULL) {
    func();
}
```

3. **Stack Misalignment:**
```c
// x86_64 requires 16-byte aligned stack
// Check assembly code for proper alignment
sub rsp, 16  ; Align stack
```

---

### System Hangs

**Symptom:** No output, system completely frozen

**Causes:**
1. Infinite loop
2. Deadlock
3. Interrupts disabled forever
4. Waiting for interrupt that never comes

**Debugging:**
```bash
# Attach GDB and interrupt
(gdb) interrupt
(gdb) backtrace
(gdb) print $rip
```

**Solutions:**

1. **Infinite Loop:**
```c
// Add debug output
while (condition) {
    kprintf("[DEBUG] Waiting... %d\n", i++);
    // Check for timeout
    if (i > 1000) {
        kernel_panic("Timeout in loop");
    }
}
```

2. **Interrupts Disabled:**
```c
// Always re-enable interrupts
cli();
// Critical section
sti();  // DON'T FORGET THIS
```

3. **Deadlock:**
```c
// Add timeout to locks (Phase 2)
if (!acquire_lock_timeout(&lock, 1000)) {
    kernel_panic("Lock timeout - possible deadlock");
}
```

---

### Timer Not Working

**Symptom:** No scheduler ticks, processes don't switch

**Debugging:**
```c
// In timer interrupt handler
void timer_handler(void) {
    static uint64_t ticks = 0;
    ticks++;
    if (ticks % 100 == 0) {
        kprintf("[TIMER] Tick %llu\n", ticks);
    }
    schedule();
}
```

**Common Causes:**
1. PIT not initialized: Call `pit_init(100)` in `kernel_main()`
2. Interrupts disabled: Call `sti()` after initialization
3. IRQ not enabled: Check PIC configuration

---

### Keyboard Not Responding

**Symptom:** PS/2 keyboard input not working

**Debugging:**
```c
// Test keyboard in ps2_init()
kprintf("[PS2] Testing keyboard...\n");
outb(0x60, 0xED);  // Set LEDs command
uint8_t response = inb(0x60);
kprintf("[PS2] Response: 0x%02X\n", response);
```

**Common Causes:**
1. PS/2 not initialized
2. IRQ 1 not enabled
3. USB keyboard (not supported in Phase 1)

**Solution for QEMU:**
```bash
# Ensure PS/2 keyboard is emulated
qemu-system-x86_64 -device isa-serial,chardev=serial0 ...
# (QEMU uses PS/2 by default)
```

---

## Hardware Issues

### ISO Won't Boot on Real Hardware

**Symptom:** USB boots on QEMU but not on real PC

**Debugging Steps:**

1. **Check UEFI Boot Mode:**
   - Enter BIOS/UEFI settings
   - Enable UEFI boot (disable Legacy/CSM)
   - Verify Secure Boot is disabled

2. **Verify ISO Structure:**
```bash
# Mount and inspect ISO
mkdir /tmp/iso
sudo mount -o loop build/AutomationOS.iso /tmp/iso
ls -R /tmp/iso
# Should see: EFI/BOOT/BOOTX64.EFI
sudo umount /tmp/iso
```

3. **Check USB Writing:**
```bash
# Ensure ISO is written correctly
sudo dd if=build/AutomationOS.iso of=/dev/sdX bs=4M status=progress
sudo sync

# Verify
sudo dd if=/dev/sdX bs=1M count=10 | hexdump -C
```

---

### Serial Output Not Working on Real Hardware

**Symptom:** No output on physical serial port

**Causes:**
- Wrong port (use COM1: 0x3F8)
- Cable not connected
- Terminal not configured

**Solution:**
```bash
# On another PC connected via serial cable
# Use terminal emulator (e.g., minicom, screen)
screen /dev/ttyUSB0 38400

# Or minicom
minicom -D /dev/ttyUSB0 -b 38400
```

---

### Framebuffer Issues on Real Hardware

**Symptom:** Screen corruption, wrong colors, or no display

**Debugging:**
```c
// Log framebuffer info
kprintf("[FB] Address: %p\n", fb_addr);
kprintf("[FB] Size: %dx%d\n", width, height);
kprintf("[FB] Pitch: %d\n", pitch);
kprintf("[FB] Pixel format: %d\n", pixel_format);
```

**Common Issues:**
1. Wrong pixel format (assume 32-bit BGRA)
2. Pitch != width * 4 (use pitch for scanline advancement)
3. Framebuffer address not mapped in page tables

---

## QEMU Issues

### OVMF Firmware Not Found

**Symptom:**
```
qemu-system-x86_64: Could not open 'OVMF.fd': No such file or directory
```

**Solution:**
```bash
# Ubuntu/Debian
sudo apt install ovmf
# Firmware usually at /usr/share/OVMF/OVMF_CODE.fd

# Arch
sudo pacman -S edk2-ovmf
# Firmware at /usr/share/edk2-ovmf/x64/OVMF.fd

# Update script
vim scripts/run-qemu.sh
# Change OVMF path to match your system
```

---

### QEMU Crashes

**Symptom:** QEMU itself crashes or exits unexpectedly

**Debugging:**
```bash
# Enable full QEMU logging
qemu-system-x86_64 -d guest_errors,unimp,int -D qemu.log ...

# Check QEMU version (may be too old)
qemu-system-x86_64 --version
# Upgrade if < 6.0
```

---

### Serial Console Not Showing Output

**Symptom:** QEMU runs but no serial output in terminal

**Solution:**
```bash
# Ensure serial is redirected to stdout
qemu-system-x86_64 -serial stdio ...

# Not:
qemu-system-x86_64 -serial mon:stdio  # This mixes monitor and serial
```

---

### QEMU Extremely Slow

**Symptoms:** Boot takes minutes instead of seconds

**Causes:**
1. KVM not enabled (Linux)
2. Insufficient host resources
3. Debug logging enabled

**Solutions:**
```bash
# Enable KVM (Linux only)
qemu-system-x86_64 -enable-kvm ...

# Check KVM
lsmod | grep kvm
# If not loaded:
sudo modprobe kvm_intel  # or kvm_amd

# Increase allocated RAM
qemu-system-x86_64 -m 512 ...  # 512 MB

# Disable debug logging
# Remove: -d int,cpu_reset
```

---

## Development Issues

### GDB Not Connecting

**Symptom:**
```
(gdb) target remote localhost:1234
Connection refused
```

**Solution:**
```bash
# Ensure QEMU started with -s flag
qemu-system-x86_64 -s -S ...  # -S pauses at startup

# Check if port is in use
netstat -an | grep 1234

# Use different port if needed
qemu-system-x86_64 -gdb tcp::1235 ...
gdb> target remote localhost:1235
```

---

### Symbols Not Loaded in GDB

**Symptom:** GDB shows assembly but no source code

**Solution:**
```bash
# Rebuild with debug symbols
make clean
make CFLAGS="-g -O0" all

# Load symbols in GDB
(gdb) file build/kernel.elf
(gdb) symbol-file build/kernel.elf

# Verify
(gdb) info sources
```

---

### Make Rebuilds Everything

**Symptom:** `make` rebuilds all files even when nothing changed

**Causes:**
- Incorrect dependencies in Makefile
- Timestamps messed up
- Cross-filesystem issues (WSL)

**Solution:**
```bash
# Check if .o files are newer than sources
ls -lt kernel/*.c kernel/*.o

# Reset timestamps
find . -name "*.o" -exec touch {} \;

# WSL: Keep code in WSL filesystem
mv /mnt/c/AutomationOS ~/AutomationOS
```

---

## Diagnostic Tools

### Memory Diagnostics

```c
// Add to kernel
void mem_debug(void) {
    kprintf("=== Memory Diagnostics ===\n");
    kprintf("Total:  %llu MB\n", pmm_get_total_memory() / 1024 / 1024);
    kprintf("Used:   %llu MB\n", pmm_get_used_memory() / 1024 / 1024);
    kprintf("Free:   %llu MB\n", pmm_get_free_memory() / 1024 / 1024);
    
    // Page table diagnostics
    kprintf("CR3: %p\n", (void*)read_cr3());
}
```

### Process Diagnostics

```c
// List all processes
void ps_debug(void) {
    kprintf("=== Process List ===\n");
    kprintf("PID\tState\t\tName\n");
    
    process_t* proc = ready_queue;
    do {
        kprintf("%d\t%s\t\t%s\n", 
                proc->pid, 
                state_to_string(proc->state),
                proc->name);
        proc = proc->next;
    } while (proc != ready_queue);
}
```

### Interrupt Diagnostics

```c
// Count interrupts
static uint64_t irq_counts[16];

void irq_debug(void) {
    kprintf("=== IRQ Statistics ===\n");
    for (int i = 0; i < 16; i++) {
        if (irq_counts[i] > 0) {
            kprintf("IRQ %d: %llu\n", i, irq_counts[i]);
        }
    }
}
```

### Stack Trace

```c
// Print stack trace
void stack_trace(void) {
    uint64_t* rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    
    kprintf("=== Stack Trace ===\n");
    for (int i = 0; i < 10 && rbp != NULL; i++) {
        uint64_t rip = rbp[1];
        kprintf("[%d] %p\n", i, (void*)rip);
        rbp = (uint64_t*)rbp[0];
    }
}
```

---

## Error Messages Reference

### Build Errors

| Error | Meaning | Solution |
|-------|---------|----------|
| `Command not found` | Tool not installed | Install with package manager |
| `undefined reference` | Missing symbol | Check linking, add source file |
| `multiple definition` | Symbol defined twice | Use `static` or `extern` |
| `relocation truncated` | Address out of range | Use `-mcmodel=large` |

### Runtime Errors

| Error | Meaning | Solution |
|-------|---------|----------|
| `Page fault` | Invalid memory access | Check pointers, add bounds checking |
| `GPF` | Protection violation | Check segments, NULL pointers |
| `Triple fault` | Exception during exception | Fix GDT/IDT, check stack |
| `Out of memory` | PMM exhausted | Free memory, increase RAM |

---

## Getting More Help

### Information to Provide

When asking for help, include:

1. **AutomationOS version:** Check `kernel/include/kernel.h`
2. **Host OS:** `uname -a` (Linux) or OS version
3. **Toolchain versions:**
   ```bash
   x86_64-elf-gcc --version
   qemu-system-x86_64 --version
   ```
4. **Full error message:** Copy entire output
5. **Steps to reproduce:** Exact commands run
6. **What you've tried:** Previous troubleshooting attempts

### Useful Logs

```bash
# Build log
make clean && make all 2>&1 | tee build.log

# QEMU log
make qemu 2>&1 | tee qemu.log

# QEMU debug log
qemu-system-x86_64 -d int,cpu_reset -D debug.log ...
```

---

## Common Pitfalls

1. **Forgetting to call init functions:** Check `kernel_main()` sequence
2. **Using standard library in kernel:** Must use kernel functions only
3. **Assuming memory is zeroed:** Always initialize variables
4. **Ignoring return values:** Check for errors (NULL, negative codes)
5. **Disabling interrupts forever:** Always re-enable with `sti()`
6. **Not testing edge cases:** NULL pointers, zero sizes, etc.
7. **Premature optimization:** Get it working first, then optimize

---

**End of Troubleshooting Guide**
