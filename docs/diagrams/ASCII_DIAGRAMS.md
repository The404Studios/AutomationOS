# ASCII Art Diagrams

Text-based diagrams for embedding in documentation, terminal display, and quick reference.

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          USERSPACE (Ring 3)                             │
├─────────────────┬───────────────┬───────────────────────────────────────┤
│  Init (PID 1)   │     Shell     │  Utilities (echo, ls, cat, ...)      │
└────────┬────────┴───────┬───────┴──────────────┬────────────────────────┘
         │                │                      │
         └────────────────┴──────────────────────┘
                          │
                  System Call Interface
                   (syscall instruction)
                          │
┌─────────────────────────┴───────────────────────────────────────────────┐
│                       KERNEL SPACE (Ring 0)                             │
│                     0xFFFFFFFF80000000                                  │
├─────────────────────────────────────────────────────────────────────────┤
│                       Core Subsystems                                   │
├────────────────┬────────────────┬────────────────┬────────────────────┤
│   Scheduler    │   Memory Mgr   │   Syscalls     │   Namespaces      │
│  (Round-Robin) │  (PMM+VMM+Heap)│  (Dispatcher)  │  (PID/Mount/...)  │
├────────────────┴────────────────┴────────────────┴────────────────────┤
│                        Device Drivers                                   │
├─────────────┬─────────────┬─────────────┬─────────────────────────────┤
│   Serial    │   Timer     │   PS/2      │   Framebuffer              │
│   (COM1)    │   (PIT)     │  Keyboard   │   (UEFI GOP)               │
├─────────────┴─────────────┴─────────────┴─────────────────────────────┤
│                    Interrupt & Architecture                             │
├──────────────┬──────────────┬──────────────┬──────────────────────────┤
│     IDT      │     GDT      │    Paging    │   Context Switch        │
│ (Interrupts) │ (Segments)   │  (4-level)   │  (Register Save)        │
└──────────────┴──────────────┴──────────────┴──────────────────────────┘
                          │
┌─────────────────────────┴───────────────────────────────────────────────┐
│                      UEFI Bootloader (AutoBoot)                         │
└─────────────────────────────────────────────────────────────────────────┘
                          │
┌─────────────────────────┴───────────────────────────────────────────────┐
│                    Hardware (x86_64 CPU, RAM, Devices)                  │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Memory Layout (Virtual Address Space)

```
64-bit Virtual Address Space (48-bit addressing)

0xFFFFFFFF_FFFFFFFF  ┌──────────────────────────────────────┐
                     │        Higher Half (Kernel)          │
                     │                                      │
0xFFFFFFFF_C0000000  ├──────────────────────────────────────┤
                     │         Kernel Heap                  │
                     │      (Slab Allocator)                │
0xFFFFFFFF_80000000  ├──────────────────────────────────────┤
                     │       Kernel Code + Data             │
                     │      (2 MB pages, 1:1 mapped)        │
                     └──────────────────────────────────────┘

0x0000800000000000   ┌──────────────────────────────────────┐
                     │    Canonical Address Hole            │
                     │      (Non-addressable)               │
0x00007FFFFFFFFFFF   └──────────────────────────────────────┘

0x00007FFF_FFFFF000  ┌──────────────────────────────────────┐
                     │        User Stack                    │
                     │       (grows down ↓)                 │
                     ├──────────────────────────────────────┤
                     │                                      │
                     │          (Unmapped)                  │
                     │                                      │
                     ├──────────────────────────────────────┤
0x0000000040000000   │        User Heap                     │
                     │       (grows up ↑)                   │
                     ├──────────────────────────────────────┤
0x0000000000400000   │      User Code + Data                │
                     │       (.text, .data, .bss)           │
                     ├──────────────────────────────────────┤
0x0000000000000000   │      Null Page (unmapped)            │
                     └──────────────────────────────────────┘

Page Size: 4 KB (0x1000 bytes)
User/Kernel Split: 0x0000800000000000
```

---

## Physical Memory Layout

```
Physical RAM

Top of RAM          ┌──────────────────────────────────────┐
                    │                                      │
                    │        Free Physical Pages           │
                    │      (Buddy Allocator)               │
                    │                                      │
                    ├──────────────────────────────────────┤
~5 MB               │      Kernel Code + Data              │
                    │      (Loaded by bootloader)          │
1 MB (0x100000)     ├──────────────────────────────────────┤
                    │      UEFI Boot Structures            │
                    │      (Memory Map, etc.)              │
0x7C00              ├──────────────────────────────────────┤
                    │      BIOS Data Area (BDA)            │
0x0500              ├──────────────────────────────────────┤
                    │      Interrupt Vector Table (IVT)    │
0x0000              └──────────────────────────────────────┘

Note: Higher-half kernel is mapped at 0xFFFFFFFF80000000 (virtual)
      → Points to 1 MB (physical)
```

---

## 4-Level Page Table Hierarchy

```
Virtual Address: 0x0000_7FFF_FFFF_F000

┌───────────────────────────────────────────────────────────────┐
│  Sign Ext │  PML4  │  PDPT  │   PD   │   PT   │   Offset   │
│  (16 bit) │ (9 bit)│ (9 bit)│ (9 bit)│ (9 bit)│  (12 bit)  │
└───────────┴────────┴────────┴────────┴────────┴────────────┘
               │         │        │        │          │
               ▼         ▼        ▼        ▼          ▼
             Index     Index    Index    Index     Page Offset
             into      into     into     into      within 4KB
             PML4      PDPT      PD       PT        page

CR3 Register ────────┐
                     ▼
           ┌─────────────────┐
           │  PML4 (512 GB)  │  Each entry covers 512 GB
           └────────┬────────┘
                    │ [index]
                    ▼
           ┌─────────────────┐
           │  PDPT (1 GB)    │  Each entry covers 1 GB
           └────────┬────────┘
                    │ [index]
                    ▼
           ┌─────────────────┐
           │   PD (2 MB)     │  Each entry covers 2 MB
           └────────┬────────┘
                    │ [index]
                    ▼
           ┌─────────────────┐
           │   PT (4 KB)     │  Each entry covers 4 KB
           └────────┬────────┘
                    │ [index]
                    ▼
           ┌─────────────────┐
           │ Physical Page   │  4 KB physical frame
           │   (4 KB)        │
           └─────────────────┘

Page Table Entry (PTE) Format (64-bit):
┌──────┬───┬───┬───┬───┬───┬───┬───┬──────────────────────────────┬───┐
│ NX   │...│ G │ D │ A │ U │ W │ P │  Physical Address [51:12]    │...│
└──────┴───┴───┴───┴───┴───┴───┴───┴──────────────────────────────┴───┘
  bit 63      8   6   5   2   1   0

P = Present    W = Write    U = User     A = Accessed
D = Dirty      G = Global   NX = No Execute
```

---

## Buddy Allocator Structure

```
Physical Memory Managed by Buddy Allocator

Order 0 (4 KB):   [●][●][●][●][●][●][●][●][●][●][●][●][●][●][●][●]
                   ↑ free   ↑ allocated

Order 1 (8 KB):   [  ●●  ][  ●●  ][  ●●  ][  ●●  ]
                     ↑ buddy pair

Order 2 (16 KB):  [    ●●●●    ][    ●●●●    ]

...

Order 10 (4 MB):  [        ●●●●●●●●●●●●●●●●        ]

Free Lists:
  free_list[0]  → 4 KB blocks   → [addr1] → [addr2] → NULL
  free_list[1]  → 8 KB blocks   → [addr3] → NULL
  free_list[2]  → 16 KB blocks  → NULL
  ...
  free_list[10] → 4 MB blocks   → [addr4] → NULL

Allocation Algorithm:
  1. Find smallest free block that fits
  2. If too large, split in half (create buddy)
  3. Repeat until right size
  4. Mark as allocated in bitmap

Free Algorithm:
  1. Mark as free in bitmap
  2. Check if buddy is free
  3. If yes, coalesce (merge buddies)
  4. Repeat up the tree
```

---

## Process State Machine

```
                         process_create()
                               │
                               ▼
                        ┌──────────────┐
                        │   CREATED    │  PCB allocated, not scheduled
                        └──────┬───────┘
                               │ scheduler_add_process()
                               ▼
          ┌────────────────────────────────────┐
          │           ┌──────────────┐         │
          │           │    READY     │         │  In scheduler queue
          │           └──────┬───────┘         │
          │                  │ schedule()      │
          │                  ▼                 │
          │           ┌──────────────┐         │
  time    │           │   RUNNING    │         │  Currently executing
  slice   │           └──┬────────┬──┘         │
  expired │              │        │            │
          │              │        │ I/O / sleep()
          │              │        ▼            │
          └──────────────┘   ┌──────────────┐ │
                             │   BLOCKED    │◄┘  Waiting for event
                             └──────┬───────┘
                                    │ I/O complete / wakeup
                                    └──────────────────────┐
                                                          │
                                                          ▼
    exit() ──────►  ┌──────────────┐              ┌──────────────┐
                    │ TERMINATED   │              │    READY     │
                    └──────┬───────┘              └──────────────┘
                           │ parent wait()
                           ▼
                       [Freed]

State Transitions:
  CREATED → READY:      Initial scheduling
  READY → RUNNING:      Scheduler picks process
  RUNNING → READY:      Time slice expired
  RUNNING → BLOCKED:    Waiting for I/O/event
  RUNNING → TERMINATED: Process exits
  BLOCKED → READY:      Event occurred
  TERMINATED → [Free]:  Zombie reaped by parent
```

---

## System Call Flow

```
Userspace (Ring 3)                 Kernel Space (Ring 0)

┌─────────────────┐
│  User Process   │
│                 │
│  write(1,       │
│    "Hello", 5)  │
└────────┬────────┘
         │ 1. Prepare registers:
         │    RAX = SYS_WRITE (3)
         │    RDI = fd (1)
         │    RSI = buf ptr
         │    RDX = count (5)
         │
         │ 2. Execute syscall instruction
         ▼
    ╔═══════════════════════════════════╗
    ║  Hardware Transition (CPL 3→0)    ║
    ║  • RIP → IA32_LSTAR MSR           ║
    ║  • Save user RIP to RCX           ║
    ║  • Save RFLAGS to R11             ║
    ║  • Load kernel RSP                ║
    ╚════════════════╤══════════════════╝
                     │
         ┌───────────┴──────────────┐
         │   Syscall Entry (asm)    │
         │  • Save all registers    │
         │  • Switch to kernel stack│
         └───────────┬──────────────┘
                     │
         ┌───────────┴──────────────┐
         │ Syscall Dispatcher (C)   │
         │  • Validate syscall num  │
         │  • Lookup handler table  │
         │  • Log audit event       │
         └───────────┬──────────────┘
                     │
         ┌───────────┴──────────────┐
         │  Syscall Handler (C)     │
         │  • Validate arguments    │
         │  • copy_from_user()      │
         │  • Call kernel subsystem │
         │  • copy_to_user()        │
         │  • Return result         │
         └───────────┬──────────────┘
                     │
         ┌───────────┴──────────────┐
         │    Restore Context       │
         │  • Set RAX = result      │
         │  • Restore registers     │
         └───────────┬──────────────┘
                     │
    ╔════════════════╧══════════════════╗
    ║  sysretq (Hardware CPL 0→3)       ║
    ║  • RIP ← RCX (user RIP)           ║
    ║  • RFLAGS ← R11                   ║
    ║  • Load user RSP                  ║
    ╚════════════════╤══════════════════╝
                     │
┌────────────────────┴─────┐
│    User Process          │
│    RAX = 5 (bytes)       │
│    Execution resumes     │
└──────────────────────────┘

Latency: ~500-1000 cycles (~0.2-0.5 μs)
```

---

## Context Switch Flow

```
         Timer IRQ (1000 Hz)
                │
                ▼
    ┌───────────────────────┐
    │   IDT Handler         │
    │   • Push registers    │
    │   • Save error code   │
    └───────────┬───────────┘
                │
                ▼
    ┌───────────────────────┐
    │   schedule()          │
    │   • Decrement slice   │
    │   • Pick next process │
    └───────────┬───────────┘
                │
         If time slice expired
                │
                ▼
    ┌───────────────────────────────────────┐
    │   context_switch(Process A → B)       │
    └───────────────────────────────────────┘
                │
        ┌───────┴───────┐
        ▼               ▼
  ┌─────────────┐  ┌─────────────┐
  │ Save A      │  │ Load B      │
  │ • RAX-R15   │  │ • RAX-R15   │
  │ • RIP       │  │ • RIP       │
  │ • RSP       │  │ • RSP       │
  │ • RFLAGS    │  │ • RFLAGS    │
  │ • CR3 (PT)  │  │ • CR3 (PT)  │
  └─────────────┘  └─────────────┘
        │               │
        └───────┬───────┘
                │
                ▼
    ┌───────────────────────┐
    │   Switch Stack        │
    │   RSP ← B->kernel_rsp │
    └───────────┬───────────┘
                │
                ▼
    ┌───────────────────────┐
    │   Update TSS          │
    │   TSS.RSP0 ← B->stack │
    └───────────┬───────────┘
                │
                ▼
    ┌───────────────────────┐
    │   TLB Flush           │
    │   (CR3 write)         │
    └───────────┬───────────┘
                │
                ▼
    ┌───────────────────────┐
    │   Return from ISR     │
    │   • Send EOI to PIC   │
    │   • Restore registers │
    │   • iretq             │
    └───────────┬───────────┘
                │
                ▼
        Process B Running

Overhead: ~1000-2000 cycles
  • Register save/restore: ~200 cycles
  • CR3 flush (TLB): ~500-1000 cycles
  • Scheduler logic: ~300-500 cycles
```

---

## Interrupt Handling Flow

```
Hardware Device                      CPU                     Kernel
     (Keyboard)                                           (Handler)

    Key Press
        │
        └──► IRQ 1 raised
                │
                └──► PIC (8259A)
                        │ Check IRQ mask
                        │ (not masked)
                        ▼
                     INTR signal
                        │
                        └──► Check RFLAGS.IF
                                │ (interrupts enabled)
                                ▼
                          Finish instruction
                                │
                                ▼
                        ╔═══════════════════════╗
                        ║  Hardware Sequence    ║
                        ║  • Push SS, RSP       ║
                        ║  • Push RFLAGS        ║
                        ║  • Clear IF           ║
                        ║  • Push CS, RIP       ║
                        ║  • Read vector: 33    ║
                        ╚═══════════╤═══════════╝
                                    │
                    ┌───────────────┴────────────────┐
                    │  IDT Lookup [33]              │
                    │  Get ISR address              │
                    └───────────────┬────────────────┘
                                    │
                    ┌───────────────┴────────────────┐
                    │  ISR (interrupt.asm)          │
                    │  • Save all registers         │
                    │  • Call device handler        │
                    └───────────────┬────────────────┘
                                    │
                    ┌───────────────┴────────────────┐
                    │  keyboard_handler() (ps2.c)   │
                    │  • Read port 0x60 (scancode)  │
                    │  • Translate to ASCII         │
                    │  • Add to input buffer        │
                    └───────────────┬────────────────┘
                                    │
                    ┌───────────────┴────────────────┐
                    │  Send EOI to PIC              │
                    │  outb(0x20, 0x20)             │
                    └───────────────┬────────────────┘
                                    │
                    ┌───────────────┴────────────────┐
                    │  Restore registers            │
                    │  iretq                        │
                    └───────────────┬────────────────┘
                                    │
                        ╔═══════════╧═══════════╗
                        ║  Hardware Return      ║
                        ║  • Pop CS, RIP        ║
                        ║  • Pop RFLAGS (IF=1)  ║
                        ║  • Pop SS, RSP        ║
                        ╚═══════════╤═══════════╝
                                    │
                                    ▼
                         Resume interrupted code

Latency: ~1.5-5.5 μs
  • PIC → CPU: ~0.1 μs
  • State save: ~0.2 μs
  • Handler: ~1-5 μs
  • State restore: ~0.2 μs
```

---

## Boot Sequence Timeline

```
Time    Stage                  Action
────────────────────────────────────────────────────────────────

0 ms    ┌──────────────────┐
        │  Power On        │  Hardware POST
        └────────┬─────────┘
~50 ms           │
        ┌────────┴─────────┐
        │  UEFI Firmware   │  • Initialize hardware
        │                  │  • Load boot services
        │                  │  • Setup memory map
        └────────┬─────────┘
~100 ms          │
        ┌────────┴─────────┐
        │  Boot Manager    │  • Read boot order
        │                  │  • Locate ESP partition
        │                  │  • Load AutoBoot.efi
        └────────┬─────────┘
~110 ms          │
        ┌────────┴─────────┐
        │  AutoBoot Entry  │  • Verify UEFI tables
        │  (boot.asm)      │  • Switch to long mode
        │                  │  • Setup minimal GDT
        └────────┬─────────┘
~120 ms          │
        ┌────────┴─────────┐
        │  AutoBoot Loader │  • Parse memory map
        │  (loader.c)      │  • Load kernel ELF
        │                  │  • Setup page tables
        │                  │  • Allocate stack
        └────────┬─────────┘
~150 ms          │ Jump to kernel
        ┌────────┴─────────┐
        │  Kernel Entry    │  • Setup kernel GDT
        │  (_start)        │  • Initialize IDT
        │                  │  • Enable paging
        └────────┬─────────┘
~152 ms          │ Jump to kmain()
        ┌────────┴─────────┐
        │  kmain()         │
        │  ┌────────────┐  │
~153 ms │  │ Phase 1:   │  │  • PMM init (buddy)
        │  │ Memory     │  │  • VMM init (paging)
        │  └────────────┘  │  • Heap init (slab)
~155 ms │  ┌────────────┐  │
        │  │ Phase 2:   │  │  • Serial console
        │  │ Drivers    │  │  • Timer (PIT @ 1kHz)
        │  └────────────┘  │  • PS/2 keyboard
~158 ms │  ┌────────────┐  │
        │  │ Phase 3:   │  │  • Process table
        │  │ Scheduler  │  │  • Idle process
        │  └────────────┘  │  • Enable timer IRQ
~160 ms │  ┌────────────┐  │
        │  │ Phase 4:   │  │  • Register handlers
        │  │ Syscalls   │  │  • Setup MSRs
        │  └────────────┘  │
~161 ms │  ┌────────────┐  │
        │  │ Phase 5:   │  │  • MAC policy
        │  │ Security   │  │  • Seccomp filters
        │  └────────────┘  │  • Audit log
~162 ms │  ┌────────────┐  │
        │  │ Phase 6:   │  │  • Load /sbin/init
        │  │ Userspace  │  │  • Switch to ring 3
        │  └────────────┘  │  • Schedule init
        └────────┬─────────┘
~163 ms          │ execve(/sbin/init)
        ┌────────┴─────────┐
        │  Init Process    │  • Mount filesystems
        │  (PID 1)         │  • Spawn shell
        │                  │  • Reap zombies
        └────────┬─────────┘
~165 ms          │
        ┌────────┴─────────┐
        │  Shell Ready  ✓  │  • Accept user input
        │                  │  • Execute commands
        └──────────────────┘

Total Boot Time: ~165 ms (typical)
```

---

## GDT (Global Descriptor Table) Layout

```
┌──────┬─────────────────────────────────────────────────────────┐
│Index │ Descriptor                                              │
├──────┼─────────────────────────────────────────────────────────┤
│  0   │ Null Descriptor (required by x86_64)                   │
├──────┼─────────────────────────────────────────────────────────┤
│  1   │ Kernel Code Segment (64-bit)                           │
│      │   Base: 0x0000000000000000                             │
│      │   Limit: 0xFFFFFFFFFFFFFFFF (ignored in long mode)     │
│      │   Type: Execute/Read, DPL=0 (Ring 0)                   │
│      │   Flags: L (64-bit), G (granularity)                   │
├──────┼─────────────────────────────────────────────────────────┤
│  2   │ Kernel Data Segment (64-bit)                           │
│      │   Base: 0x0000000000000000                             │
│      │   Limit: 0xFFFFFFFFFFFFFFFF (ignored in long mode)     │
│      │   Type: Read/Write, DPL=0 (Ring 0)                     │
├──────┼─────────────────────────────────────────────────────────┤
│  3   │ User Code Segment (64-bit)                             │
│      │   Base: 0x0000000000000000                             │
│      │   Limit: 0xFFFFFFFFFFFFFFFF                            │
│      │   Type: Execute/Read, DPL=3 (Ring 3)                   │
│      │   Flags: L (64-bit), G (granularity)                   │
├──────┼─────────────────────────────────────────────────────────┤
│  4   │ User Data Segment (64-bit)                             │
│      │   Base: 0x0000000000000000                             │
│      │   Limit: 0xFFFFFFFFFFFFFFFF                            │
│      │   Type: Read/Write, DPL=3 (Ring 3)                     │
├──────┼─────────────────────────────────────────────────────────┤
│  5   │ TSS (Task State Segment)                               │
│      │   Contains kernel stack pointer (RSP0)                 │
│      │   Used for privilege level transitions                 │
└──────┴─────────────────────────────────────────────────────────┘

Selector Format (16-bit):
┌─────────────┬─────┬─────┐
│    Index    │ TI  │ RPL │
│   (13 bit)  │(1bit│(2bit│
└─────────────┴─────┴─────┘
               │     │
               │     └─ Requested Privilege Level (0-3)
               └─────── Table Indicator (0=GDT, 1=LDT)

Kernel Selectors:
  CS = 0x08 (index 1, RPL=0)
  DS = 0x10 (index 2, RPL=0)

User Selectors:
  CS = 0x1B (index 3, RPL=3)
  DS = 0x23 (index 4, RPL=3)
```

---

## IDT (Interrupt Descriptor Table) Layout

```
┌────────┬────────────────────────────────────────────────────┐
│Vector  │ Handler                                            │
├────────┼────────────────────────────────────────────────────┤
│ 0-31   │ CPU Exceptions                                     │
├────────┼────────────────────────────────────────────────────┤
│   0    │ Divide by Zero (#DE)                               │
│   1    │ Debug (#DB)                                        │
│   2    │ Non-Maskable Interrupt (NMI)                       │
│   3    │ Breakpoint (#BP)                                   │
│   4    │ Overflow (#OF)                                     │
│   5    │ Bound Range Exceeded (#BR)                         │
│   6    │ Invalid Opcode (#UD)                               │
│   7    │ Device Not Available (#NM)                         │
│   8    │ Double Fault (#DF)                                 │
│   10   │ Invalid TSS (#TS)                                  │
│   11   │ Segment Not Present (#NP)                          │
│   12   │ Stack-Segment Fault (#SS)                          │
│   13   │ General Protection Fault (#GP)                     │
│   14   │ Page Fault (#PF)                                   │
│   16   │ x87 FPU Error (#MF)                                │
│   17   │ Alignment Check (#AC)                              │
│   18   │ Machine Check (#MC)                                │
│   19   │ SIMD Floating-Point Exception (#XM)               │
│   20   │ Virtualization Exception (#VE)                     │
│   21   │ Control Protection Exception (#CP)                 │
├────────┼────────────────────────────────────────────────────┤
│ 32-47  │ Hardware Interrupts (IRQs)                         │
├────────┼────────────────────────────────────────────────────┤
│   32   │ IRQ 0: Timer (PIT)                                 │
│   33   │ IRQ 1: Keyboard (PS/2)                             │
│   34   │ IRQ 2: Cascade (slave PIC)                         │
│   35   │ IRQ 3: COM2 (serial)                               │
│   36   │ IRQ 4: COM1 (serial)                               │
│   37   │ IRQ 5: LPT2 (parallel)                             │
│   38   │ IRQ 6: Floppy disk                                 │
│   39   │ IRQ 7: LPT1 (parallel)                             │
│   40   │ IRQ 8: RTC (Real-Time Clock)                       │
│   41   │ IRQ 9: ACPI                                        │
│   42   │ IRQ 10: Available                                  │
│   43   │ IRQ 11: Available                                  │
│   44   │ IRQ 12: PS/2 Mouse                                 │
│   45   │ IRQ 13: FPU Exception                              │
│   46   │ IRQ 14: Primary ATA                                │
│   47   │ IRQ 15: Secondary ATA                              │
├────────┼────────────────────────────────────────────────────┤
│ 48-255 │ User-Defined / Future Use                          │
└────────┴────────────────────────────────────────────────────┘

IDT Entry Format (16 bytes):
┌────────────────┬────┬────┬────────────────┐
│ Offset [15:0]  │ CS │Attr│ Offset [31:16] │  (Lower 8 bytes)
├────────────────┴────┴────┴────────────────┤
│        Offset [63:32]        │  Reserved  │  (Upper 8 bytes)
└──────────────────────────────┴────────────┘

Attributes:
  • Type: Interrupt Gate (0xE) or Trap Gate (0xF)
  • DPL: Descriptor Privilege Level (0=kernel, 3=user)
  • P: Present bit (1=valid)
  • IST: Interrupt Stack Table index (0-7, 0=none)
```

---

These ASCII diagrams can be embedded directly into markdown files for quick reference without requiring diagram generation tools.

