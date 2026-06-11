# brick record: SMP-PROFILE-0

> Typed intent before anything moves (the user's framing): the scheduler now
> knows WHAT a task is (sched_class) and WHAT a CPU is for (cpu_role) as
> declared data, and every placement flows through ONE named funnel -- while
> behaving byte-for-byte like the classless F3-6 seam. BATCH exists; nothing
> migrates.

```yaml
brick: SMP-PROFILE-0
status: complete
branch: brick/smp-profile-0
base: brick/smp-f3-6-choosecpu (901f079, the frozen F3-6 head)
commits: [203d22d]
acceptance: "SMPPROFILE: PASS normal_home=1 batch_declared=1 pinned_rt_legal=1 submit_funnel=1 no_behavior_change=1"
proof: scripts/smpprofile_smoke.sh (same SMP_IPI dispatch profile, qemu -smp 2)
```

## what landed

- **`sched_class_t`** -- NORMAL=0 (the memset default IS the correct default,
  the F3-2 lesson made structural), BATCH=1 (**data only**: a multi-CPU-mask
  BATCH task still routes home CPU0 -- layer 3 stays a stub), PINNED_RT=2
  (names what pinned_cpu already enforced), INTERACTIVE=3 / RECOVERY=4
  RESERVED for laws 4/6 so the enum never reshuffles under them.
- **`cpu_role_t` + `scheduler_cpu_role()`** -- CPU0=GENERAL, CPU1=
  PINNED_WORKER. Static intent, the other half of layer 2's class x role for
  F3-7.
- **`sched_profile_t p->sched`** -- AT THE END of process_t (the F3-1
  new-field law), **GATED on SMP_SCHED && SMP_SCHED_DISPATCH** so every
  non-dispatch build keeps a byte-identical process_t layout (default
  kernel.elf hash-equal `6f99ed9f`). The loose F3-2 fields deliberately stay
  loose: relocating allowed_cpus/pinned_cpu into p->sched touches every
  ctor/validator/seam site -- its own future checkpoint, recorded not snuck.
- **`scheduler_submit_task()`** -- THE named funnel (the policy doc's
  pipeline): choose_cpu -> legality RE-ASSERT -> the proven enqueue sink.
  The re-assert cannot fire today; it exists so a future buggy layer-3
  balancer is caught loudly BEFORE the (also mandatory) F3-2 enqueue gate
  refuses it -- two independent legality walls. One-shot narration (first 4
  submissions; the wake path calls at frequency and must stay serial-quiet).
- **choose_cpu reads the profile** -- observation plus one real check: a task
  DECLARED PINNED_RT with no actual pin is a declaration bug, said loudly
  (still routes by mask -- no behavior change).
- **All three consumers** route through the funnel: home wakes
  (scheduler_add_process_home), the F2 kthread (now declared PINNED_RT), the
  cpu1hello placement (declared PINNED_RT; the F3-6 marker line kept verbatim
  so choosecpu_smoke.sh still greps true).

## the live evidence

The funnel's first-4 narration caught exactly the right picture on the
acceptance boot:

```
[SCHED] submit: pid=4  'ap_ktest'        class=2 -> cpu1
[SCHED] submit: pid=5  'cpu1hello'       class=2 -> cpu1
[SCHED] submit: pid=10 'sbin/forktest'   class=0 -> cpu0
[SCHED] submit: pid=22 'sbin/matmuljobs' class=0 -> cpu0
```

Two declared PINNED_RT workers to CPU1, two ordinary desktop wakes (memset-
default NORMAL) home to CPU0 -- typed intent visible on real traffic, zero
routing change.

## byte-identity

Default kernel.elf `6f99ed9f` hash-equal (the gated process_t field is the
load-bearing trick). Dispatch profile changes by design; its gate is the full
regression ladder (CHOOSECPU + F3-6 live marker + TLBSHOOT+NEG + IPIWAKE +
IPILINK + F2 + APCURRENT + CPU1HELLO + 0 invariant + 0 panic), all green.

## boundary held (user-set hard no's)

No actual migration · no work stealing · no desktop split · no per-mm
shootdown (G3 parked behind the G2 pin audit) · no global PREEMPT.

## next (the user's stated order at the F3-6 freeze)

SMP-H1 BKL-LITE → SMP-F3-7 BATCH-CLASS / controlled CPU1 placement →
DESKTOP-SPLIT. The pieces F3-7 needs are now all in place: the typed BATCH
class (data), the role table, the funnel with its legality re-assert, the G1
IPI kick, and the F3-3a two-lock discipline.
