# ELF Loading Code Flow Reference

**Purpose:** Quick reference for tracing init ELF loading through the kernel

## Call Stack

```
kernel.c:kernel_main()
  ↓
kernel.c:340 → initrd_get_file("sbin/init", &init_size)
  ↓
kernel.c:348 → elf_load_and_exec(init_data, init_size, "/sbin/init")
  ↓
  ├─ exec.c:209 → elf_validate_header(ehdr)
  │    ↓
  │    └─ elf_loader.c:24 → Validates ELF magic, class, machine, type
  │
  ├─ exec.c:230-288 → Load PT_LOAD segments
  │    ↓
  │    ├─ For each PT_LOAD program header:
  │    │   ├─ Calculate aligned virtual address range
  │    │   ├─ pmm_alloc_page() → Allocate physical pages
  │    │   ├─ vmm_map_page() → Map to user virtual addresses
  │    │   ├─ memcpy() → Copy segment data from ELF buffer
  │    │   └─ memset() → Zero BSS region (memsz > filesz)
  │    │
  │    └─ exec.c:290-310 → Setup user stack (8MB at 0x7FFFFFFFE000)
  │         ├─ pmm_alloc_page() → Allocate 2048 stack pages
  │         └─ vmm_map_page() → Map with PAGE_WRITE | PAGE_USER
  │
  ├─ exec.c:316 → process_create(name, entry_point)
  │    ↓
  │    └─ process.c:58-153 → Create process control block
  │         ├─ Allocate PID atomically
  │         ├─ kmalloc(sizeof(process_t))
  │         ├─ pmm_alloc_page() → Allocate kernel stack
  │         ├─ Initialize CPU context:
  │         │   ├─ context.rip = entry_point (ELF e_entry)
  │         │   ├─ context.rsp = kernel_stack_top
  │         │   ├─ context.rflags = 0x202 (IF=1)
  │         │   └─ context.cr3 = read_cr3() (page table)
  │         ├─ Create/clone namespaces
  │         └─ Add to process_table[]
  │
  ├─ exec.c:324-326 → Override context for user mode
  │    ├─ context.rip = ehdr->e_entry
  │    ├─ context.rsp = USER_STACK_TOP (0x7FFFFFFFE000)
  │    └─ context.rflags = 0x202
  │
  ├─ exec.c:329 → Set state = PROCESS_READY
  │
  └─ exec.c:332 → scheduler_add_process(proc)
       ↓
       └─ scheduler.c:57-102 → Add to ready queue
            ├─ Take reference (prevents premature free)
            ├─ Set state = PROCESS_READY
            ├─ Add to tail of ready_queue
            └─ Increment ready_count
```

## Scheduler Startup Flow

```
kernel.c:361 → scheduler_start()
  ↓
scheduler.c:316-345 → Start first process
  ↓
  ├─ scheduler.c:320 → scheduler_pick_next()
  │    ↓
  │    └─ scheduler.c:157-219 → Pick from ready queue
  │         ├─ Remove from head of ready_queue
  │         ├─ If time_slice == 0: time_slice = DEFAULT_TIME_SLICE (10)
  │         └─ Return process (with reference)
  │
  ├─ scheduler.c:331 → Set state = PROCESS_RUNNING
  ├─ scheduler.c:332 → process_set_current(first)
  │
  └─ scheduler.c:338 → enter_usermode(rip, rsp)
       ↓
       └─ usermode.c:92-132 → Prepare for ring 3 transition
            ↓
            ├─ Allocate user stack (if not provided)
            ├─ Allocate kernel stack for TSS
            ├─ tss_set_kernel_stack(kernel_stack)
            │
            └─ usermode.asm:12-57 → IRETQ to user mode
                 ↓
                 ├─ Set DS/ES/FS/GS = 0x23 (user data selector)
                 ├─ Push IRETQ frame:
                 │   ├─ SS = 0x23 (user data, RPL=3)
                 │   ├─ RSP = user_stack
                 │   ├─ RFLAGS = 0x202 (IF=1, IOPL=0)
                 │   ├─ CS = 0x1B (user code, RPL=3)
                 │   └─ RIP = entry_point
                 │
                 └─ IRETQ → Transition to ring 3
                      ↓
                      └─ CPU loads CS=0x1B → CPL=3 (user mode)
                         Execution continues at RIP with RSP
```

## Key Memory Addresses

### User Space (CPL=3)
```
0x0000000000000000  ← Start of user space
      ...
0x0000000000400000  ← Typical ELF load address
      ...
0x00007FFFFFFFE000  ← USER_STACK_TOP (grows down)
0x00007FFFFFFFFFFF  ← End of user space
```

### Kernel Space (CPL=0)
```
0x0000800000000000  ← KERNEL_SPACE_START
      ...
0xFFFF880000000000  ← Physical memory mapping
      ...
0xFFFFFFFFFFFFFFFF  ← End of address space
```

## Process Context Structure

```c
typedef struct {
    uint64_t rip;      // Instruction pointer (ELF entry point)
    uint64_t rsp;      // Stack pointer (USER_STACK_TOP for init)
    uint64_t rflags;   // Flags register (0x202 = IF enabled)
    uint64_t cr3;      // Page table base (from read_cr3())
    // ... other saved registers ...
} cpu_context_t;
```

## Critical Constants

```c
// User stack (defined in exec.c)
#define USER_STACK_SIZE  (8 * 1024 * 1024)     // 8MB
#define USER_STACK_TOP   0x00007FFFFFFFE000ULL // Just below kernel space

// User space boundary (defined in mem.h)
#define USER_SPACE_END   0x0000800000000000ULL // 128TB

// Page flags for user segments
#define PAGE_PRESENT  0x001  // Page is present in memory
#define PAGE_WRITE    0x002  // Page is writable
#define PAGE_USER     0x004  // Page is accessible from user mode

// GDT selectors
#define USER_CODE_SELECTOR  0x1B  // GDT entry 3 + RPL=3
#define USER_DATA_SELECTOR  0x23  // GDT entry 4 + RPL=3

// ELF constants
#define ELFCLASS64     2      // 64-bit ELF
#define EM_X86_64     62      // x86-64 architecture
#define ET_EXEC        2      // Executable file
#define ET_DYN         3      // Position-independent executable
#define PT_LOAD        1      // Loadable segment
```

## Debug Checkpoints

### Checkpoint 1: ELF Found in Initrd
```
[KERNEL] Found init: XXXXX bytes
```
**If missing**: initrd not mounted or sbin/init not present

### Checkpoint 2: ELF Header Validated
```
[EXEC] Valid ELF64 executable
```
**If missing**: Invalid magic, wrong class, or wrong architecture

### Checkpoint 3: Segments Loaded
```
[EXEC]   Segment N loaded successfully
```
**If missing**: Out of memory or mapping failure

### Checkpoint 4: Process Created
```
[PROCESS] Created process '/sbin/init' (PID 1)
```
**If missing**: Process creation failed (no memory or namespaces)

### Checkpoint 5: Process Scheduled
```
[SCHEDULER] Added process '/sbin/init' (PID 1) to ready queue
```
**If missing**: scheduler_add_process() not called

### Checkpoint 6: Scheduler Started
```
[SCHEDULER] First process selected: '/sbin/init' (PID 1)
```
**If missing**: Ready queue was empty

### Checkpoint 7: Ring Transition
```
[SCHEDULER] Transitioning to ring 3 (user mode)...
```
**If you see this**: Transition is about to happen via IRETQ

### Checkpoint 8: User Mode Entered
```
<-- No more kernel output after this point -->
```
**If you reach here**: Init is executing in user mode!

## Common Failure Modes

### 1. Segment Fault Before User Mode
**Symptom**: Triple fault or page fault before IRETQ  
**Cause**: Invalid memory mapping or page table corruption  
**Check**: VMM page table integrity, PMM allocator

### 2. Triple Fault on IRETQ
**Symptom**: CPU reset immediately after "Transitioning to ring 3"  
**Cause**: Invalid IRETQ frame (bad CS/SS selectors or stack)  
**Check**: GDT configuration, TSS setup, stack alignment

### 3. Page Fault in User Mode
**Symptom**: Page fault exception with error code  
**Cause**: Segment not mapped, wrong permissions, or stack overflow  
**Check**: PT_LOAD loading, page flags (PAGE_USER), stack allocation

### 4. General Protection Fault
**Symptom**: GP fault with error code  
**Cause**: Privilege violation, bad segment selector, or invalid instruction  
**Check**: Segment selectors (0x1B/0x23), entry point address

## Files Involved

| File | Purpose | Key Functions |
|------|---------|---------------|
| `kernel/kernel.c` | Boot sequence | Loads init from initrd |
| `kernel/fs/exec.c` | ELF loading | `elf_load_and_exec()` |
| `kernel/fs/elf_loader.c` | ELF validation | `elf_validate_header()` |
| `kernel/core/sched/process.c` | Process management | `process_create()` |
| `kernel/core/sched/scheduler.c` | Scheduling | `scheduler_add_process()`, `scheduler_start()` |
| `kernel/core/usermode.c` | Ring transition | `start_usermode()` wrapper |
| `kernel/arch/x86_64/usermode.asm` | Assembly transition | `enter_usermode()` with IRETQ |

## Testing the Implementation

1. **Build the kernel** with enhanced debug output
2. **Run with QEMU** and capture serial output
3. **Verify each checkpoint** is reached in order
4. **Check for error messages** if execution stops
5. **Analyze final checkpoint** to determine failure mode

## Success Criteria

✅ All 8 checkpoints reached  
✅ No error messages in kernel log  
✅ No triple faults or exceptions  
✅ Smooth transition from checkpoint 7 to silence (user mode)  
✅ Init process running with PID 1 in user space (ring 3)
