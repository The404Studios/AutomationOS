# AutomationOS Quick Reference

**Version:** 0.1.0 | **Phase:** 1 | **Updated:** 2026-05-26

---

## System at a Glance

```
┌─────────────────────────────────────────────────────────────┐
│                      AutomationOS Phase 1                    │
│                   x86_64 UEFI Operating System               │
└─────────────────────────────────────────────────────────────┘

Architecture:    x86_64 (64-bit)
Boot Protocol:   UEFI
Kernel Type:     Monolithic, Higher-half (0xFFFFFFFF80000000)
Memory:          Buddy allocator (PMM) + 4-level paging (VMM)
Scheduler:       Round-robin, preemptive (10ms time slice)
```

---

## Quick Commands

### Build & Run
```bash
make all                    # Build everything
make qemu                   # Run in QEMU
make qemu-debug             # Run with GDB
make test                   # Integration tests
make clean                  # Clean build
```

### Development
```bash
make kernel                 # Build kernel only
make bootloader             # Build bootloader only
make userspace              # Build userspace only
make iso                    # Generate ISO only
```

### Debugging
```bash
# Terminal 1
make qemu-debug

# Terminal 2
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break kernel_main
(gdb) continue
```

---

## Memory Layout

```
0xFFFFFFFFFFFFFFFF ┌────────────────────┐
                   │   Kernel Space     │
0xFFFFFFFF80000000 ├────────────────────┤ Kernel Base
                   │  Non-canonical     │
0x00007FFFFFFFFFFF ├────────────────────┤
                   │   User Space       │
                   │                    │
0x0000000000000000 └────────────────────┘

Kernel Regions:
  Code/Data:  0xFFFFFFFF80000000 - 0xFFFFFFFF80FFFFFF (16 MB)
  Heap:       0xFFFFFFFF81000000 - 0xFFFFFFFF8FFFFFFF (~256 MB)
```

---

## Key Data Structures

### Process Control Block (PCB)
```c
typedef struct process {
    uint32_t pid;              // Process ID
    process_state_t state;     // CREATED, READY, RUNNING, etc.
    cpu_context_t context;     // Saved registers
    void* kernel_stack;        // Kernel mode stack
    void* user_stack;          // User mode stack
    uint64_t time_slice;       // Remaining ticks
    char name[64];             // Process name
} process_t;
```

### Memory Map Entry
```c
typedef struct {
    uint64_t base;             // Start address
    uint64_t length;           // Size in bytes
    uint32_t type;             // 1=usable, 2=reserved
} memory_map_entry_t;
```

---

## System Calls

| # | Name | Description | Args |
|---|------|-------------|------|
| 0 | `SYS_EXIT` | Exit process | status |
| 1 | `SYS_FORK` | Fork process | - |
| 2 | `SYS_READ` | Read from fd | fd, buf, count |
| 3 | `SYS_WRITE` | Write to fd | fd, buf, count |
| 8 | `SYS_GETPID` | Get PID | - |
| 9 | `SYS_SLEEP` | Sleep ms | milliseconds |

### Calling Convention
- Syscall number: `rax`
- Arguments: `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`
- Return: `rax`
- Instruction: `syscall`

---

## Interrupts & IRQs

### Exception Vectors (0-31)
```
0:  Divide by Zero        13: General Protection Fault
6:  Invalid Opcode        14: Page Fault
```

### Hardware IRQs (32-47)
```
32: PIT Timer (100 Hz)    33: PS/2 Keyboard
```

### Programmable Interrupt Controller (PIC)
```
Master PIC: 0x20 (command), 0x21 (data)
Slave PIC:  0xA0 (command), 0xA1 (data)
EOI: outb(0x20, 0x20)
```

---

## I/O Ports

### Serial (COM1)
```
Base: 0x3F8
  +0: Data register
  +1: Interrupt enable
  +3: Line control
  +5: Line status
```

### PS/2 Keyboard
```
0x60: Data port
0x64: Status/Command port
```

### PIT (Timer)
```
0x40: Channel 0 (system timer)
0x43: Mode/Command register
Frequency = 1193182 / divisor
```

---

## API Quick Reference

### Memory Management
```c
// Physical Memory
void* pmm_alloc_page(void);
void pmm_free_page(void* page);

// Virtual Memory
void* vmm_map_page(void* virt, void* phys, uint32_t flags);
void vmm_unmap_page(void* virt);

// Heap
void* kmalloc(size_t size);
void kfree(void* ptr);
```

### Process Management
```c
process_t* process_create(const char* name, void* entry);
void scheduler_add_process(process_t* proc);
process_t* process_get_current(void);
```

### Drivers
```c
// Serial
void serial_putchar(char c);
void serial_write(const char* str, size_t len);

// Keyboard
char ps2_getchar(void);

// Timer
uint64_t timer_get_ticks(void);
void timer_sleep(uint32_t ms);
```

### Kernel Utilities
```c
int kprintf(const char* format, ...);
void kernel_panic(const char* msg) __attribute__((noreturn));
#define ASSERT(cond) /* ... */
```

---

## Compiler Flags

### Kernel C Flags
```makefile
-std=c11                # C11 standard
-ffreestanding          # Freestanding environment
-nostdlib               # No standard library
-mcmodel=large          # 64-bit addressing
-mno-red-zone           # No red zone (for interrupts)
-mno-sse -mno-sse2      # No SSE (avoid float regs)
-Wall -Wextra -Werror   # All warnings as errors
-O2 -g                  # Optimize + debug symbols
```

### Linker Flags
```makefile
-T kernel.ld            # Linker script
-nostdlib               # No standard library
-zmax-page-size=0x1000  # 4KB page alignment
```

---

## Error Codes

```c
#define ESUCCESS    0   // Success
#define ENOTSUP    -1   // Not supported
#define EINVAL     -2   // Invalid argument
#define EBADF      -3   // Bad file descriptor
#define ENOMEM     -4   // Out of memory
#define ESRCH      -5   // No such process
```

---

## GDB Commands

```
break kernel_main       # Set breakpoint
continue                # Resume execution
step                    # Step into
next                    # Step over
print variable          # Print variable
x/16x 0xaddr            # Examine memory (hex)
info registers          # Show registers
backtrace               # Stack trace
disassemble             # Show assembly
layout src              # Show source window
```

---

## File Structure

```
kernel/
├── arch/x86_64/       # x86_64 architecture code
├── core/              # Core subsystems
│   ├── mem/           # Memory management
│   ├── sched/         # Scheduler
│   └── syscall/       # System calls
├── drivers/           # Device drivers
├── lib/               # Kernel library
├── include/           # Public headers
└── kernel.c           # Main entry point

boot/
├── boot.asm           # UEFI entry
├── loader.c           # Kernel loader
└── boot.h             # Boot structures

userspace/
├── libc/              # C library
├── init/              # Init process (PID 1)
└── shell/             # Simple shell
```

---

## Boot Sequence

```
1. UEFI Firmware
   └─> Loads EFI/BOOT/BOOTX64.EFI

2. AutoBoot (bootloader)
   └─> Gets memory map
   └─> Sets up framebuffer
   └─> Loads kernel
   └─> Jumps to kernel entry

3. Kernel Entry (boot.asm)
   └─> Sets up GDT
   └─> Sets up paging
   └─> Calls kernel_main()

4. kernel_main()
   └─> serial_init()
   └─> gdt_init()
   └─> pmm_init()
   └─> vmm_init()
   └─> heap_init()
   └─> idt_init()
   └─> pit_init(100)
   └─> framebuffer_init()
   └─> ps2_init()
   └─> sti()
   └─> Start init process
```

---

## Common Patterns

### Allocating a Page
```c
void* phys = pmm_alloc_page();
vmm_map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE);
// Use memory...
vmm_unmap_page(virt);
pmm_free_page(phys);
```

### Creating a Process
```c
process_t* proc = process_create("myproc", entry_point);
if (proc) {
    scheduler_add_process(proc);
}
```

### Writing a Driver Init
```c
void mydriver_init(void) {
    kprintf("[MYDRIVER] Initializing...\n");
    
    // Setup hardware
    outb(IO_PORT, ENABLE_CMD);
    
    // Test presence
    if (inb(IO_PORT + STATUS) == 0xFF) {
        kprintf("[MYDRIVER] Not found\n");
        return;
    }
    
    kprintf("[MYDRIVER] Ready\n");
}
```

---

## Debugging Checklist

When something goes wrong:

- [ ] Check serial output for error messages
- [ ] Verify initialization order in kernel_main()
- [ ] Check for NULL pointer dereferences
- [ ] Ensure interrupts are enabled (sti())
- [ ] Verify memory is mapped before access
- [ ] Check stack size (increase if overflow)
- [ ] Use ASSERT() for invariants
- [ ] Add kprintf() for flow tracing
- [ ] Attach GDB and inspect state
- [ ] Check QEMU logs with -d int,cpu_reset

---

## Performance Metrics

```
Context Switch:     ~500 cycles (~0.25 µs @ 2 GHz)
System Call:        ~150 cycles (~75 ns @ 2 GHz)
PMM Allocation:     O(log n) average
VMM Mapping:        O(1) with TLB
Scheduler Tick:     Every 10ms (100 Hz)
```

---

## Resource Limits (Phase 1)

```
Max Processes:      256 (configurable)
Page Size:          4 KB
Time Slice:         10 ms
Kernel Stack:       4 KB per process
User Stack:         4 KB per process
Max Syscalls:       256
IRQ Vectors:        16 (0-15)
```

---

## Useful Links

- **Documentation:** [docs/INDEX.md](INDEX.md)
- **Architecture:** [docs/ARCHITECTURE.md](ARCHITECTURE.md)
- **API Reference:** [docs/API_REFERENCE.md](API_REFERENCE.md)
- **Troubleshooting:** [docs/TROUBLESHOOTING.md](TROUBLESHOOTING.md)

---

## Version Info

Check version in `kernel/include/kernel.h`:
```c
#define KERNEL_VERSION_MAJOR 0
#define KERNEL_VERSION_MINOR 1
#define KERNEL_VERSION_PATCH 0
```

---

**Quick Reference Card | AutomationOS v0.1.0 | Phase 1 - Core Foundation**
