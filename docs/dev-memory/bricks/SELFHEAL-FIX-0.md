# brick record: SELFHEAL-FIX-0

> "it works on the T410 ... but the self healing is not working." Seven diagnosis
> agents later: recovery WORKED end to end -- detection, overlay, kill, respawn,
> re-arm all sound -- but the respawned compositor came back with an EMPTY window
> table and no client re-register protocol existed. The windows ARE the desktop;
> recovery that loses them reads as "not working".

```yaml
brick: SELFHEAL-FIX-0
status: complete
branch: brick/selfheal-fix-0
base: brick/ide-xfile-0 (4766b1d)
request: >
  User on the T410 with the idewave2 ISO (T410_SAFE kernel + DESKTOP_MINIMAL=1
  SELFHEAL=1 userspace): "the self healing is not working" + "run 7 agents and
  fix the self heal".
diagnosis: >
  7 parallel read-only agents (ship plumbing / init respawn chain / cwatchdog
  detector / heartbeat publisher / freeze-class matrix / recovery action /
  proof harness + regression hunt). CLEAN: flag plumbing, initrd packaging,
  SYS_RECOVERY_OVERLAY (always registered, sti inside), sys_kill (immediate
  teardown in every state incl. BLOCKED via wait_object_abort), heartbeat
  placement (once per loop iteration incl. idle), init's respawn (PID tracked +
  updated; cwatchdog itself respawned), SHM ownership (init-owned, never
  tombstoned), watchdog re-arm (seeded=0). THE HOLE: compositor _start zeroes
  g_windows (compositor_m8.c:5221) and clients have no reconnect trigger -- the
  original selfheal_smoke.sh asserted serial markers only and never checked the
  desktop CONTENTS after recovery, so the empty-desktop outcome shipped proven.
  Known structural limits documented, not fixed here: a ring-3 tight-loop
  freeze is undetectable on the cooperative kernel (FREEZE_MODE=1 = SKIP by
  design; PREEMPT's ring-3 hard-spin preemption gap tracked separately); a
  kernel IF=0 spin kills everything incl. the watchdog (mitigated by the
  iteration-cap law); a non-compositor spinner starving the desktop gets the
  wrong remedy (kill the innocent compositor) -- DESKTOP_MINIMAL removes the
  storm that caused that class.
checkpoints:
  - id: H0
    title: window registry in the heartbeat page -- recovery restores the desktop
    commits: [2a071f3]
    result: >
      SELFHEAL v2 (selfheal.h): sh_winreg_ent_t[16] at offset 256 of the SAME
      4 KiB init-owned heartbeat page (heartbeat used 64 B). The compositor
      mirrors {win_id, client_pid, shm_id, buf_w/h, x/y, title} via
      selfheal_reg_sync after create/destroy and once per second on the clock
      pulse (16 plain stores; covers moves/snaps/titles without hooking every
      mutation site; PH_CLOSING windows skipped so recovery never resurrects
      one). A respawned instance (the was_init latch) runs
      selfheal_restore_windows before the frame loop: re-attach each entry's
      buffer by shm_id -- the CLIENT owns that segment, so a dead client's
      segment died with it and the failed shmat IS the liveness test -- with
      the same OOB-extent guard as handle_create, rebuilding windows under
      their ORIGINAL win_ids (client handles + queued commits stay valid;
      reply queues re-resolve lazily by pid; g_next_win_id bumped past the max
      restored id). Clients never participate.
  - id: H1
    title: slow-hardware recovery timeouts
    commits: [00354ef]
    result: >
      RESUME_TIMEOUT_MS 5000->10000, ATTACH_RETRIES 40->80 (~20 s),
      BREAKER_WINDOW 60s->90s. The single-core T410 can take >5 s to respawn
      the compositor into its frame loop; a too-tight window printed FAIL on
      succeeding recoveries and a tight breaker turned the second real freeze
      into stays-frozen-forever. Detection cadence unchanged.
  - id: harness
    title: the T410-profile proof the original smoke never had
    result: >
      build_test/selfheal_t410_check.sh: exact shipping profile (T410_SAFE=1
      SCHED_DEBUG=0 kernel + DESKTOP_MINIMAL=1 SELFHEAL=1 userspace) +
      FREEZE_TEST=1 FREEZE_MODE=0 blocking freeze + the full serial marker
      chain + ">=2 SELFHEAL: restored win=" assertions + a post-recovery QMP
      screendump. FAILING-THEN-PASSING: baseline (fix stashed) = recovery
      chain complete but restored_windows=0, screenshot = bare desktop, empty
      taskbar (shfix_base_recovered.png -- the user's exact T410 symptom);
      with the fix = "SELFHEAL-FIX: PASS recovery=1 restored_windows=4
      no_storm=1 no_fault=1", screenshot shows terminal/filemanager/netman/
      browser back with taskbar buttons and sane z-order
      (shfix_recovered.png). Plus a 100 s shipping-build (no freeze hook)
      no-false-trip soak; that build IS automationos-t410-selfheal.iso.
review:
  default_build_changed: false      # everything behind -DSELFHEAL; default initrd untouched
  all_waits_bounded: true           # restore is a bounded 16-entry walk; no new waits
  hardware_init_gated: n/a
  touches_userspace: true
  touches_kernel: false
  preserves_known_good_t410: true   # pure userspace; T410_SAFE profile proven in-harness
  smoke_proves_claim: true          # failing-then-passing with the SAME harness
  raw_pointers_or_truncation: >
    registry single-writer discipline: only read at respawn when the writer is
    dead; titles bounded to 48 B both directions; restore validates extent vs
    actual segment size before any blit can read it.
verdict: pass
done: >
  SELFHEAL-FIX-0 COMPLETE. Recovery now means RECOVERY: the overlay plays, the
  frozen compositor dies, and the desktop comes back with its windows -- same
  ids, same titles, taskbar repopulated -- with nothing asked of the clients.
next:
  - flash automationos-t410-selfheal.iso; on the T410 confirm a real freeze
    recovers WITH windows (and serial shows "SELFHEAL: restored win=").
  - parked: PREEMPT ring-3 hard-spin preemption (makes FREEZE_MODE=1
    recoverable); per-app supervision; kernel-side (timer-ISR) stall detector.
```
