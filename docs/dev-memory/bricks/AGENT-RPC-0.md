# brick record: AGENT-RPC-0

> The typed-tool layer on top of CHANNEL-0: `CH_MSG` framed messages → `TOOL_RUN`/`TOOL_RESULT`.
> The in-OS landing point for the chainlayer2 typed-agent rail (structured tool calls, NOT terminal
> scraping). Builds on the frozen CHANNEL-0 (P0–P4) + TERMINAL-0 (T0–T4) substrate.
> Spec (P5/P6 design): `docs/superpowers/specs/2026-06-08-channel-0-design.md`.

```yaml
brick: AGENT-RPC-0
status: in-progress
branch: brick/agent-rpc-0
base: brick/terminal-0            # off the published TERMINAL-0 HEAD (47a2bc0); CHANNEL-0 substrate included
request: >
  The agent must never scrape the terminal. Give it a typed rail: framed message packets over the
  existing CHANNEL-0 rings (CH_MSG), then TOOL_RUN/TOOL_RESULT semantics + an agent runtime. Reuse the
  byte-channel rings; keep the kernel dumb; keep every step additive so CH_BYTE (all of TERMINAL-0) is
  byte-for-byte unchanged.
design_principles:
  - reuse the CHANNEL-0 SPSC rings + ends; CH_MSG just frames them (one msg_packet_t header + payload)
  - message-atomic: a write commits the whole frame or nothing; a read returns exactly one whole message
  - bounded + explicit errors: oversize (header+len > ring cap) is EMSGSIZE, not EAGAIN, so the later
    syscall semantics stay clean; transiently-full is EAGAIN; empty read is EAGAIN
  - substrate before surface: prove the kernel framing by boot self-test BEFORE any syscall/userspace
    exposure or TOOL_RUN dispatch
checkpoints:
  - id: P5a
    title: typed CH_MSG message framing (kernel substrate) + boot self-test
    commits: [f0a91fd]
    files:
      - kernel/include/channel.h          # msg_packet_t {type,flags,len,request_id} + write_msg/read_msg + p5 proto
      - kernel/ipc/channel.c              # channel_write_msg/channel_read_msg (atomic) + channel_selftest_p5
      - kernel/include/errno.h            # +EMSGSIZE (-90): header+len > ring cap (the hard oversize boundary)
      - kernel/core/syscall/syscall.c     # call channel_selftest_p5() at syscall_init (after p2)
    tests: [build_test/p5_verify.sh]
    result: >
      quick_build clean (kernel 495224 B, SCHED_DEBUG=0 milestone profile); ISO byte-identical to the
      frozen TERMINAL-0 (38281216); serial shows [CHAN] p5 msg selftest PASS
      (w=30 r=14 rid_ok=1 empty=EAGAIN:1 oversize=EMSGSIZE:1 payload='/bin/cc main.c'); the P1 byte
      selftest + p2 binding selftest still PASS (default CH_BYTE path unchanged); SCREENSHOT
      (screenshots/p5final.png) == t4final.png (clean desktop, 0 panic, no regression)
    design:
      - msg_packet_t = a fixed 16-byte header (no padding: 2+2+4+8, naturally aligned) followed on the
        ring by `len` payload bytes. Raw header bytes round-trip the SPSC ring.
      - channel_write_msg: EINVAL on misuse / non-CH_MSG; EMSGSIZE if 16+len > ring cap (can never fit)
        OR on len-wrap; EAGAIN if it momentarily won't fit (free < total); else copy header+payload and
        bump head ONCE -> the frame appears atomically (SMP-safe in shape, not just cooperative-safe).
      - channel_read_msg: peeks the header without consuming (so an undersized-buffer caller gets
        EMSGSIZE with the message left intact to retry); EAGAIN if < one whole message is queued; on
        success consumes header+payload and returns the payload length (0 = valid empty message).
      - additive: only adds new functions + a boot selftest + a new errno; channel_write/read (the
        CH_BYTE byte path that all of TERMINAL-0 uses) is untouched.
    review:
      default_build_changed: false      # byte-identical ISO; CH_BYTE path + P1/p2 selftests intact
      all_waits_bounded: true           # no loops beyond the bounded header+payload copy; no parking
      hardware_init_gated: n/a          # pure software primitive
      touches_userspace: false          # P5a is kernel-only (no syscall surface yet -> P5b)
      preserves_known_good_t410: true
      smoke_proves_claim: true          # boot selftest exercises write/read/EAGAIN/EMSGSIZE end-to-end
      raw_pointers_or_truncation: none  # bounded by ring cap; oversize rejected (EMSGSIZE) not truncated
    verdict: pass
  - id: P5b
    title: expose CH_MSG packets to userspace via dedicated SYS_CH_SENDMSG/RECVMSG (byte channels stay pure)
    commits: [2a06157]
    files:
      - kernel/include/syscall.h          # SYS_CH_SENDMSG=102, SYS_CH_RECVMSG=103
      - kernel/include/channel.h          # sys_ch_sendmsg/recvmsg protos
      - kernel/ipc/channel.c              # the two handlers (copy-in/out, CH_MAX_IO cap, errno propagation)
      - kernel/core/syscall/syscall.c     # register 102/103
      - userspace/lib/channel.h           # ch_sendmsg/ch_recvmsg + ch_msg_hdr (mirrors msg_packet_t) + CH_EAGAIN/CH_EMSGSIZE
      - userspace/apps/msgtest/msgtest.c  # NEW self-spawning userspace round-trip proof (crt0-linked)
      - scripts/build_all.sh              # compile + stage sbin/msgtest
      - userspace/init/main.c             # init spawns sbin/msgtest at boot
    tests: [build_test/p5_verify.sh]
    result: >
      quick_build clean (kernel 495304 B); 88 sbin entries (msgtest added); serial
      'MSGTEST: PASS eagain=1 emsgsize=1 send=1 roundtrip=1 reply=PONG rid=0x1234abcd' -- a real
      userspace CH_MSG round-trip (parent->child->parent) PLUS EAGAIN + EMSGSIZE proven across the
      syscall boundary; the P1 byte + p2 binding + p5 framing selftests still PASS (CH_BYTE unchanged);
      desktop screenshot (p5bcheck.png) == t4final.png, 0 panic
    design:
      - DEDICATED packet syscalls (user's call): byte channels keep sys_ch_write/read = stream
        semantics forever; CH_MSG gets explicit SENDMSG/RECVMSG so later agent bugs are auditable.
      - sendmsg: copy 16-byte header in, reject len > CH_MAX_IO (64 KiB) with EMSGSIZE before any
        payload access, copy payload in, channel_write_msg (atomic; EMSGSIZE/EAGAIN/total). recvmsg:
        read exactly one packet into kernel buffers, copy header+payload out; EAGAIN if none queued,
        EMSGSIZE if the user buffer is too small (message left intact). Both rights-checked (WRITE/READ).
      - userspace proof is a real program, not a kernel selftest: msgtest self-spawns (parent holds the
        master, the child's fd0(READ)+fd1(WRITE) are the channel slave end at the deterministic handles
        1/2); the child stays SILENT (its fd1 IS the channel). Genuine cross-process round-trip.
    review:
      default_build_changed: false      # CH_BYTE syscalls untouched; byte/binding/framing selftests pass
      all_waits_bounded: true           # msgtest poll loops are bounded + yield; no kernel blocking added
      hardware_init_gated: n/a
      touches_userspace: true           # new sbin/msgtest + lib wrappers + init spawn (the userspace surface)
      preserves_known_good_t410: true
      smoke_proves_claim: true          # MSGTEST: PASS in serial = userspace round-trip + EAGAIN + EMSGSIZE
      raw_pointers_or_truncation: none  # copy_from/to_user bounded; CH_MAX_IO cap; oversize->EMSGSIZE
    verdict: pass
  - id: P6a
    title: define the TOOL_RUN/TOOL_RESULT wire schema (schema only -- lock the contract before dispatch)
    commits: [9178d89]
    files:
      - userspace/lib/agent_rpc.h         # NEW shared header: type IDs, version, packed payloads, encode/validate
      - userspace/apps/rpctest/rpctest.c  # NEW encode/decode/validate selftest (schema only; no channels)
      - docs/AGENT_RPC_WIRE.md            # NEW authoritative wire-contract doc
      - scripts/build_all.sh              # compile + stage sbin/rpctest
      - userspace/init/main.c             # init spawns sbin/rpctest
    tests: [build_test/p6a_verify.sh]
    result: >
      build_all clean (kernel unchanged); serial 'RPCTEST: PASS v=1 run_sz=392 res_sz=16
      rej(len=1,ver=1,fld=1,enc=1,resver=1)' -- both structs encode->validate round-trip and EVERY
      rejection path fires (bad length/version/field, over-long encode, result version). P5b MSGTEST +
      P1/p2/p5 selftests still PASS; p6afinal.png clean, 0 panic.
    design:
      - SCHEMA ONLY (user's call): lock the wire contract before any dispatch (spawn/fd passing is where
        the risk starts). No spawn, no fd passing, no stdout channel -- that is P6b.
      - the payload of a CH_MSG packet IS one fixed-size struct; msg_packet_t.type selects it
        (MSG_TOOL_RUN 0x0101 / MSG_TOOL_RESULT 0x0102). tool_run_t = 392 B (version/flags/path_len/
        args_len/reserved + path[120] + args[256]); tool_result_t = 16 B (stdout_handle always 0 in P6a).
      - versioned + validated: receivers reject unknown version (forward-safety) and any length/field/NUL
        violation, with explicit AR_E_* codes. The schema is pure userspace policy; the kernel ring is opaque.
      - GOTCHA fixed: rpctest's hand-rolled out() omitted the syscall "=a" output, so gcc cached a stale
        rax (clobbered by syscall) and a later write ran as a stray sys_open. Capture the return. (The
        schema logic was correct first try -- RPCTEST: PASS printed even through the garbled tail.)
    review:
      default_build_changed: false      # userspace-only; kernel byte-identical; all prior selftests pass
      all_waits_bounded: true           # pure in-memory encode/validate; no loops beyond bounded copies
      hardware_init_gated: n/a
      touches_userspace: true
      touches_kernel: false             # the wire schema is userspace policy; the ring is opaque
      preserves_known_good_t410: true
      smoke_proves_claim: true          # RPCTEST: PASS = encode/validate round-trip + all 5 rejections
      raw_pointers_or_truncation: none  # bounded path[120]/args[256]; lengths validated; over-long rejected
    verdict: pass
  - id: P6b
    title: path-only TOOL_RUN runner -- agent -> runner -> tool (/bin/free) -> stdout read -> TOOL_RESULT
    commits: [f12cecc]
    files:
      - userspace/apps/toolrun/toolrun.c  # NEW self-spawning agent+runner proof (crt0-linked)
      - scripts/build_all.sh              # compile + stage sbin/toolrun
      - userspace/init/main.c             # init spawns sbin/toolrun
    tests: [build_test/p6b_verify.sh]
    result: >
      build_all clean (kernel unchanged); serial 'RUNNER: PASS path=/bin/free exit=0 stdout_bytes=183
      handle=3 sent=1' + 'TOOLRUN: PASS sent=1 result=1 type=1 rid=1 valid=1 handle_nz=1 exit=0' -- the
      full chain: agent sends a path-only TOOL_RUN -> runner validates + spawns /bin/free w/ stdout
      bound to a CH_BYTE channel -> runner drains 183 B of stdout (the 'stdout read') -> TOOL_RESULT
      { exit_code=0, stdout_handle } returned + validated by the agent. RPCTEST/MSGTEST/[CHAN] still
      PASS; p6bcheck.png clean, 0 panic.
    design:
      - PATH ONLY (user's call): the runner rejects args_len != 0 (args are P6c) and reserved != 0.
        No shell, no arg parser, no PATH lookup (path used verbatim), no env, no stdin, no stderr
        capture, one run, only the stdout byte channel inherited, no streaming, no privilege expansion.
      - topology: self-spawn (parent=agent, child=runner over a shared CH_MSG ctrl, like msgtest); the
        runner spawns the tool (a grandchild) with stdout bound to a CH_BYTE channel the runner created.
      - TRUST-BOUNDARY NOTE: CHANNEL-0 only transfers handles parent->child (spawn_ex, narrowed rights),
        no up-transfer -> the RUNNER reads the child's stdout (the achievable 'stdout read');
        TOOL_RESULT.stdout_handle is the runner's handle (a non-zero reference token). Agent-side
        stdout-read-via-handle (cross-process deref) is deferred to P6c with args. (NOT faked.)
      - GOTCHA avoided: the runner's fd1 IS the ctrl channel, so its diagnostics go to fd2 (unbound ->
        serial), never fd1; the agent (unbound stdio) prints the verdict to fd1. Both out() helpers
        capture the syscall "=a" output (the rpctest stale-rax lesson).
    review:
      default_build_changed: false      # userspace-only; kernel byte-identical; all prior selftests pass
      all_waits_bounded: true           # bounded poll+yield loops (recv, waitpid-WNOHANG+drain, send)
      hardware_init_gated: n/a
      touches_userspace: true
      touches_kernel: false
      preserves_known_good_t410: true
      smoke_proves_claim: true          # RUNNER+TOOLRUN PASS = the typed dispatch chain executed
      raw_pointers_or_truncation: none  # typed recv buffers (tool_run_t/tool_result_t); bounded drain
    verdict: pass
next_checkpoints:
  - P6c argv (NUL-separated argv bytes, validated) + cross-process stdout delivery to the agent (handle transfer)
deferred:
  - P7 async submission/completion batch · P8 NIC RX/TX as channels
```
