# User Mode Quick Start Guide

## What is User Mode?

User mode (ring 3) is a CPU privilege level with restricted access:
- Cannot execute privileged instructions (HLT, CLI, STI, LGDT, etc.)
- Cannot access kernel memory
- Cannot perform I/O operations directly
- Must use syscalls to interact with kernel

## Why User Mode?

**Security**: Isolates user programs from kernel
**Stability**: Buggy user code can't crash the kernel  
**Protection**: One process can't interfere with another

## Quick Integration

### 1. Initialize TSS (after GDT)

```c
#include "include/usermode.h"

void kernel_main(boot_info_t* boot_info) {
    // ... other initialization ...
    
    gdt_init();  // Must be called first
    tss_init();  // Initialize TSS after GDT
    
    // ... continue initialization ...
}
```

### 2. Switch to User Mode

```c
// Get entry point of your userspace program
uint64_t entry_point = load_elf_program("/bin/init");

// Switch to user mode (never returns)
start_usermode(entry_point);
```

### 3. User Code Example

```c
// This runs in ring 3
void user_main(void) {
    // Use syscalls for everything
    const char* msg = "Hello from userspace!\n";
    
    asm volatile(
        "mov rax, 3\n"      // SYS_WRITE
        "mov rdi, 1\n"      // stdout
        "mov rsi, %0\n"     // buffer
        "mov rdx, 23\n"     // length
        "syscall\n"
        :
        : "r"(msg)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11"
    );
}
```

## Key Functions

### `tss_init(void)`
Initialize Task State Segment. Must be called after GDT setup.

### `start_usermode(uint64_t entry)`
Switch to user mode at given entry point. Allocates stacks automatically. Never returns.

### `get_cpl(void)`
Returns current privilege level (0 = kernel, 3 = user). Useful for debugging.

### `tss_set_kernel_stack(uint64_t stack)`
Update kernel stack pointer in TSS. Used during context switches.

## Segment Selectors

| Selector | Ring | Type | Usage |
|----------|------|------|-------|
| 0x08 | 0 | Code | Kernel code segment |
| 0x10 | 0 | Data | Kernel data/stack segment |
| 0x1B | 3 | Code | User code segment |
| 0x23 | 3 | Data | User data/stack segment |
| 0x28 | 0 | TSS  | Task State Segment |

**Note**: Last 2 bits are RPL (Requested Privilege Level)
- 0x18 + RPL 3 = 0x1B (user code)
- 0x20 + RPL 3 = 0x23 (user data)

## Testing

### Build and Run
```bash
make clean
make
make qemu
```

### Expected Output
```
[GDT] GDT loaded (5 segments)
[TSS] TSS loaded (selector 0x28)
[USERMODE] Switching to user mode...
[USERMODE] CPL will change: 0 (kernel) -> 3 (user)
[SYSCALL] Dispatching syscall 3
Hello from userspace! (Ring 3)
```

### Verify with GDB
```gdb
# Check current privilege level
info registers cs
# CS should be 0x1b in user mode

# Check TSS is loaded
info registers tr
# TR should be 0x28

# Check TSS contents
x/26xg &tss
# RSP0 should have valid kernel stack address
```

## Common Mistakes

### ❌ Forgetting to call tss_init()
```c
gdt_init();
start_usermode(entry);  // WRONG! TSS not initialized
```

**Result**: Triple fault / system reboot

### ✅ Correct order
```c
gdt_init();
tss_init();
start_usermode(entry);  // Correct
```

### ❌ Using wrong segment selectors
```c
push 0x18  // WRONG! Missing RPL bits
push 0x1B  // Correct (0x18 + RPL 3)
```

### ❌ Not aligning stack
```c
uint64_t stack = (uint64_t)malloc(16384);
// stack may not be 16-byte aligned!
```

**Result**: #GP on function call

### ✅ Align stack properly
```c
uint64_t stack = (uint64_t)malloc(16384) + 16384;
stack &= ~0xF;  // Align to 16 bytes
```

## Next Steps

1. **ELF Loader**: Load userspace binaries from disk
2. **Process Management**: Create process table, fork/exec
3. **Memory Mapping**: Separate page tables for each process
4. **Context Switching**: Switch between multiple user processes

## File Summary

| File | Purpose | LOC |
|------|---------|-----|
| `kernel/arch/x86_64/usermode.asm` | IRETQ transition | 60 |
| `kernel/arch/x86_64/gdt.c` | TSS setup | 50 |
| `kernel/arch/x86_64/gdt.asm` | TSS flush | 5 |
| `kernel/core/usermode.c` | High-level API | 140 |
| `kernel/include/tss.h` | TSS structure | 40 |
| `kernel/include/usermode.h` | Public API | 30 |
| `kernel/core/test_usermode.c` | Test program | 50 |
| `kernel/core/init_usermode.c` | Initialization | 80 |
| **Total** | | **~455** |

## Troubleshooting

### System Triple Faults
- Check TSS is initialized: `tss_init()` called?
- Verify kernel stack is valid: `TSS.RSP0 != 0`
- Ensure GDT is loaded before TSS

### Syscalls Don't Work
- Verify MSR_STAR is configured: `syscall_msr_init()`
- Check syscall entry saves registers correctly
- Ensure SYSRET uses correct segment selectors

### #GP (General Protection Fault)
- Verify segment DPL matches CPL
- Check RPL bits in segment selectors (0x1B, 0x23)
- Ensure stack is 16-byte aligned

### User Code Doesn't Execute
- Check entry point is valid address
- Verify stack pointer is correct
- Use GDB to step through `enter_usermode`

## Support

For questions or issues:
1. Check logs for error messages
2. Use GDB to inspect registers and memory
3. Review documentation in `docs/USERMODE_IMPLEMENTATION.md`
4. Test with the included test program first

## Summary

User mode support is now implemented! The kernel can:
✅ Initialize TSS with kernel stack  
✅ Switch from ring 0 to ring 3  
✅ Handle syscalls (ring 3 → ring 0 → ring 3)  
✅ Execute user code with reduced privileges  

Next step: Load ELF binaries and create processes!
