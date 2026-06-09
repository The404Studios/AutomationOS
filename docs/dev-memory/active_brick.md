# active_brick — what's in flight right now

> Warm memory. Refresh per checkpoint. One active brick at a time.

## CHANNEL-0 — capability-backed shared-ring channel primitive
- **branch:** `brick/channel-0` (off `t410-recovery`) · **spec:** `docs/superpowers/specs/2026-06-08-channel-0-design.md`
- **record:** [`bricks/CHANNEL-0.md`](bricks/CHANNEL-0.md)
- **goal:** one kernel primitive (handle → shared rings) = our StdIO model + the structured-tool rail.
- **build order:** P0 handle table · P1 ring + `ch_*` syscalls + self-test · P2 spawn binds child
  stdio · P3 sys_write/read route fd0/1/2 · P4 terminal holds master, externals' output in the grid.
- **status:** **P0–P4 LANDED — the visible win works.** P0/P1 (`41a1c0a`) primitive + syscalls;
  P2 (`2d745b9`) `SYS_SPAWN_EX` binds child stdio; P3 (`3d46209`) `sys_write`/`sys_read` route the
  bound stdio; **P4: the terminal `ch_create`s a channel, spawns externals via `spawn_ex` bound to
  it, and drains the master into the grid (bounded ≤4 KB/frame).** SCREENSHOT-VERIFIED: `/bin/free`'s
  output appears in the terminal grid (child → write(1) → channel → drain → grid) instead of vanishing
  to serial; selftests PASS, desktop, 0 panic, default build unchanged. The kernel substrate (P0–P3)
  + the terminal holder (P4) = CHANNEL-0's core goal done. userspace wrapper: `userspace/lib/channel.h`.
- **next (CHANNEL-0 follow-ons, separate bricks):** P3.5 blocking `ch_wait` (wait_object) · P5 typed
  `CH_MSG`/`msg_packet_t` message channels · P6 AGENT-RPC-0 (`TOOL_RUN`/`TOOL_RESULT`) · TERMINAL-0
  polish (output-before-prompt, mid-line editing, scrollback, VT/ANSI parser, dead-code deletion).

## Parked / adjacent
- `brick/usb-ehci-0` — EHCI driver, **E1/E2 landed** (routing ledger); E3 (routing decision) next.
  The T410 is EHCI-only; UHCI brick works on QEMU only.
- `brick/terminal-0` (not created) — TERMINAL-0 quick-wins + the ~3,000-line dead-terminal-code
  deletion, after CHANNEL-0 P4.
- `docs/repo-presentation` — the README/docs overhaul, **committed local-only, awaiting push OK**.
- AI track north star: chainlayer2 (host agent) + external llama.cpp/GGUF backend + this dev-memory
  as training data. CHANNEL-0's `CH_MSG`/AGENT-RPC-0 = the typed-tool substrate.
