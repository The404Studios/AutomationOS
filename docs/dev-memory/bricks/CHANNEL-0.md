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
next_checkpoints:
  - P2: sys_spawn arg3 -> install channel as the child's slave-end stdio in elf_load_and_exec
  - P3: sys_write fd1/2 + sys_read fd0 route to the bound channel (before the serial/ps2 fallback)
  - P4: terminal_m3.c ch_create + spawn-bound + drain master into the grid; SYS_WAITPID for $?
deferred:
  - P5 typed msg_packet_t channels · P6 AGENT-RPC-0 (TOOL_RUN/TOOL_RESULT) · P7 async batch · P8 NIC rings
```
