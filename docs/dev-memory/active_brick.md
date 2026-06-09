# active_brick — what's in flight right now

> Warm memory. Refresh per checkpoint. One active brick at a time.

## TERMINAL-0 (COMPLETE — clean milestone, worth pushing) — make CHANNEL-0 console output human-stable
- **branch:** `brick/terminal-0` (off `brick/channel-0`) · **spec:** `docs/superpowers/specs/2026-06-09-terminal-0-design.md`
- **why:** CHANNEL-0 P4 made external output appear, but messy (after-prompt, no scrollback, half-built
  line editing, escapes-as-boxes). Make it human-stable BEFORE the typed agent rail (AGENT-RPC-0).
- **checkpoints (one commit each):** **T0** output-before-next-prompt (waitpid-lite: defer the prompt
  until the bound child exits, poll `SYS_WAITPID` WNOHANG each frame, bounded await) ← do first ·
  T1 scrollback ring · T2 line-editing cleanup (cursor_pos + Left/Right/Home/End/Del, kill `[TERM] key`
  spam) · T3 minimal ANSI/VT (SGR color first — `grid_color[][]` substrate exists) · T4 delete ~3000
  lines dead terminal code. ALL userspace (`terminal_m3.c`); does NOT touch the kernel primitive.
- **status:** **T0 + T1 + T2 + T3 LANDED.** T0 (`3eac1bb`): defer the prompt for a bound child + waitpid-lite
  (`SYS_WAITPID` WNOHANG=1, non-blocking) → prompt prints AFTER the output. T1: **scrollback ring**
  (256-line `sb[][]` ring; `cur_row` → `sb_head`/`sb_view`/`sb_follow` viewport; `grid_putchar` is the
  ONE append path; PageUp/PageDown scroll, PageDown re-follows). SCREENSHOT-verified (`t0demo.png`,
  `t1demo.png` — a scrolled-back viewport holds lines the old discard grid lost); build_all clean;
  desktop. **T2 (`97faa0c`): clean line editing** — `line_cursor` + redraw-current-line (`redraw_input_line`);
  Left/Right/Home/End/Backspace/Delete/insert all cursor-aware; `[TERM] key` spam gated behind
  `TERM_DEBUG_KEYS` (default off). Screenshot-verified (`t2check.png`: a multi-line self-check runs
  the three acceptance cases through the SAME edit primitives → PASS lines in scrollback; `t2final.png`
  = clean build with the temp self-check removed). **T3 (`a6b1efc`): minimal ANSI/VT SGR colour** —
  a 3-state parser (`ANSI_TEXT`/`ESC`/`CSI`) sits BEFORE `grid_putchar` (ANSI = state mutation, not
  output); colour ONLY (`ESC[0m` reset, `30-37`/`90-97` fg, `39` default, `1` bold retro-brightens so
  `31;1`==`1;31`); 32-byte CSI buffer, overflow→TEXT; child drain routes through `term_write()` +
  `ansi_reset()` on child-exit; builtins/prompt keep explicit colour (no ANSI). Screenshot-verified
  (`t3check.png`: red/green/bright-red render, `ESC[999m` sane in default, NO escape junk; `t3final.png`
  clean). **T4 (`6aa9ce2` + `f3c8958`): deletion surgery** — removed the superseded OLD terminal app
  island (terminal.c[main] + renderer/buffer/tabs/pty_impl/vt_parser/profiles/utils + terminal.h + the
  dead dir Makefiles/README) and the `grid_backspace` orphan: **5,241 lines deleted** with a
  BYTE-IDENTICAL ISO (38281216) + identical screenshot (`t4check.png`) proving nothing live was removed.
  Kept: sh_git.c (live shell), font_integration.c/.h (verify-script refs). record:
  `docs/dev-memory/bricks/TERMINAL-0.md`. **TERMINAL-0 COMPLETE** — the rail is human-stable end to end
  (output-before-prompt → scrollback → line editing → ANSI colour); `brick/terminal-0` is a clean
  milestone worth pushing. **Next brick (user's call):** AGENT-RPC-0 / CHANNEL-0 P5 (typed `CH_MSG`),
  or resume `brick/usb-ehci-0` E3, or the latency/real-hardware track.

## CHANNEL-0 — FROZEN / COMPLETE (P0–P4, pushed to origin)
- **branch:** `brick/channel-0` (PUSHED, commit `1dd5107`) · **spec:** `docs/superpowers/specs/2026-06-08-channel-0-design.md`
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
