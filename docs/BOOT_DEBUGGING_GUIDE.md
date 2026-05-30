# AutomationOS Boot Debugging Guide

**Quick reference for diagnosing boot failures**

## Common Boot Failure Symptoms

### 1. QEMU Immediately Exits / Resets

**Symptom:** QEMU window closes immediately or repeatedly resets

**Likely Causes:**
- Triple fault (CPU exception handler missing or broken)
- Invalid page tables
- Stack overflow/corruption
- Jump to invalid memory address

**Debug Steps:**

1. **Enable QEMU logging:**
   ```bash
   qemu-system-x86_64 \
       -d int,cpu_reset,guest_errors \
       -D qemu.log \
       -no-reboot \
       ...
   ```
   Check `qemu.log` for exception details.

2. **Check for triple fault:**
   Look for "Triple fault" in qemu.log
   ```
   check_exception old: 0xffffffff new 0xe
   Triple fault
   ```

3. **Common causes of triple fault:**
   - IDT not initialized before enabling interrupts
   - Invalid IDT gate addresses
   - Stack not set up properly
   - Page fault with no handler
   - Divide by zero with no handler

**Fix:**
- Initialize IDT before STI
- Verify interrupt handler addresses
- Check stack pointer (RSP) is valid
- Add catch-all exception handlers

---

### 2. Black Screen, No Output

**Symptom:** QEMU runs but shows nothing

**Likely Causes:**
- Serial console not initialized
- Framebuffer address incorrect
- UEFI boot services not exited properly
- CPU halted with interrupts disabled

**Debug Steps:**

1. **Check serial output:**
   ```bash
   qemu-system-x86_64 \
       -serial stdio \  # Serial to terminal
       ...
   ```
   If you see output here, graphics are the issue.

2. **Add early debug output:**
   ```c
   // Very first thing in kernel_main():
   serial_init();  // Must be first!
   kprintf("Kernel started\n");  // Verify we get here
   ```

3. **Test with minimal kernel:**
   ```c
   void kernel_main(boot_info_t* boot_info) {
       serial_init();
       kprintf("Hello from kernel!\n");
       while(1) hlt();  // Stop here
   }
   ```

**Fix:**
- Initialize serial console ASAP
- Verify boot_info framebuffer address is non-zero
- Check bootloader passes control correctly
- Ensure interrupts enabled if using timer

---

### 3. Hangs at Specific Initialization Phase

**Symptom:** Boot proceeds to certain point then stops

**Likely Causes:**
- Infinite loop in init function
- Deadlock (waiting for interrupt that never comes)
- Corrupted data structure causing hang
- Out of memory

**Debug Steps:**

1. **Add debug prints between each init:**
   ```c
   kprintf("[KERNEL] Starting GDT init...\n");
   gdt_init();
   kprintf("[KERNEL] GDT init complete\n");
   
   kprintf("[KERNEL] Starting PMM init...\n");
   pmm_init(boot_info->memory_map, boot_info->memory_map_count);
   kprintf("[KERNEL] PMM init complete\n");
   
   // ... and so on
   ```

2. **Use hang detection:**
   ```c
   #define HANG_CHECK(msg) kprintf("[HANG_CHECK:%d] %s\n", __LINE__, msg)
   
   HANG_CHECK("Before GDT");
   gdt_init();
   HANG_CHECK("After GDT");
   ```

3. **Check for infinite loops:**
   - Add timeout counters
   - Check while() conditions
   - Verify loop exit conditions

**Fix:**
- Identify which init function hangs
- Add timeouts to busy-wait loops
- Check that hardware is responding (e.g., PIT, PIC)
- Verify memory map is valid

---

### 4. Kernel Panic

**Symptom:** "KERNEL PANIC: ..." message appears

**Debug Steps:**

1. **Read panic message:**
   - File name and line number
   - Panic message
   - Register dump (if available)

2. **Common panic causes:**
   ```
   "Out of memory" → Check PMM, heap size
   "Page fault" → Check page tables, invalid pointer
   "Invalid boot info magic" → Bootloader handoff issue
   "Init must be PID 1" → Process table corruption
   ```

3. **Add stack trace:**
   ```c
   void kernel_panic(const char* msg) {
       kprintf("\n");
       kprintf("╔════════════════════════════════════════╗\n");
       kprintf("║        KERNEL PANIC                    ║\n");
       kprintf("╚════════════════════════════════════════╝\n");
       kprintf("\n");
       kprintf("Panic: %s\n", msg);
       kprintf("\n");
       kprintf("Stack trace:\n");
       dump_stack();  // If implemented
       kprintf("\n");
       kprintf("Registers:\n");
       kprintf("  RIP: %p\n", get_rip());
       kprintf("  RSP: %p\n", get_rsp());
       kprintf("  CR3: %p\n", (void*)read_cr3());
       kprintf("\n");
       kprintf("System halted.\n");
       while(1) { cli(); hlt(); }
   }
   ```

**Fix:**
- Address root cause from panic message
- Add NULL checks before dereferencing
- Validate input parameters
- Check return values

---

### 5. Bootloader Menu Doesn't Appear

**Symptom:** UEFI loads but no boot menu

**Likely Causes:**
- boot.conf not found or corrupted
- Graphics Output Protocol failed
- Console I/O not available

**Debug Steps:**

1. **Verify boot.conf exists:**
   ```bash
   sudo mount esp.img mnt
   ls -la mnt/EFI/BOOT/
   cat mnt/EFI/BOOT/boot.conf
   sudo umount mnt
   ```

2. **Check bootloader debug output:**
   ```bash
   qemu-system-x86_64 \
       -serial stdio \
       -display none \  # Disable graphics
       ...
   ```

3. **Simplify menu:**
   Edit boot.conf to have just one entry, short timeout.

**Fix:**
- Ensure boot.conf is in same directory as BOOTX64.EFI
- Check file permissions (should be readable)
- Fall back to text mode if GOP fails
- Add error messages in bootloader

---

### 6. Kernel Loads but Crashes Immediately

**Symptom:** Bootloader reports success, then instant crash/reset

**Likely Causes:**
- Invalid entry point
- Stack not set up
- BSS not cleared
- Calling convention mismatch

**Debug Steps:**

1. **Verify entry point:**
   ```bash
   readelf -h build/kernel.elf | grep Entry
   # Should match linker script
   ```

2. **Check bootloader passes boot_info correctly:**
   ```c
   // First line of kernel_main():
   void kernel_main(boot_info_t* boot_info) {
       // boot_info should be in RDI (x86-64 calling convention)
       // Don't use boot_info yet! Just print:
       asm volatile("hlt");  // Stop immediately
   }
   ```
   If this hangs cleanly, kernel entry is OK.

3. **Test stack:**
   ```c
   void kernel_main(boot_info_t* boot_info) {
       volatile int test = 42;  // Stack write
       test++;                   // Stack read
       asm volatile("hlt");
   }
   ```

**Fix:**
- Verify boot.asm sets up stack before calling kernel_main
- Check linker script places BSS correctly
- Ensure calling convention matches (RDI for first arg)
- Check GCC flags (-mno-red-zone, -mcmodel=kernel)

---

### 7. Scheduler Starts but Nothing Happens

**Symptom:** "Starting scheduler" prints, then silence

**Likely Causes:**
- No processes in ready queue
- Timer interrupt not firing
- Context switch broken
- Process context invalid

**Debug Steps:**

1. **Verify process added to scheduler:**
   ```c
   kprintf("[DEBUG] Adding process PID %d to scheduler\n", init->pid);
   scheduler_add_process(init);
   kprintf("[DEBUG] Process added, state=%d\n", init->state);
   ```

2. **Check timer interrupt:**
   ```c
   // In IRQ0 handler:
   void irq0_handler() {
       static uint64_t ticks = 0;
       ticks++;
       if (ticks % 100 == 0) {  // Every second (100Hz)
           kprintf("[TIMER] Tick %llu\n", ticks);
       }
       scheduler_tick();
   }
   ```

3. **Test context switch manually:**
   ```c
   // Before scheduler_start():
   kprintf("[DEBUG] About to switch to PID %d\n", init->pid);
   kprintf("[DEBUG] Entry point: %p\n", (void*)init->context.rip);
   kprintf("[DEBUG] Stack: %p\n", (void*)init->context.rsp);
   kprintf("[DEBUG] CR3: %p\n", (void*)init->context.cr3);
   ```

**Fix:**
- Ensure at least one process is READY
- Verify PIT initialized and IRQ0 unmasked
- Check context_switch.asm is correct
- Validate process context values

---

### 8. Init Process Crashes

**Symptom:** Init starts but immediately faults

**Likely Causes:**
- Invalid entry point
- User stack not mapped
- Page tables incorrect
- Segment selectors wrong

**Debug Steps:**

1. **Check for page fault:**
   ```
   Page Fault: address=0x400000, error=0x7
   ```
   - Error code: bit 0 (present), bit 1 (write), bit 2 (user)
   - If bit 0 is 0: page not present
   - If bit 2 is 1: user mode access

2. **Verify user page tables:**
   ```c
   // After ELF load, before starting:
   void debug_page_tables(process_t* proc) {
       kprintf("[DEBUG] Checking user page tables:\n");
       
       // Check if entry point is mapped
       uint64_t entry = proc->context.rip;
       if (paging_is_mapped(proc->context.cr3, entry)) {
           kprintf("[DEBUG] Entry %p is mapped\n", (void*)entry);
       } else {
           kprintf("[ERROR] Entry %p NOT mapped!\n", (void*)entry);
       }
       
       // Check if stack is mapped
       uint64_t stack = proc->context.rsp;
       if (paging_is_mapped(proc->context.cr3, stack)) {
           kprintf("[DEBUG] Stack %p is mapped\n", (void*)stack);
       } else {
           kprintf("[ERROR] Stack %p NOT mapped!\n", (void*)stack);
       }
   }
   ```

3. **Verify segment selectors:**
   ```c
   kprintf("[DEBUG] CS=0x%x (should be 0x23)\n", init->context.cs);
   kprintf("[DEBUG] SS=0x%x (should be 0x1B)\n", init->context.ss);
   ```

**Fix:**
- Ensure ELF segments mapped with USER flag
- Map user stack with write permissions
- Set CS to user code segment (0x23)
- Set SS to user data segment (0x1B)
- Set DS, ES to user data segment

---

## Debug Macros

### Boot Phase Marker
```c
#define BOOT_PHASE(name) do { \
    kprintf("\n"); \
    kprintf("╔════════════════════════════════════════╗\n"); \
    kprintf("║  %-38s ║\n", name); \
    kprintf("╚════════════════════════════════════════╝\n"); \
    kprintf("\n"); \
} while(0)

// Usage:
BOOT_PHASE("Memory Management Initialization");
pmm_init(...);
vmm_init();
heap_init();
```

### Assertion with Panic
```c
#define ASSERT(condition, msg) do { \
    if (!(condition)) { \
        kprintf("[ASSERT FAILED] %s:%d: %s\n", __FILE__, __LINE__, msg); \
        kernel_panic("Assertion failed: " #condition); \
    } \
} while(0)

// Usage:
ASSERT(boot_info != NULL, "Boot info is NULL");
ASSERT(boot_info->magic == BOOT_MAGIC, "Invalid boot info magic");
```

### Debug Dump
```c
#define DEBUG_DUMP(var) \
    kprintf("[DEBUG:%s:%d] %s = %p\n", __FILE__, __LINE__, #var, (void*)(var))

// Usage:
DEBUG_DUMP(boot_info);
DEBUG_DUMP(boot_info->memory_map);
DEBUG_DUMP(init->context.rip);
```

### Memory Dump
```c
void memdump(const char* label, void* addr, size_t len) {
    kprintf("[MEMDUMP] %s at %p:\n", label, addr);
    uint8_t* bytes = (uint8_t*)addr;
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0) {
            kprintf("%p: ", (void*)(addr + i));
        }
        kprintf("%02x ", bytes[i]);
        if (i % 16 == 15 || i == len - 1) {
            kprintf("\n");
        }
    }
}

// Usage:
memdump("ELF Header", elf_data, 64);
memdump("Stack", (void*)proc->context.rsp, 128);
```

---

## QEMU Debug Flags

### Useful QEMU Options

```bash
# Full debug output
qemu-system-x86_64 \
    -d int,cpu_reset,guest_errors,mmu,unimp \
    -D qemu_debug.log \
    -no-reboot \
    -no-shutdown \
    ...

# GDB debugging
qemu-system-x86_64 \
    -s \          # GDB server on :1234
    -S \          # Start paused
    ...

# Then in another terminal:
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

### Debug Flag Reference

| Flag | What It Logs |
|------|-------------|
| `-d int` | Interrupts and exceptions |
| `-d cpu_reset` | CPU resets (triple faults) |
| `-d guest_errors` | Guest OS errors |
| `-d mmu` | Page table walks and TLB |
| `-d in_asm` | Guest assembly instructions |
| `-d out_asm` | Host assembly (TCG) |
| `-d unimp` | Unimplemented features |

---

## GDB Commands

### Basic Debugging

```gdb
# Load symbols
file build/kernel.elf

# Connect to QEMU
target remote :1234

# Set breakpoints
break kernel_main
break *0x100000

# Run to breakpoint
continue

# Step through code
stepi          # Step one instruction
nexti          # Step over calls
step           # Step into functions
next           # Step over functions

# Examine registers
info registers
print $rsp
print $rip

# Examine memory
x/16x $rsp         # 16 hex words at stack
x/16i $rip         # 16 instructions at RIP
x/s 0x400000       # String at address

# Backtrace
backtrace
frame 0

# Watch memory
watch *(uint64_t*)0x400000

# Continue execution
continue
```

### Advanced Debugging

```gdb
# Dump page tables
define dump_pagetables
    set $pml4 = $cr3 & ~0xfff
    printf "PML4: %p\n", $pml4
    x/512gx $pml4
end

# Dump IDT
define dump_idt
    set $idtr = 0
    sidt $idtr
    set $idt_base = *(uint64_t*)($idtr+2)
    set $idt_limit = *(uint16_t*)$idtr
    printf "IDT: base=%p, limit=%d entries\n", $idt_base, $idt_limit/16
    x/256gx $idt_base
end

# Dump GDT
define dump_gdt
    set $gdtr = 0
    sgdt $gdtr
    set $gdt_base = *(uint64_t*)($gdtr+2)
    set $gdt_limit = *(uint16_t*)$gdtr
    printf "GDT: base=%p, limit=%d bytes\n", $gdt_base, $gdt_limit
    x/8gx $gdt_base
end
```

---

## Checklist for Each Boot Phase

### Phase 1: Bootloader
- [ ] UEFI firmware loads
- [ ] BOOTX64.EFI found and executed
- [ ] boot.conf found and parsed
- [ ] Boot menu displays (if configured)
- [ ] Kernel file found
- [ ] ELF validation passes
- [ ] Kernel segments loaded
- [ ] boot_info structure filled
- [ ] Boot services exited cleanly
- [ ] Jump to kernel entry point

### Phase 2: Kernel Early Init
- [ ] boot.asm _start executes
- [ ] Stack set up
- [ ] BSS cleared
- [ ] kernel_main called with boot_info
- [ ] Serial console initialized
- [ ] Kernel banner printed

### Phase 3: Kernel Subsystems
- [ ] GDT initialized
- [ ] PMM initialized, memory detected
- [ ] VMM initialized
- [ ] Heap initialized
- [ ] IDT initialized
- [ ] Syscall MSRs set up
- [ ] PIT timer initialized
- [ ] Framebuffer initialized (if present)
- [ ] Keyboard initialized
- [ ] Namespace system initialized
- [ ] Process management initialized
- [ ] Scheduler initialized
- [ ] Boot time measured and displayed

### Phase 4: Init Process (Future)
- [ ] VFS initialized
- [ ] Initrd mounted
- [ ] /sbin/init found
- [ ] ELF loaded into user space
- [ ] Init process created (PID 1)
- [ ] User page tables set up
- [ ] User stack mapped
- [ ] Process marked READY
- [ ] Added to scheduler
- [ ] Scheduler started

### Phase 5: Userspace (Future)
- [ ] Init process executes
- [ ] Init prints startup message
- [ ] Init forks and execs shell
- [ ] Shell displays prompt

---

## Emergency Recovery

### If System Completely Broken

1. **Start with known-good bootloader:**
   ```bash
   cd boot
   git checkout HEAD -- *.c *.h
   make clean
   make
   ```

2. **Test with minimal kernel:**
   ```c
   // minimal_kernel.c
   void kernel_main(void* boot_info) {
       // Do nothing, just halt
       while(1) {
           asm volatile("cli; hlt");
       }
   }
   ```

3. **Add features back incrementally:**
   - Serial console only
   - Add GDT
   - Add IDT (no handlers yet)
   - Add PMM (just init, no alloc)
   - ... and so on

4. **Use git bisect to find breaking commit:**
   ```bash
   git bisect start
   git bisect bad HEAD
   git bisect good <last-known-good-commit>
   # Git will check out intermediate commits
   # Test each one and mark good/bad
   git bisect good  # or git bisect bad
   ```

---

## Common Error Messages

| Error | Likely Cause | Fix |
|-------|-------------|-----|
| "Triple fault" | IDT/exception handler | Initialize IDT before STI |
| "Page fault at 0x0" | NULL pointer dereference | Add NULL checks |
| "Page fault at 0x400000" | User memory not mapped | Check ELF loader, page tables |
| "Out of memory" | PMM exhausted | Reduce memory usage, check leaks |
| "Invalid boot info magic" | Bootloader/kernel mismatch | Rebuild both, check struct |
| "Divide error" | Division by zero | Add exception handler, check math |
| "General protection fault" | Invalid segment/privilege | Check GDT, segment selectors |

---

## Quick Tests

### Test 1: Bootloader Only
```bash
# Should see boot menu
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -drive file=esp.img,format=raw -serial stdio
```

### Test 2: Kernel Loads
```bash
# Should see "AutomationOS v1.0.0"
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -drive file=esp.img,format=raw -serial stdio | grep "AutomationOS"
```

### Test 3: All Inits Complete
```bash
# Should see "Entering idle loop"
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -drive file=esp.img,format=raw -serial stdio | grep "idle loop"
```

### Test 4: Specific Subsystem
```bash
# Check if PMM initialized
qemu-system-x86_64 ... -serial stdio | grep "PMM"

# Check boot time
qemu-system-x86_64 ... -serial stdio | grep "boot time"
```

---

**Remember:** Boot debugging is incremental. Test each phase independently before moving to the next. Add debug output liberally. When in doubt, simplify and add features back one at a time.
