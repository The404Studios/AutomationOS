# ELF Loading Debug Trace Enhancement

**Date:** 2026-05-26  
**Task:** Add comprehensive debug output to track init ELF loading, validation, and execution

## Overview

Enhanced debug output has been added throughout the ELF loading and process creation pipeline to verify that `/sbin/init` can be:
1. Loaded from initrd
2. Validated as a proper ELF64 executable
3. Segments mapped to correct virtual addresses
4. Process created with valid context
5. Added to scheduler and executed

## Files Modified

### 1. `kernel/fs/exec.c` - ELF Loading and Execution

#### Enhanced ELF Header Validation
```c
// Added detailed magic number, class, machine, and type validation output
kprintf("[EXEC]   Magic: 0x%02x 0x%02x 0x%02x 0x%02x\n", ...);
kprintf("[EXEC]   Class: %d (64-bit=%d)\n", ...);
kprintf("[EXEC]   Machine: %d (x86_64=%d)\n", ...);
kprintf("[EXEC]   Type: %d (exec=%d, dyn=%d)\n", ...);
```

#### Program Header Processing
```c
// For each program header, show type and details
kprintf("[EXEC]   Segment %d: type=0x%x offset=0x%lx filesz=0x%lx memsz=0x%lx\n", ...);

// For PT_LOAD segments, show detailed loading info
kprintf("[EXEC]   Loading PT_LOAD segment %d:\n", i);
kprintf("[EXEC]     Virtual address: 0x%016lx (aligned: 0x%016lx - 0x%016lx)\n", ...);
kprintf("[EXEC]     Size: filesz=0x%lx memsz=0x%lx pages=%lu\n", ...);
kprintf("[EXEC]     Flags: %s%s%s\n", R/W/X);
```

#### Page Allocation and Mapping
```c
// Track page allocation progress
kprintf("[EXEC]     Allocating %lu pages...\n", num_pages);
kprintf("[EXEC]       Page %lu: vaddr=0x%016lx -> phys=%p\n", ...);

// Show segment data copying
kprintf("[EXEC]     Copying 0x%lx bytes from offset 0x%lx to vaddr 0x%016lx\n", ...);

// Show BSS zeroing
kprintf("[EXEC]     Zeroing BSS: 0x%lx bytes at 0x%016lx\n", ...);
```

#### User Stack Setup
```c
kprintf("[EXEC] Setting up user stack: 0x%016lx - 0x%016lx (%lu pages)\n", ...);
kprintf("[EXEC]   Stack page %lu: vaddr=0x%016lx -> phys=%p\n", ...);
kprintf("[EXEC] Stack setup complete, initial RSP=0x%016lx\n", stack_ptr);
```

#### Process Creation and Scheduling
```c
kprintf("[EXEC] Creating process control block...\n");
kprintf("[EXEC] Process structure created: PID=%d name='%s'\n", ...);
kprintf("[EXEC] Setting up CPU context...\n");
kprintf("[EXEC]   RIP=0x%016lx (entry point)\n", ...);
kprintf("[EXEC]   RSP=0x%016lx (stack pointer)\n", ...);
kprintf("[EXEC]   RFLAGS=0x%lx (IF=%d)\n", ...);
kprintf("[EXEC]   CR3=0x%016lx (page table)\n", ...);
kprintf("[EXEC] Process state set to READY\n");
kprintf("[EXEC] Adding process to scheduler...\n");
kprintf("[EXEC] SUCCESS: Process PID=%d loaded and scheduled\n", ...);
```

### 2. `kernel/core/sched/process.c` - Process Control Block Creation

Enhanced output in `process_create()`:
```c
kprintf("[PROCESS] Created process '%s' (PID %d)\n", ...);
kprintf("[PROCESS]   Entry point: %p\n", entry_point);
kprintf("[PROCESS]   State: %d (CREATED=%d, READY=%d)\n", ...);
kprintf("[PROCESS]   Kernel stack: %p - %p\n", ...);
kprintf("[PROCESS]   Context: RIP=%p RSP=%p RFLAGS=0x%lx CR3=0x%lx\n", ...);
kprintf("[PROCESS]   Time slice: %d, Ref count: %d\n", ...);
kprintf("[PROCESS]   Namespaces: %p\n", proc->namespaces);
```

### 3. `kernel/core/sched/scheduler.c` - Scheduler Startup

Enhanced output in `scheduler_start()`:
```c
kprintf("[SCHEDULER] ========================================\n");
kprintf("[SCHEDULER] Starting scheduler...\n");
kprintf("[SCHEDULER] ========================================\n");
kprintf("[SCHEDULER] Picking first process from ready queue...\n");
kprintf("[SCHEDULER] First process selected: '%s' (PID %d)\n", ...);
kprintf("[SCHEDULER]   Entry point: 0x%016lx\n", ...);
kprintf("[SCHEDULER]   Stack pointer: 0x%016lx\n", ...);
kprintf("[SCHEDULER]   RFLAGS: 0x%lx\n", ...);
kprintf("[SCHEDULER]   CR3: 0x%016lx\n", ...);
kprintf("[SCHEDULER]   Time slice: %d ticks\n", ...);
kprintf("[SCHEDULER]   State: %d -> RUNNING\n", ...);
kprintf("[SCHEDULER] Current process set to PID %d\n", ...);
kprintf("[SCHEDULER] ========================================\n");
kprintf("[SCHEDULER] Transitioning to ring 3 (user mode)...\n");
kprintf("[SCHEDULER] ========================================\n");
```

## Expected Boot Trace

When the kernel boots and loads init, you should see output like this:

```
[KERNEL] Loading /sbin/init...
[KERNEL] Found init: 12345 bytes

[EXEC] Loading ELF from memory: /sbin/init (12345 bytes)
[EXEC] Validating ELF header...
[EXEC]   Magic: 0x7f 0x45 0x4c 0x46
[EXEC]   Class: 2 (64-bit=2)
[EXEC]   Machine: 62 (x86_64=62)
[EXEC]   Type: 2 (exec=2, dyn=3)
[EXEC] Valid ELF64 executable
[EXEC]   Entry point: 0x0000000000401000
[EXEC]   Program headers: 3 entries at offset 0x40

[EXEC]   Segment 0: type=0x6 offset=0x40 filesz=0x1f8 memsz=0x1f8
[EXEC]   Segment 1: type=0x1 offset=0x1000 filesz=0x500 memsz=0x500
[EXEC]   Loading PT_LOAD segment 1:
[EXEC]     Virtual address: 0x0000000000401000 (aligned: 0x0000000000401000 - 0x0000000000402000)
[EXEC]     Size: filesz=0x500 memsz=0x500 pages=1
[EXEC]     Flags: R-X
[EXEC]     Allocating 1 pages...
[EXEC]       Page 0: vaddr=0x0000000000401000 -> phys=0xffff880001234000
[EXEC]     Copying 0x500 bytes from offset 0x1000 to vaddr 0x0000000000401000
[EXEC]   Segment 1 loaded successfully

[EXEC]   Segment 2: type=0x1 offset=0x2000 filesz=0x100 memsz=0x200
[EXEC]   Loading PT_LOAD segment 2:
[EXEC]     Virtual address: 0x0000000000402000 (aligned: 0x0000000000402000 - 0x0000000000403000)
[EXEC]     Size: filesz=0x100 memsz=0x200 pages=1
[EXEC]     Flags: RW-
[EXEC]     Allocating 1 pages...
[EXEC]       Page 0: vaddr=0x0000000000402000 -> phys=0xffff880001235000
[EXEC]     Copying 0x100 bytes from offset 0x2000 to vaddr 0x0000000000402000
[EXEC]     Zeroing BSS: 0x100 bytes at 0x0000000000402100
[EXEC]   Segment 2 loaded successfully

[EXEC] Setting up user stack: 0x00007ffffff00000 - 0x00007fffffffe000 (2048 pages)
[EXEC]   Stack page 0: vaddr=0x00007ffffff00000 -> phys=0xffff880001236000
[EXEC]   Stack page 2047: vaddr=0x00007fffffffd000 -> phys=0xffff880001a35000
[EXEC] Stack setup complete, initial RSP=0x00007fffffffe000

[EXEC] Creating process control block...
[PROCESS] Created process '/sbin/init' (PID 1)
[PROCESS]   Entry point: 0x0000000000401000
[PROCESS]   State: 0 (CREATED=0, READY=1)
[PROCESS]   Kernel stack: 0xffff880001a36000 - 0xffff880001a37000
[PROCESS]   Context: RIP=0x401000 RSP=0x7fffffffe000 RFLAGS=0x202 CR3=0x1000
[PROCESS]   Time slice: 0, Ref count: 1
[PROCESS]   Namespaces: 0xffff880001a38000

[EXEC] Process structure created: PID=1 name='/sbin/init'
[EXEC] Setting up CPU context...
[EXEC]   RIP=0x0000000000401000 (entry point)
[EXEC]   RSP=0x00007fffffffe000 (stack pointer)
[EXEC]   RFLAGS=0x202 (IF=1)
[EXEC]   CR3=0x0000000000001000 (page table)
[EXEC] Process state set to READY
[EXEC] Adding process to scheduler...
[SCHEDULER] Added process '/sbin/init' (PID 1) to ready queue (time_slice: 0, ref_count: 2)
[EXEC] SUCCESS: Process PID=1 loaded and scheduled
[EXEC]   Entry point: 0x0000000000401000
[EXEC]   Stack top: 0x00007fffffffe000
[EXEC]   Process ready to execute

[KERNEL] Init process started (PID 1)
[KERNEL] Initializing TSS for usermode...
[KERNEL] TSS initialized
[KERNEL] Starting scheduler...

[SCHEDULER] ========================================
[SCHEDULER] Starting scheduler...
[SCHEDULER] ========================================
[SCHEDULER] Picking first process from ready queue...
[SCHEDULER] First process selected: '/sbin/init' (PID 1)
[SCHEDULER]   Entry point: 0x0000000000401000
[SCHEDULER]   Stack pointer: 0x00007fffffffe000
[SCHEDULER]   RFLAGS: 0x202
[SCHEDULER]   CR3: 0x0000000000001000
[SCHEDULER]   Time slice: 10 ticks
[SCHEDULER]   State: 1 -> RUNNING
[SCHEDULER] Current process set to PID 1
[SCHEDULER] ========================================
[SCHEDULER] Transitioning to ring 3 (user mode)...
[SCHEDULER] ========================================

[USERMODE] ==========================================
[USERMODE] Transitioning to User Mode (Ring 3)
[USERMODE] ==========================================
[USERMODE] Entry point: 0x0000000000401000
[USERMODE] User stack allocated: 0xffff880001b00000 - 0xffff880001b04000 (size: 16 KB)
[USERMODE] Kernel stack allocated: 0xffff880001b05000 - 0xffff880001b09000 (size: 16 KB)
[USERMODE] TSS.RSP0 set to 0xffff880001b09000
[USERMODE] Switching to user mode...
[USERMODE] CPL will change: 0 (kernel) -> 3 (user)
[USERMODE] Segments: CS=0x1B, DS/SS=0x23

<-- Init process begins execution in user mode -->
```

## Verification Criteria

The ELF loading is working correctly if you see:

1. **ELF Magic Validation**: `Magic: 0x7f 0x45 0x4c 0x46` (7F 'E' 'L' 'F')
2. **ELF Class**: `Class: 2` (ELFCLASS64 = 2)
3. **Machine Type**: `Machine: 62` (EM_X86_64 = 62)
4. **Entry Point**: Valid user space address (< 0x00007FFF00000000)
5. **Segments Loaded**: All PT_LOAD segments successfully mapped
6. **Pages Allocated**: Physical pages allocated and mapped to virtual addresses
7. **Stack Setup**: 8MB user stack at 0x00007FFFFFFFE000
8. **Process Created**: PID assigned, state = READY
9. **Context Valid**: RIP = entry point, RSP = stack top, RFLAGS = 0x202 (IF=1)
10. **Scheduled**: Process added to ready queue and picked by scheduler
11. **Usermode Transition**: IRETQ executed to enter ring 3

## Debugging Failed Loading

If ELF loading fails, look for these error patterns:

### Invalid Magic Number
```
[EXEC]   Magic: 0x?? 0x?? 0x?? 0x??
[EXEC] ERROR: Invalid ELF header
```
**Fix**: Check that initrd contains valid ELF binary at `sbin/init`

### Wrong Architecture
```
[EXEC]   Machine: ?? (x86_64=62)
[ELF] Wrong architecture (machine=??, expected 62)
```
**Fix**: Rebuild init binary for x86_64 target

### Out of Memory
```
[EXEC]   ERROR: Out of physical memory at page ??/??
```
**Fix**: Increase available physical memory or reduce init binary size

### Segment Outside User Space
```
[EXEC]   ERROR: Segment vaddr 0x???? is outside user space
```
**Fix**: Rebuild init with correct linker script (user space = 0x00000000 - 0x00007FFFFFFFFFFF)

### No Processes in Ready Queue
```
[SCHEDULER] ERROR: No processes in ready queue!
```
**Fix**: Check that `scheduler_add_process()` was called successfully

## Code Changes Summary

- **kernel/fs/exec.c**: Added 15+ new debug kprintf statements
- **kernel/core/sched/process.c**: Enhanced process creation debug output
- **kernel/core/sched/scheduler.c**: Added detailed scheduler startup trace
- **Total LOC added**: ~50 lines of debug output

## Next Steps

1. Build the kernel with enhanced debug output
2. Boot with QEMU and capture serial output
3. Verify all debug milestones are reached
4. Check for any errors or unexpected values
5. If init fails to execute, analyze where the trace stops

## Related Files

- `kernel/include/elf.h` - ELF structure definitions
- `kernel/fs/elf_loader.c` - ELF validation helper functions
- `kernel/arch/x86_64/usermode.asm` - IRETQ-based ring 3 transition
- `kernel/core/usermode.c` - User mode entry point wrapper
