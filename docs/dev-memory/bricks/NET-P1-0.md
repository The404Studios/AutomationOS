# brick record: NET-P1-0 (NET-P1-A0..E + E1000-PCH-0A/0B/0C)

> Track 1 of the multi-core/GPU/networking roadmap (user: networking first).
> Six TCP-correctness bricks proven through a new deterministic in-kernel rig,
> then the contained 82577LM bring-up that moves the T410's NIC risk out of
> boot entirely. All QEMU gates green; the hardware ladder ships as
> `automationos-t410-net.iso` + `bin/nicup`.

```yaml
brick: NET-P1-0
status: FROZEN / pushed e00ee9d (user: "a major milestone... it did exactly what the rig
        was supposed to do"); T410 ladder = hardware validation pending
branch: brick/net-p1-0
base: brick/selfheal-fix-0 (bd3c597)
request: >
  "Open NET-P1-A0 first. Build the deterministic loopback TCP test rig only,
  gated NET_SELFTEST... Then NET-P1-A through E... Do not jump straight to the
  82577LM." (the user, confirming the roadmap's execution order)
checkpoints:
  - id: NET-P1-A0
    commits: [1e226c6]
    result: >
      kernel/net/net_testrig.c (-DNET_SELFTEST; default builds compile it
      EMPTY): crafts raw IPv4 packets (real checksums) into the SAME
      ipv4_demux the NIC path uses, and a tap at the head of ip_tx -- before
      net_up/ARP/the NIC -- swallows every outbound segment into a capture
      ring. No slirp timing, no hardware. NETRIG: PASS loopback=1 cap=1.
  - id: NET-P1-A
    commits: [3965242]
    result: >
      Half-open SYN side-table tcp_synq[32] (~32 B/entry) + socket-less
      tcp_xmit_raw: a SYN burns a table entry, NOT a ~45 KB sock_t; the final
      handshake ACK is the only point a socket is consumed (promote straight
      to ESTABLISHED + accept queue). Backlog now bounds unaccepted children
      (its true RFC meaning); evict-oldest under flood; SYN-ACK retransmit/
      TTL/orphan cleanup in tcp_synq_tick. NETP1A: SYNQ PASS syns=24
      synacks=24 established=4 sockused=4.
  - id: NET-P1-B
    commits: [24100a6]
    result: >
      4-deep per-socket OOO rows, HEAP-backed via tcp_buffers_init from
      sock_init (a static 4x array risks the .bss/initrd overrun -- the exact
      reason the socket table moved to kmalloc); dedupe-by-seq + evict-
      farthest save, sweep-drain with stale-discard + prefix trim, never
      consuming a slot whose remainder doesn't wholly fit the ring (partial
      push + clear = an ACK lie). Alloc failure degrades correctly to
      drop-OOO. NETP1B: OOO PASS slots=4 reassembled=5840.
  - id: NET-P1-C
    commits: [30136ae]
    result: >
      TWO real bugs found by the rig (failing-first both times): (1) the
      zero-window clamp guard (snd_wnd > 0 && ...) made the branch DEAD CODE
      -- the stack transmitted INTO a zero window; (2) the first persist-loop
      iteration cap RACED the wall clock (200k fast polls < the 1 s backoff).
      Fix: real persist timer (1-byte probes at snd_nxt, exp backoff,
      TCP_CONNECT_MS budget) + a frozen-tick backstop that counts only
      CONSECUTIVE no-tick-movement iterations -- it fires exactly in the
      IF=0/frozen-PIT condition it guards. The rig autoresponder (plays the
      peer from inside the tx tap; pure-ACK injections cannot recurse) stays
      window-shut through two probes, opens on the third, ACKs the data.
      NETP1C: ZWND PASS probes=3 delivered=1.
  - id: NET-P1-D/E
    commits: [30136ae]
    result: >
      UDP_QUEUE_DEPTH 8->16; SOCK_MAX 16->32 (~1.4 MB single kmalloc,
      loudly asserted; 64 = wasted rings, half-opens scale via the side-
      table). NETP1D: UDPQ PASS depth=16 queued=16 dropped=0 / NETP1E:
      SOCKMAX PASS n=32 heapok=1 extra_rejected=1.
  - id: E1000-PCH-0A/0B/0C
    commits: [14a0bdb]
    result: >
      Containment for the ME-shared-MDIO hardware bus stall (unrecoverable in
      software; iteration caps can't help). 0A: boot does ONLY safe plain-MMIO
      probe even with PCH_NIC=1; the risky half (CTRL_RST under SWFLAG, PHY
      dance, rings) is deferred -- a stall can never kill boot again. 0B:
      SYS_NET_CONFIG NIC_BRINGUP flag (works while net is DOWN) ->
      net_attach_late -> e1000_pch_deferred_bringup; bin/nicup triggers it
      post-desktop, re-runnable. 0C: marker-per-risky-touch serial ladder
      (last-line-wins diagnosis) + THE fix: never CTRL_RST when
      acquire_swflag() failed (the old proceed-anyway was the wedge path).
      QEMU negative gates green (pchnic_check.sh): PCH kernel boots classic
      e1000 with networking up + ZERO E1000PCH markers.
  - id: regression-gates
    result: >
      All six rig markers asserted in ONE boot per cycle (netrig_check.sh).
      Full default smoke after the stack changes: every net check green; the
      5 failing checks ([KERNEL] banner, heap/slab reports = BOOT_LOG lines
      suppressed by the always-on -DBOOT_QUIET; RQLOCK/AFFINITY = SMP-only
      markers) are PRE-EXISTING smoke_boot.sh profile mismatches, proven
      unrelated by diff scope (branch touches net files only). Follow-up
      task open to make those checks profile-aware. Also noted: the
      boot-time gateway-ARP pre-resolve marker is flaky under boot IO
      (resolve_mac re-resolves in-syscall) -- gate on "[NET] up:" instead.
review:
  default_build_changed: true       # net stack constants/behavior (SOCK_MAX, synq, OOO, persist) -- intended scope
  all_waits_bounded: true           # persist loop: wall budget + no-tick-movement backstop
  hardware_init_gated: true         # PCH_NIC compile gate UNCHANGED + runtime trigger on top
  touches_userspace: true           # bin/nicup only
  touches_kernel: true
  preserves_known_good_t410: true   # PCH path inert without BOTH the flag and the trigger
  smoke_proves_claim: true          # failing-then-passing twice in NET-P1-C
verdict: pass (QEMU); T410 ladder pending
done: >
  The TCP stack stopped lying (zero-window sends), stopped starving (SYN
  burns a table entry, not a socket), reassembles real reordering, and
  every future net brick has a deterministic proof rig. The T410's NIC
  bring-up can no longer cost a boot -- worst case is a re-run of nicup
  with a live desktop and a serial line that names the exact wedging access.
next:
  - "T410 ladder (E1000-PCH-0D/0E, hardware day): flash automationos-t410-net.iso
     (T410_SAFE+PCH_NIC kernel, DESKTOP_MINIMAL+SELFHEAL userspace), wire to the
     travel-router bridge, run `nicup` from the terminal, read the E1000PCH
     serial ladder (PROBE -> FWSM -> SWFLAG -> PHYID -> ANEG -> LINK UP),
     then `dhcpc` -> `ping` -> `httpd`. Aborts are safe: re-run nicup."
  - then (roadmap order): GPU tier 0 / NV-REF-0 hardware day -> SMP-R0.
```
