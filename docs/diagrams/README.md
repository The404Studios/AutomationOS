# AutomationOS Visual Documentation

This directory contains comprehensive visual documentation for AutomationOS, including architecture diagrams, sequence diagrams, state machines, and data flow diagrams.

## Quick Links

- 🏗️ [System Architecture](#system-architecture)
- 🧠 [Memory Architecture](#memory-architecture)
- 🚀 [Boot Process](#boot-process)
- 📞 [System Calls](#system-calls)
- ⚙️ [Process Scheduling](#process-scheduling)
- 💾 [Memory Management](#memory-management)
- 🔄 [State Machines](#state-machines)
- 🔌 [Component Architecture](#component-architecture)
- 📊 [Data Flow](#data-flow)

---

## System Architecture

### Overall System Architecture
![System Architecture](system_architecture.svg)

**File:** `system_architecture.svg` ([source](src/system_architecture.dot))

Shows the complete system architecture including:
- Userspace layer (Ring 3): Init, shell, utilities
- System call interface (syscall instruction)
- Kernel space (Ring 0): Scheduler, memory manager, drivers
- Security subsystems: MAC, seccomp, audit, crypto
- Hardware layer and UEFI bootloader

**Key Components:**
- **Userspace:** Init process (PID 1), shell, user utilities
- **Core Subsystems:** Process scheduler, memory manager, syscall dispatcher
- **Drivers:** Serial console, timer (PIT), PS/2 keyboard, framebuffer
- **Security:** MAC policy, seccomp filtering, audit logging, cryptography
- **Architecture:** GDT, IDT, paging, context switching

---

## Memory Architecture

### Virtual and Physical Memory Layout
![Memory Layout](memory_layout.svg)

**File:** `memory_layout.svg` ([source](src/memory_layout.dot))

Illustrates the complete memory architecture:
- **Virtual Address Space:** 48-bit addressing with higher-half kernel
- **Physical Memory:** Buddy allocator managing physical RAM
- **Page Tables:** 4-level paging hierarchy (PML4 → PDPT → PD → PT)
- **Memory Regions:**
  - Kernel space: `0xFFFFFFFF80000000` and above
  - User space: `0x0000000000000000` to `0x00007FFFFFFFFFFF`
  - Canonical hole: Non-addressable region between user and kernel

**Key Concepts:**
- **Kernel Offset:** `0xFFFFFFFF80000000` (higher-half mapping)
- **Page Size:** 4 KB
- **Virtual → Physical Translation:** Through 4-level page tables in CR3
- **Buddy Allocator:** Orders 0-10 (4 KB to 4 MB blocks)
- **Slab Allocator:** Kernel heap with 32B to 8KB caches

---

## Boot Process

### Boot Sequence Flowchart
![Boot Sequence](boot_sequence.svg)

**File:** `boot_sequence.svg` ([source](src/boot_sequence.dot))

Detailed boot flow from power-on to shell:

1. **Power On** → UEFI Firmware (POST, hardware init)
2. **UEFI Boot Manager** → Load AutoBoot.efi from ESP
3. **AutoBoot Entry** (boot.asm) → Switch to long mode, setup GDT
4. **AutoBoot Loader** (loader.c) → Load kernel ELF, setup page tables
5. **Kernel Entry** (_start) → Initialize GDT/IDT, jump to kmain()
6. **kmain()** → Initialize subsystems:
   - Phase 1: Memory (PMM, VMM, heap)
   - Phase 2: Drivers (serial, timer, PS/2, framebuffer)
   - Phase 3: Scheduler (process table, idle process)
   - Phase 4: System calls (handlers, MSR setup)
   - Phase 5: Security (MAC, seccomp, audit)
   - Phase 6: Userspace (load init, switch to ring 3)
7. **Init Process** → Spawn shell, mount filesystems
8. **Shell Ready** ✓

**Boot Timeline:**
- UEFI firmware: ~100ms
- AutoBoot loader: ~50ms
- Kernel initialization: ~10ms
- Userspace startup: ~5ms
- **Total:** ~165ms (typical)

---

## System Calls

### System Call Sequence Diagram
![Syscall Sequence](seq_syscall.svg)

**File:** `seq_syscall.svg` ([source](src/seq_syscall.mmd))

Detailed sequence of a system call from userspace to kernel and back:

**Example:** `write(1, "Hello", 5)`

1. **Userspace:** Prepare syscall arguments in registers
   - RAX = `SYS_WRITE` (3)
   - RDI = fd (1 = stdout)
   - RSI = buffer pointer
   - RDX = count (5)

2. **syscall instruction:** Hardware transition to kernel
   - CPL 3 → 0 (ring 3 → ring 0)
   - RIP → IA32_LSTAR MSR (syscall entry point)
   - Save user RIP to RCX, RFLAGS to R11

3. **Syscall Entry** (syscall.asm): Save user context
   - Push all registers to kernel stack
   - Switch to kernel RSP

4. **Syscall Dispatcher** (syscall.c): Validate and dispatch
   - Check syscall number bounds
   - Lookup handler in syscall table
   - Log syscall in audit buffer

5. **Syscall Handler** (handlers.c): Process request
   - Validate arguments (fd, buffer, size)
   - Copy data from user space (copy_from_user)
   - Call kernel subsystem (e.g., serial_write)
   - Return result

6. **Return Path:** Restore user context
   - Set RAX to return value
   - Restore all registers
   - sysretq instruction

7. **Back to Userspace:** Resume execution
   - CPL 0 → 3 (ring 0 → ring 3)
   - User code receives return value in RAX

**Performance:**
- Total latency: 500-1000 cycles (~0.2-0.5 μs on 2 GHz CPU)
- Context switch overhead: ~200 cycles
- Handler execution: variable (100-5000 cycles)

### System Call Data Flow
![Syscall Data Flow](dataflow_syscall.svg)

**File:** `dataflow_syscall.svg` ([source](src/dataflow_syscall.dot))

Shows data transformations through the syscall path:
1. User args → Registers
2. Registers → Kernel stack
3. User buffer → Kernel buffer (copy_from_user)
4. Kernel processing
5. Result → User buffer (copy_to_user)
6. Result → RAX register

**Security Checks:**
- Syscall number validation
- Pointer bounds checking
- User/kernel address separation
- Buffer size limits
- Permission checks (UID, capabilities)
- Seccomp filtering
- Audit logging

---

## Process Scheduling

### Context Switch Sequence
![Context Switch](seq_context_switch.svg)

**File:** `seq_context_switch.svg` ([source](src/seq_context_switch.mmd))

Detailed process context switch triggered by timer interrupt:

1. **Timer IRQ** (PIT @ 1000 Hz): Hardware interrupt
2. **IDT Handler**: Save interrupted process state
3. **Scheduler**: Decide if context switch needed
   - Decrement current process time slice
   - If time slice expired, pick next process
4. **context_switch()**: Save/restore CPU context
   - Save Process A: all registers + CR3 to PCB
   - Load Process B: restore registers + CR3 from PCB
   - Switch kernel stacks
   - Update TSS RSP0
5. **TLB Flush**: CR3 write invalidates TLB
6. **Return**: Process B now running

**Overhead:**
- Register save/restore: ~200 cycles
- CR3 flush (TLB): ~500-1000 cycles
- Scheduler logic: ~300-500 cycles
- **Total:** ~1000-2000 cycles (~0.5-1 μs)

### Process State Machine
![Process States](fsm_process.svg)

**File:** `fsm_process.svg` ([source](src/fsm_process.mmd))

Process lifecycle state transitions:

- **CREATED** → **READY**: Process created, added to scheduler
- **READY** → **RUNNING**: Scheduler selects process for execution
- **RUNNING** → **READY**: Time slice expired (preemption)
- **RUNNING** → **BLOCKED**: I/O request or sleep()
- **RUNNING** → **TERMINATED**: exit() syscall
- **BLOCKED** → **READY**: I/O complete or wakeup
- **TERMINATED** → [Exit]: Zombie reaped by parent

**State Descriptions:**
- **CREATED:** PCB allocated, not yet scheduled
- **READY:** In scheduler queue, waiting for CPU
- **RUNNING:** Currently executing on CPU (user or kernel mode)
- **BLOCKED:** Waiting for I/O, timer, or signal
- **TERMINATED:** Zombie state, exit status saved for parent

### Scheduler State Machine
![Scheduler States](fsm_scheduler.svg)

**File:** `fsm_scheduler.svg` ([source](src/fsm_scheduler.mmd))

Scheduler algorithm flow:

1. **Idle State:** No processes ready, CPU in HLT
2. **Scheduling:** Timer interrupt triggers schedule()
   - Check current process time slice
   - If expired, mark READY and select next
   - Validate process state (skip BLOCKED/TERMINATED)
   - Context switch to selected process
3. **Executing:** Process running, time slice counting down
4. **Back to Scheduling:** Next timer interrupt

**Algorithm:** Round-robin with 10ms time slices (Phase 1)
- Fair CPU time distribution
- No priority levels yet
- Simple FIFO queue
- No starvation

**Metrics:**
- Context switches: ~100-1000/sec
- Switch overhead: ~1-2 μs
- Scheduler overhead: ~0.1-0.5% CPU
- Throughput: ~1000 processes/sec

---

## Memory Management

### Memory Page State Machine
![Page States](fsm_memory_page.svg)

**File:** `fsm_memory_page.svg` ([source](src/fsm_memory_page.mmd))

Physical page lifecycle:

- **Free** → **Allocated**: pmm_alloc_page()
- **Allocated** → **Mapped**: vmm_map_page() creates PTE
- **Mapped** → **In Use**: Various uses (heap, stack, page tables, DMA)
- **Clean** ↔ **Dirty**: Write access sets dirty bit
- **Allocated** → **Copy-on-Write**: fork() optimization
- **Allocated** → **Swapped**: Page out (future feature)
- **Allocated** → **Free**: pmm_free_page() / kfree()

**Page States:**
- **Free:** In buddy allocator free list
- **Allocated:** Assigned but not mapped
- **Mapped:** Virtual mapping exists (PTE present)
- **In Use:** Active usage (heap, stack, code, data)
- **COW:** Shared read-only, copy on write
- **Dirty:** Modified, needs writeback
- **Swapped:** On disk (future: swap space)

**Page Flags (x86_64 PTE):**
- **PRESENT** (bit 0): Page in physical RAM
- **WRITE** (bit 1): Writable page
- **USER** (bit 2): User-accessible
- **ACCESSED** (bit 5): Read/written
- **DIRTY** (bit 6): Modified
- **GLOBAL** (bit 8): TLB not flushed on CR3 write

### Memory Subsystem Components
![Memory Components](component_memory.svg)

**File:** `component_memory.svg` ([source](src/component_memory.dot))

Detailed memory management subsystem architecture:

**Physical Memory Manager (pmm.c):**
- `pmm_init()`: Initialize buddy allocator
- `pmm_alloc_page()`: Allocate 4KB page (O(1))
- `pmm_free_page()`: Free and coalesce buddies (O(log n))
- Buddy allocator: Orders 0-10 (4 KB to 4 MB)

**Virtual Memory Manager (vmm.c):**
- `vmm_init()`: Setup kernel page tables
- `vmm_map_page()`: Create virtual → physical mapping
- `vmm_unmap_page()`: Remove mapping
- `vmm_get_physical()`: Translate virtual → physical
- 4-level page tables: PML4 → PDPT → PD → PT

**Kernel Heap (heap.c):**
- `heap_init()`: Initialize slab allocator
- `kmalloc(size)`: Allocate from appropriate cache
- `kfree(ptr)`: Return to cache
- Slab caches: 32B, 64B, 128B, 256B, 512B, 1KB, 2KB, 4KB, 8KB

**User Memory Validation:**
- `validate_user_buffer()`: Check buffer in user space
- `validate_user_string()`: Check null-terminated string
- `copy_from_user()`: Safe copy from user to kernel
- `copy_to_user()`: Safe copy from kernel to user

**Performance:**
- Page allocation: ~100-200 cycles
- Page mapping: ~100-200 cycles
- kmalloc: O(1) for cached sizes
- Page fault: ~5-10 μs
- TLB miss: ~100 cycles
- TLB hit rate: >99%

### Page Fault Handling Sequence
![Page Fault](seq_page_fault.svg)

**File:** `seq_page_fault.svg` ([source](src/seq_page_fault.mmd))

Page fault exception handling flow:

1. **User Access:** Access unmapped page (e.g., 0x400000)
2. **MMU Walk:** CPU walks page tables, finds PRESENT=0
3. **Exception 14:** Page fault exception raised
4. **IDT Handler:** Read CR2 (faulting address) and error code
5. **Analyze Fault:**
   - Invalid address (null, kernel space) → SIGSEGV
   - Valid demand paging → Allocate page
6. **Page Allocation:**
   - `pmm_alloc_page()`: Allocate physical frame
   - Zero page (security: prevent data leak)
   - Update page table entry (PTE)
   - Flush TLB with invlpg
7. **Resume:** Retry faulting instruction
8. **Success:** MMU finds page present, access succeeds

**Latency:**
- Minor fault (allocation): ~5-10 μs
- Major fault (disk I/O): ~1-10 ms (Phase 2+)
- TLB miss (no fault): ~100 cycles

---

## Component Architecture

### Kernel Component Diagram
![Kernel Components](component_kernel.svg)

**File:** `component_kernel.svg` ([source](src/component_kernel.dot))

Complete kernel component architecture showing all subsystems and their relationships:

**Architecture Layer (arch/x86_64/):**
- boot.asm, gdt.c/asm, idt.c, interrupt.asm
- paging.c, syscall.asm, syscall_init.c, context_switch.asm

**Core Subsystems (core/):**
- **Memory (mem/):** pmm.c, vmm.c, heap.c
- **Scheduler (sched/):** process.c, scheduler.c, context.c
- **Syscalls (syscall/):** syscall.c, handlers.c
- **Namespaces (namespace/):** ns_pid.c, ns_mount.c, ns_net.c, ns_ipc.c, ns_uts.c
- **Resource Limits (rlimit/):** cpu.c, memory.c
- **Performance:** perf.c

**Device Drivers (drivers/):**
- serial.c (COM1 console)
- pit.c (Timer @ 1000 Hz)
- ps2.c (Keyboard)
- framebuffer.c (VGA/UEFI GOP)

**Security Subsystems (security/):**
- **MAC (mac/):** label.c, policy.c (Mandatory Access Control)
- **Seccomp (seccomp/):** filter.c (Syscall filtering)
- namespace.c (Namespace security)

**Audit (audit/):**
- log.c (Audit logging)
- buffer.c (Ring buffer)

**Cryptography (crypto/):**
- sha256.c (Hashing)
- rsa.c (Public key)
- verify.c (Signature verification)

**Library (lib/):**
- string.c, printf.c, panic.c

**Dependencies:**
- kmain() → Initialize all subsystems
- VMM → PMM (allocate pages)
- Heap → PMM (allocate slab pages)
- Scheduler → Process table
- Syscall dispatcher → Handlers
- Handlers → Core subsystems

---

## Data Flow

### System Call Data Flow
![Syscall Data Flow](dataflow_syscall.svg)

**File:** `dataflow_syscall.svg` ([source](src/dataflow_syscall.dot))

Complete data flow through system call execution (see [System Calls](#system-calls) section above for details).

---

## Interrupt Handling

### Interrupt Sequence Diagram
![Interrupt Handling](seq_interrupt.svg)

**File:** `seq_interrupt.svg` ([source](src/seq_interrupt.mmd))

Hardware interrupt handling flow (example: keyboard key press):

1. **Device:** Keyboard controller raises IRQ 1
2. **PIC (8259A):** Check interrupt mask register (IMR)
   - If masked: Ignore interrupt
   - If enabled: Send INTR signal to CPU
3. **CPU:** Check RFLAGS.IF (Interrupt Enable Flag)
   - If IF=0: Defer interrupt
   - If IF=1: Handle interrupt
4. **Hardware Interrupt Sequence:**
   - Finish current instruction
   - Push SS, RSP (if privilege change)
   - Push RFLAGS, clear IF (disable interrupts)
   - Push CS, RIP
   - Read vector from PIC (IRQ 1 → vector 33)
5. **IDT Lookup:** Get ISR address from IDT[33]
6. **ISR Handler:**
   - Save all registers
   - Call device handler (keyboard_handler)
   - Read scancode from port 0x60
   - Translate scancode to ASCII
   - Add to input buffer
   - Send EOI to PIC (End of Interrupt)
   - Restore registers
7. **iretq:** Return from interrupt
   - Pop CS, RIP, RFLAGS (restore IF), SS, RSP
   - Resume interrupted code

**Latency Breakdown:**
- PIC to CPU: ~0.1 μs
- CPU state save: ~0.2 μs
- ISR handler: ~1-5 μs
- State restore: ~0.2 μs
- **Total:** ~1.5-5.5 μs

**Nested Interrupts:**
- IF cleared on entry (no nesting by default)
- Can be enabled for high-priority IRQs
- Requires separate interrupt stacks (IST)

---

## How to Generate Diagrams

All diagrams are generated from source files in `docs/diagrams/src/`:

```bash
# Install required tools (Ubuntu/Debian)
sudo apt install graphviz
npm install -g @mermaid-js/mermaid-cli

# Install required tools (macOS)
brew install graphviz
npm install -g @mermaid-js/mermaid-cli

# Generate all diagrams
bash scripts/generate-diagrams.sh
```

The script will:
1. Find all `.dot` (Graphviz) files and generate SVG
2. Find all `.mmd` (Mermaid) files and generate SVG
3. Find all `.puml` (PlantUML) files and generate SVG (optional)
4. Output files to `docs/diagrams/`

### Manual Generation

**Graphviz (DOT):**
```bash
dot -Tsvg docs/diagrams/src/system_architecture.dot -o docs/diagrams/system_architecture.svg
```

**Mermaid:**
```bash
mmdc -i docs/diagrams/src/seq_syscall.mmd -o docs/diagrams/seq_syscall.svg -b transparent
```

**PlantUML (if installed):**
```bash
plantuml -tsvg docs/diagrams/src/example.puml -o docs/diagrams/
```

---

## Diagram Source Formats

### Graphviz DOT
- Best for: Architecture diagrams, component diagrams, data flow
- Syntax: Declarative graph description
- Tools: Graphviz (`dot` command)
- Examples: `system_architecture.dot`, `memory_layout.dot`

### Mermaid
- Best for: Sequence diagrams, state machines, flowcharts
- Syntax: Markdown-like text description
- Tools: Mermaid CLI (`mmdc` command) or web editor
- Examples: `seq_syscall.mmd`, `fsm_process.mmd`
- Online editor: https://mermaid.live/

### PlantUML
- Best for: UML diagrams (class, sequence, component)
- Syntax: Simple text description
- Tools: PlantUML (`plantuml` command)
- Online editor: https://www.plantuml.com/

---

## Contributing Diagrams

When adding new diagrams:

1. **Create source file** in `docs/diagrams/src/`
   - Use `.dot` for Graphviz
   - Use `.mmd` for Mermaid
   - Use `.puml` for PlantUML

2. **Add generation command** in source file header
   ```
   // Generate with: dot -Tsvg filename.dot -o ../filename.svg
   ```

3. **Run generation script**
   ```bash
   bash scripts/generate-diagrams.sh
   ```

4. **Add to this README** with description and links

5. **Embed in relevant docs** with:
   ```markdown
   ![Description](docs/diagrams/filename.svg)
   ```

---

## Diagram Inventory

| Diagram | Type | Source | Purpose |
|---------|------|--------|---------|
| System Architecture | Graphviz | [system_architecture.dot](src/system_architecture.dot) | Overall system design |
| Memory Layout | Graphviz | [memory_layout.dot](src/memory_layout.dot) | Virtual/physical memory |
| Boot Sequence | Graphviz | [boot_sequence.dot](src/boot_sequence.dot) | Boot process flow |
| Syscall Sequence | Mermaid | [seq_syscall.mmd](src/seq_syscall.mmd) | System call execution |
| Context Switch | Mermaid | [seq_context_switch.mmd](src/seq_context_switch.mmd) | Process switching |
| Page Fault | Mermaid | [seq_page_fault.mmd](src/seq_page_fault.mmd) | Page fault handling |
| Interrupt Handling | Mermaid | [seq_interrupt.mmd](src/seq_interrupt.mmd) | Hardware interrupts |
| Process States | Mermaid | [fsm_process.mmd](src/fsm_process.mmd) | Process lifecycle |
| Scheduler States | Mermaid | [fsm_scheduler.mmd](src/fsm_scheduler.mmd) | Scheduler algorithm |
| Page States | Mermaid | [fsm_memory_page.mmd](src/fsm_memory_page.mmd) | Memory page lifecycle |
| Kernel Components | Graphviz | [component_kernel.dot](src/component_kernel.dot) | Kernel architecture |
| Memory Components | Graphviz | [component_memory.dot](src/component_memory.dot) | Memory subsystem |
| Syscall Data Flow | Graphviz | [dataflow_syscall.dot](src/dataflow_syscall.dot) | Data transformations |

**Total:** 13 diagrams covering all major subsystems

---

## Related Documentation

- **[ARCHITECTURE.md](../ARCHITECTURE.md)** - System architecture overview
- **[API_REFERENCE.md](../API_REFERENCE.md)** - Complete API documentation
- **[DEVELOPMENT_GUIDE.md](../DEVELOPMENT_GUIDE.md)** - How to extend the system
- **[README.md](../../README.md)** - Project overview

---

**Last Updated:** 2026-05-26  
**AutomationOS Version:** 0.1.0  
**Diagrams:** 13 total (architecture, sequence, state, component, data flow)
