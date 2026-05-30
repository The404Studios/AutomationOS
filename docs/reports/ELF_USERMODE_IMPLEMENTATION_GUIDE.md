# ELF Loader and Usermode Implementation Guide

## Overview

This guide walks through enabling ELF loading and ring 3 usermode execution in AutomationOS. The implementation is complete but currently disabled due to heap not being fully operational. Once VMM+heap are fixed, follow this guide to enable and test.

## Prerequisites

**CRITICAL:** These must be working before enabling ELF loading:

1. ✅ **Physical Memory Manager (PMM)** - `pmm_alloc_page()` working
2. ⚠️ **Virtual Memory Manager (VMM)** - `vmm_map_page()` working
3. ⚠️ **Kernel Heap** - `kmalloc()` working
4. ✅ **GDT** - Global Descriptor Table initialized
5. ✅ **TSS** - Task State Segment initialized
6. ✅ **Initrd** - Initial RAM disk loaded

**Current Status:**
- VMM and heap are implemented but may have issues
- ELF loader code is written and ready
- Usermode transition code is complete
- Test programs are available

## Architecture Overview

### Components

```
┌─────────────────────────────────────────────────────────────┐
│                    Kernel Space (Ring 0)                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. ELF Loader (kernel/fs/elf_loader.c)                    │
│     - Parse ELF64 headers                                  │
│     - Load PT_LOAD segments into memory                    │
│     - Setup user stack                                     │
│                                                             │
│  2. Usermode Transition (kernel/arch/x86_64/usermode.asm)  │
│     - Setup segment selectors (0x1B, 0x23)                 │
│     - Use IRETQ to jump to ring 3                          │
│                                                             │
│  3. TSS (Task State Segment)                               │
│     - Stores kernel stack (RSP0)                           │
│     - Used for syscall/interrupt ring 3→0 transitions      │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                           ↓ IRETQ
┌─────────────────────────────────────────────────────────────┐
│                    User Space (Ring 3)                      │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  User Program (userspace/test_minimal.c)                   │
│     - Runs with reduced privileges                         │
│     - Cannot access kernel memory                          │
│     - Uses syscall instruction for kernel services         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Memory Layout

```
User Space:
0x0000000000000000 - 0x0000800000000000  (User accessible)
  ├─ 0x400000    - User code (.text)
  ├─ 0x401000    - User data (.data, .bss)
  └─ 0x7FFFFFFFE000 - User stack (grows down, 8MB)

Kernel Space:
0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF  (Kernel only)
  ├─ 0xFFFFFFFF90000000 - Kernel heap (16MB)
  └─ Higher addresses - Kernel code/data
```

## Step-by-Step Implementation

### Step 1: Verify Prerequisites

**File:** `kernel/fs/elf_loader_test.c` (already created)

Run the test suite to verify all prerequisites:

```c
// In your kernel initialization (after heap_init):
#include "../include/elf_loader_test.h"

// Run all safe tests (doesn't enter ring 3)
elf_loader_test_suite(0);
```

**Expected Output:**
```
[TEST 1] Heap Allocation Test
=============================
[PASS] kmalloc(64) = 0xFFFFFFFF90000040
[PASS] kmalloc(4096) = 0xFFFFFFFF90001000
[PASS] Heap working correctly

[TEST 2] ELF Header Validation
===============================
[PASS] Valid ELF64 header accepted
[PASS] Invalid magic rejected
[PASS] Wrong architecture rejected
[PASS] Kernel space entry rejected
[PASS] ELF header validation working

[TEST 3] Initrd File Access
===========================
[WARN] test_minimal not found in initrd
[INFO] Build it with: make -C userspace tests
[INFO] Then add it to initrd and rebuild

[TEST 4] GDT and TSS Setup
==========================
[INFO] Current CPL (privilege level): 0
[PASS] Running in ring 0 (kernel mode)
[PASS] TSS structure available at 0xXXXXXXXX
[PASS] GDT and TSS configured
```

**Troubleshooting:**
- If Test 1 fails: Heap is not working - fix heap_init() first
- If Test 2 fails: Check ELF header parsing logic
- If Test 4 fails: GDT/TSS not initialized - call gdt_init() and tss_init()

### Step 2: Build Userspace Test Program

**File:** `userspace/test_minimal.c` (already created)

Build the minimal test program:

```bash
# From kernel root directory
cd userspace
make tests
```

This creates `userspace/bin/test_minimal` - a minimal ELF64 executable.

**Verify the binary:**
```bash
file bin/test_minimal
# Expected: bin/test_minimal: ELF 64-bit LSB executable, x86-64, version 1 (SYSV), statically linked

readelf -h bin/test_minimal
# Check: Entry point is in user space (e.g., 0x400000)
```

### Step 3: Add Test Program to Initrd

You need to include `test_minimal` in the initrd so the kernel can load it.

**Option A: Manual initrd creation**
```bash
# Create a simple tar-based initrd
mkdir -p initrd_files
cp userspace/bin/test_minimal initrd_files/
tar -C initrd_files -cf initrd.tar test_minimal
```

**Option B: Use existing initrd build script** (if available)
```bash
./scripts/build-initrd.sh
```

**Verify:**
```bash
tar -tf initrd.tar | grep test_minimal
# Should show: test_minimal
```

### Step 4: Test ELF Loading (Dry Run)

Before attempting ring 3 transition, test ELF loading in isolation:

```c
// In kernel initialization
elf_loader_test_suite(5);  // Test 5: ELF load dry run
```

**Expected Output:**
```
[TEST 5] ELF Load (Dry Run)
===========================
[ELF] Loading ELF: test_minimal
[ELF] Found file: test_minimal (size=XXXX bytes)
[ELF] Valid ELF64 executable
[ELF]   Entry point: 0x0000000000400000
[ELF]   Program headers: 3 entries at offset 0x40
[ELF]   Segment: vaddr=0x0000000000400000 size=0xXXX pages=X flags=R-X
[ELF]   Segment: vaddr=0x0000000000401000 size=0xXXX pages=X flags=RW-
[ELF] Setting up user stack: 0x00007FFFFFF00000 - 0x00007FFFFFFFE000 (2048 pages)
[ELF] Stack initialized, RSP=0x00007FFFFFFFE000
[ELF] Load complete: entry=0x0000000000400000 stack=0x00007FFFFFFFE000
[PASS] ELF loaded successfully
[INFO] Entry point: 0x0000000000400000
[INFO] Stack pointer: 0x00007FFFFFFFE000
[PASS] Entry point in user space
[PASS] Stack in user space
```

**Troubleshooting:**
- If "Out of physical memory": PMM not working or exhausted
- If "File not found": test_minimal not in initrd
- If entry/stack not in user space: ELF loader bug

### Step 5: Test Ring 3 Transition

**⚠️ WARNING:** This test enters ring 3 and does not return!

Only run this when you're ready to test full usermode execution.

```c
// In kernel initialization (final test)
elf_loader_test_suite(6);  // Test 6: Full execution
```

**Expected Output:**
```
[TEST 6] ELF Load and Execute (Ring 3)
=======================================
[INFO] This test will transition to ring 3 and execute user code
[INFO] If test_minimal works, you should see 'Hello from Ring 3!'
[INFO] This function will NOT return (entering user mode)

[ELF] Loading ELF: test_minimal
[ELF] Found file: test_minimal (size=XXXX bytes)
[ELF] Valid ELF64 executable
...
[INFO] Transitioning to ring 3...
=====================================

Hello from Ring 3!
```

**What happens:**
1. ELF loader loads `test_minimal` into memory
2. User stack is allocated and mapped
3. `start_usermode()` sets up TSS kernel stack
4. `enter_usermode()` (asm) uses IRETQ to jump to ring 3
5. CPU now in CPL=3, executes `_start()` in test_minimal.c
6. User program calls sys_write to print message
7. User program exits with sys_exit

**Troubleshooting:**

| Symptom | Cause | Fix |
|---------|-------|-----|
| Triple fault / reboot | Bad segment selectors | Check GDT entries 3,4 are user code/data |
| General Protection Fault | Stack not in user space | Check stack < USER_SPACE_END |
| Page fault | Segments not mapped | Check vmm_map_page() calls succeed |
| No output, hangs | Syscall handler not working | Implement syscall handlers (sys_write) |
| Returns to kernel | IRETQ failed | Check RFLAGS, CS, SS setup in usermode.asm |

## File Reference

### Kernel Files

| File | Purpose |
|------|---------|
| `kernel/fs/elf_loader.c` | ELF64 parser and loader |
| `kernel/include/elf.h` | ELF data structures and constants |
| `kernel/arch/x86_64/usermode.asm` | Assembly code for ring 0→3 transition |
| `kernel/core/usermode.c` | C wrapper for usermode transition |
| `kernel/include/usermode.h` | Usermode API |
| `kernel/arch/x86_64/gdt.c` | GDT and TSS setup |
| `kernel/include/tss.h` | TSS data structure |
| `kernel/fs/elf_loader_test.c` | Test suite (NEW) |
| `kernel/include/elf_loader_test.h` | Test suite header (NEW) |

### Userspace Files

| File | Purpose |
|------|---------|
| `userspace/test_minimal.c` | Minimal test program (NEW) |
| `userspace/test_program.ld` | Linker script for user programs (NEW) |
| `userspace/Makefile` | Build userspace programs (UPDATED) |
| `userspace/init.c` | Init process (existing) |

## Debugging Tips

### 1. Enable Verbose Logging

All ELF loader operations print status messages. Watch for:
- `[ELF] Loading ELF: ...` - Start of load
- `[ELF] Segment: ...` - Each PT_LOAD segment
- `[ELF] Load complete: ...` - Success

### 2. Check CPL After Transition

Add this to your user program:
```c
static inline int get_cpl(void) {
    unsigned short cs;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    return cs & 0x3;
}

void _start(void) {
    int cpl = get_cpl();
    // If cpl == 3, we're in ring 3!
    // If cpl == 0, transition failed
}
```

### 3. Single-Step Through IRETQ

Use QEMU with GDB:
```bash
qemu-system-x86_64 -s -S ...
gdb
(gdb) target remote :1234
(gdb) break enter_usermode
(gdb) continue
(gdb) stepi  # Step through IRETQ
(gdb) info registers cs  # Check CS selector
```

### 4. Memory Dump

Check loaded segments:
```bash
(gdb) x/20i 0x400000  # Dump user code
(gdb) x/20gx 0x7FFFFFFFE000  # Dump user stack
```

## Common Issues

### Issue 1: Heap Not Working

**Symptom:** Test 1 fails, kmalloc returns NULL

**Fix:**
1. Check `heap_init()` is called after `vmm_init()`
2. Verify PMM has free pages: `pmm_alloc_page()` works
3. Check heap pages are mapped: `vmm_map_page()` succeeds
4. Review `kernel/core/mem/heap.c` for bugs

### Issue 2: VMM Mapping Fails

**Symptom:** ELF loader gets "Out of memory" or page faults

**Fix:**
1. Verify `vmm_init()` was called
2. Check `paging_init()` set up page tables correctly
3. Ensure PMM has enough free pages
4. Review `kernel/core/mem/vmm.c` and `kernel/arch/x86_64/paging.c`

### Issue 3: GDT/TSS Not Set Up

**Symptom:** Test 4 fails, or GPF on ring transition

**Fix:**
1. Call `gdt_init()` early in boot
2. Call `tss_init()` after GDT
3. Verify GDT entries:
   - Entry 0: Null
   - Entry 1: Kernel code (0x08)
   - Entry 2: Kernel data (0x10)
   - Entry 3: User code (0x18, RPL=3 → selector 0x1B)
   - Entry 4: User data (0x20, RPL=3 → selector 0x23)
   - Entry 5-6: TSS (0x28)

### Issue 4: Syscall Handler Not Implemented

**Symptom:** User program runs but sys_write doesn't print

**Fix:**
1. Implement syscall handler (see `kernel/arch/x86_64/syscall.asm`)
2. Implement sys_write in `kernel/core/syscall/handlers.c`
3. Use `copy_from_user()` to safely access user buffers
4. Return to usermode correctly (SYSRETQ)

## Next Steps

Once ELF loading and ring 3 work:

1. **Implement more syscalls**
   - sys_read, sys_open, sys_close
   - sys_fork, sys_exec, sys_wait
   - See Linux syscall table for reference

2. **Build a real init**
   - Use `userspace/init.c`
   - Mount root filesystem
   - Spawn shell

3. **Add process management**
   - Task structs
   - Process creation (fork/exec)
   - Context switching between user processes

4. **Implement signals**
   - Signal delivery to user processes
   - Signal handlers in userspace

5. **Add virtual memory per-process**
   - Separate page tables per process
   - Copy-on-write
   - Demand paging

## Testing Checklist

- [ ] Test 1: Heap working
- [ ] Test 2: ELF header validation working
- [ ] Test 3: Initrd contains test_minimal
- [ ] Test 4: GDT and TSS configured
- [ ] Test 5: ELF loads successfully (dry run)
- [ ] Test 6: Ring 3 transition works
- [ ] User program prints "Hello from Ring 3!"
- [ ] User program can call syscalls
- [ ] User program exits cleanly

## References

### x86_64 Specifications
- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3, Chapter 3 (Protected Mode)
- AMD64 Architecture Programmer's Manual, Volume 2 (System Programming)

### ELF Specification
- Tool Interface Standard (TIS) Executable and Linking Format (ELF) Specification
- System V Application Binary Interface - AMD64 Architecture Processor Supplement

### Code Examples
- Linux kernel: `fs/binfmt_elf.c` (ELF loader)
- Linux kernel: `arch/x86/entry/entry_64.S` (syscall entry)
- xv6-x86_64: Simple teaching OS with ELF loader

## Support

If you encounter issues:

1. Check test output carefully - tests print diagnostic info
2. Review this guide's troubleshooting sections
3. Verify all prerequisites are met
4. Use GDB to debug ring transition
5. Check kernel logs for error messages

Good luck! Once this works, you have a real usermode capable OS!
