# brick record: SMP-G0 (IPI-LINK)

> The smallest possible proof that the BSP can interrupt CPU1 and CPU1 can
> handle it safely. SMP_IPI now means LINKED + INITIALIZED + PROVEN (the
> user-set design rule), making panic.c's `SMP_FOUNDATION && SMP_IPI` gate
> actually satisfiable. Both of the brick spec's predicted dangers were real
> and are now explicit checks, not assumptions.

```yaml
brick: SMP-G0
status: complete
branch: brick/smp-g0-ipi-link
base: brick/smp-f3-5 (632a522, the frozen F3-5 head)
commits: [ea45305]
acceptance: "IPILINK: PASS ipi_resched=1 cpu1_count=1"
proof: scripts/ipilink_smoke.sh (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1, qemu -smp 2)
```

## what landed

- **Build gate `SMP_IPI=1`** (requires SMP=1): compiles the dormant
  `kernel/arch/x86_64/ipi.c` + `ipi_handlers.asm`, defines `-DSMP_IPI`.
  `ipi_fixed.c` is an older duplicate-symbol variant -- NEVER add it alongside.
- **`ipi_init()`** runs after `lapic_init()`, BEFORE `try_start_cpu1()` (the
  IDT is shared -- every IPI gate exists before CPU1 is alive enough to take
  one). Claims vectors 0x50-0x55, captures the BSP APIC id.
- **The G0 selftest** (after the Brick F2 checkpoint, BSP serial-safe window):
  ONE `IPI_RESCHEDULE` to CPU1 -> CPU1's handler counts it + LAPIC EOI + iretq
  (NO scheduling action -- the handler's `schedule()` stays TODO; that is G1)
  -> BSP bounded-polls (~100 ms TSC) -> the acceptance line.

## the two predicted dangers (both real)

1. **Vector collision -- CONFIRMED.** `IPI_RESCHEDULE=0x40` sat dead on
   `AP_LAPIC_TIMER_VECTOR=0x40` (Brick E: CPU1's LAPIC timer gate at
   IDT[0x40]). An unmodified `ipi_init()` would have silently overwritten the
   gate and killed CPU1's tick. FIX: the IPI block moved to **0x50-0x56**
   (priority class 5 > the timer's class 4), with the landscape made explicit:
   `_Static_assert`s in ipi.c (exceptions 0x00-0x1F / PIC 0x20-0x2F / AP timer
   0x40 / spurious 0xFF) + a runtime `idt_gate_present()` free-check before
   ANY gate is claimed (occupied -> loud serial FATAL, subsystem stays
   disarmed, every sender no-ops on `ipi_ready==0`).
2. **Shadow-zone globals -- BOUNDED + a stale-doc find.** IPI state is touched
   from handlers that run under ARBITRARY CR3, so it must sit below the user
   link base. Arrays sized `IPI_MAX_CPUS=8` (NOT smp.h's MAX_CPUS=256 -- that
   was ~120 KB of packed .bss for a 2-CPU machine); the smoke hard-gates every
   ipi.c global `< 0x200000` via nm (landed at 0x19bd60-0x19cbc0). EN-ROUTE
   FIND: the LIVE user link base is **0x800000** (`userspace.ld`); linker.ld's
   "0x200000 user-ELF load base" comment is STALE (predates the move), so the
   packed .bss ending at 0x25d000 (SMP) / 0x236000 (default) is NOT the hazard
   it appears. Gate kept at the stricter historical bound anyway.

## the third trap (found en route, not predicted)

`ipi.c` was salvage written against smp.c's model -- and smp.c is NOT in the
build. The killer: `percpu_data[]` LINKS anyway (ap_boot.c defines a minimal
array for the health monitor) but its `.apic_id` is NEVER FILLED, so the old
`cpu_to_apic_id(1)` returned 0 = **the BSP's own APIC id**: every "CPU1" IPI
would have gone to the sender itself. Wrong-target, not link-fail -- invisible
to the strict linker, fatal to the proof. FIX: resolve through the live
ap_boot.c seam -- `smp_cpu1_apic_id()` (MADT capture in try_start_cpu1;
0xFFFFFFFF sentinel -> the IPI is DROPPED, never sent to a garbage id) +
`cpu1_is_online()`. tlb_uni.c already provides the TLB handler symbols, so no
TLB code was pulled in.

## byte-identity (laws 2/8)

Every non-SMP_IPI build is BYTE-IDENTICAL to HEAD: default kernel.elf
`6f99ed9f` and F3-5 kernel-smp.elf `ad072cc6` hash-equal built from the frozen
base and from this tree (the idt.c/ap_boot.c/kernel.c edits are all `#ifdef
SMP_IPI`; idt_gate_present itself is gated for this reason).

## boundary held (user-set hard no's)

No wake scheduling · no TLB shootdown sends · no panic-stop behavior change
beyond gated availability (kernel_panic's `ipi_stop_all_cpus` simply becomes
linkable+armed, exactly the SMP-R0 design) · no desktop split · no global
PREEMPT · no work stealing.

## next

SMP-G1 IPI-WAKE: the reschedule handler actually wakes CPU1's scheduler
(kills the tick-poll wake latency). The handler seam is already in place.
