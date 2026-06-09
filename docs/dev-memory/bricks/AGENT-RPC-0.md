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
next_checkpoints:
  - P5b syscall surface for CH_MSG (frame-aware sys_ch_write/read or SYS_CH_SENDMSG/RECVMSG) + userspace wrapper
  - P6 AGENT-RPC-0 typed tool calls (TOOL_RUN/TOOL_RESULT) + the agent runtime
deferred:
  - P7 async submission/completion batch · P8 NIC RX/TX as channels
```
