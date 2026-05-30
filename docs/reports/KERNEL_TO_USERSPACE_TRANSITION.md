# Kernel-to-Userspace Transition Implementation

## Summary

Successfully implemented the kernel-to-userspace transition for AutomationOS, enabling the kernel to load `/sbin/init` from initrd and execute it as PID 1 in ring 3 (user mode).

## Changes Made

### 1. Scheduler Start Function (`kernel/core/sched/scheduler.c`)

Added `scheduler_start()` function that:
- Picks the first process from the ready queue
- Sets it as the current running process
- Transitions to ring 3 (user mode) using `enter_usermode()`
- Never returns (the timer interrupt will handle scheduling from this point)

**Key Implementation Details:**
```c
void scheduler_start(void) {
    // Pick the first process to run
    process_t* first = scheduler_pick_next();
    
    // Set as current process
    first->state = PROCESS_RUNNING;
    process_set_current(first);
    
    // Jump to user mode (ring 3) via IRETQ
    enter_usermode(first->context.rip, first->context.rsp);
}
```

### 2. Header Updates (`kernel/include/sched.h`)

Added function declaration:
```c
void scheduler_start(void) NORETURN;  // Start scheduler (does not return)
```

### 3. Kernel Main Updates (`kernel/kernel.c`)

Added three critical initialization steps:

#### a. Process Table Initialization (after VFS)
```c
kprintf("[KERNEL] Initializing process table...\n");
process_init();
kprintf("[KERNEL] Process table initialized\n");
```

#### b. Scheduler Initialization (after process init)
```c
kprintf("[KERNEL] Initializing scheduler...\n");
scheduler_init();
kprintf("[KERNEL] Scheduler initialized\n");
```

#### c. TSS Initialization (before scheduler start)
```c
/* Initialize TSS for ring 3 transitions */
kprintf("[KERNEL] Initializing TSS for usermode...\n");
tss_init();
kprintf("[KERNEL] TSS initialized\n");
```

### 4. Include Added (`kernel/core/sched/scheduler.c`)

Added usermode header:
```c
#include "../../include/usermode.h"
```

## Boot Flow

The complete boot flow is now:

1. **Kernel Initialization**
   - Serial, GDT, IDT setup
   - PMM, VMM, Heap initialization
   - Timer, Keyboard, VFS initialization
   - **Process table initialization** (NEW)
   - **Scheduler initialization** (NEW)

2. **Initrd Loading**
   - Mount initrd filesystem
   - Locate `/sbin/init` binary
   - Load ELF binary using `elf_load_and_exec()`

3. **Init Process Setup**
   - Parse ELF headers and segments
   - Allocate and map user memory pages
   - Setup user stack (8MB at 0x00007FFFFFFFE000)
   - Create process control block (PCB)
   - Add to scheduler ready queue

4. **Ring Transition** (NEW)
   - Initialize TSS (Task State Segment)
   - Enable interrupts (`sti()`)
   - Call `scheduler_start()`
   - Transition to ring 3 via `enter_usermode()`

5. **User Mode Execution**
   - Init process runs at CPL=3
   - Timer interrupts preempt and return to kernel
   - Scheduler handles context switching

## Technical Details

### TSS (Task State Segment)
Required for:
- Storing kernel stack pointer (RSP0)
- Hardware uses TSS.RSP0 when transitioning from ring 3 to ring 0
- Set up by `tss_init()` before first user mode transition

### User Mode Transition (`enter_usermode`)
Uses IRETQ instruction to change privilege level:
1. Setup user data segments (DS, ES, FS, GS = 0x23)
2. Push IRETQ stack frame:
   - SS (0x23 - user data segment)
   - RSP (user stack pointer)
   - RFLAGS (IF=1, IOPL=0)
   - CS (0x1B - user code segment)
   - RIP (entry point)
3. Execute IRETQ → transitions to ring 3

### Segment Selectors
- **Kernel Code**: 0x08 (GDT entry 1, RPL=0)
- **Kernel Data**: 0x10 (GDT entry 2, RPL=0)
- **User Code**: 0x1B (GDT entry 3, RPL=3)
- **User Data**: 0x23 (GDT entry 4, RPL=3)

## Dependencies

This implementation relies on:
1. **Keyboard init fix** (agent 1) - Ensures timer/interrupts work correctly
2. **elf_load_and_exec()** (agent 2) - Already implemented in `kernel/fs/exec.c`
3. **Initrd mounting** (agent 3) - Already working in `kernel/kernel.c`

## Expected Output

When init successfully starts, you should see:

```
[KERNEL] Loading /sbin/init...
[KERNEL] Found init: XXXXX bytes
[EXEC] Loading ELF from memory: /sbin/init (XXXXX bytes)
[EXEC] Valid ELF64 executable
[EXEC]   Entry point: 0x00000000XXXXXXXX
[EXEC]   Program headers: X entries
[EXEC]   Loading segment: vaddr=0x00000000XXXXXXXX size=0xXXX pages=X
[EXEC] Setting up user stack: 0x00007FFFFFF00000 - 0x00007FFFFFFFE000
[EXEC] Process created and scheduled: PID=1 entry=0x00000000XXXXXXXX stack=0x00007FFFFFFFE000
[KERNEL] Init process started (PID 1)
[KERNEL] Initializing TSS for usermode...
[KERNEL] TSS initialized
[KERNEL] Starting scheduler...
[SCHEDULER] Starting scheduler...
[SCHEDULER] Starting first process: '/sbin/init' (PID 1)
[SCHEDULER]   Entry: 0x00000000XXXXXXXX
[SCHEDULER]   Stack: 0x00007FFFFFFFE000
[SCHEDULER] Transitioning to ring 3...
```

Followed by init's output (from user mode):
```
AutomationOS Init (PID 1)
```

## Debugging Tips

### Page Fault
If you see a page fault:
- Check that user pages are mapped with `PAGE_USER` flag
- Verify stack is allocated and mapped
- Check that virtual addresses are in user space (< 0x00007FFFFFFFFFFF)

### General Protection Fault (GPF)
If you see GPF:
- Check segment selectors (0x1B for CS, 0x23 for SS/DS)
- Verify GDT has user code/data segments with DPL=3
- Check that TSS is initialized before transition

### Triple Fault / Reboot
If system reboots:
- TSS not initialized → kernel has no valid stack on interrupt
- Invalid IDT entries → can't handle timer interrupt
- Stack not mapped → page fault on first user instruction

## Files Modified

1. `kernel/core/sched/scheduler.c` - Added `scheduler_start()` function
2. `kernel/include/sched.h` - Added `scheduler_start()` declaration
3. `kernel/kernel.c` - Added process_init(), scheduler_init(), tss_init() calls

## Next Steps

After this implementation:
1. Verify init starts and prints message
2. Check that timer interrupts work (preemption)
3. Test context switching between kernel and user mode
4. Implement system calls for init to interact with kernel
5. Add more userspace programs

## Status

✅ Implementation complete
⏳ Waiting for build/test with proper toolchain
🔄 Ready for integration testing
