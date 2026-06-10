# brick record: SMP-G2 (TLBSHOOT-MIN)

> The rung that makes shared kernel mappings safe before the desktop leans on
> both CPUs (the user's framing). Minimal-correct: a bounded, ack-counted
> KERNEL-range shootdown + the loud pin/no-migration model that makes user
> cross-flush unnecessary BY CONSTRUCTION -- with a runtime audit as the
> forcing function the day that construction changes.

```yaml
brick: SMP-G2
status: complete
branch: brick/smp-g2-tlbshoot-min
base: brick/smp-g1-ipi-wake (ab81eff, the frozen G1 head)
commits: [9f1f621]
acceptance: |
  TLBSHOOT: PASS kernel_flush=1 acked=1 bounded=1 invariant=1 (latency_us=382)
  TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1 (procs_checked=3 multi_cpu_masks=0)
proof: scripts/tlbshoot_smoke.sh (same SMP_IPI build, qemu -smp 2)
```

## the G2 model: user=local, kernel=cross

THE PIN/NO-MIGRATION ASSUMPTION (the loud block lives in ipi.c): every task
runs on exactly ONE cpu, forever -- F3-2 ctor defaults are CPU0-only, CPU1
residents are explicitly pinned, fork inherits nothing, no migration
primitive exists. THEREFORE a user address space is only ever loaded on its
task's one CPU and user-mapping changes need ONLY a local invlpg. KERNEL/
global mappings are the opposite -- shared into every CR3, cached by every
core -- so they get `ipi_tlb_flush_kernel_range()`:

- local invlpg loop (full CR3-reload fallback for 0 or > TLBSHOOT_MAX_PAGES=64)
- `IPI_TLB_FLUSH_PAGE` (vector 0x57 -- the stash-mined SMP-R0 harvest,
  renumbered from its pre-G0 0x47 into the checked block) to CPU1; the remote
  handler reads the request block from ipi.c's low .bss (law 15: it runs
  under arbitrary CR3 -- safe because kernel ranges exist in every address
  space) and invlpg's precisely
- TSC-bounded 50 ms ack wait; single in-flight request serialized by the
  dedicated `tlbshoot_lock`

`tlb_pinning_audit` (inside the selftest) walks every live process via
process_get_by_pid: ANY multi-CPU affinity mask fails TLBSHOOT_NEG loudly --
the gate that forces real per-mm shootdown work BEFORE the assumption rots.

## law 16 at the wait

- `IF==1` asserted at entry (an ack wait with IF=0 can deadlock behind its
  own IPI). Necessary-not-sufficient: plain spin_lock does NOT cli in this
  kernel, so never-hold-rq/heap/scheduler/fs-locks remains call-site review.
- The bounded timeout converts any deadlock into a loud `[TLB_INVARIANT]`
  violation instead of a hang.
- TLB_INVARIANT validator = LOG+COUNT, never panic (the F3-0 discipline).
  Violation classes: IF=0 entry, ack timeout, ack overrun.

## en-route find: a FALSE-ACK hole closed

The G0-registered `IPI_TLB_FLUSH` (0x51) handler called tlb_uni.c's
`tlb_handle_ipi_flush` -- which is a single-CPU **NO-OP** ("no IPIs on a
single CPU"). A remote CPU would have **ACKED WITHOUT FLUSHING**: the sender
believes the remote TLB is clean, the stale entry lives on -- the worst
possible shootdown failure, invisible until a corruption months later. Both
0x51/0x55 handlers now do a real local full flush (precision is 0x57's job).
Also fixed: `ipi_tlb_flush_all`'s ack wait was UNBOUNDED -- now TSC-bounded
per law 16.

## also surfaced

`__builtin_popcountll` emits a libgcc `__popcountdi2` call in this
freestanding -O0 build (not linked) -- manual bit-clear loop instead. Same
family as the fs:0x28 toolchain reflexes: freestanding means BUILTINS may
still be library calls.

## byte-identity (laws 2/8)

Default `6f99ed9f` + F3-5 SMP `ad072cc6` hash-equal -- all changes are
`#ifdef SMP_IPI` or live in SMP_IPI-only sources. scheduler.c untouched this
brick.

## boundary held (user-set hard no's)

No general per-mm shootdown · no unpinned migration · no fork-on-CPU1 · no
desktop split · no global PREEMPT · no scheduler rewrite · no blocking under
rq/heap/scheduler/fs locks.

## next

G3 candidates: precision per-mm shootdown (only if/when the pin audit's
forcing function fires), PCID-aware remote flush, or back up the ladder to
the scheduler policy rungs (choose_cpu seam) now that kernel mappings are
shootdown-safe.
