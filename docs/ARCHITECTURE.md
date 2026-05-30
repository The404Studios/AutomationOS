# AutomationOS Architecture Overview

**Version:** 0.1.0  
**Phase:** 1 - Core Foundation  
**Last Updated:** 2026-05-26

---

## Visual Documentation

**📊 [Complete Diagram Collection](diagrams/README.md)** - 13 professional diagrams covering all subsystems

Quick links to visual documentation:
- **[System Architecture Diagram](diagrams/system_architecture.svg)** - Overall system design
- **[Memory Layout Diagram](diagrams/memory_layout.svg)** - Virtual/physical memory
- **[Boot Sequence Diagram](diagrams/boot_sequence.svg)** - Boot process flowchart
- **[Syscall Sequence Diagram](diagrams/seq_syscall.svg)** - System call execution
- **[Context Switch Diagram](diagrams/seq_context_switch.svg)** - Process switching
- **[ASCII Diagrams](diagrams/ASCII_DIAGRAMS.md)** - Text-based diagrams for quick reference

---

## Table of Contents

1. [Introduction](#introduction)
2. [System Overview](#system-overview)
3. [Boot Process](#boot-process)
4. [Memory Architecture](#memory-architecture)
5. [Process Management](#process-management)
6. [System Calls](#system-calls)
7. [Driver Architecture](#driver-architecture)
8. [Interrupt Handling](#interrupt-handling)
9. [Data Flow](#data-flow)
10. [Security Model](#security-model)

---

## Introduction

AutomationOS is a modern, from-scratch operating system built for AI and automation workloads. The Phase 1 implementation delivers a minimal but complete foundation with a bootable x86_64 kernel, memory management, process scheduling, and basic userspace.

### Design Philosophy

- **Simplicity First:** Clean, understandable code over premature optimization
- **Modern Standards:** x86_64-only, UEFI boot, 64-bit addressing
- **AI-Native:** Designed from the ground up for AI/ML workloads (Phase 3+)
- **Testable:** Comprehensive testing infrastructure from day one

### Key Technologies

- **Architecture:** x86_64 (64-bit Intel/AMD)
- **Boot Protocol:** UEFI
- **Memory Model:** Higher-half kernel (0xFFFFFFFF80000000)
- **Page Size:** 4KB
- **Address Space:** 48-bit virtual addressing (4-level paging)

---

## System Overview

### Component Diagram

```
┌─────────────────────────────────────────────────────────┐
│                    Userspace (Ring 3)                    │
├─────────────────────────────────────────────────────────┤
│  Init Process  │   Shell   │  Utilities (echo, ls, ...)│
│      (PID 1)   │           │                            │
└─────────────────┬───────────┴────────────────────────────┘
                  │
         System Call Interface (syscall instruction)
                  │
┌─────────────────┴───────────────────────────────────────┐
│                   Kernel Space (Ring 0)                  │
├─────────────────────────────────────────────────────────┤
│  Process Scheduler  │  Memory Manager  │  System Calls  │
├─────────────────────────────────────────────────────────┤
│  Drivers: Serial, PS/2, Framebuffer, Timer (PIT)        │
├─────────────────────────────────────────────────────────┤
│  Interrupt Handlers: IDT, IRQs, Exceptions              │
├─────────────────────────────────────────────────────────┤
│  x86_64 Architecture: GDT, Paging, Context Switching    │
└─────────────────────────────────────────────────────────┘
                  │
┌─────────────────┴───────────────────────────────────────┐
│                    UEFI Bootloader                       │
│               (AutoBoot - Custom UEFI)                   │
└─────────────────────────────────────────────────────────┘
```

### Directory Structure

```
AutomationOS/
├── boot/                   # UEFI bootloader (AutoBoot)
│   ├── boot.asm           # UEFI entry point
│   ├── loader.c           # Kernel loading logic
│   └── boot.h             # Boot structures
│
├── kernel/                # Kernel core
│   ├── arch/x86_64/       # Architecture-specific code
│   │   ├── boot.asm       # Early kernel entry
│   │   ├── gdt.c/asm      # Global Descriptor Table
│   │   ├── idt.c          # Interrupt Descriptor Table
│   │   ├── interrupt.asm  # Interrupt handlers
│   │   ├── paging.c       # Page table management
│   │   ├── syscall.asm    # System call entry
│   │   └── context_switch.asm
│   │
│   ├── core/              # Core kernel subsystems
│   │   ├── mem/           # Memory management
│   │   │   ├── pmm.c      # Physical memory (buddy)
│   │   │   └── vmm.c      # Virtual memory (paging)
│   │   ├── sched/         # Process scheduling
│   │   │   ├── scheduler.c # Round-robin scheduler
│   │   │   ├── process.c   # Process management
│   │   │   └── context.c   # Context switching
│   │   └── syscall/       # System call interface
│   │       ├── syscall.c   # Dispatcher
│   │       └── handlers.c  # Syscall implementations
│   │
│   ├── drivers/           # Device drivers
│   │   ├── serial.c       # COM1 serial console
│   │   ├── ps2.c          # PS/2 keyboard
│   │   ├── framebuffer.c  # VGA/UEFI framebuffer
│   │   └── pit.c          # Programmable Interval Timer
│   │
│   ├── lib/               # Kernel library
│   │   ├── string.c       # String functions
│   │   ├── printf.c       # Kernel printf
│   │   └── panic.c        # Panic handler
│   │
│   ├── include/           # Kernel headers
│   │   ├── kernel.h       # Core definitions
│   │   ├── types.h        # Type definitions
│   │   ├── mem.h          # Memory API
│   │   ├── sched.h        # Scheduler API
│   │   ├── syscall.h      # System call API
│   │   ├── drivers.h      # Driver API
│   │   └── x86_64.h       # x86_64 definitions
│   │
│   └── kernel.c           # Kernel main entry
│
└── userspace/             # Userspace programs
    ├── libc/              # Minimal C library
    │   ├── syscall.c      # Syscall wrappers
    │   ├── string.c       # String functions
    │   └── stdio.c        # Basic I/O
    ├── init/              # Init process (PID 1)
    │   └── init.c
    └── shell/             # Simple shell
        ├── shell.c        # Main shell loop
        └── parser.c       # Command parser
```

---

## Boot Process

### Boot Sequence

```
Power On
    │
    ▼
┌──────────────────┐
│  UEFI Firmware   │  - Initialize hardware
│                  │  - Setup memory map
└────────┬─────────┘  - Load BOOTX64.EFI
         │
         ▼
┌──────────────────┐
│  AutoBoot        │  - Parse memory map
│  (boot/loader.c) │  - Setup framebuffer
└────────┬─────────┘  - Jump to kernel entry
         │
         ▼
┌──────────────────┐
│  Kernel Entry    │  - Setup GDT
│  (boot.asm)      │  - Setup paging
└────────┬─────────┘  - Jump to kernel_main
         │
         ▼
┌──────────────────┐
│  kernel_main()   │  1. Initialize serial
│  (kernel.c)      │  2. Initialize GDT
│                  │  3. Initialize PMM
│                  │  4. Initialize VMM
│                  │  5. Initialize heap
│                  │  6. Initialize IDT
│                  │  7. Initialize PIT timer
│                  │  8. Initialize framebuffer
│                  │  9. Initialize PS/2
│                  │ 10. Enable interrupts
│                  │ 11. Start init process
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│  Init Process    │  - PID 1
│  (init.c)        │  - Launch shell
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│  Shell           │  - User interaction
│  (shell.c)       │  - Command execution
└──────────────────┘
```

### Detailed Boot Steps

#### 1. UEFI Firmware Phase

- Hardware initialization
- POST (Power-On Self Test)
- Boot device detection
- Load `EFI/BOOT/BOOTX64.EFI` from ESP (EFI System Partition)

#### 2. AutoBoot Bootloader Phase

**Entry:** `efi_main()` in `boot/loader.c`

```c
void efi_main(void* image_handle, efi_system_table_t* systab) {
    // 1. Get memory map from UEFI
    // 2. Setup graphics mode (framebuffer)
    // 3. Load kernel from disk
    // 4. Jump to kernel entry point
}
```

**Boot Info Passed to Kernel:**
- Memory map (usable/reserved regions)
- Framebuffer address and dimensions
- Kernel entry point: `0xFFFFFFFF80100000`

#### 3. Early Kernel Initialization

**Entry:** `_start` in `kernel/arch/x86_64/boot.asm`

```nasm
; Setup stack
mov rsp, kernel_stack_top

; Setup GDT
call gdt_init

; Setup paging (identity + higher-half)
call setup_paging

; Jump to C code
call kernel_main
```

#### 4. Kernel Main

**Entry:** `kernel_main(boot_info_t* boot_info)` in `kernel/kernel.c`

See initialization sequence in [System Overview](#system-overview).

---

## Memory Architecture

### Address Space Layout

```
┌─────────────────────────────────────────┐ 0xFFFFFFFFFFFFFFFF
│       Kernel Space (Higher Half)        │
├─────────────────────────────────────────┤ 0xFFFFFFFF80000000
│                                         │   ↑ Kernel Base
│         (Non-canonical Hole)            │
│                                         │
├─────────────────────────────────────────┤ 0x00007FFFFFFFFFFF
│          User Space (Lower)             │
│                                         │
│  ┌──────────────────────────┐           │
│  │  User Stack (grows down) │           │
│  │          ↓               │           │
│  ├──────────────────────────┤           │
│  │       Heap (grows up)    │           │
│  │          ↑               │           │
│  ├──────────────────────────┤           │
│  │    BSS (uninitialized)   │           │
│  ├──────────────────────────┤           │
│  │    Data (initialized)    │           │
│  ├──────────────────────────┤           │
│  │    Text (code)           │           │
│  └──────────────────────────┘           │
└─────────────────────────────────────────┘ 0x0000000000000000
```

### Memory Management Subsystems

#### 1. Physical Memory Manager (PMM)

**Location:** `kernel/core/mem/pmm.c`

**Algorithm:** Buddy Allocator

**Features:**
- Page-based allocation (4KB granularity)
- Coalescing of free blocks
- O(log n) allocation/deallocation
- Memory statistics tracking

**API:**
```c
void pmm_init(memory_map_entry_t* mmap, uint32_t mmap_count);
void* pmm_alloc_page(void);
void pmm_free_page(void* page);
uint64_t pmm_get_free_memory(void);
```

**Data Structures:**
```c
typedef struct page {
    struct page* next;    // Free list chain
    uint8_t order;        // Buddy order (0-10)
    bool is_free;         // Allocation status
} page_t;

static page_t* free_lists[11];  // Orders 0-10
```

#### 2. Virtual Memory Manager (VMM)

**Location:** `kernel/core/mem/vmm.c`

**Algorithm:** 4-Level Paging (PML4 → PDPT → PD → PT)

**Features:**
- 48-bit virtual addressing
- Demand paging support
- Copy-on-write (COW) support
- Page table caching

**API:**
```c
void vmm_init(void);
void* vmm_map_page(void* virt, void* phys, uint32_t flags);
void vmm_unmap_page(void* virt);
void* vmm_get_physical(void* virt);
```

**Page Table Entry Format:**
```
63-52: Available for OS
51-12: Physical Address
11-0:  Flags (Present, Write, User, etc.)
```

**Page Flags:**
- `PAGE_PRESENT` (0x01): Page is present in memory
- `PAGE_WRITE` (0x02): Page is writable
- `PAGE_USER` (0x04): Accessible from user mode

#### 3. Kernel Heap

**Location:** `kernel/core/mem/vmm.c` (integrated)

**Algorithm:** Slab Allocator (simplified)

**Features:**
- Variable-size allocations
- Memory pooling for common sizes
- Heap expansion on demand

**API:**
```c
void heap_init(void);
void* kmalloc(size_t size);
void kfree(void* ptr);
```

### Memory Regions

| Region | Start | End | Purpose |
|--------|-------|-----|---------|
| Kernel Code/Data | 0xFFFFFFFF80000000 | 0xFFFFFFFF80FFFFFF | 16 MB |
| Kernel Heap | 0xFFFFFFFF81000000 | 0xFFFFFFFF8FFFFFFF | ~256 MB |
| Memory Map | 0x0000000000100000 | Variable | Physical RAM |

---

## Process Management

### Process States

```
   ┌─────────┐
   │ CREATED │
   └────┬────┘
        │ scheduler_add_process()
        ▼
   ┌────────┐
   │ READY  │◄───────────────┐
   └────┬───┘                │
        │ schedule()         │
        ▼                    │
   ┌─────────┐               │
   │ RUNNING │───────────────┘
   └────┬────┘   time slice expired
        │
        │ sys_exit()
        ▼
   ┌────────────┐
   │ TERMINATED │
   └────────────┘
```

### Process Control Block (PCB)

**Location:** Defined in `kernel/include/sched.h`

```c
typedef struct process {
    uint32_t pid;              // Process ID
    uint32_t parent_pid;       // Parent PID
    process_state_t state;     // Current state
    cpu_context_t context;     // Saved registers
    void* kernel_stack;        // Kernel mode stack
    void* user_stack;          // User mode stack
    uint64_t time_slice;       // Remaining ticks
    uint64_t total_time;       // Total CPU time
    struct process* next;      // Queue linkage
    char name[64];             // Process name
} process_t;
```

### CPU Context

Saved during context switches:

```c
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cr3;  // Page directory
} cpu_context_t;
```

### Scheduler

**Location:** `kernel/core/sched/scheduler.c`

**Algorithm:** Round-Robin with Time Slicing

**Time Quantum:** 10ms (configurable)

**Scheduler Tick:** Called from PIT timer interrupt (100 Hz)

**Data Structure:**
```c
static process_t* ready_queue = NULL;  // Circular queue
static process_t* current_process = NULL;
```

**Scheduler Workflow:**
```
Timer Interrupt (100 Hz)
    │
    ▼
schedule()
    │
    ├──> Decrement current process time slice
    │
    ├──> If time slice == 0:
    │    ├──> Save current process context
    │    ├──> Pick next process (round-robin)
    │    ├──> Restore next process context
    │    └──> context_switch()
    │
    └──> Return
```

### Context Switching

**Location:** `kernel/arch/x86_64/context_switch.asm`

**Assembly Implementation:**
```nasm
context_switch:
    ; Save current context
    push all registers
    mov [from->context.rsp], rsp
    
    ; Switch page tables
    mov cr3, to->context.cr3
    
    ; Restore new context
    mov rsp, [to->context.rsp]
    pop all registers
    ret
```

---

## System Calls

### System Call Interface

**Mechanism:** x86_64 `syscall` instruction

**Entry Point:** `syscall_entry` in `kernel/arch/x86_64/syscall.asm`

**Calling Convention:**
- System call number: `rax`
- Arguments: `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`
- Return value: `rax`

### System Call Table

| Number | Name | Description |
|--------|------|-------------|
| 0 | `SYS_EXIT` | Terminate process |
| 1 | `SYS_FORK` | Create child process |
| 2 | `SYS_READ` | Read from file descriptor |
| 3 | `SYS_WRITE` | Write to file descriptor |
| 4 | `SYS_OPEN` | Open file |
| 5 | `SYS_CLOSE` | Close file descriptor |
| 6 | `SYS_WAITPID` | Wait for process |
| 7 | `SYS_EXECVE` | Execute program |
| 8 | `SYS_GETPID` | Get process ID |
| 9 | `SYS_SLEEP` | Sleep for milliseconds |

### System Call Flow

```
┌──────────────┐
│  User Code   │  syscall(SYS_WRITE, ...)
└──────┬───────┘
       │
       ▼
┌──────────────┐
│  libc        │  wrapper function
│  syscall()   │  mov rax, SYS_WRITE
└──────┬───────┘  syscall instruction
       │
       ▼
┌──────────────────────┐
│  Kernel              │
│  syscall_entry       │  (syscall.asm)
│  ├─ Save user regs   │
│  ├─ Switch to kernel │
│  └─ Call dispatcher  │
└──────┬───────────────┘
       │
       ▼
┌──────────────────────┐
│  syscall_dispatch()  │  (syscall.c)
│  ├─ Validate number  │
│  ├─ Check args       │
│  └─ Call handler     │
└──────┬───────────────┘
       │
       ▼
┌──────────────────────┐
│  sys_write()         │  (handlers.c)
│  └─ Implement logic  │
└──────┬───────────────┘
       │
       ▼ return
┌──────────────────────┐
│  syscall_entry       │
│  ├─ Restore user ctx │
│  └─ sysretq          │
└──────┬───────────────┘
       │
       ▼
┌──────────────┐
│  User Code   │  result in rax
└──────────────┘
```

### Error Codes

```c
#define ESUCCESS    0   // Success
#define ENOTSUP    -1   // Not supported
#define EINVAL     -2   // Invalid argument
#define EBADF      -3   // Bad file descriptor
#define ENOMEM     -4   // Out of memory
#define ESRCH      -5   // No such process
```

---

## Driver Architecture

### Driver Model

All drivers follow a simple initialization pattern:

```c
void driver_init(void);         // Initialize driver
void driver_operation(...);     // Driver operations
```

### Serial Driver (COM1)

**Location:** `kernel/drivers/serial.c`

**Purpose:** Debug console and logging

**Initialization:**
```c
void serial_init(void) {
    outb(0x3F8 + 1, 0x00);  // Disable interrupts
    outb(0x3F8 + 3, 0x80);  // Enable DLAB
    outb(0x3F8 + 0, 0x03);  // Set divisor (38400 baud)
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);  // 8n1
    outb(0x3F8 + 2, 0xC7);  // Enable FIFO
}
```

**API:**
- `serial_putchar(char c)` - Write single character
- `serial_write(const char* str, size_t len)` - Write string

### PS/2 Keyboard Driver

**Location:** `kernel/drivers/ps2.c`

**Purpose:** Keyboard input

**Ports:**
- Data: 0x60
- Status/Command: 0x64

**API:**
- `ps2_init(void)` - Initialize controller
- `ps2_getchar(void)` - Read character (blocking)

### Framebuffer Driver

**Location:** `kernel/drivers/framebuffer.c`

**Purpose:** Graphics output

**API:**
- `framebuffer_init(void* addr, uint32_t w, uint32_t h, uint32_t pitch)`
- `framebuffer_clear(uint32_t color)`
- `framebuffer_putchar(char c, uint32_t x, uint32_t y, uint32_t color)`

### PIT (Programmable Interval Timer)

**Location:** `kernel/drivers/pit.c`

**Purpose:** System timer (scheduler ticks)

**Configuration:**
- Frequency: 100 Hz (10ms per tick)
- IRQ: 0

**API:**
- `pit_init(uint32_t frequency)` - Configure timer
- `timer_get_ticks(void)` - Get tick count
- `timer_sleep(uint32_t ms)` - Sleep (busy-wait)

---

## Interrupt Handling

### Interrupt Descriptor Table (IDT)

**Location:** `kernel/arch/x86_64/idt.c`

**Size:** 256 entries

**Entry Format:**
```c
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} idt_entry_t;
```

### Interrupt Vectors

| Vector | Type | Description |
|--------|------|-------------|
| 0-31 | Exceptions | CPU exceptions |
| 32-47 | IRQs | Hardware interrupts |
| 48-127 | Reserved | Future use |
| 128 | Syscall | System call (legacy) |
| 129-255 | Available | Custom interrupts |

### Exception Handlers

Standard x86_64 exceptions (0-31):
- 0: Division by zero
- 6: Invalid opcode
- 13: General protection fault
- 14: Page fault

### IRQ Handlers

Hardware interrupts (32-47):
- 32: PIT timer (scheduler tick)
- 33: PS/2 keyboard

### Interrupt Handler Flow

```
Hardware Interrupt (e.g., Timer)
    │
    ▼
┌─────────────────┐
│  IDT Lookup     │  Vector 32 → interrupt_handler_32
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  interrupt.asm  │  - Save context
│                 │  - Call C handler
│                 │  - Restore context
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  timer_handler()│  - Increment ticks
│                 │  - Call schedule()
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  Send EOI       │  Acknowledge interrupt
└────────┬────────┘
         │
         ▼
    iretq (return)
```

---

## Data Flow

### Typical User Program Execution

```
User starts shell
    │
    ▼
Shell reads input (SYS_READ)
    ├──> System call entry
    ├──> sys_read() handler
    ├──> PS/2 driver
    └──> Return to userspace
    │
    ▼
Shell parses command
    │
    ▼
Shell executes program (SYS_FORK + SYS_EXECVE)
    ├──> Create new process
    ├──> Load program binary
    └──> Schedule process
    │
    ▼
Program runs
    │
    ▼
Program writes output (SYS_WRITE)
    ├──> System call entry
    ├──> sys_write() handler
    ├──> Serial/framebuffer driver
    └──> Return to userspace
    │
    ▼
Program exits (SYS_EXIT)
    ├──> System call entry
    ├──> sys_exit() handler
    ├──> Terminate process
    └──> Return to parent (shell)
```

---

## Security Model

### Privilege Levels

AutomationOS uses x86_64 privilege rings:

- **Ring 0 (Kernel):** Full hardware access
- **Ring 3 (User):** Restricted access

### Memory Protection

1. **Page-Level Protection:**
   - Kernel pages: `PAGE_PRESENT | PAGE_WRITE` (ring 0 only)
   - User pages: `PAGE_PRESENT | PAGE_WRITE | PAGE_USER` (ring 3 accessible)

2. **Address Space Isolation:**
   - Each process has its own page table (CR3)
   - Kernel mapped in higher half (shared across processes)

3. **Stack Protection:**
   - Separate kernel and user stacks
   - Stack canaries (future enhancement)

### System Call Validation

All system calls validate:
1. System call number in range
2. User pointers are valid
3. Buffer sizes are reasonable
4. File descriptors are valid

### Limitations (Phase 1)

- No ASLR (Address Space Layout Randomization)
- No DEP (Data Execution Prevention) enforcement
- No capability-based security
- No sandboxing

These will be addressed in Phase 2+.

---

## Performance Characteristics

### Scheduler Overhead

- Context switch time: ~500 cycles (~0.25 μs @ 2 GHz)
- Scheduler invocation: Every 10ms (100 Hz timer)
- Worst-case scheduling latency: 10ms

### Memory Allocation

- PMM allocation: O(log n) average case
- VMM page mapping: O(1) with TLB caching
- kmalloc(): O(1) for common sizes

### System Call Overhead

- Syscall instruction: ~100 cycles
- Handler dispatch: ~50 cycles
- Total overhead: ~150-200 cycles (~75-100 ns @ 2 GHz)

---

## Future Enhancements (Phase 2+)

1. **File System:**
   - Custom AutoFS implementation
   - VFS layer for multiple filesystems

2. **Networking:**
   - TCP/IP stack
   - virtio-net driver

3. **Multi-threading:**
   - Thread support (lightweight processes)
   - Futex primitives

4. **IPC:**
   - Message passing
   - Shared memory

5. **AI Integration:**
   - ML model loading
   - Inference API
   - GPU acceleration (CUDA/ROCm)

---

## References

- [OSDev Wiki](https://wiki.osdev.org/)
- [Intel Software Developer Manual](https://www.intel.com/sdm)
- [System V ABI](https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf)
- [UEFI Specification](https://uefi.org/specifications)

---

**End of Architecture Overview**
