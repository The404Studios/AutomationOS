# brick record: SMP-G1 (IPI-WAKE)

> The first brick that makes CPU1 responsiveness FASTER instead of merely
> proven (the user's framing). CPU1 wakes from IPI instead of waiting for its
> 100 Hz tick: worst-case wake went from the 10,000 us tick floor to 143 us
> measured, and a real enqueue dispatches in 280 us end-to-end.

```yaml
brick: SMP-G1
status: complete
branch: brick/smp-g1-ipi-wake
base: brick/smp-g0-ipi-link (9f700c6, the frozen G0 head)
commits: [e30d2fd]
acceptance: "IPIWAKE: PASS enqueue_to_cpu1=1 first_instruction_lt_1ms=1 no_lost_wake=1"
proof: scripts/ipiwake_smoke.sh (same SMP_IPI build as G0, qemu -smp 2)
measured: ping acks=32/32 max=143us; cpu1hello enqueue->dispatch=280us; tick floor was 10000us
```

## the wake path (all `#ifdef SMP_IPI`)

1. **Handler** (`ipi_handle_reschedule`): sets `ipi_need_resched[cpu]` -- its
   ONLY scheduling action. No `schedule()` from interrupt context; dispatch
   stays in `ap_cooperative_schedule` (the no-broad-rewrite hard no).
2. **Enqueue kick** (`scheduler_add_process_to_cpu`): a REAL foreign enqueue
   sends `IPI_RESCHEDULE` to the target -- AFTER the rq_lock drops, never
   while holding a lock. Idempotent re-adds don't spam the ICR; a kick to a
   busy CPU is consumed harmlessly.
3. **The lost-wakeup close** (`ap_scheduler_loop`):
   ```
   cli                      ; any in-flight IPI is now held PENDING
   check rq / need_resched  ; the handler cannot interleave here
   sti; hlt                 ; a pending IPI is delivered at the hlt boundary
                            ; (law 12's sti shadow) and WAKES it
   ```
   Without this, an IPI landing between the empty pick and the hlt was
   ABSORBED (handler runs, flag set, iretq resumes... straight into hlt) and
   the runnable task waited up to 10 ms for the tick. `ready_count` is read
   without the rq_lock: a stale miss is covered by the enqueuer's IPI pending
   at the hlt; a stale hit re-loops once. Never blocks.

## proof design (why it can't false-PASS)

- **no_lost_wake pings** run in the ONE window where CPU1 is genuinely
  hlt-parked on an EMPTY runqueue: after `scheduler_init_secondary_cpu`,
  BEFORE `ap_spawn_test_kthread` (any later and the F2 kthread keeps CPU1
  busy). Ticks CANNOT ack a ping -- only the IPI handler sets need_resched --
  so each of the 32 acks proves an IPI woke the hlt. Sub-1 ms latency is
  ~10x faster than even a best-case tick rescue.
- **enqueue_to_cpu1 / first_instruction**: kernel.c stamps TSC at the REAL
  cpu1hello `scheduler_add_process_to_cpu` (which now sends the kick); CPU1
  stamps TSC at its first switch into that pid (the task's first instruction
  follows within the context_switch_asm tail). 280 us end-to-end through the
  live path: IPI -> kthread's next yield -> priority pick -> switch.
- The composite IPIWAKE line is assembled by the smoke from the two serial
  measurements + the full G0/F3-5 regression ladder (the F3-5 convention).

## the gate worked on its author

The G1 proof globals (`g_g1_*`) first lived in scheduler.c and landed at
0x23c000 -- the smoke's nm gate FAILED the build. Correctly: they are read
from CPU1 contexts that can hold a USER CR3 (the AP dying path runs
`ap_cooperative_schedule` under the dead task's address space), so law 15
applies. Moved to ipi.c's low packed .bss (0x19bf60-0x19bfa0). A brick's own
hard gate catching the brick's own code is exactly what the gates are for.

## byte-identity (laws 2/8)

Default `6f99ed9f` + F3-5 SMP `ad072cc6` hash-equal with all G1 code in tree
-- every change is `#ifdef SMP_IPI`, including the `enqueued` local in
scheduler_add_process_to_cpu (gated specifically so the -O0 build emits no
new stores in non-IPI configs).

## boundary held (user-set hard no's)

No TLB shootdown · no desktop split · no global PREEMPT · no work stealing ·
no broad scheduler rewrite (the loop diff is the cli/check/sti;hlt pattern +
proof plumbing; ap_cooperative_schedule gained one one-shot TSC stamp).

## next

G2/G3 (TLB shootdown family -- the stash-mined ipi_tlb_flush_page_handler
asm), or wire `scheduler_add_process_home`'s wake path benefits wider once a
second ring-3 consumer exists. The wake fabric is now: enqueue -> kick ->
flag -> race-free hlt -- generic for any future CPU1 workload.
