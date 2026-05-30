# User Mode Implementation Documentation

## Overview

This document describes the implementation of user mode (ring 3) support in AutomationOS. User mode is essential for running userspace applications with reduced privileges, providing security and isolation from the kernel.

## Architecture

### Privilege Levels (Rings)

x86_64 supports 4 privilege levels (rings):
- **Ring 0**: Kernel mode (highest privilege)
- **Ring 1**: Device drivers (unused in most modern OSes)
- **Ring 2**: Device drivers (unused in most modern OSes)  
- **Ring 3**: User mode (lowest privilege)

AutomationOS uses only Ring 0 (kernel) and Ring 3 (user).

### Key Concepts

#### Current Privilege Level (CPL)
The CPL is stored in the bottom 2 bits of the CS (Code Segment) register. The CPU checks CPL for every privileged operation.

#### Segments
- **Kernel Code (0x08)**: CPL=0, DPL=0, executable
- **Kernel Data (0x10)**: CPL=0, DPL=0, read/write
- **User Code (0x1B)**: CPL=3, DPL=3, executable
- **User Data (0x23)**: CPL=3, DPL=3, read/write

The last 2 bits (RPL) indicate the requested privilege level.

#### Task State Segment (TSS)
The TSS stores the kernel stack pointer (RSP0) that the CPU loads when transitioning from ring 3 to ring 0 (e.g., during a syscall or interrupt). This prevents userspace from controlling the kernel stack.

## Implementation Files

### Core Files

1. **kernel/arch/x86_64/usermode.asm**
   - Assembly function `enter_usermode(entry, stack)`
   - Uses IRETQ to transition from ring 0 to ring 3
   - Sets up user segments (CS=0x1B, DS/SS=0x23)
   - Enables interrupts (IF=1), clears IOPL (no I/O privileges)

2. **kernel/arch/x86_64/gdt.c**
   - Extended to support TSS descriptor
   - Defines TSS structure (104 bytes)
   - Functions: `tss_init()`, `tss_set_kernel_stack()`

3. **kernel/arch/x86_64/gdt.asm**
   - Added `tss_flush()` function
   - Loads TSS using LTR instruction

4. **kernel/core/usermode.c**
   - High-level API: `start_usermode(entry)`
   - Allocates user and kernel stacks
   - Sets up TSS with kernel stack
   - Calls assembly function to switch modes

5. **kernel/include/tss.h**
   - TSS structure definition
   - TSS management function declarations

6. **kernel/include/usermode.h**
   - Public API for user mode support

### Test Files

7. **kernel/core/test_usermode.c**
   - Simple test program that runs in ring 3
   - Executes syscalls to verify transitions work
   - Demonstrates sys_write and sys_getpid

8. **kernel/core/init_usermode.c**
   - Initialization wrapper
   - Functions: `init_usermode_support()`, `test_usermode_switch()`

## How It Works

### Initialization Sequence

1. **GDT Setup** (`gdt_init`)
   - Creates 5 standard GDT entries (null, kernel code/data, user code/data)
   - Adds TSS descriptor (16 bytes, entries 5-6)
   - Loads GDT with `lgdt`

2. **TSS Setup** (`tss_init`)
   - Zeros out TSS structure
   - Sets IOMAP base to sizeof(TSS) (no I/O bitmap)
   - Loads TSS descriptor into TR register with `ltr`

3. **Stack Allocation** (`start_usermode`)
   - Allocates 16KB user stack
   - Allocates 16KB kernel stack
   - Sets TSS.RSP0 = kernel stack top

4. **Mode Switch** (`enter_usermode`)
   - Builds IRETQ frame on current (kernel) stack:
     - SS (0x23 - user data)
     - RSP (user stack)
     - RFLAGS (IF=1, IOPL=0)
     - CS (0x1B - user code)
     - RIP (entry point)
   - Executes IRETQ
   - CPU switches to ring 3!

### Privilege Transition (Ring 3 → Ring 0 → Ring 3)

#### Syscall Entry (User → Kernel)

1. User code executes `SYSCALL` instruction
2. CPU checks MSR_LSTAR for kernel entry point
3. CPU saves:
   - RCX = return RIP
   - R11 = RFLAGS
4. CPU loads:
   - RIP = syscall_entry (kernel)
   - CS = kernel code (from MSR_STAR)
   - **BUT**: Stack is still user stack!
5. Kernel needs to switch to kernel stack immediately

**Note**: On syscall entry, the stack is NOT automatically switched. The TSS.RSP0 is only used for interrupts, not SYSCALL. For proper syscall handling, the kernel must manually switch stacks in the syscall entry code.

#### Syscall Exit (Kernel → User)

1. Kernel completes syscall handler
2. Restores user registers from stack
3. Executes `SYSRET` instruction
4. CPU loads:
   - RIP = RCX (saved return address)
   - RFLAGS = R11
   - CS = user code (from MSR_STAR + 16)
   - SS = user data (from MSR_STAR + 8)
5. Back to ring 3!

#### Interrupt Entry (User → Kernel)

1. Interrupt occurs (timer, keyboard, etc.)
2. CPU automatically:
   - Loads kernel stack from TSS.RSP0
   - Switches to kernel code segment
   - Pushes interrupt frame
3. IDT handler executes
4. IRETQ returns to user mode

## Security Features

### Privilege Separation
- User code runs at CPL=3
- Cannot execute privileged instructions (HLT, LGDT, LTR, etc.)
- Attempting privileged ops triggers #GP (General Protection Fault)

### Stack Isolation
- User stack is separate from kernel stack
- Kernel stack is set in TSS, not controlled by userspace
- Prevents userspace from corrupting kernel stack

### Memory Protection (Future)
- Page tables can mark pages as user-accessible (U/S bit)
- Ring 3 code can only access user pages
- Kernel pages are inaccessible from ring 3

### I/O Protection
- IOPL=0 in user mode RFLAGS
- User code cannot use IN/OUT instructions
- Must use syscalls for device access

## Testing

### Manual Test

Run the test script:
```bash
./test_usermode.sh
```

This verifies:
- Object files are built
- TSS symbols exist
- User mode symbols exist

### Boot Test

Build and run in QEMU:
```bash
make clean
make
make qemu
```

Look for these messages in serial output:
```
[GDT] GDT loaded (5 segments)
[TSS] Initializing Task State Segment...
[TSS] TSS loaded (selector 0x28)
[USERMODE] Transitioning to User Mode (Ring 3)
[USERMODE] Entry point: 0x...
[USERMODE] User stack allocated: ...
[USERMODE] Kernel stack allocated: ...
[USERMODE] TSS.RSP0 set to 0x...
[USERMODE] Switching to user mode...
[USERMODE] CPL will change: 0 (kernel) -> 3 (user)
[SYSCALL] Dispatching syscall 3  // sys_write
Hello from userspace! (Ring 3)
[SYSCALL] Dispatching syscall 8  // sys_getpid
```

### Verification Checklist

- [ ] TSS initialized successfully
- [ ] GDT contains user segments (0x1B, 0x23)
- [ ] CPL changes from 0 to 3
- [ ] User code executes
- [ ] Syscalls transition back to kernel (CPL=0)
- [ ] SYSRET returns to user mode (CPL=3)
- [ ] No General Protection Faults (#GP)

## Debugging

### Check CPL

In user mode code:
```c
uint8_t get_cpl(void) {
    uint16_t cs;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    return cs & 0x3;
}
```

Expected: 3 in user mode, 0 in kernel mode.

### GDB Commands

```gdb
# Break on mode switch
break enter_usermode

# Check segments
info registers cs ss ds es

# Expected in ring 3:
# cs = 0x1b (user code)
# ss = 0x23 (user data)

# Check TSS
x/26xg &tss

# TSS.RSP0 should contain kernel stack address
```

### Common Issues

1. **#GP (General Protection Fault)**
   - Check segment selectors are correct (0x1B, 0x23)
   - Verify DPL=3 in GDT entries
   - Ensure RPL=3 in segment selectors

2. **Triple Fault / Reboot**
   - TSS may not be initialized
   - Kernel stack may be invalid
   - Check TSS.RSP0 is valid address

3. **Syscall Doesn't Return**
   - Verify MSR_STAR is configured correctly
   - Check syscall entry saves/restores registers
   - Ensure SYSRET uses correct segments

4. **Stack Corruption**
   - Ensure stack is 16-byte aligned
   - Allocate enough stack space (16KB minimum)
   - Check stack grows downward

## Future Enhancements

### Phase 1 (Current)
- [x] Basic ring 0 → ring 3 transition
- [x] TSS setup
- [x] Syscall handler integration
- [x] Simple test program

### Phase 2 (Next)
- [ ] ELF loader for userspace programs
- [ ] Process management (fork, exec)
- [ ] Memory mapping for user processes
- [ ] User page tables with U/S bit

### Phase 3 (Future)
- [ ] Signal handling
- [ ] Context switches between processes
- [ ] FPU/SSE state saving
- [ ] Multithreading support

## References

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3
  - Chapter 6: Interrupt and Exception Handling
  - Chapter 7: Task Management
  - Chapter 5: Protection

- AMD64 Architecture Programmer's Manual, Volume 2
  - Chapter 8: System Management

- OSDev Wiki
  - https://wiki.osdev.org/Task_State_Segment
  - https://wiki.osdev.org/Getting_to_Ring_3
  - https://wiki.osdev.org/SYSENTER

## Summary

This implementation provides a complete user mode (ring 3) transition system for AutomationOS. The key components are:

1. **GDT** with user segments (0x1B, 0x23)
2. **TSS** with kernel stack (TSS.RSP0)
3. **Assembly function** to execute IRETQ
4. **Syscall integration** for kernel transitions
5. **Test program** to verify functionality

Total LOC: ~350 lines across 8 files.

The system is ready for ELF loading and process management!
