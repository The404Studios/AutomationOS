# active_brick — what's in flight right now

> Warm memory. Refresh per checkpoint. One active brick at a time.

## CHANNEL-0 — capability-backed shared-ring channel primitive
- **branch:** `brick/channel-0` (off `t410-recovery`) · **spec:** `docs/superpowers/specs/2026-06-08-channel-0-design.md`
- **record:** [`bricks/CHANNEL-0.md`](bricks/CHANNEL-0.md)
- **goal:** one kernel primitive (handle → shared rings) = our StdIO model + the structured-tool rail.
- **build order:** P0 handle table · P1 ring + `ch_*` syscalls + self-test · P2 spawn binds child
  stdio · P3 sys_write/read route fd0/1/2 · P4 terminal holds master, externals' output in the grid.
- **status:** **P0/P1/P2 landed.** P0/P1 (`41a1c0a`): primitive + 5 syscalls + `[CHAN] selftest PASS`.
  P2: **`SYS_SPAWN_EX` (101)** binds a child's fd0/1/2 to channels — additive (`SYS_SPAWN` untouched),
  child gets the **slave end with narrowed rights** (stdin READ, stdout/stderr WRITE), master never
  leaked, refs transfer on success / freed on failure; `[CHAN] p2 selftest PASS`.
  **Next: P3** — `sys_write` fd1/2 (`handlers.c:731`) + `sys_read` fd0 (`:628`) route to the bound
  channel (`stdio_chan[fd]`) before the serial/ps2 fallback (unbound = unchanged).

## Parked / adjacent
- `brick/usb-ehci-0` — EHCI driver, **E1/E2 landed** (routing ledger); E3 (routing decision) next.
  The T410 is EHCI-only; UHCI brick works on QEMU only.
- `brick/terminal-0` (not created) — TERMINAL-0 quick-wins + the ~3,000-line dead-terminal-code
  deletion, after CHANNEL-0 P4.
- `docs/repo-presentation` — the README/docs overhaul, **committed local-only, awaiting push OK**.
- AI track north star: chainlayer2 (host agent) + external llama.cpp/GGUF backend + this dev-memory
  as training data. CHANNEL-0's `CH_MSG`/AGENT-RPC-0 = the typed-tool substrate.
