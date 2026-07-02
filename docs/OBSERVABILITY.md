# AutomationOS Observability Guide

**Version:** 2.0  
**Last Updated:** 2026-06-01

---

## Table of Contents

1. [Overview](#overview)
2. [Boot Stage Checkpoints](#boot-stage-checkpoints)
3. [Exception Decoder Output](#exception-decoder-output)
4. [Driver Error Messages](#driver-error-messages)
5. [Adding New Observability Points](#adding-new-observability-points)
6. [Logging Conventions](#logging-conventions)
7. [Troubleshooting with Observability Data](#troubleshooting-with-observability-data)

---

## Overview

AutomationOS provides comprehensive observability infrastructure to diagnose boot failures, runtime exceptions, driver issues, and system crashes. This document explains how to interpret diagnostic output and add new observability points to the kernel.

### Key Observability Systems

- **Boot UI**: Progress tracking during system initialization
- **Exception Decoder**: Detailed CPU exception analysis with register dumps
- **Panic Handler**: Comprehensive crash diagnostics with stack traces
- **Driver Logging**: Standardized error reporting across all drivers
- **Debug Macros**: Assertion framework for development builds

---

## Boot Stage Checkpoints

### Overview

The boot UI system (`kernel/core/boot_ui.c`) provides a clean, progress-tracked boot sequence that replaces verbose debug spam with a simple percentage-based display.

### Boot Stages

| Stage | Checkpoint | Description |
|-------|-----------|-------------|
| 1 | 10% | GDT Initialization |
| 2 | 20% | IDT and Interrupt Setup |
| 3 | 30% | Physical Memory Manager |
| 4 | 40% | Virtual Memory Manager |
| 5 | 50% | System Calls |
| 6 | 60% | Device Drivers |
| 7 | 70% | File Systems |
| 8 | 80% | User Mode Support |
| 9 | 90% | Init Process |
| 10 | 100% | Boot Complete |

### Boot UI Functions

```c
void boot_banner(void);           // Display AutomationOS banner
void boot_stage(const char* msg); // Update progress (auto-increments)
void boot_complete(void);         // Final 100% message
void boot_debug(const char* fmt, ...); // Debug output (BOOT_QUIET mode)
```

### Example Output

**Quiet Mode (default):**
```
╔═══════════════════════════════════════════════════════════╗
║           █████╗ ██╗   ██╗████████╗ ██████╗               ║
║          ██╔══██╗██║   ██║╚══██╔══╝██╔═══██╗              ║
║          ███████║██║   ██║   ██║   ██║   ██║              ║
║          ██╔══██║██║   ██║   ██║   ██║   ██║              ║
║          ██║  ██║╚██████╔╝   ██║   ╚██████╔╝              ║
║          ╚═╝  ╚═╝ ╚═════╝    ╚═╝    ╚═════╝               ║
║              AutomationOS v2.0 - Phase 2                  ║
╚═══════════════════════════════════════════════════════════╝

[100%] Boot complete!
```

**Verbose Mode (BOOT_QUIET = 0):**
```
[BOOT] [ 10%] Initializing GDT
[BOOT] [ 20%] Setting up IDT
[BOOT] [ 30%] Physical Memory Manager
...
[BOOT] [100%] Boot complete!
```

### Checkpoint Meanings

#### Stage 1: GDT Initialization (10%)
- **What it means**: CPU segmentation tables configured
- **Failure here**: Early CPU initialization issue, bad multiboot info
- **Files**: `kernel/arch/x86_64/gdt.c`

#### Stage 2: IDT Setup (20%)
- **What it means**: Interrupt handlers installed, PIC configured
- **Failure here**: Interrupt system broken, double fault on first interrupt
- **Files**: `kernel/arch/x86_64/idt.c`, `kernel/arch/x86_64/interrupt.asm`

#### Stage 3: Physical Memory Manager (30%)
- **What it means**: RAM detected and tracked via bitmap allocator
- **Failure here**: Memory map parsing failed, insufficient memory
- **Files**: `kernel/core/mem/pmm.c`

#### Stage 4: Virtual Memory Manager (40%)
- **What it means**: Kernel page tables set up, paging enabled
- **Failure here**: Page table corruption, CR3 load failure
- **Files**: `kernel/core/mem/vmm.c`, `kernel/arch/x86_64/paging.c`

#### Stage 5: System Calls (50%)
- **What it means**: Syscall interface registered and ready
- **Failure here**: MSR write failure, syscall table corruption
- **Files**: `kernel/core/syscall/handlers.c`

#### Stage 6: Device Drivers (60%)
- **What it means**: PS/2, PCI, storage, network drivers initialized
- **Failure here**: Driver probe failure, IRQ conflicts
- **Files**: `kernel/drivers/*`
- **Note**: Most driver failures are non-fatal; check driver error messages

#### Stage 7: File Systems (70%)
- **What it means**: VFS and AutoFS initialized, initrd mounted
- **Failure here**: Initrd missing/corrupt, filesystem code broken
- **Files**: `kernel/fs/*`, `kernel/fs/autofs/*`

#### Stage 8: User Mode Support (80%)
- **What it means**: TSS configured, user-mode entry points ready
- **Failure here**: TSS setup failure, IRET configuration broken
- **Files**: `kernel/arch/x86_64/usermode.c`, `kernel/arch/x86_64/tss.c`

#### Stage 9: Init Process (90%)
- **What it means**: First user-space process loaded and started
- **Failure here**: ELF loader failure, init binary missing/corrupt
- **Files**: `kernel/fs/exec.c`, `kernel/core/usermode.c`

#### Stage 10: Boot Complete (100%)
- **What it means**: System fully initialized, scheduler running
- **Success**: Desktop should appear shortly

---

## Exception Decoder Output

### Overview

The exception handler (`kernel/arch/x86_64/idt.c::exception_handler`) provides detailed diagnostic information for CPU exceptions with intelligent decoding of error codes and system state.

### Exception Banner Format

```
================================================================================
                             CPU EXCEPTION                                      
================================================================================
Exception: <exception name> (vector <number>)
Privilege level: <User|Kernel> (CPL=<0-3>)

Faulting instruction:
  RIP: 0x<16-digit hex>
  CS:  0x<4-digit hex>

<Exception-specific details>

================================================================================
```

### Common Exception Types

#### 1. Page Fault (#PF, Vector 14)

**Example Output:**
```
Exception: Page Fault (vector 14)
Privilege level: User (CPL=3)

Faulting instruction:
  RIP: 0x0000000000401234
  CS:  0x001b

Page fault details:
  Faulting address (CR2): 0x0000000000000000

  Error code breakdown (0x00000004):
    [ ] Page present (page was not present)
    [X] Write access (write operation)
    [ ] User mode access (kernel mode)
    [ ] Reserved bit violation
    [ ] Instruction fetch violation
    [ ] Protection key violation

  Probable cause: NULL pointer dereference (write)
```

**Error Code Bits:**
- Bit 0 (P): Page present (1) vs not present (0)
- Bit 1 (W/R): Write (1) vs read (0)
- Bit 2 (U/S): User mode (1) vs kernel mode (0)
- Bit 3 (RSVD): Reserved bit violation
- Bit 4 (I/D): Instruction fetch violation
- Bit 5-15: Protection key / SGX violations

**Common Causes by CR2 Value:**
- `CR2 = 0x0000000000000000`: NULL pointer dereference
- `CR2 = 0x0000xxxxxxxxxxxx` (user space): Invalid user pointer
- `CR2 = 0xFFFFxxxxxxxxxxxx` (kernel space): Kernel memory corruption
- `CR2 near stack pointer`: Stack overflow

#### 2. General Protection Fault (#GP, Vector 13)

**Example Output:**
```
Exception: General Protection Fault (vector 13)
Privilege level: Kernel (CPL=0)

Faulting instruction:
  RIP: 0xFFFFFFFF80123456
  CS:  0x0008

General Protection Fault details:
  Error code: 0x0028

  Segment selector: 0x0028
  Table: GDT (Global Descriptor Table)
  Index: 5
  External event: No
```

**Error Code Format (non-zero):**
```
Bits 15-3: Segment selector index
Bit 2-1: Table indicator (00=GDT, 01=IDT, 10=LDT, 11=IDT)
Bit 0: External event
```

**Common Causes:**
- Invalid segment selector
- NULL segment selector
- Privilege violation (ring 3 accessing ring 0 resources)
- Invalid instruction in current mode
- Writing to read-only segment

#### 3. Double Fault (#DF, Vector 8)

**Example Output:**
```
Exception: Double Fault (vector 8)
Privilege level: Kernel (CPL=0)

  DOUBLE FAULT - Catastrophic error during exception handling!
  Error code: 0x00000000 (always zero on #DF)
```

**Causes:**
- Exception handler itself caused an exception
- Stack overflow in exception handler
- Invalid IDT entry
- IST stack corruption (if using IST)

**Critical Note:** Double faults use IST1 (Interrupt Stack Table 1) to avoid stack corruption. Without IST, a double fault typically triple-faults (instant reboot).

#### 4. Other Common Exceptions

| Vector | Name | Common Cause |
|--------|------|--------------|
| 0 | Divide Error | Division by zero or overflow |
| 6 | Invalid Opcode | Bad instruction, corrupt code |
| 11 | Segment Not Present | Invalid segment descriptor |
| 12 | Stack Fault | Stack segment violation |
| 17 | Alignment Check | Misaligned memory access (if AC flag set) |
| 18 | Machine Check | Hardware error |
| 19 | SIMD Exception | SSE/AVX floating point error |

### User-Mode Exception Handling

When an exception occurs in user mode (CPL=3):

1. **CR3 Validation**: System checks if the faulting CR3 matches the current process
2. **Process Termination**: Faulting process is killed with exit status 139 (128 + SIGSEGV)
3. **Scheduler Handoff**: Scheduler switches to another process
4. **Safety Check**: If `schedule()` returns, system panics (should never happen)

**Example User Exception:**
```
[EXCEPTION] Terminating faulting process 'testapp' (PID 42)
```

### Kernel-Mode Exception Handling

When an exception occurs in kernel mode (CPL=0):

1. **Always Fatal**: Kernel exceptions trigger a full panic
2. **Full Diagnostics**: Complete register dump and stack trace
3. **Emergency Sync**: Filesystem sync attempted before halt

**Example Kernel Exception:**
```
[FATAL] Exception in kernel mode - system cannot continue
Kernel Page Fault at RIP=0xFFFFFFFF80123456, CR2=0xFFFF800000000000, err=0x2

╔════════════════════════════════════════╗
║  KERNEL PANIC - SYSTEM HALTED          ║
╚════════════════════════════════════════╝
```

---

## Panic Handler Output

### Overview

The enhanced panic handler (`kernel/lib/panic.c`) provides comprehensive crash diagnostics including stack traces, register dumps, and memory inspection.

### Full Panic Output Format

```
╔════════════════════════════════════════╗
║  KERNEL PANIC - SYSTEM HALTED          ║
╚════════════════════════════════════════╝

Error: <panic message>

Stack trace:
  [0] RIP: 0x<address>  RBP: 0x<address>
  [1] RIP: 0x<address>  RBP: 0x<address>
  ...
  [N] RIP: 0x<address>  RBP: 0x<address>

Register dump:
  RAX: 0x<value>  RBX: 0x<value>  RCX: 0x<value>  RDX: 0x<value>
  RSI: 0x<value>  RDI: 0x<value>  RBP: 0x<value>  RSP: 0x<value>
  R8:  0x<value>  R9:  0x<value>  R10: 0x<value>  R11: 0x<value>
  R12: 0x<value>  R13: 0x<value>  R14: 0x<value>  R15: 0x<value>

  RFLAGS: 0x<value> [<decoded flags>]

  CR0: 0x<value> [<decoded bits>]
  CR2: 0x<value> (page fault address)
  CR3: 0x<value> (page table base, PCID=<value>)
  CR4: 0x<value> [<decoded bits>]

Memory dump around 0x<fault address>:
  0x<addr>: <16 bytes hex> |<16 ascii chars>|
  ...
  0x<addr>: <16 bytes hex> |<16 ascii chars>| <--

System information:
  Kernel version: <major>.<minor>.<patch>

Attempting emergency filesystem sync...
  ✓ Filesystems synced successfully

════════════════════════════════════════════════════════════
  System halted. Please reboot.
════════════════════════════════════════════════════════════
```

### Stack Trace Details

The stack trace walks the frame pointer chain (RBP) to show the call stack:

```
Stack trace:
  [0] RIP: 0xFFFFFFFF80123456  RBP: 0xFFFF800000100F00
  [1] RIP: 0xFFFFFFFF80123200  RBP: 0xFFFF800000100F30
  [2] RIP: 0xFFFFFFFF80120000  RBP: 0xFFFF800000100F60
```

**Interpreting Stack Frames:**
- **RIP**: Return address (where execution will resume)
- **RBP**: Frame pointer (start of this stack frame)
- **Frame number**: 0 = innermost (most recent call)

**Invalid Stack Traces:**
```
  [3] <invalid frame pointer: 0x0000000000000000>
  [4] <frame pointer not increasing>
  <trace truncated at 16 frames>
  <no valid frames found>
```

**Causes:**
- Stack corruption
- Optimized builds with omitted frame pointers
- Overflowed stack
- Invalid RBP value

### Register Dump Details

#### RFLAGS Decoded

```
RFLAGS: 0x0000000000000246 [PF ZF IF ]
```

**Common Flags:**
- **CF**: Carry Flag (arithmetic carry/borrow)
- **PF**: Parity Flag (even parity)
- **ZF**: Zero Flag (result was zero)
- **SF**: Sign Flag (result was negative)
- **IF**: Interrupt Flag (interrupts enabled)
- **DF**: Direction Flag (string operation direction)
- **OF**: Overflow Flag (signed overflow)
- **TF**: Trap Flag (single-step mode)

#### Control Registers Decoded

**CR0:**
```
CR0: 0x0000000080050033 [PE MP EM NE WP PG ]
```
- **PE**: Protected Mode Enabled
- **MP**: Monitor Coprocessor
- **EM**: Emulation (FPU emulated)
- **TS**: Task Switched
- **ET**: Extension Type
- **NE**: Numeric Error
- **WP**: Write Protect (ring 0 respects read-only pages)
- **PG**: Paging Enabled

**CR2:** Page fault linear address (only meaningful after page fault)

**CR3:** Page table base + PCID
```
CR3: 0x0000000001A3F005 (page table base, PCID=5)
```
- Bits 12-51: Physical address of PML4 table
- Bits 0-11: PCID (Process Context ID) if PCIDE is enabled

**CR4:**
```
CR4: 0x00000000000206A0 [PAE PGE PCIDE ]
```
- **PAE**: Physical Address Extension
- **PGE**: Page Global Enable
- **PCIDE**: Process Context ID Enable

### Memory Dump Details

Shows 144 bytes (9 lines × 16 bytes) around the faulting address:

```
Memory dump around 0xFFFF800000001000:
  0xFFFF800000000FC0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
  0xFFFF800000000FD0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
  0xFFFF800000000FE0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
  0xFFFF800000000FF0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|
  0xFFFF800000001000: 48 89 E5 48 83 EC 20 48 C7 45 F8 00 00 00 00 48  |H..H.. H.E.....H| <--
  0xFFFF800000001010: 8B 45 F8 48 89 C7 E8 00 00 00 00 48 83 C4 20 5D  |.E.H.......H.. ]|
  0xFFFF800000001020: C3 55 48 89 E5 48 83 EC 10 48 C7 45 F8 00 00 00  |.UH..H...H.E....|
  0xFFFF800000001030: 00 48 8B 45 F8 48 89 C7 E8 00 00 00 00 90 C9 C3  |.H.E.H..........|
  0xFFFF800000001040: ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??  |user space|
```

**Legend:**
- **Hex column**: Raw byte values
- **ASCII column**: Printable characters (32-126), dots otherwise
- **`<--`**: Marks the line containing the faulting address
- **`user space`**: Address below kernel space (0xFFFF800000000000), not dumped

### Assertion Failures

When `ASSERT()` or `ASSERT_ALWAYS()` fails:

```
================================================================================
                           ASSERTION FAILED                                    
================================================================================
  Expression: ptr != NULL
  File: kernel/core/mem/heap.c
  Line: 123
================================================================================

╔════════════════════════════════════════╗
║  KERNEL PANIC - SYSTEM HALTED          ║
╚════════════════════════════════════════╝

Error: Assertion failure (see above for details)
<... full panic output follows ...>
```

**Assertion Macros:**
```c
ASSERT(expr);         // Debug builds only (-DDEBUG)
ASSERT_ALWAYS(expr);  // Always compiled, even in release
```

---

## Driver Error Messages

### Overview

Drivers use standardized logging prefixes to identify their output. All driver messages follow the format:

```
[DRIVER] <severity>: <message>
```

### Standard Logging Patterns

#### Initialization Messages

```c
kprintf("[PS/2] Initializing PS/2 controller...\n");
kprintf("[AHCI] AHCI driver initialized successfully\n");
kprintf("[PTY] Pseudo-terminal driver initialized (%u max pairs)\n", count);
```

#### Warning Messages

```c
kprintf("[PS/2] Warning: Keyboard port test failed (0x%x)\n", result);
kprintf("[PS/2] Warning: controller config read timed out; using defaults\n");
```

**Meaning:** Non-fatal issue, driver continues with degraded functionality or defaults.

#### Error Messages

```c
kprintf("[AHCI] ERROR: Port %d command timeout\n", port);
kprintf("[E1000] ERROR: Failed to allocate RX buffers\n");
```

**Meaning:** Operation failed but driver remains operational for other requests.

#### Fatal Error Pattern

```c
if (!critical_resource) {
    kprintf("[DRIVER] FATAL: <reason>\n");
    kernel_panic("Driver initialization failed");
}
```

**Meaning:** Driver cannot continue; system halt required.

### Common Driver Prefixes

| Prefix | Driver | Subsystem |
|--------|--------|-----------|
| `[PS/2]` | PS/2 Controller | Keyboard/Mouse |
| `[AHCI]` | AHCI SATA | Storage |
| `[E1000]` | Intel E1000 | Network |
| `[PTY]` | Pseudo-terminals | Character Devices |
| `[PCI]` | PCI Bus | Bus Enumeration |
| `[ACPI]` | ACPI | Power Management |
| `[USB]` | USB Stack | USB Devices |
| `[VGA]` | VGA/Framebuffer | Graphics |
| `[NVIDIA]` | NVIDIA GPU | Graphics |
| `[AUDIO]` | Audio Subsystem | Sound |

### Driver-Specific Error Codes

#### PS/2 Keyboard/Mouse

```
[PS/2] Warning: Keyboard port test failed (0xNN)
```

**0x00**: Port test passed  
**0xFF**: Port fault (floating bus, no device)  
**Other**: Hardware fault or communication error

```
[PS/2] Mouse port test timed out (no device?); skipping mouse
```

**Meaning:** No mouse detected on PS/2 port; non-fatal, continues without mouse.

#### AHCI Storage

```
[AHCI] Port N: Device signature: 0xNNNNNNNN
```

**0x00000101**: ATA hard drive  
**0xEB140101**: ATAPI CD/DVD  
**0xC33C0101**: SATA port multiplier  
**0xFFFFFFFF**: No device

```
[AHCI] ERROR: Port N command timeout
```

**Cause:** Drive not responding; may indicate hardware failure or drive spin-up delay.

#### Network Drivers (E1000, etc.)

```
[E1000] Link up: 1000 Mbps Full-Duplex
[E1000] Link down
```

**Meaning:** Physical link state change.

```
[E1000] ERROR: TX queue full
```

**Cause:** Network transmit backlog; may indicate slow receiver or network congestion.

### Troubleshooting Driver Issues

#### Silent Boot After Driver Stage

**Symptom:** Boot hangs at 60% (Device Drivers stage)  
**Likely Cause:** Driver infinite loop or IRQ storm

**Debug:**
1. Enable verbose boot (`BOOT_QUIET = 0` in `kernel/core/boot_ui.c`)
2. Check last driver message printed
3. Disable suspected driver in `kernel/kernel.c`

#### Repeated Driver Errors

**Symptom:** Same error message spammed continuously  
**Likely Cause:** IRQ handler firing without clearing interrupt source

**Fix Pattern:**
```c
void driver_irq_handler(void) {
    // 1. Read interrupt status
    uint32_t status = read_status_register();
    
    // 2. Handle the interrupt
    process_interrupt(status);
    
    // 3. CRITICAL: Acknowledge interrupt
    write_status_register(status); // Write-1-to-clear
    
    // 4. Send EOI to PIC/APIC
    pic_send_eoi(irq_number);
}
```

#### Hardware Not Detected

**Symptom:** `[DRIVER] No devices detected`  
**Possible Causes:**
1. Hardware not present in VM/physical machine
2. PCI enumeration failed
3. Wrong MMIO base address
4. Device in D3 power state (ACPI issue)

**Debug:**
```bash
# In QEMU, check PCI devices
info pci

# Check kernel PCI enumeration
grep "PCI" /var/log/kernel.log
```

---

## Adding New Observability Points

### When to Add Observability

Add logging/diagnostics when:
- Initializing a new subsystem
- Entering/exiting critical code paths
- Handling recoverable errors
- Detecting anomalous conditions
- Performance-critical operations complete

**Don't add logging for:**
- Every function entry/exit (too verbose)
- Fast-path operations (syscalls, interrupts)
- Inner loops

### Boot Stage Checkpoints

To add a new boot stage checkpoint:

1. **Update boot progress total**:
```c
// kernel/core/boot_ui.c
static const int boot_total_stages = 11;  // Increase count
```

2. **Add checkpoint in init sequence**:
```c
// kernel/kernel.c (or wherever initialization happens)
boot_stage("Initializing new subsystem");
new_subsystem_init();
```

3. **Document in this file**: Add entry to [Boot Stages](#boot-stages) table

### Driver Logging Standards

Follow this pattern for new drivers:

```c
void driver_init(void) {
    kprintf("[DRIVER] Initializing <driver name>...\n");
    
    // Initialization logic
    if (critical_failure) {
        kprintf("[DRIVER] FATAL: <reason>\n");
        return -1;  // Or panic if truly unrecoverable
    }
    
    if (non_critical_issue) {
        kprintf("[DRIVER] Warning: <issue>; continuing with <workaround>\n");
    }
    
    kprintf("[DRIVER] <driver name> initialized successfully\n");
    return 0;
}
```

**IRQ Handler Logging:**
```c
void driver_irq_handler(void) {
    // NO kprintf in IRQ context unless debugging!
    // IRQ handlers must be fast.
    
    // For debugging, use a global counter:
    #ifdef DEBUG_IRQ
    g_irq_count++;
    #endif
    
    // Handle interrupt...
}
```

**Error Reporting:**
```c
int driver_operation(void) {
    if (error_condition) {
        kprintf("[DRIVER] ERROR: <specific error> (code=0x%x)\n", error_code);
        return -ERRNO;  // Return appropriate errno value
    }
    return 0;
}
```

### Exception Decoders

To add decoding for a new exception type:

1. **Update exception name table**:
```c
// kernel/arch/x86_64/idt.c
static const char* get_exception_name(uint64_t vector) {
    static const char* names[] = {
        ...
        "New Exception Type",  // Add at appropriate vector index
        ...
    };
    ...
}
```

2. **Add decoder function**:
```c
static void print_new_exception_details(uint64_t err_code) {
    kprintf("New exception details:\n");
    kprintf("  Error code: 0x%llx\n", err_code);
    
    // Decode error code bits
    kprintf("  Bit 0: %s\n", (err_code & 1) ? "Set" : "Clear");
    // ... more decoding ...
    
    kprintf("  Probable cause: <human-readable interpretation>\n");
}
```

3. **Wire into exception handler**:
```c
// kernel/arch/x86_64/idt.c :: exception_handler
if (int_no == NEW_VECTOR) {
    print_new_exception_details(err_code);
}
```

### Panic Handler Extensions

To add new panic diagnostics:

```c
// kernel/lib/panic.c :: kernel_panic
void kernel_panic(const char* message) {
    cli();
    
    // ... existing diagnostics ...
    
    // Add new diagnostic section:
    print_custom_diagnostic();
    
    // ... rest of panic handler ...
}
```

**Example: Add TSS dump**:
```c
static void print_tss_dump(void) {
    kprintf("TSS state:\n");
    // Read TSS
    // Print relevant fields
    kprintf("  RSP0: 0x%016llx\n", tss.rsp0);
    kprintf("  IST1: 0x%016llx\n", tss.ist1);
}
```

### Performance Counters

For performance-critical paths, use counters instead of logging:

```c
// Global counters (in BSS, zero-initialized)
static uint64_t g_syscall_count = 0;
static uint64_t g_page_fault_count = 0;

// Increment in hot path (no kprintf!)
void syscall_handler(void) {
    g_syscall_count++;
    // ... handle syscall ...
}

// Expose via debugfs or /proc
int proc_read_stats(void) {
    kprintf("Syscalls: %llu\n", g_syscall_count);
    kprintf("Page faults: %llu\n", g_page_fault_count);
}
```

### Assertions for Invariants

Add assertions to catch logic errors during development:

```c
void critical_function(void* ptr) {
    ASSERT(ptr != NULL);              // Debug builds only
    ASSERT_ALWAYS(initialized);        // Always checked
    
    // Function logic
}
```

**When to use ASSERT vs ASSERT_ALWAYS:**
- **ASSERT**: Performance-sensitive checks, caught during testing
- **ASSERT_ALWAYS**: Security-critical invariants, must never fail in production

---

## Logging Conventions

### Severity Levels (Informal)

AutomationOS doesn't have formal log levels, but follows these conventions:

| Pattern | Severity | Use Case |
|---------|----------|----------|
| `[SUBSYS] <msg>` | Info | Normal operation |
| `[SUBSYS] Warning: <msg>` | Warning | Degraded functionality |
| `[SUBSYS] ERROR: <msg>` | Error | Operation failed |
| `[SUBSYS] FATAL: <msg>` | Fatal | Subsystem cannot continue |
| `kernel_panic("<msg>")` | Critical | System halt |

### Prefix Conventions

- **Subsystem name in brackets**: `[PS/2]`, `[AHCI]`, `[VFS]`
- **Capital letters**: Subsystem name is uppercase or mixed case
- **Consistent prefix**: Same prefix for all messages from a subsystem

### Message Format

**Good:**
```c
kprintf("[AHCI] Port %d: %s detected\n", port, device_type);
kprintf("[AHCI] ERROR: Command timeout on port %d (TFD=0x%x)\n", port, tfd);
```

**Bad:**
```c
kprintf("ahci: detected device\n");           // No port number, vague
kprintf("[AHCI] ERROR!!!\n");                 // No context
kprintf("Something went wrong in AHCI\n");    // No prefix
```

### Hexadecimal Values

- **Always prefix with `0x`**: `0x1234`, not `1234h` or `1234`
- **Use appropriate width**: `0x%02x` for bytes, `0x%016llx` for 64-bit addresses
- **Uppercase hex**: `0xABCD`, not `0xabcd` (for consistency)

### Multi-line Diagnostics

Use consistent indentation for related information:

```c
kprintf("Page fault details:\n");
kprintf("  Faulting address: 0x%016llx\n", cr2);
kprintf("  Error code: 0x%llx\n", err_code);
kprintf("  Access type: %s\n", (err_code & 2) ? "write" : "read");
```

---

## Troubleshooting with Observability Data

### Scenario 1: Boot Hangs at 60%

**Symptoms:**
- Boot progress stops at "Device Drivers" (60%)
- No further output
- System appears frozen

**Debug Steps:**

1. **Enable verbose boot**:
```c
// kernel/core/boot_ui.c
#define BOOT_QUIET 0
```

2. **Rebuild and check last message**:
```bash
make clean && make
qemu-system-x86_64 -cdrom automationos.iso -serial stdio
```

3. **Identify stuck driver**: Last driver message indicates culprit

4. **Disable driver temporarily**:
```c
// kernel/kernel.c
// ps2_init();  // Comment out to skip
```

5. **Report issue**: Note driver name, QEMU version, error message

### Scenario 2: Kernel Panic on Boot

**Symptoms:**
- Red panic banner appears
- Register dump shown
- System halted

**Debug Steps:**

1. **Note the panic message**: Top of panic output shows root cause

2. **Check RIP value**: Points to faulting instruction
   - If RIP in kernel range (`0xFFFFFFFF80xxxxxx`): Kernel bug
   - If RIP in user range (`0x00000000004xxxxx`): Should be impossible during boot

3. **Examine stack trace**: Shows call chain leading to panic

4. **Check CR2 if page fault**:
   - `CR2 = 0x0`: NULL pointer dereference
   - `CR2 = unmapped address`: Use-after-free or bad pointer

5. **Cross-reference with recent changes**: `git log` and `git diff`

### Scenario 3: Exception in User Process

**Symptoms:**
```
[EXCEPTION] Terminating faulting process 'myapp' (PID 12)
```

**Debug Steps:**

1. **Check application code**: User process has a bug (NULL deref, bad pointer, etc.)

2. **Review exception details**: Printed before termination message

3. **Test with simple app**: Verify usermode infrastructure works

4. **Use GDB if available**: Attach to process before crash

### Scenario 4: Driver Spam

**Symptoms:**
- Same driver error message repeating rapidly
- System sluggish or unresponsive
- Serial console flooded

**Debug Steps:**

1. **Identify driver**: Check message prefix

2. **Disable driver IRQ**:
```c
// In driver init
irq_mask(driver_irq_number);  // Temporary workaround
```

3. **Check IRQ acknowledge**: Ensure driver clears interrupt source

4. **Verify EOI sent**: `pic_send_eoi()` or APIC equivalent

### Scenario 5: Silent Hang (No Output)

**Symptoms:**
- Boot starts normally
- Suddenly freezes with no message
- No panic, no exception

**Possible Causes:**
- Triple fault (instant reboot on real hardware, freeze in QEMU)
- Infinite loop with interrupts disabled
- Stack overflow in IRQ handler

**Debug Steps:**

1. **Enable QEMU debug logging**:
```bash
qemu-system-x86_64 -d int,cpu_reset -D qemu.log -cdrom automationos.iso
```

2. **Check `qemu.log`**: Look for triple fault, exception cascade

3. **Add checkpoint messages**:
```c
kprintf("Checkpoint 1\n");
suspicious_function();
kprintf("Checkpoint 2\n");  // If this doesn't print, bug is in suspicious_function
```

4. **Use GDB**:
```bash
qemu-system-x86_64 -s -S -cdrom automationos.iso &
gdb kernel/kernel.elf
(gdb) target remote :1234
(gdb) continue
# Wait for hang, then Ctrl+C
(gdb) backtrace
```

---

## Summary

AutomationOS provides rich observability infrastructure:

1. **Boot Progress**: Track initialization with percentage-based checkpoints
2. **Exception Decoding**: Detailed CPU exception analysis with error code breakdown
3. **Panic Diagnostics**: Stack traces, register dumps, memory inspection
4. **Driver Logging**: Standardized error reporting across all drivers
5. **Assertion Framework**: Catch invariant violations during development

**Key Principles:**
- Log state changes and errors, not normal flow
- Use consistent prefixes for subsystems
- Provide actionable information (addresses, error codes, context)
- Decode hardware values into human-readable form
- Balance verbosity (quiet boot) with debuggability (verbose mode available)

For more information:
- Exception details: `kernel/arch/x86_64/idt.c`
- Panic handler: `kernel/lib/panic.c`
- Boot UI: `kernel/core/boot_ui.c`
- Driver examples: `kernel/drivers/*`
- Troubleshooting: `docs/TROUBLESHOOTING.md`

---

**End of Observability Guide**
