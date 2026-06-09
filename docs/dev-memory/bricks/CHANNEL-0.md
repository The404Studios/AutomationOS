# brick record: CHANNEL-0

> The first machine-readable brick record — dog-foods the schema in
> [`../README.md`](../README.md). A successful engineering trajectory (the patch-replay dataset unit).

```yaml
brick: CHANNEL-0
status: in-progress
branch: brick/channel-0
base: t410-recovery
spec: docs/superpowers/specs/2026-06-08-channel-0-design.md
request: >
  Don't clone the Unix /dev/tty stack or do syscall-per-byte stdio, and don't make the AI scrape
  terminal text. Build one capability-backed primitive: shared-memory rings + handles + typed
  messages, as the single rail for console StdIO, IPC, AI tool calls, async I/O, networking.
gates: [additive]          # new syscalls (96-100) nothing binds until opt-in; default build unchanged
design_principles:
  - kernel-dumb / userspace-policy (the "terminal device" is a userspace channel holder, not a kernel tty)
  - two SPSC rings per channel (to_master=child stdout, to_slave=child stdin); creator=master, child=slave
  - capability rights per handle (READ/WRITE/DUP/TRANSFER/SIGNAL/ADMIN)
  - byte channels for humans (CH_BYTE), typed message channels for agents (CH_MSG, P5+)
  - take io_uring's rings/batching/capabilities; skip its opcode sprawl + worker threads (security history)
checkpoints:
  - id: P0+P1
    title: handle table + channel object + ch_* syscalls + boot self-test
    commit: 41a1c0a
    files:
      - kernel/include/channel.h          # channel_t, ch_ring_t, rights, API (new)
      - kernel/ipc/channel.c              # rings, handle table, ch_* kernel ops + 5 syscalls + selftest (new)
      - kernel/include/sched.h            # process_t.ch_handles[32] + stdio_chan[3]
      - kernel/include/syscall.h          # SYS_CH_CREATE/WRITE/READ/WAIT/CLOSE = 96-100
      - kernel/core/syscall/syscall.c     # register handlers + call channel_selftest()
      - kernel/core/sched/process.c       # channel_cleanup_process() at teardown
      - scripts/quick_build.sh            # compile kernel/ipc/channel.c
    tests: [build_test/channel_p1.sh]
    result: "91 compiled / SUCCESS; [CHAN] selftest PASS (slave wrote 4, master read 4: 'PING'); desktop reached; 0 panic"
    review:
      default_build_changed: false       # additive: nothing binds a channel until opt-in
      all_waits_bounded: true            # ch_wait is a non-blocking readiness poll; rings bounded
      hardware_init_gated: n/a           # pure software primitive
      touches_userspace: false           # P0/P1 is kernel-only
      preserves_known_good_t410: true
      smoke_proves_claim: true           # the boot self-test exercises the ring end-to-end
      raw_pointers_or_truncation: none   # bounded rings, rights-checked handles, kmalloc'd copies
    verdict: pass
  - id: P2
    title: SYS_SPAWN_EX binds a child's fd0/1/2 to channels (slave end, narrowed rights)
    files:
      - kernel/ipc/channel.c            # sys_spawn_ex + channel_install_spawn_stdio + g_exec_stdio + p2 selftest
      - kernel/include/channel.h        # protos
      - kernel/include/syscall.h        # SYS_SPAWN_EX = 101
      - kernel/core/syscall/syscall.c   # register + channel_selftest_p2()
      - kernel/fs/exec.c                # install hook in elf_load_and_exec (before scheduling)
    tests: [build_test/channel_p1.sh]
    result: "91 compiled / SUCCESS; [CHAN] p2 selftest PASS (slave-READ denied; bad-handle denied); P1 still PASS; desktop reached; 0 panic"
    design:
      - additive: SYS_SPAWN_EX is new (#101); SYS_SPAWN untouched; plain spawn leaves child stdio unbound
      - capability hygiene: parent passes HANDLES (needs CH_R_TRANSFER), not pointers; child gets the
        SLAVE end only, with narrowed rights (stdin=READ, stdout/stderr=WRITE); master never leaked
      - ref ownership: staged+ref'd in sys_spawn_ex; transferred to the child handle on success
        (freed at child teardown), or unref'd by sys_spawn_ex on any spawn failure (no leak/double-free)
    review:
      default_build_changed: false
      all_waits_bounded: true
      touches_userspace: false
      preserves_known_good_t410: true
      smoke_proves_claim: true        # synthetic binding/rights selftest (end-to-end output is P3/P4)
      raw_pointers_or_truncation: none
    verdict: pass
  - id: P3
    title: sys_write fd1/2 + sys_read fd0 route to the bound channel (routing only, non-blocking)
    files: [kernel/core/syscall/handlers.c]   # +#include channel.h; route inside the fd1/2 and fd0 blocks
    tests: [build_test/channel_p1.sh]
    result: "91 compiled / SUCCESS; P1 + p2 selftests still PASS; desktop reached; 0 panic"
    design:
      - routing only: no terminal work, no blocking. sys_write fd1/2 -> channel_write if stdio_chan[fd]
        bound (CH_R_WRITE), else serial/VGA. sys_read fd0 -> channel_read if bound (CH_R_READ), else ps2.
      - non-blocking: ring full on write -> partial (bytes accepted); ring empty on read -> 0. No parking.
      - return semantics: sys_write -> bytes written; sys_read -> >0 bytes / 0 no-data / <0 error (EFAULT/EBADF).
      - unbound fd (stdio_chan[fd]==0) falls through to the serial/ps2 path byte-for-byte unchanged.
    review:
      default_build_changed: false       # unbound path identical; every existing program boots clean
      all_waits_bounded: true            # non-blocking, no parking (blocking I/O deferred to P3.5/P5)
      touches_userspace: false
      preserves_known_good_t410: true
      smoke_proves_claim: partial        # no-regression proven at boot; the BOUND path is observable at P4 (needs a reader)
      raw_pointers_or_truncation: none   # bounded 512B read buffer; rights-checked handles; heap_buf freed
    verdict: pass
  - id: P4
    title: terminal holds the master, spawns externals bound to a channel, drains master -> grid (THE VISIBLE WIN)
    files:
      - userspace/lib/channel.h                       # new: ch_create/read/write/wait/close + spawn_ex wrappers
      - userspace/apps/terminal/terminal_m3.c         # spawn_bound() + try_external/try_spawn_image route through it + per-frame drain
    tests: [build_test/channel_p4.sh, build_test/shot.sh]
    result: "build_all clean; SCREENSHOT shows /bin/free output IN the terminal grid (child->write(1)->channel->drain->grid); [CHAN] + p2 selftests PASS; desktop reached; 0 panic"
    design:
      - terminal is the holder + renderer; kernel is the dumb ring; child is the slave writer (model preserved)
      - spawn_bound: ch_create(CH_BYTE) -> spawn_ex(path,args,0,ch,ch) -> track {ch,pid}; fallback to plain spawn
      - bounded drain (<=4KB/frame, 512B reads) in the main loop so a noisy child can't freeze the single-core GUI
      - non-blocking, no job control / Ctrl-C / pipes / colors / VT upgrades (deferred)
    review:
      default_build_changed: false      # unbound programs unchanged; terminal opens + builtins render normally
      all_waits_bounded: true           # bounded per-frame drain; non-blocking ch_read
      touches_userspace: true           # this IS the terminal-integration brick
      preserves_known_good_t410: true
      smoke_proves_claim: true          # screenshot = human-visible proof; build_all + boot = no-regression
      raw_pointers_or_truncation: none
    verdict: pass
status_note: "P0-P4 = CHANNEL-0 core complete (kernel substrate + the visible terminal win)."
next_checkpoints:
  - P3.5 blocking ch_wait (wait_object) | P5 typed CH_MSG message channels | P6 AGENT-RPC-0 | TERMINAL-0 polish
  - P4: terminal_m3.c ch_create + spawn-bound + drain master into the grid; SYS_WAITPID for $?
deferred:
  - P5 typed msg_packet_t channels · P6 AGENT-RPC-0 (TOOL_RUN/TOOL_RESULT) · P7 async batch · P8 NIC rings
```
