# brick record: SMP-F3-7 (BATCH-CLASS)

> The first brick where CPU1 becomes useful for ORDINARY workload classes
> (the user's framing) -- an unpinned, un-special-cased BATCH task routed to
> CPU1 by the seam's new layer-3 branch, IPI-kicked, BKL-protected, exited
> and reaped like any process. Took FOUR iterations to land honestly; all
> three failures were root-caused, none papered over.

```yaml
brick: SMP-F3-7
status: complete
branch: brick/smp-f3-7-batchclass
base: brick/smp-h1-bkl-lite (17d8c67, the frozen H1 head)
commits: [4f859c0]
acceptance: "BATCHCLASS: PASS batch_cpu1=1 normal_cpu0=1 pinned_rt_cpu1=1 illegal_clamped=1 ipi_wake=1 bkl_safe=1"
proof: scripts/batchclass_smoke.sh (THE FULL STACK: +SMP_IPI +SMP_BKL +SMP_BATCH, qemu -smp 2)
measured: batchdemo enqueue->dispatch 2000us (tick floor 10000us); reads=48 errors=0; reaped by init
```

## the one branch (the policy doc's promise, due since F3-3a)

`scheduler_choose_cpu` layer 3, `#ifdef SMP_BATCH`:
BATCH-class + legal-CPU1 -> the PINNED_WORKER core (law 5: batch fills
idle). PLACEMENT only -- decides where a task is ENQUEUED; nothing queued or
running ever moves. NORMAL falls through home; PINNED_RT resolved at layer 2;
bounded on every side by layer-1 legality, the funnel re-assert, and the
F3-2 enqueue gate. No load measurement (pressure counters are F3-9): BATCH
simply prefers the worker core whenever legal. PROFILE-0's batch-routes-home
selftest expectation flips WITH the gate; the printed CORE line stays
identical so every frozen smoke keeps grepping true.

## the live proof

batchdemo: class=BATCH, mask CPU0|CPU1, pinned_cpu=CPU_NONE, parented to
init -- the call site does NOT choose a CPU; `scheduler_submit_task` does.
Serial: "the seam chose cpu1" -> dispatch 2000us after enqueue -> 3 marks +
48 FS reads (BKL-marked syscalls, on CPU1, [BKL] engaged, 0 warnings) ->
exit 7 -> init reap. The 2 ms is legitimate cooperative hand-off (CPU1 was
time-sharing cpu1hello at that moment); the rigorous IDLE IPI-wake number
remains G1's ping ladder (32/32 < 1 ms), re-verified in the same boot.

## the three en-route finds (the brick's real teaching)

1. **SPAWN ORDER (run 1):** batchdemo spawned AFTER the 60 s CPU1 bklstorm
   -- the "wake latency" measured 578 ms of STORM-SHARING, then the demo
   starved past the QEMU window. Moved before the storms (near-idle CPU1),
   documented at the call site. Lesson: a latency proof is only as good as
   the idleness assumption behind it.
2. **THRESHOLD HONESTY (run 2):** 2 ms against a busy CPU1 is not an
   idle-wake failure; the smoke's bound is now the 10 ms tick floor with the
   reasoning written into the script, and the sub-ms idle proof explicitly
   delegated to the G1 ping gate in the same boot.
3. **CROSS-CPU SERIAL SHREDDING (run 3, the keeper):** with CPU1 printing at
   storm volume, per-byte UART interleave shredded whole BSP lines
   ("[INIT] Process 16 exited with status [SYSCALL] sys_stat...") --
   destroying the batchdemo reap evidence NON-DETERMINISTICALLY (passed run
   2, failed run 3). FIX: a bounded best-effort serial LINE lock around
   kprintf's single batched serial_write (printf.c, gated SMP_BATCH): whole
   lines are now atomic across CPUs; a same-CPU IRQ re-entry degrades to one
   unserialized line, never a hang. Every acceptance line in every future
   SMP boot benefits.

## ⚠ THE AUDIT GAP (surfaced, not hidden -- read before DESKTOP-SPLIT)

batchdemo is the FIRST process with a multi-CPU allowed_cpus mask -- exactly
what the G2 `tlb_pinning_audit` exists to flag. The audit stayed green only
because it runs in the pre-spawn F2 window; a post-spawn run would FAIL on
batchdemo. The REAL invariant ("a user address space is only ever LOADED on
one CPU") still holds: placement is enqueue-once, re-wakes deterministically
re-choose CPU1 (BATCH+legal-CPU1 has one answer), and no migration primitive
exists. But the audit's mask-popcount HEURISTIC no longer matches reality.
**Recommended next checkpoint (before or with DESKTOP-SPLIT): evolve the
audit to track the true invariant -- p->ran_on_cpus |= (1<<cpu) at dispatch,
audit popcount(ran_on_cpus) <= 1 -- so the forcing function fires on actual
cross-CPU address-space use, not on declared masks.** Until then the mask
audit + this record are the guard.

## byte-identity

Default `6f99ed9f` hash-equal. The BKL profile contains zero F3-7 code
(every change `#ifdef SMP_BATCH`; BKL-profile baseline recorded `1478b7c7`).
batchdemo ships in every initrd (inert without the gated spawn).

## boundary held (user-set hard no's)

No work stealing · no desktop split · no unpinned ARBITRARY migration (the
branch is a deterministic enqueue-time placement) · no general per-mm
shootdown · no global PREEMPT.

## next

DESKTOP-SPLIT (the user's stated ladder) -- with the ran_on_cpus audit
evolution as its opening safety checkpoint, per the gap above.
