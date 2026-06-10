# brick record: SMP-F3-5 (cpu1hello)

> The first ring-3 process on CPU1, and — the part the user called the real
> danger — its EXIT: per-CPU current, cross-CPU parent wake, CR3 leaving the
> dying address space, and teardown that cannot free under a stale CPU1 CR3.
> "That is where the old corruption class lives."

```yaml
brick: SMP-F3-5
status: complete (commit ca1378a on brick/smp-r0, branch-named brick/smp-f3-5;
        awaiting review/push)
base: SMP-R0 freeze (7be017d)
acceptance: "CPU1HELLO: PASS markers=5 exit=42 reaped=1 cpu1_idle=1 desktop_alive=1
            + F2 VERIFY delta>0 + APCURRENT: PASS + 0 [SCHED_INVARIANT] + 0 panic
            (scripts/cpu1_smoke.sh, qemu -smp 2) -- the exact user-set string, hit."
hard_boundary_held: no IPI, no desktop split, no work stealing, no global
        PREEMPT, no scheduler policy expansion. BLOCKED-on-CPU1 explicitly out
        of scope (cpu1hello never blocks; wo_park on the AP is a later brick).
checkpoints:
  - F3-5a/b: cpu1hello ELF (userspace/tests, 968 B, shipped always, inert
      without the gate) loads via elf_load_and_exec while the BSP scheduler is
      not yet running (the CPU0 enqueue is inert), then dequeue ->
      parent=init (pid+create_seq identity) -> pin -> enqueue on cpus[1].
  - F3-5c: trampoline iretq to ring 3 ON CPU1 (serial: "[TRAMPOLINE] PID 5
      entering usermode"); context_switch_asm loads the user CR3 from the
      fresh context; tss_set_kernel_stack already pointed CPU1's TSS at the
      task's kernel stack.
  - F3-5d: 5 "CPU1HELLO mark" writes + yields, then exit(42) -- CPU1 syscalls
      route through the per-CPU LSTAR; schedule() and sys_yield gained
      cpu_id()!=0 routing to ap_cooperative_schedule (both BSP bodies write
      the GLOBAL current -- the corruption F3-4 exists to prevent).
  - F3-5e: init reaped both zombies ("[INIT] Process 5 exited with status 42",
      "[INIT] Process 4 exited") -- the parent wake is HOME-routed
      (scheduler_add_process_home in the waitqueue wake sites: the woken task
      goes to ITS cpu, never the waker's).
  - F3-5f: the dying path -- a TERMINATED current is NEVER resumed; its
      RUNNING ref rides cpu->pending_unref to the SUCCESSOR and drops on the
      next pass (schedule_tail-style). That ref outliving the switch IS the
      CR3/stack protection: CPU0's reaper can run concurrently, and teardown
      ("[PAGING] Destroyed address space") only happens after the drop. CR3
      leaves the dying space AT the switch (context_switch_asm restores the
      successor's). The F2 kthread retires through the SAME path, so the
      machinery is proven twice (ring-3 and ring-0 deaths).
  - F3-5g: first CPU1 kmalloc/kfree checkpoint green (one-shot, in the dying
      path -- heap_lock proven from a real CPU1 teardown context).
en_route_finds:  # each with a failing run first
  - "ORDERING RACE: the spawn originally sat in the F2 block, BEFORE the BSP's
     syscall_init -- CPU1 dispatched cpu1hello within a tick and every syscall
     no-op'd against the unregistered dispatch table: writes vanished,
     sys_exit RETURNED to userspace, crt0's hlt guard took a CPL3 #GP. The
     exception handler terminated it, the BSP survived, the desktop lived,
     and the dying path cleaned up -- the user's acceptable-failure ladder
     proven by accident. Fix: enqueue only after syscall table + MSR init."
  - "HARNESS REGEX: the PID extraction matched the '1' in 'cpu1hello'
     (bare [0-9]+ over the whole line); isolate the 'PID <n>' token first."
  - "THE LATENT BUG (the brick's best find): ap_scheduler_loop used a BARE
     hlt, inheriting IF from idle's RESTORED rflags. If any lock section had
     IF clear at idle's original switch-out, the restored IF=0 turned hlt
     into a PERMANENT park -- observed as a FLAKY never-wakes idle after the
     dying handoff (pending ref never dropped, PCB never freed; one run
     worked, the next didn't). Invisible for the entire F2 era because the
     forever-looping kthread meant CPU1 never executed hlt after first
     dispatch. Fix: the canonical `sti; hlt` idle idiom (same as the BSP
     idle thread). LAW-WORTHY: an idle loop's hlt must never rely on
     inherited IF."
review:
  default_build_changed: false   # all routing gated; scheduler_add_process_home
                                 # is behavior-identical on non-DISPATCH builds
  all_waits_bounded: true
  touches_kernel: true
  smoke_proves_claim: true       # the exact acceptance string from a live run
verdict: pass
done: >
  CPU1 ran, syscalled, and KILLED a real ring-3 process without touching
  CPU0's state: per-CPU current held, the parent wake crossed CPUs safely,
  CR3 left the dying space at the switch, and teardown waited for the
  successor's ref drop. Law 8's five audits are all green. The old corruption
  class is dead on this path.
next:
  - SMP-G0 IPI-LINK (compile ipi.c, define SMP_IPI, link-map < 0x200000,
    one IPI_RESCHEDULE round-trip) -> G1 IPI-WAKE (kills the worst-case
    tick-poll wake latency the sti;hlt loop now has).
  - parked from this brick: wo_park/BLOCKED on CPU1; the resume-marker
    narration can drop once G-series harnesses supersede it.
```
