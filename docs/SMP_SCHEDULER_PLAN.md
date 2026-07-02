I now have everything I need. All key facts are confirmed against the actual source:

- `cpu_id()` is a hard `return 0` stub in stubs.c:28 (smp.c version not compiled).
- `LAPIC_TIMER_VECTOR` (the periodic-timer LVT vector) is set in lapic.c but I need its numeric value — it's defined elsewhere; the timer ISR path through IDT[32] uses PIC EOI (`outb(PIC1_COMMAND, PIC_EOI)`), so a LAPIC timer vector colliding with IDT[32] would double-EOI. This is a real constraint for the plan.
- The AP trampoline does `lgdt`/`lidt` from the param block but **never `ltr`** — no TSS on the AP at all. The param block has only 6 fields (cr3, stack_top, entry, arg, gdtr, idtr); a TSS selector field must be added.
- `tss` is a single global (gdt.c:36), `ist1_stack` is single, `tss_set_kernel_stack()` writes the one global, and context.c writes the single global `kernel_rsp_save`.
- `schedule_from_irq` is already per-`this_cpu()`-aware via `scheduler_pick_next()`, has the hard ring-3 guard, and synthesizes a first-dispatch iretq frame for never-run processes.

I have what I need to write the plan.

---

# SMP Process Scheduling on CPU1 — Brick-by-Brick Execution Plan

## Verified ground truth (corrections to the maps before you start)

The maps blend "today" with several different planned states and contradict each other. Confirmed against source:

1. **`cpu_id()` lives in `kernel/stubs.c:28`** as a hard `return 0`. The smp.c version is **not compiled** (quick_build.sh never adds smp.c; only lapic.c + ap_boot.c + ap_trampoline.asm under `SMP=1`). **Ignore every map reference to "smp.c:153 cpu_id" — that file is dead.** You will add the real `cpu_id()` to ap_boot.c (which *is* compiled), not stubs.c.
2. **The AP trampoline never executes `ltr`.** It does `lgdt`/`lidt` from the param block and that is all. There is no TSS on CPU1 today. The param block (ap_trampoline.asm:208) has exactly 6 fields and **no slot for a TSS selector** — you must add one.
3. **The timer path is PIC-based, not LAPIC.** `schedule_from_irq` does `outb(PIC1_COMMAND, PIC_EOI)` (scheduler.c:1217). The PIC delivers IRQ0 to the BSP only. `lapic_timer_init` programs `LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC` but that LVT vector is **not** what the preempt build wires into IDT[32]. CPU1's tick must come from the **LAPIC timer**, and its ISR must EOI the **LAPIC** (`lapic_write(LAPIC_EOI,0)`), never the PIC.
4. **`schedule_from_irq` is already per-CPU-correct** via `this_cpu()` and has the hard ring-3 guard (scheduler.c:1249) plus first-dispatch frame synthesis (scheduler.c:1316+). This is the single biggest piece of luck: the dispatch core does not need rewriting, only its preconditions (per-CPU runqueue, per-CPU TSS, per-CPU current, a LAPIC tick).
5. **The single riskiest changes are global TSS/`kernel_rsp_save` and global `current_process`** (gdt.c:36, context.c:131-135, process.c:28). These are written by the BSP today and become cross-CPU clobbers the instant CPU1 enters/exits ring 3.

The SMP build is `SMP=1 bash scripts/quick_build.sh` → `build/kernel-smp.elf`, run with QEMU `-smp 2`. Every brick below builds and boots **that** kernel and is gated so `SMP=1` without the new sub-gate is byte-for-byte the current brick-6 coprocessor kernel.

---

## Risk ordering and the gate philosophy

Bricks are ordered so that **each one is independently bootable and the dangerous shared-state changes land before CPU1 ever touches them in anger.** No brick enables CPU1-in-ring-3 until per-CPU TSS, per-CPU `kernel_rsp_save`, per-CPU current, and a private runqueue all exist and are individually proven. Add one master sub-gate to quick_build.sh:

```
SMP=1 SMP_SCHED=1 bash scripts/quick_build.sh   # -DSMP_SCHED, build/kernel-smp.elf
```

Every change below is wrapped `#ifdef SMP_SCHED`. With `SMP=1` alone you get today's coprocessor kernel unchanged — that is your rollback for the entire series at any moment.

---

## BRICK A — Real `cpu_id()` (lowest risk, unblocks everything)

**Why first:** `this_cpu()`, `preempt_disable()`, per-CPU TSS, and per-CPU runqueues are all `cpus[cpu_id()]`. With `cpu_id()==0` on CPU1, *every* per-CPU write from CPU1 corrupts CPU0's slot. This is the root enabler and is observable on its own.

**Files / functions:**
- `kernel/arch/x86_64/ap_boot.c`: add `uint32_t cpu_id(void)` under `#ifdef SMP_SCHED` (ap_boot.c is compiled in the SMP build; stubs.c's is not, so there's no duplicate-symbol clash as long as you gate). Read xAPIC ID from `lapic_read(0x20) >> 24`, then map LAPIC id → logical id by scanning the known AP APIC id (CPU1's APIC id captured from MADT in `try_start_cpu1`). Return 0 for the BSP's APIC id, 1 for the AP's, and **panic/return 0xFFFF on any unexpected id** (bounds-guard per hazard #3).
- `kernel/stubs.c:28`: wrap the stub `#ifndef SMP_SCHED` so only one definition compiles.

**Per-CPU state:** none new. Store the BSP and AP APIC ids in two file-scope vars in ap_boot.c, set during bring-up before the AP runs anything that calls `cpu_id()`.

**Locking/IPI/TLB/timer:** none. Pure read of a CPU-local MMIO register.

**Checkpoint test:** In `ap_main`, before the worker loop, `kprintf("[SMP] AP cpu_id=%u (expect 1)\n", cpu_id())` and on the BSP after bring-up print `cpu_id()` (expect 0). Serial must show `cpu_id=1` from the AP and `0` from the BSP. The existing brick-6 `worktest` (g_worktest==500500) must still pass — proves the coprocessor path is intact.

**Rollback:** Drop `SMP_SCHED`. The `#ifndef` restores the constant-0 stub. Zero residual effect.

---

## BRICK B — Per-CPU TSS + per-CPU IST + the `ltr` on CPU1 (highest *latent* risk, fixed before use)

**Why second:** This is the most dangerous *shared* state (hazards #1/#3/#4 across the context-switch and gdt maps: one global TSS means CPU1's ring-3 entry uses CPU0's RSP0 → wrong-stack corruption → #DF on the shared IST → triple fault). It must exist **before** CPU1 can possibly enter ring 3. We build and prove it while CPU1 is still in the coprocessor loop (never takes an interrupt), so a bug here cannot yet fire — de-risking by construction.

**Files / functions:**
- `kernel/arch/x86_64/gdt.c`:
  - `tss_t tss` → `static tss_t tss_array[MAX_CPUS]` (under `#ifdef SMP_SCHED`; keep the scalar in the `#else`).
  - `ist1_stack[8192]` → `ist1_stacks[MAX_CPUS][8192]`.
  - **GDT must gain a second TSS descriptor.** Today GDT is `gdt[7]` with the TSS at [5-6]. Extend to `gdt[9]`; put CPU0's TSS at selector `0x28` (entries 5-6, unchanged) and CPU1's TSS at selector `0x38` (entries 7-8). Both CPUs share the one GDT (the AP already `lgdt`s the BSP image) — they just `ltr` different selectors. This avoids per-CPU GDTs entirely (simpler, and the map's "per-CPU GDT" suggestion is unnecessary).
  - `tss_init()`: loop `c in 0..MAX_CPUS`, zero each TSS, set `iomap_base`, point each `ist*.ist1` at its own `ist1_stacks[c]`, install both descriptors.
  - `tss_set_kernel_stack(uint64_t)` → write `tss_array[cpu_id()].rsp0`. **No lock needed** — each CPU writes only its own TSS slot (this is the correct refutation of the map's `tss_lock` proposal; a lock there would be pure contention).
  - New `void gdt_ap_load_tss(void)` → `ltr $0x38` for the AP.
- `kernel/arch/x86_64/ap_trampoline.asm`: add a `+64 tss_sel` field to the param block (bump the `times` padding accordingly) and, **after `lidt`**, `ltr` the selector from the param block. Alternatively (cleaner, less asm risk) skip the trampoline edit and have `ap_main` call `gdt_ap_load_tss()` as its very first C statement — the AP doesn't take interrupts yet, so loading TR in C before any `sti` is safe. **Prefer the C path** to keep the fragile real-mode asm untouched.
- `smp.c`/`ap_boot.c` param-block writer: only needed if you take the asm path.

**Per-CPU state:** `tss_array[MAX_CPUS]`, `ist1_stacks[MAX_CPUS][8192]`, second TSS GDT descriptor.

**Locking/IPI/TLB/timer:** none. TR load is CPU-local.

**Checkpoint test:** `ap_main` calls `gdt_ap_load_tss()`, then `kprintf("[SMP] AP TR loaded, tss_array[1].rsp0=%p\n", ...)`. Add a `str` (store TR) read-back assert that TR==0x38 on the AP and 0x28 on the BSP. Boot, confirm both, and confirm `worktest` still passes. **Critically: still no `sti` on CPU1 — this brick changes descriptor tables only, takes zero interrupts, so a latent RSP0 bug cannot fire yet.**

**Rollback:** Drop `SMP_SCHED` → scalar `tss`/`ist1_stack`/`gdt[7]` restored, no `ltr` on AP.

---

## BRICK C — Per-CPU `kernel_rsp_save` (the syscall-entry twin of Brick B)

**Why:** `kernel_rsp_save` (context.c:135, defined in syscall.asm) is the SYSCALL-entry stack pointer, a second global that clobbers exactly like TSS.RSP0 (hazard #2, #7 in the context map). Must be per-CPU before CPU1 runs ring-3 code that can `syscall`.

**Files / functions:**
- `kernel/arch/x86_64/syscall.asm`: `kernel_rsp_save: dq 0` → `kernel_rsp_save_array: times MAX_CPUS dq 0`. At syscall entry, index by CPU. **Do not call `cpu_id()` on the syscall fast path** (50-100 cycle MADT scan — hazard #10). Instead use `swapgs` + `GS:[0]` per-CPU base, or the cheap interim: read xAPIC id inline (`mov rsp` after a small LAPIC read) — but the clean answer is to stash this CPU's `kernel_rsp` in its TSS and load from there, or set GS.base at AP init to `&percpu[cpu]`. **Recommended:** set `IA32_KERNEL_GS_BASE` per CPU in Brick B's AP init, `swapgs; mov rsp, gs:[KRSP_OFF]`. This also pays off for every later per-CPU access.
- `kernel/core/sched/context.c:134-135`: write `kernel_rsp_save_array[cpu_id()] = kstack_top` (context_switch is not the hot syscall path, so `cpu_id()` here is fine).

**Per-CPU state:** `kernel_rsp_save_array[MAX_CPUS]`; optionally a `percpu` struct reachable via GS.base.

**Locking/IPI/TLB/timer:** none.

**Checkpoint test:** Keep CPU1 in the coprocessor loop. On the **BSP**, run the normal desktop and confirm syscalls still work (the BSP now indexes `kernel_rsp_save_array[0]` — proves the array path is correct on the known-good CPU before CPU1 ever uses it). Print `kernel_rsp_save_array[0]` after a context switch; it must match TSS.RSP0. Smoke suite must stay green.

**Rollback:** Drop `SMP_SCHED` → scalar `kernel_rsp_save` restored.

---

## BRICK D — Per-CPU current + per-CPU idle thread + per-CPU runqueue init (no CPU1 dispatch yet)

**Why:** Before CPU1 calls `schedule_from_irq`, `cpus[1]` needs a valid `idle_thread`, initialized `rq_active/rq_expired`, and `current_thread`. The map confirms `schedule_from_irq` and `scheduler_pick_next` are *already* `this_cpu()`-based, so once `cpu_id()` is real (Brick A) and `cpus[1]` is populated, CPU1's dispatch core works **with no edits to the pick/switch logic**. We also make `current_thread` authoritative for dispatch while keeping the global `current_process` as a read shadow (hazard #1/RACE-003) — minimal, since `process_set_current` already dual-writes via `cpu_set_current_thread` (scheduler.c:150).

**Files / functions:**
- `kernel/core/sched/scheduler.c`:
  - `scheduler_init()`: after creating CPU0's idle thread, under `#ifdef SMP_SCHED` call `create_idle_thread(1)` and `runqueue_init(&cpus[1].rq_active/.rq_expired)`, set `cpus[1].online`, `cpus[1].apic_id`. Each idle thread already gets its own `kernel_stack` via `process_create` (so CPU1's idle has its own RSP0 — feeds Brick B). **The idle thread must NOT be enqueued** (already the invariant, scheduler.c comment at pick_next; preserve for CPU1).
  - Keep the **global `scheduler_lock`** for now (correctness over contention — explicitly deferred to Brick H). Both CPUs serialize on it; that's fine and safe.
  - `sleep_list_*`: leave on CPU0 only this brick (do not let CPU1 run sleepers yet — hazard #6). Pin CPU1's first workload to non-sleeping processes.
- `kernel/core/sched/process.c`: add `process_t* current_process_on_cpu(uint32_t c){ return cpus[c].current_thread; }` accessor; document the global as a shadow.

**Per-CPU state:** `cpus[1].idle_thread`, `cpus[1].rq_active/rq_expired`, `cpus[1].current_thread`, `cpus[1].online`.

**Locking/IPI/TLB/timer:** global `scheduler_lock` made IRQ-safe (`spin_lock_irqsave`) on all runqueue entry points so CPU1's future timer IRQ can't deadlock against CPU0 holding it (hazard #2 in the primitives map). No IPI/TLB yet.

**Checkpoint test:** Still no `sti` on CPU1. On the BSP, `kprintf` the addresses of `cpus[0].idle_thread`, `cpus[1].idle_thread`, their `kernel_stack`s — must be **distinct, non-NULL** (hazard #9: shared idle stack = corruption). Smoke green. `worktest` green.

**Rollback:** Drop `SMP_SCHED`.

---

## BRICK E — LAPIC timer tick on CPU1, ISR proven, but scheduling still inhibited (de-risk the interrupt itself)

**Why:** Separate "CPU1 takes a periodic interrupt and EOIs correctly" from "CPU1 context-switches." This isolates the timer/EOI plumbing (PIC-vs-LAPIC EOI mismatch is a classic triple-fault source) from the dispatch logic.

**Files / functions:**
- `kernel/arch/x86_64/lapic.c`: ensure `LAPIC_TIMER_VECTOR` ≠ 32 (don't collide IDT[32]'s PIC handler). Use a dedicated vector (e.g. 0x40+). Wire that IDT vector to a new `lapic_timer_isr` (asm stub mirroring `irq0_preempt`) that calls a C `lapic_tick(frame)`.
- New `lapic_tick(interrupt_frame_t*)`: **EOI the LAPIC** (`lapic_write(LAPIC_EOI, 0)`), bump a per-CPU tick counter, then **return without calling `schedule_from_irq`** (gated off this brick). On the BSP this path is unused (BSP keeps PIC IRQ0).
- `kernel/arch/x86_64/ap_boot.c` `ap_main`: after Bricks A-D, call `lapic_timer_init(100)` (100 Hz), then `sti`. Keep the coprocessor `worktest` loop running underneath (it's ring-0, so even when scheduling turns on later the ring-3 guard protects it — map confirms).

**Per-CPU state:** `cpus[1]` tick counter; per-CPU LAPIC timer is inherently local.

**Locking/IPI/TLB/timer:** LAPIC timer armed on CPU1 only. EOI is CPU-local. PIC untouched (BSP keeps IRQ0). `lapic_send_ipi` ICR writes will need a lock later (Brick G) but none sent yet.

**Checkpoint test:** CPU1 prints `[SMP] AP tick %llu` every ~100 ticks from `lapic_tick`. Counter climbs steadily and the BSP desktop keeps running — proves CPU1 takes interrupts, EOIs the LAPIC correctly, and **does not double-EOI the PIC** (which would wedge BSP's IRQ0 → frozen desktop). If the desktop freezes, the EOI target is wrong; fix before proceeding. `worktest` still green.

**Rollback:** Drop `SMP_SCHED` (removes `sti` + timer init) → CPU1 back to masked coprocessor.

---

## BRICK F — THE RISKIEST STEP: first AP context-switch into a process

**This is the single riskiest checkpoint in the entire plan.** Everything before this was reversible plumbing that could not fault because CPU1 never entered ring 3. Here CPU1 loads a user CR3, sets TSS.RSP0, and `iretq`s to CPL 3 for the first time. A wrong RSP0 (Brick B), wrong `kernel_rsp_save` (Brick C), wrong CR3, or a clobbered `current` triple-faults CPU1.

**How to de-risk it (do all of these):**

1. **One pinned, hand-picked, non-sleeping, non-forking test process** — not the real apps. Add a kernel-spawned ring-3 loop (`for(;;){ getpid(); yield(); }`) created at boot, marked `affinity=1`. CPU1 runs exactly this and nothing else. This bounds the blast radius: the BSP desktop is untouched on `cpus[0]`.
2. **Enable the dispatch call in `lapic_tick` behind a second gate** (`SMP_SCHED_DISPATCH`) so Brick E's proven timer can be re-disabled instantly without rebuilding the world.
3. **Reuse the proven path, change nothing in it.** `lapic_tick` now calls the *existing* `schedule_from_irq(frame)`. Because `cpu_id()` is real, `this_cpu()` is `cpus[1]`, `scheduler_pick_next` picks from CPU1's runqueue, and the first-dispatch frame-synthesis branch (scheduler.c:1316) handles the never-run process. The hard ring-3 guard (scheduler.c:1249) protects CPU1's own coprocessor loop and the kernel. **Zero edits to schedule_from_irq** — that's the whole point of the prior bricks.
4. **CR3 sanity assert** in the AP dispatch path: before the switch, assert `next->context.cr3 != 0` and that bit-flags are sane (hazard #4). Cheap, one compare, catches the worst case.
5. **Watchdog:** the existing `health_monitor`/`cpu_hb[1]` heartbeat (ap_boot.c) must keep bumping from the test process's path; if CPU1 stops heartbeating, the BSP logs and you have a precise failure point.
6. **Keep the global `scheduler_lock`** (still IRQ-safe). Contention is irrelevant with one CPU1 process.

**Files / functions:** `lapic_tick` → call `schedule_from_irq`; add the pinned test process spawn (e.g. in kernel.c init under the gate); add `affinity` field to `process_t` and make `scheduler_add_process` route `affinity==1` to `cpus[1].rq_*` (small, the map's stub at scheduler_smp is not used; do it in the live scheduler.c `scheduler_add_process`).

**Per-CPU state:** `process_t.affinity` (or `last_cpu`); CPU1's `current_thread` now genuinely names a ring-3 process.

**Locking/IPI/TLB/timer:** global `scheduler_lock` (IRQ-safe). Still **no cross-CPU TLB shootdown** — safe because the test process is pinned to CPU1 and the BSP never mutates its page tables (no shared AS, no munmap of it).

**Checkpoint test (the money shot):** Serial shows the pinned process's `getpid` syscalls being serviced **with `cpu_id()==1`** (log the CPU in the syscall path), CPU1 tick counter climbing, `cpu_hb[1]` climbing, **and the BSP desktop/compositor still responsive on `cpus[0]`**. Run for 60+ seconds: no triple fault, no #DF, no freeze. This proves a real user process runs on CPU1 in parallel with CPU0.

**Rollback:** Drop `SMP_SCHED_DISPATCH` (keeps Brick E's harmless timer) or drop `SMP_SCHED` entirely. Because the test process is pinned and isolated, a regression cannot corrupt the BSP's runqueue or apps.

---

## BRICK G — Cross-CPU TLB shootdown + IPI reschedule (correctness for real shared/unmapped pages)

**Why before migrating real apps:** Real apps exit, fork, and have pages unmapped/reclaimed. The moment CPU1 runs a process whose page tables the BSP can modify (or whose pages the PMM reclaims), stale TLB entries on CPU1 read freed memory (hazard #7 across maps; the higher-half alias class of bug from your memory). This must be correct before the desktop apps move.

**Files / functions:**
- `kernel/arch/x86_64/ipi.c`: implement `ipi_handle_reschedule` (today a stub, ipi.c:345) to just set `need_resched` on this CPU (do **not** call `schedule()` directly from the IPI handler — flag and let the timer/return path switch). Add a real `IPI_TLB_FLUSH` handler that does local `invlpg`/CR3 reload + ack.
- TLB flush path (`tlb.c`): when CPU0 unmaps a page, if `cpus[1].current_cr3` matches the affected AS, send `IPI_TLB_FLUSH` to CPU1 and wait for ack. Add `volatile uint64_t current_cr3` to per-CPU state, written at context-switch time (hazard #6: must be `volatile`/atomic since read cross-CPU).
- **Lock `lapic_send_ipi` ICR writes** with `ipi_send_lock` (hazard #11: concurrent ICR writes lose IPIs).
- **Fix the IPI call-arg lifetime**: the branch has two versions (ipi.c stores a *pointer* to a caller-stack `ipi_call_t`; ipi_fixed.c stores by *value*). **Adopt the by-value `ipi_fixed.c` semantics** (hazard #3 in primitives map) and delete/retire the pointer version to remove the use-after-free.

**Per-CPU state:** `current_cr3` (volatile) per CPU; IPI ack counters.

**Locking/IPI/TLB/timer:** `ipi_send_lock`; TLB-flush IPI with bounded-spin ack-wait (never hold a lock during the wait — hazard #4). No nested lock with `scheduler_lock`.

**Checkpoint test:** A test where the BSP unmaps a page that CPU1's process has cached, then CPU1 reads it: must fault/get-new-value, not read stale. Add a deliberate `[SMP] TLB shootdown CPU0->CPU1 ack` log and confirm the round trip. `ipi_reschedule(1)` from the BSP must make an idle CPU1 pick up a newly-readied affine process within one tick. Smoke green.

**Rollback:** Drop `SMP_SCHED`. (Keep this brick's IPI fixes even in the coprocessor build if they're strict improvements — they're guarded but harmless.)

---

## BRICK H — Per-CPU runqueue locks + sleep-list spinlock (remove the global bottleneck)

**Why now, not earlier:** With two CPUs scheduling under one global `scheduler_lock`, every switch on either CPU blocks the other — this *caps* the FPS win. Also the `g_sleep_list` `cli/restore` guard is unsafe once both CPUs wake sleepers (hazard #3/RACE-005). Split locks only after correctness is proven, so a lock-ordering bug is isolated to this brick.

**Files / functions:**
- `scheduler.c`: move `scheduler_lock` into `cpu_t` as `rq_lock`; `scheduler_add_process/remove/pick_next` take `this_cpu()->rq_lock`. For cross-CPU enqueue (BSP readies an affine-to-CPU1 process), **lock lower cpu_id first** (deadlock order, matches the migrate ordering the map documents).
- Add global `sleep_list_lock`; replace `save_flags_cli/restore` in `sleep_list_push/remove/wake_due` with it. Pass `cpu_id` to `sleep_list_wake_due` so a woken sleeper is re-queued to the **CPU whose timer fired**, not hardcoded CPU0.
- `ready_count` → per-CPU.

**Per-CPU state:** `cpu_t.rq_lock`, per-CPU `ready_count`; global `sleep_list_lock`.

**Locking/IPI/TLB/timer:** per-CPU `rq_lock` (IRQ-safe); global `sleep_list_lock`; documented order rq_lock(lower-cpu-first) → never nested under sleep_list_lock.

**Checkpoint test:** Two pinned CPU-bound processes (one per CPU) run concurrently; lock-contention counter (add one) shows near-zero cross-CPU blocking vs the global-lock baseline. Sleepers on both CPUs wake correctly. No deadlock over a 5-minute soak.

**Rollback:** Drop `SMP_SCHED` → single global lock + cli/restore sleep list restored.

---

## BRICK I — Migrate the desktop: compositor pinned to CPU0, the 6-7 apps to CPU1 (the FPS milestone)

**Files / functions:**
- Affinity placement: in `process_create`/`exec`, the **compositor** (and init/UI) get `affinity=0`; the worker apps get `affinity=1`. Simplest: a small allow-list by name, or "fork's child inherits a balancing affinity" (round-robin onto CPU1 for non-UI procs).
- `scheduler_add_process` already routes by `affinity` (Brick F). The desktop ISO build must link `kernel-smp.elf` + `SMP_SCHED`/`SMP_SCHED_DISPATCH`, and `run-qemu.sh` pass `-smp 2`.

**Checkpoint test:** Boot the desktop on `-smp 2`. Per-CPU tick/ready_count logs show the compositor's ticks on CPU0 and app ticks on CPU1. Measure compositor frame interval before/after (you already de-prioritized it to ~105 ms round-robin among 7 procs on one core). With the apps off CPU0, the compositor should schedule far more often → measurably lower frame interval / higher FPS. That delta **is** the milestone.

**Rollback:** Drop `SMP_SCHED` → single-core desktop.

---

## The minimal first milestone that measurably helps FPS

You do **not** need work-stealing, load-balancing, per-CPU sleep lists, affinity syscalls, or removing the `cpu1_job` slot to move the FPS needle. The minimum is **Bricks A→F→I with the global lock kept**:

- A (real `cpu_id`), B (per-CPU TSS/IST), C (per-CPU `kernel_rsp_save`), D (CPU1 idle+runqueue), E (CPU1 LAPIC tick), F (first AP ring-3 switch, proven on one pinned process), G (TLB shootdown — required once real apps fork/exit), then I (compositor→CPU0, apps→CPU1) under the **still-global `scheduler_lock`**.

The compositor stops sharing its core with 6-7 apps; even with global-lock contention, going from "compositor scheduled ~1-in-7 on one core" to "compositor owns CPU0" is the dominant FPS term. **Brick H (lock split) is a scalability follow-up, not a prerequisite for the visible win.**

---

## Lock ordering (final, across all bricks)

`rq_lock(lower cpu_id first)` → `process_table_lock` → `sleep_list_lock` → `cpu1_job_lock` → `ipi_send_lock`. TLB-shootdown ack-waits hold **no** lock. IPI handlers only set `need_resched`; they never call `schedule()` re-entrantly. Per-CPU TSS/`kernel_rsp_save`/`current_cr3`/`time_slice` need **no lock** (single-writer = the owning CPU) — only `current_cr3` needs `volatile` because it is read cross-CPU for shootdown targeting.

## Files you will touch (authoritative list)

`kernel/stubs.c` (gate the cpu_id stub), `kernel/arch/x86_64/ap_boot.c` (real cpu_id, AP TSS load, lapic_timer_init+sti, dispatch), `kernel/arch/x86_64/gdt.c` (per-CPU TSS/IST/2nd descriptor), `kernel/arch/x86_64/syscall.asm` (per-CPU kernel_rsp_save), `kernel/core/sched/context.c:131-135` (per-CPU TSS/RSP writes), `kernel/core/sched/scheduler.c` (per-CPU idle/runqueue init, affinity routing, rq_lock split, sleep_list_lock), `kernel/core/sched/process.c` (current_process_on_cpu accessor, affinity field), `kernel/arch/x86_64/lapic.c` (timer vector ≠32, lapic_tick ISR, LAPIC EOI), `kernel/arch/x86_64/ipi.c` (reschedule handler, TLB-flush handler, ICR lock, by-value call args), `kernel/arch/x86_64/tlb.c` (cross-CPU shootdown), `scripts/quick_build.sh` (SMP_SCHED / SMP_SCHED_DISPATCH sub-gates), `scripts/run-qemu.sh` (-smp 2 for SMP build), and `kernel/arch/x86_64/ap_trampoline.asm` **only if** you choose the asm `ltr` path (the C-path in ap_main avoids touching it — recommended).