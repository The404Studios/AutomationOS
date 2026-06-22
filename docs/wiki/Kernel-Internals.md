# Kernel Internals

This page documents how the AutomationOS kernel actually works — memory, processes,
the (cooperative) scheduler, system calls, and interrupts — at the level of the real
source files under `kernel/`. It describes **what the code does today**, not what it
might do later. Where a subsystem has more code on disk than is wired into the build
(SMP, preemption), that is called out explicitly.

A few facts to anchor everything below, all verifiable in the tree:

- **The scheduler is cooperative.** A process keeps the CPU until it calls
  `SYS_YIELD` (or blocks/exits). The timer interrupt does **not** preempt — see the
  `NOTE` in `kernel/core/sched/scheduler.c` and `timer_handler()` in
  `kernel/drivers/pit.c`, which only bumps a tick counter.
- **The default build is single-core.** With no build flags set, `scripts/quick_build.sh`
  compiles `scheduler.c` and the single-CPU `tlb_uni.c`; the SMP-only sources are left
  out. Multi-core is a **gated, opt-in** variant: `SMP=1` (with a deeper `SMP_SCHED=1`
  sub-gate) adds `-DSMP_FOUNDATION` and the AP bring-up sources and writes a *separate*
  `build/kernel-smp.elf` — the default `build/kernel.elf` is byte-for-byte unchanged. See
  [Multi-core (SMP, gated)](#multi-core-smp-gated-experimental) below.
- **Boot is GRUB Multiboot1 on legacy BIOS.** The Multiboot1 header magic
  `0x1BADB002` lives at the top of `kernel/arch/x86_64/boot.asm`. There is no UEFI path.

---

## Memory

Source: `kernel/core/mem/` (`pmm.c`, `paging.c` lives in `kernel/arch/x86_64/`,
`heap.c`, `slab.c`, `vma.c`, `cow.c`, `vmm.c`, `vma_rbtree.c`).

### Physical memory — `pmm.c`

The physical memory manager hands out 4 KiB page frames. It is built from three
cooperating structures:

- **Buddy free lists** (`free_lists[MAX_ORDER+1]`, `MAX_ORDER = 10`) — the slow-path
  source of pages, supporting orders 0–10.
- **A flat allocation bitmap** (`pmm_bitmap`, `1 = free`) covering the full 4 GiB /
  4 KiB = 2^20-frame range (128 KiB of `.bss`). This lets `pmm_alloc_pages(count)`
  find *N* contiguous frames in one 64-bit-word scan using `__builtin_ctzll`, with a
  resume hint cursor (`pmm_bitmap_hint`).
- **Per-CPU page caches** (`cpu_caches[MAX_CPUS]`, 16 pages each) — the fast path.
  `pmm_alloc_page()` pops from `cpu_caches[cpu_id()]` in O(1); on a miss it refills 16
  pages from the global pool. On a single-core build `cpu_id()` is always 0, so this is
  effectively one cache, but the lock discipline (leaf cache lock, root global lock,
  release-before-refill) is already in place.

`pmm_init()` walks the Multiboot memory map. Critically, it only adds frames **below
1 GiB** at first (`PMM_INITIAL_MAX_PHYS = 0x40000000`), because boot.asm has only
identity-mapped the low range. It skips the first 1 MiB (BIOS/VGA/IVT), the kernel
image (`0x100000`–`__kernel_end`), and the GRUB-loaded initrd (reserved via
`pmm_reserve_initrd()`). Frames above 1 GiB are added later by
`pmm_add_remaining_pages()` once paging extends the identity map.

| Function | Role |
|----------|------|
| `pmm_alloc_page()` | O(1) single-frame alloc from per-CPU cache (refills on miss) |
| `pmm_free_page()` | Return a frame to the per-CPU cache |
| `pmm_alloc_pages(count)` | Contiguous multi-frame alloc via bitmap scan |
| `pmm_get_free_memory()` / `pmm_get_used_memory()` / `pmm_get_total_memory()` | Accounting |

Out-of-memory in `pmm_alloc_page()` calls `kernel_panic("PMM: Out of memory")` — there
is no graceful reclaim path in this build.

### Virtual memory & 4-level paging — `kernel/arch/x86_64/paging.c`

x86_64 long mode uses 4-level paging: **PML4 → PDPT → PD → PT**, indexed by
`PML4_INDEX`/`PDPT_INDEX`/`PD_INDEX`/`PT_INDEX`. `paging_init()` does the following, in
order:

1. **Enables `EFER.NXE`** (No-Execute, MSR `0xC0000080` bit 11) so PTE bit 63 (`PAGE_NX`)
   is honored. This is what makes user-space W^X (RX code / NX data, applied by the ELF
   loader in `kernel/fs/exec.c`) actually enforced.
2. **Reuses the boot PML4** (read from CR3) rather than building a new one, so freshly
   allocated page tables stay reachable through the existing identity map.
3. **Extends the identity map**: boot.asm maps 0–512 MiB with 2 MiB huge pages;
   `paging_init()` fills PD entries 256–511 to reach **1 GiB**, then allocates PDs to
   cover **up to 16 GiB** (`total_gb = 16`), all with 2 MiB huge pages
   (`PTE_PRESENT | PTE_WRITE | PS`). The identity map means *virtual address == physical
   address* for kernel/RAM access — the kernel never needs a separate phys→virt offset
   to touch a PMM frame.

`paging_map_page(virt, phys, flags)` walks/creates the four levels against the
**active** PML4 (`active_pml4`, switchable per-process via `paging_set_target()`). Two
safety rules are enforced here:

- Kernel-space virtual addresses (`>= KERNEL_SPACE_START`) are **refused** the `PAGE_USER`
  bit, so ring 3 can never reach kernel memory.
- Intermediate table entries inherit `PTE_USER` when the final page is user-accessible,
  and an existing **2 MiB huge page is split** into 512 × 4 KiB PTEs (preserving flags,
  including bit 63) when a finer mapping is needed.

Per-process address spaces are created by deep-copying the identity hierarchy so each
process gets its own PML4 (its CR3); `paging_kernel_cr3()` returns the kernel's master
PML4 for alias-free copies during ELF loading.

### Kernel heap — `heap.c`

`kmalloc()` / `kfree()` serve a fixed 16 MiB heap at the higher-half virtual address
**`HEAP_START = 0xFFFFFFFF90000000`** (`heap_init()` maps it). The allocator is a
**segregated free-list** design (13 size-class bins, not the older first-fit the
directory README describes): each block carries a 64-byte header with a `MAGIC` canary,
`prev_phys` link, and bin links. `kmalloc()` rounds up to 16 bytes, pops the smallest
fitting bin (splitting the remainder), and `kfree()` coalesces with the physically
adjacent and preceding blocks in O(1). Every returned pointer is 16-byte aligned.

### Slabs and VMAs

- `slab.c` (`slab_alloc()` / `slab_cache_create()`) provides object caches for
  fixed-size kernel allocations.
- `vma.c` backs `SYS_MMAP`: an **eager** anonymous-memory allocator. Each `mmap`
  rounds `len` up to whole pages, allocates them one at a time from the PMM, and maps
  them `PAGE_USER|PAGE_PRESENT` (+`PAGE_WRITE`) into the target address space at the
  anonymous VA window (`VMM_ANON_VA_BASE`, 4 GiB). There is **no demand paging** — every
  page is backed immediately. A per-address-space bump cursor picks VAs.
- `vma_rbtree.c` (`vma_add()`) and `cow.c` provide the red-black VMA tree and
  copy-on-write plumbing used by the page-fault path and `fork`.

---

## Processes and the cooperative scheduler

Source: `kernel/core/sched/` (`scheduler.c`, `process.c`, `context.c`),
`kernel/arch/x86_64/context_switch.asm`, `kernel/core/usermode.c`, and the PCB in
`kernel/include/sched.h`.

### The process control block

Every process is a `process_t` (`kernel/include/sched.h`). Key fields:

| Field | Meaning |
|-------|---------|
| `pid`, `parent_pid`, `state` | Identity and `process_state_t` (RUNNING / READY / BLOCKED / TERMINATED) |
| `context` (`cpu_context_t`) | Saved GP registers + `rip`, `rflags`, **`cr3`**, and a 512-byte FXSAVE area |
| `kernel_stack`, `user_stack` | Ring-0 stack (8 KiB) and ring-3 stack |
| `user_entry`, `user_rsp` | First-run entry point and stack |
| `time_slice` | Remaining quantum in ticks (decremented but **not** used to preempt — see below) |
| `resume_mode` | `RESUME_CRETURN` (cooperative C-return) or `RESUME_IRETQ` (unused on this build) |
| `fd_table[1024]` | Per-process open files (fd ≥ 3; stdio handled out-of-band) |
| `child_wait`, `exit_status` | Blocking `waitpid` support |

The currently running process is the global `current_process` (`extern` in `sched.h`),
read/written via `process_get_current()` / `process_set_current()`.

### Creating, forking, exec'ing, exiting

- **`process_create(name, entry)`** (`process.c`) allocates the PCB, an 8 KiB kernel
  stack, and a new PML4 (kernel higher-half shallow-copied, identity map deep-copied),
  and zeroes the context.
- **`sys_fork`** (`kernel/core/syscall/handlers.c`) reads the parent's saved user
  RIP/RSP/RFLAGS off the parent kernel stack, calls `process_create()`, deep-copies every
  user page with `fork_copy_user_pages()`, then builds a fresh **IRETQ frame** on the
  child's kernel stack so the child's first run lands in ring 3 with **`RAX = 0`** (the
  child's fork return value). The parent gets the child PID.
- **`sys_execve` / `sys_spawn`** load an ELF from the initrd via `kernel/fs/elf_loader.c`
  + `exec.c`, applying W^X page protections.
- **`sys_exit`** marks the process `PROCESS_TERMINATED`, wakes any parent blocked in
  `waitpid` (`process_on_terminate()`), removes it from the scheduler, and calls
  `schedule()` — which never returns to the dead process.

### The run queues — `scheduler.c`

The scheduler is structured as a Linux-style **O(1) multi-level feedback queue**: 140
priority levels (nice −20…+19 maps to priority 100±nice), an `active`/`expired` runqueue
pair, and a 140-bit bitmap so `bitmap_ffs()` finds the highest-priority non-empty queue
in constant time. `scheduler_add_process()` enqueues (idempotently, via the `on_queue`
flag) and `scheduler_pick_next()` dequeues; a process keeps its remaining `time_slice`
across yields (a deliberate fairness fix — see
`docs/BUG_FIX_SCHEDULER_TIME_SLICE.md`).

### Cooperative switching — the important part

Although `schedule()` is *written* to support timer-driven preemption (it decrements
`time_slice` and, at zero, would switch), that path is **disabled**. The decisive comment
in `scheduler.c` reads:

> ```c
> // NOTE: Timer-driven preemption disabled until kernel stack frames are
> // properly managed for context switches inside interrupt handlers.
> // For now, cooperative multitasking via SYS_YIELD only.
> ```

So in practice the only place a normal process gives up the CPU is **`sys_yield`**
(`SYS_YIELD`, syscall 15): it adds the current process to the back of the ready queue,
picks the next runnable process with a kernel stack, updates `TSS.RSP0`, and calls
`context_switch()`. The other entries into `schedule()` are `sys_exit` and the page-fault
handler killing a faulting process (`idt.c`) — never the timer.

`context_switch()` (`context.c`) primes the target's FPU state if needed, flushes any
pending lazy TLB work, sets the `scheduler_in_switch` re-entrancy guard, and calls
`context_switch_asm(from, to)` (`context_switch.asm`). That stub saves the outgoing
GP registers, `RIP`, `RFLAGS`, and **CR3** into `from->context`, then restores the
incoming context — **loading the target's CR3 switches the address space**. Cooperative
suspends use `RESUME_CRETURN`: the switch returns via a normal `ret`. A brand-new process
instead `ret`s into `process_enter_usermode_trampoline()` (`usermode.c`).

### Reaching ring 3 — `usermode.c`

`start_usermode(entry, stack)` (and the per-process trampoline) set `TSS.RSP0` to the
process's kernel stack and call `enter_usermode()` (`usermode.asm`), which builds an
IRETQ frame and drops to **ring 3** with user segments **CS = 0x23, SS/DS = 0x1B**.
`get_cpl()` can confirm the current privilege level (0 kernel / 3 user).

### FPU/SSE is enabled for ring 3

A common misconception is that this kernel is integer-only. It is not: **SSE/FPU is
enabled at boot and context-switched per task.** `cpu_enable_fpu_sse()` (`paging.c`,
called from `paging_init()`) clears `CR0.EM`, sets `CR0.MP`/`CR0.NE` and
`CR4.OSFXSR`/`CR4.OSXMMEXCPT`, then `fninit`s the x87 unit and loads `MXCSR = 0x1F80`
(all exceptions masked, round-to-nearest). Every context switch `fxsave64`/`fxrstor64`s
the 512-byte per-task FXSAVE block in the PCB (`context_switch.asm`), so XMM/x87 state is
preserved across switches in **both** the cooperative and the preemptive paths
(`context.c` seeds a clean template for new processes). The only thing that can't emit
float code is the **on-device `cc` compiler** — that's a codegen gap, not a kernel
restriction; **gcc-built userspace uses floats fine**. This is proven at runtime by
`sbin/floattest` (scalar `mulss`/`addss`/`divss`, a 2×2 float matmul, and an
auto-vectorized reduction; its `FLOATTEST: PASS` is a smoke check), and exercised harder
by `sbin/matbench`, a SIMD (`v4sf`) float matmul benchmark that compares a scalar
baseline against a hand-vectorized SSE kernel. Ring-3 SSE is the guarded foundation for
the software rasterizer and tensor kernels.

---

## Preemptive scheduling (gated, experimental)

> **The default build is still cooperative and unchanged.** Everything in this section
> is compiled **only** under `-DPREEMPTIVE`, which is **off by default**. With the flag
> unset, the symbols below do not exist, `build/kernel.elf` is byte-for-byte what it was,
> and the cooperative model above is the whole story. This is a gated, opt-in,
> *experimental* variant — **not** the default — landed so it can be stress-tested before
> it ever becomes the default.

### Building the opt-in variant

```sh
PREEMPT=1 bash scripts/quick_build.sh    # -> build/kernel-preempt.elf
```

The `PREEMPT=1` gate in `scripts/quick_build.sh` adds `-DPREEMPTIVE` to **both** the
nasm flags (so `interrupt.asm` and `context_switch.asm` assemble their
`%ifdef PREEMPTIVE` blocks) **and** the gcc `CFLAGS` (so `scheduler.c` compiles
`schedule_from_irq()` and `idt.c` points IDT[32] at `irq0_preempt`), and writes a
**separate** output, `build/kernel-preempt.elf`, so a preemptive build never touches the
normal `build/kernel.elf`. With `PREEMPT` unset the whole block is skipped.

### How a tick becomes a context switch

In the preemptive build, IDT vector 32 (IRQ0, the PIT) points at **`irq0_preempt`**
(`interrupt.asm`) instead of the cooperative `irq0`. The PIT still runs at **1000 Hz**
(`pit_init(1000)`). `irq0_preempt` pushes a full `interrupt_frame_t` onto the kernel
stack (the GP regs plus the CPU-pushed `RIP/CS/RFLAGS/RSP/SS`), passes a pointer to it to
**`schedule_from_irq(frame)`**, then — after the C handler may have *rewritten the frame
in place* — pops the (possibly new) registers and does a single **`iretq`**. That one
`iretq` is the actual control transfer: CR3 and `TSS.RSP0` were already updated by
`schedule_from_irq` for the incoming process. The stub bypasses `idt.c`'s `irq_handler`,
so `schedule_from_irq` sends the PIC EOI itself and calls a weak `pit_tick()` hook to
keep the tick counter advancing.

### The safety model — a non-preemptible kernel

The linchpin is a **hard ring-3 guard** in `schedule_from_irq`:

> ```c
> // HARD RING-3 GUARD — non-preemptible kernel (CRITICAL safety).
> if ((frame->cs & 3) != 3) {
>     return;  // kernel was running: leave frame untouched, resume it.
> }
> ```

If the timer interrupted code whose CS has RPL ≠ 3 — i.e. the **kernel itself** was
running (a syscall, an IRQ handler, any kernel critical section) — `schedule_from_irq`
**refuses to switch** and leaves the frame untouched, so the trailing `iretq` simply
resumes the interrupted kernel code. The timer therefore **only ever preempts userspace,
never mid-kernel.** This makes the kernel **non-preemptible**, which is what lets a
codebase that assumes single-threaded kernel execution stay correct: the scheduler can
never switch away while a kernel data structure is half-updated or a lock is held. It is
the single most important invariant of this scheduler. (A second guard only preempts a
process actually in `PROCESS_RUNNING`, so a process blocked in `sti`/`hlt` inside
`wq_block_current` isn't wrongly re-readied.)

### Two resume mechanisms — and the cooperative→iretq bridge

A process can be suspended two ways, tracked by `resume_mode` in the PCB:

- **`RESUME_CRETURN`** — suspended cooperatively (`SYS_YIELD` or a blocking syscall), or
  brand-new. Its saved `context.rip` is a **kernel continuation**, so the normal
  `context_switch()` (which ends in `ret`) resumes it.
- **`RESUME_IRETQ`** — preempted by the timer IRQ while in **ring 3**, so its saved
  context holds its *ring-3* `RIP`/`RSP`/regs. A `ret` into that would run user code at
  CPL 0 (runaway → **#DF**); it must be resumed by restoring the ring-3 state and
  `iretq`-ing.

`schedule_from_irq` performs an in-place `iretq` swap only when the **incoming** process
is `RESUME_IRETQ` (it saves the outgoing ring-3 frame with `context_save_irq`, rewrites
the on-stack frame for the incoming one with `context_load_irq`, then switches CR3
PCID-safely). A cooperatively-suspended successor is left on the ready queue for the
cooperative path to pick up.

Because the IRQ path makes many processes `RESUME_IRETQ`, the **cooperative** resume
sites had a latent bug: `sys_yield`/`schedule`/`wq_block_current` end in `ret`, which
would jump to a `RESUME_IRETQ` process's ring-3 RIP at CPL 0 (#DF). The fix is a new asm
helper **`context_switch_to_iretq`** plus a router, **`cooperative_switch_to()`**, which
sends `RESUME_IRETQ` successors through a proper ring-3 `iretq` and `RESUME_CRETURN`
through the plain `context_switch`. In a non-preemptive build no process is **ever**
`RESUME_IRETQ`, so `cooperative_switch_to` is always the plain `context_switch` path —
identical behaviour to before.

### The fairness fix — IRQ first-dispatch

The first cut of the preemptive scheduler survived faults but **starved new tasks**: the
IRQ path could only *resume* an already-ring-3 process, so a brand-new process — whose
first ring-3 entry normally goes through the cooperative trampoline — never started while
a non-yielding CPU hog monopolized the core. (Stress proof: only burner 0 ran; burners
1–5 starved.) The fix detects a never-run userspace process by its one reliable
signature — `context.rip == process_enter_usermode_trampoline` — and gives it its **first
ring-3 entry directly from the IRQ** by *synthesizing* a fresh `iretq` frame
(`RIP = user_entry`, `CS = 0x23`, `RSP = user_rsp`, `SS = 0x1B`, `RFLAGS = 0x202`, GP regs
zeroed; TSS/CR3/PCID set like the resume path). After that the process is `RESUME_IRETQ`
like any preempted task. A genuine mid-kernel `RESUME_CRETURN` task (`rip != trampoline`)
still defers to the cooperative path. A gated `[SCHED] first-dispatch …` log line per
never-run process gives causal proof in the serial trace.

### Validation — the stress harness

`scripts/stress_preempt.sh` is the decisive end-to-end proof (and the regression guard).
It builds `kernel-preempt.elf`, packages a `STRESS=1` ISO whose `init` spawns **six
never-yielding `sbin/cpuburn` burners** (integer churn + a 2×2 float/SSE matmul + a dot
reduction each iteration, so they stress XMM across preemptive switches) plus three
`floattest` runs, soaks it ~60 s with serial capture, and an `EXIT` trap **always**
rebuilds the default cooperative kernel so the tree is never left preemptive. The stress
workload is itself gated (`-DPREEMPT_STRESS`, off by default), so a cooperative build
never spawns a non-yielder — on the cooperative kernel the first burner would hang the
box, which is the entire point.

Measured result: **all six burners progress equally** (interleaved heartbeats == fair
time-slicing of ring 3), **zero** page-faults / GP faults / panics / double-faults,
`FLOATTEST: PASS` under preemption, and the desktop stays alive. The default cooperative
build is unaffected and **boot smoke is 43/43** (`scripts/smoke_boot.sh`; a `GAMETEST=1`
harness additionally proves 25/25 games+apps spawn and survive).

This is real, validated, and committed — but it is still **gated and experimental**, and
**not the default**. SMP (true multi-core) is a separate, larger concurrency-safety
project, also gated — see the next section.

---

## Multi-core (SMP, gated, experimental)

> **The default build is single-core and unchanged.** Everything here is compiled **only**
> under `-DSMP_FOUNDATION`, off by default. With the flag unset the AP sources are not
> compiled, `build/kernel.elf` is byte-for-byte what it was, and the single-core story
> above is the whole story.

Like preemption, multi-core is an opt-in, separately-built variant rather than a flip of
the default. `scripts/quick_build.sh` exposes two nested gates:

```sh
SMP=1 bash scripts/quick_build.sh              # -> build/kernel-smp.elf : BSP LAPIC + AP bring-up
SMP=1 SMP_SCHED=1 bash scripts/quick_build.sh  # -> adds real per-CPU scheduling (CPU1 runs processes)
```

`SMP=1` defines `-DSMP_FOUNDATION` and pulls in the AP-bring-up sources (`lapic.c`,
`ap_boot.c`, `ap_trampoline.asm`) that the default build leaves out, writing the separate
`build/kernel-smp.elf`. The deeper `SMP_SCHED=1` sub-gate (which requires `SMP=1`) adds
`-DSMP_SCHED` and the real per-CPU scheduler so CPU1 runs actual processes; every scheduler
change for it is wrapped `#ifdef SMP_SCHED`. There is also a test-only
`SMP_FORCE_AP_FAIL=1` knob that forces AP start to fail, to prove the BSP degrades cleanly
to single-core rather than hanging — never set it for a shipping image. The deliberate
choice of `-DSMP_FOUNDATION` (rather than the older `-DCONFIG_SMP`) is so the default
`#ifndef CONFIG_SMP` cooperative scheduler and the single-CPU `tlb_uni.c` stay selected
unless you explicitly opt in.

---

## System calls

Source: `kernel/core/syscall/` (`syscall.c` dispatcher, `handlers.c` implementations),
`kernel/arch/x86_64/syscall_init.c` + `syscall.asm`, and the numbers in
`kernel/include/syscall.h`.

### SYSCALL/SYSRET MSR setup

User code enters the kernel with the `syscall` instruction. `syscall_msr_init()`
(`syscall_init.c`) programs the MSRs:

| MSR | Value | Purpose |
|-----|-------|---------|
| `IA32_EFER` (`0xC0000080`) | set bit 0 (SCE) | Enable the `SYSCALL`/`SYSRET` instructions |
| `IA32_STAR` | kernel CS `0x08`, SYSRET base `0x10` | Segment selectors loaded on entry/return |
| `IA32_LSTAR` | `&syscall_entry` | Kernel entry RIP for `SYSCALL` |
| `IA32_FMASK` | `0x200` | Clears `RFLAGS.IF` on entry (interrupts off in the syscall prologue) |

Because `SYSCALL` does **not** switch RSP, `syscall_entry` (`syscall.asm`) manually saves
the user RSP, loads the kernel stack (`kernel_rsp_save`), pushes the full user register
set, marshals arguments into the System V ABI order, and calls `syscall_dispatch()`. On
return it restores registers, forces `IF=1` for user space, and executes `o64 sysret`.

### Dispatch

`syscall_dispatch(num, a1..a6)` (`syscall.c`) bounds-checks `num < MAX_SYSCALLS` (256),
looks up `syscall_table[num]`, and calls it. Unregistered numbers return the canonical
**negative** `ENOTSUP`; out-of-range numbers return negative `EINVAL`. Two hot read-only
calls — `SYS_GETPID` and `SYS_GET_TICKS_MS` — are inlined ahead of the table lookup for
latency. The handler signature is uniform:
`int64_t handler(uint64_t a1, …, uint64_t a6)`.

### Syscall numbers — and a footgun

AutomationOS uses its **own** syscall numbering, *not* the Linux ABI. This is the single
most common mistake when hand-writing user-space against this kernel:

> ⚠️ **`SYS_WRITE` is 3, and syscall number 1 is `SYS_FORK` — not `write`.** On Linux,
> syscall 1 is `write`; here, calling syscall 1 *forks the process*. Always pull the
> constants from `kernel/include/syscall.h`; never assume Linux numbers.

A representative excerpt of the table (full list in `syscall.h`):

| # | Name | Handler | Notes |
|---|------|---------|-------|
| 0 | `SYS_EXIT` | `sys_exit` | Terminate; never returns to caller |
| **1** | **`SYS_FORK`** | `sys_fork` | **Not `write`** — the footgun |
| 2 | `SYS_READ` | `sys_read` | |
| **3** | **`SYS_WRITE`** | `sys_write` | fd, buf, count; ≤ 1 MiB per call |
| 4 | `SYS_OPEN` | `sys_open` | |
| 5 | `SYS_CLOSE` | `sys_close` | |
| 6 | `SYS_WAITPID` | `sys_waitpid` | Blocks parent on `child_wait` |
| 7 | `SYS_EXECVE` | `sys_execve` | ELF from initrd, W^X applied |
| 8 | `SYS_GETPID` | (inlined) | Fast-path before table lookup |
| 9 | `SYS_SLEEP` | `sys_sleep` | |
| 14 | `SYS_READ_EVENT` | `sys_read_event` | Input events |
| **15** | **`SYS_YIELD`** | `sys_yield` | The cooperative-scheduling hand-off |
| 16 | `SYS_SPAWN` | `sys_spawn` | Launch ELF by path |
| 37 | `SYS_MMAP` | `sys_mmap` | Eager anonymous mapping (see `vma.c`) |
| 39 | `SYS_FB_ACQUIRE` | `sys_fb_acquire` | Map framebuffer into caller |
| 40 | `SYS_GET_TICKS_MS` | (inlined) | Monotonic ms since boot (PIT-driven) |
| 88 | `SYS_RECOVERY_OVERLAY` | `sys_recovery_overlay` | Self-heal recovery animation |
| 106 | `SYS_SPAWN_EX_ARGV` | `sys_spawn_ex_argv` | Spawn with an explicit argv vector (agent rail) |

Numbers 41+ are an integrator-assigned extended map (RTC, RNG, sockets, clipboard,
notifications, poll/select, futex, sendfile, batch, channels, signals). `syscall_init()`
registers the whole table; roughly ~80 numbers are defined and the dispatcher reports the
count at boot. Two later blocks — the **WiFi control plane** and the **audio mixer** — are
documented below; they sit at numbers 113–124.

### WiFi control plane — `SYS_WLAN_*` (113–117, plus `SYS_WLAN_DIAG` at 124)

The WiFi syscalls live in `kernel/net/wlansyscall.c`, with the kernel/userspace ABI fixed
in `kernel/include/uapi/wlan.h` (every struct carries an `ABI_SIZE` constant and a
`_Static_assert` so any drift is a compile error). They are deliberately **thin**: each
handler finds the WiFi interface (`netif_get_wifi_default()`), validates the user struct,
and dispatches **through `netif_t.wifi`** — a `wifi_ops` vtable (`kernel/include/wifi.h`).
This `wifi_ops` pointer is the **swap seam**: the simulated backend
(`kernel/drivers/net/wireless/sim/wifisim.c`, `WIFI_SIM=1`) and the from-scratch Intel
**iwlwifi** DVM driver (`kernel/drivers/net/wireless/intel/iwlwifi/`) implement the exact
same contract, so the syscall layer never touches a driver directly.

| # | Name | Handler | Payload (UAPI struct) |
|---|------|---------|-----------------------|
| 113 | `SYS_WLAN_SCAN` | `sys_wlan_scan` | fills `uapi_wlan_bss_t[]`, returns count (≤16/call) |
| 114 | `SYS_WLAN_CONNECT` | `sys_wlan_connect` | `uapi_wlan_connect_t` (SSID + passphrase) |
| 115 | `SYS_WLAN_STATUS` | `sys_wlan_status` | `uapi_wlan_status_t` (state + RSSI) |
| 116 | `SYS_WLAN_DISCONNECT` | `sys_wlan_disconnect` | none |
| 117 | `SYS_WLAN_SET_KEY` | `sys_wlan_set_key` | `uapi_wlan_setkey_t` (supplicant installs PTK/GTK) |
| 124 | `SYS_WLAN_DIAG` | `sys_wlan_diag` | `uapi_wlan_diag_t` (radio bring-up diagnostics) |

All return `ENOTSUP` (negative) when no WiFi interface is registered — e.g. a wired-only or
no-NIC build — so they are safe no-ops rather than failures there.

**`SYS_WLAN_DIAG` (124)** is the bring-up observability path. The Intel radio has no
emulator, so on the physical T410 it has to be iterated by reading *where* bring-up
stopped. Rather than require a serial cable, the active WiFi backend updates a single
snapshot store — `kernel/net/wifidiag.c` — as detect → APM → ALIVE → NVM → register → scan
proceeds (and records the step + a message on any failure). `sys_wlan_diag` just
`copy_to_user`s that `uapi_wlan_diag_t` (card name, family, RF-kill, stage, MAC, channel
count, last scan count, status message). The Network Manager (`userspace/apps/netman`)
renders it as a live "Radio:" line — see `../../screenshots/netman_diag.png`. Writers in
`wifidiag.c`: `wifi_diag_card/_rfkill/_stage/_mac/_alive/_scan/_msg`, a single-writer
(bring-up path) / single-reader (the syscall) discipline that needs no lock on this
uniprocessor build.

### Audio mixer — `SYS_AUDIO_*` (118–123)

The audio mixer surface (handlers in `kernel/core/syscall/syscall.c`, defined in
`syscall.h`) is a thin shim over the Intel HDA driver (built only with `HDA_ENABLE=1`); the
Sound Manager app (`userspace/apps/soundman`) drives it.

| # | Name | Handler | Effect |
|---|------|---------|--------|
| 118 | `SYS_AUDIO_VOLUME` | `sys_audio_volume` | set output volume 0..100 → `hda_set_volume` |
| 119 | `SYS_AUDIO_MUTE` | `sys_audio_mute` | set mute 0\|1 → `hda_set_mute` |
| 120 | `SYS_AUDIO_OUTPUTS` | — | RESERVED → `ENOTSUP` |
| 121 | `SYS_AUDIO_SELECT` | — | RESERVED → `ENOTSUP` |
| 122 | `SYS_AUDIO_TEST` | `sys_audio_test` | play a test tone (arg1 = freq Hz, arg2 = ms, capped) |
| 123 | `SYS_AUDIO_STATUS` | `sys_audio_status` | fills `audio_status_t` {present, volume, muted, codec_vendor} |

The audio handlers return cleanly (negative) when no HDA controller/codec is present, so
they are safe no-ops on audioless boots.

> **Number map.** The audio mixer owns the contiguous block **118–123** with no overlap,
> so `SYS_AUDIO_VOLUME` at 118 is fully reachable. (`SYS_WLAN_DIAG` was briefly assigned 118
> as well; it has since been moved to its own free slot, **124**, so there is no longer any
> collision.) Always pull constants from `kernel/include/syscall.h`.

---

## Interrupts

Source: `kernel/arch/x86_64/idt.c`, `interrupt.asm`, and `kernel/drivers/pit.c`.

### The IDT

`idt_init()` (`idt.c`) builds a 256-entry IDT. Vectors 0–31 are the CPU exceptions
(stubs `isr0`–`isr31` from `interrupt.asm`), and 32–47 are the hardware IRQs
(`irq0`–`irq15`). Gates are interrupt gates (`0x8E`, DPL 0). Two vectors run on a
dedicated stack via **IST1** so a kernel stack overflow can still be handled cleanly:
**#DF (8)** and **NMI (2)**. The exception handler prints a named reason
(`exception_messages[]`); a user-mode fault verifies the faulting process by matching
live CR3 against `current->context.cr3` before terminating it (refusing to kill the wrong
process on a mismatch), then calls `schedule()` to switch away.

### The 8259 PIC

The legacy **8259 PIC** pair is remapped by `pic_remap()` so the IRQs don't collide with
CPU exceptions:

| Controller | Ports | IRQs | Mapped to vectors |
|------------|-------|------|-------------------|
| Master | `0x20` / `0x21` | 0–7 | INT 32–39 |
| Slave | `0xA0` / `0xA1` | 8–15 | INT 40–47 (cascaded via IRQ2) |

All IRQ lines are **masked at boot** (`0xFF` to both data ports) and unmasked
individually when a driver registers a handler via `irq_register_handler()`. The IRQ
dispatch path sends EOI (`0x20`) to the master (and the slave, for IRQ 8–15) after the
handler returns.

### The PIT tick — what powers `SYS_GET_TICKS_MS`

`pit_init(frequency)` (`pit.c`) programs PIT channel 0 in Mode 2 (rate generator),
computes the divisor with nearest-integer rounding, derives the **actual** frequency from
that divisor (avoiding drift), and registers `timer_handler` on IRQ0. The handler is
deliberately minimal:

> ```c
> // COOPERATIVE MODE: only increments the tick counter.
> // Does NOT call schedule() -- preemptive scheduling is deferred.
> static void timer_handler(void) {
>     timer_ticks++;
>     scheduler_tick();   // no-op / load-balance stub on single core
> }
> ```

`timer_get_ticks_ms()` converts the tick count to milliseconds
(`timer_ticks * 1000 / timer_frequency`), and that is exactly what the inlined
**`SYS_GET_TICKS_MS`** (syscall 40) returns to user space. `timer_sleep(ms)` busy-waits
on the same counter (using `hlt` only when `RFLAGS.IF` is set). Because IRQ0 never calls
`schedule()`, the timer advances time but never preempts — reinforcing the cooperative
model from the interrupt side.

---

## What is *not* here (yet)

To keep this page honest:

- **No preemption *in the default build*.** The default `kernel.elf` is cooperative: the
  timer interrupt only counts ticks, the `time_slice` countdown in `schedule()` is gated
  off behind the `NOTE` in `scheduler.c`, and tasks switch only at `SYS_YIELD`,
  `sys_exit`, or a fatal fault. There **is** now a gated, opt-in preemptive variant
  (`PREEMPT=1 bash scripts/quick_build.sh` → `build/kernel-preempt.elf`) — validated but
  experimental and **not** the default. See
  [Preemptive scheduling](#preemptive-scheduling-gated-experimental) above.
- **No SMP *in the default build*.** The default `kernel.elf` is single-core: it compiles
  the `#ifndef CONFIG_SMP` scheduler and the single-CPU `tlb_uni.c`, and leaves the
  AP-bring-up sources out. A gated, opt-in multi-core variant
  (`SMP=1` / `SMP=1 SMP_SCHED=1` → `build/kernel-smp.elf`) is validated but experimental
  and **not** the default. See [Multi-core (SMP, gated)](#multi-core-smp-gated-experimental)
  above.
- **No UEFI.** Boot is GRUB Multiboot1 (`0x1BADB002`) on legacy BIOS.

These are tracked under "Planned" in the [Roadmap](../ROADMAP.md).

---

## See also

- [Home](Home.md)
- [Architecture](Architecture.md)
- [Drivers & I/O](Drivers-and-IO.md)
- [Desktop & Apps](Desktop-and-Apps.md)
- [Building & Running](Building-and-Running.md)
- [Roadmap](../ROADMAP.md)
