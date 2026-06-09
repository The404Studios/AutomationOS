# active_brick — what's in flight right now

> Warm memory. Refresh per checkpoint. One active brick at a time.

## AGENT-RPC-0 (ACTIVE) — the typed-tool rail on top of CHANNEL-0 (the chainlayer2 north star)
- **branch:** `brick/agent-rpc-0` (off the published `brick/terminal-0` HEAD `47a2bc0`) · **spec (P5/P6):**
  `docs/superpowers/specs/2026-06-08-channel-0-design.md` · **record:** [`bricks/AGENT-RPC-0.md`](bricks/AGENT-RPC-0.md)
- **why:** now that the console is human-stable (TERMINAL-0), give the agent a TYPED rail — framed
  `CH_MSG` packets → `TOOL_RUN`/`TOOL_RESULT` — so it never scrapes terminal text.
- **checkpoints (one commit each):** **P5a** kernel `CH_MSG` framing + boot selftest ← done ·
  P5b syscall surface (frame-aware `sys_ch_write/read` or `SYS_CH_SENDMSG/RECVMSG`) + userspace wrapper ·
  P6 `TOOL_RUN`/`TOOL_RESULT` + the agent runtime. Substrate-before-surface; additive (CH_BYTE untouched).
- **status:** **P5a LANDED (substrate).** `msg_packet_t {type,flags,len,request_id}` framed on the
  existing SPSC rings; `channel_write_msg`/`channel_read_msg` are **message-atomic** (whole frame or
  nothing; one packet per read; header peeked so an undersized buffer gets `EMSGSIZE` with the message
  intact). Hard boundary: header+len > ring cap → **`EMSGSIZE` (-90, new)**, NOT EAGAIN. Proven by
  `channel_selftest_p5()` at boot: serial `[CHAN] p5 msg selftest PASS (w=30 r=14 rid_ok=1
  empty=EAGAIN:1 oversize=EMSGSIZE:1 payload='/bin/cc main.c')`; P1/p2 selftests still PASS (default
  CH_BYTE path unchanged); ISO byte-identical to TERMINAL-0; `p5final.png` == `t4final.png` (0 panic).
  **P5b (`2a06157`): the syscall surface.** Dedicated **`SYS_CH_SENDMSG` (102) / `SYS_CH_RECVMSG`
  (103)** — byte channels keep `sys_ch_write/read` (pure stream semantics) forever; CH_MSG gets
  explicit packet syscalls (auditable). Handlers copy header+payload across the user boundary,
  cap at `CH_MAX_IO` (64 KiB→`EMSGSIZE`), propagate `EAGAIN`/`EMSGSIZE`. Userspace proof = a real
  self-spawning program `sbin/msgtest` (parent holds master; child fd0(READ)+fd1(WRITE) = slave end;
  child silent since its fd1 IS the channel): serial `MSGTEST: PASS eagain=1 emsgsize=1 send=1
  roundtrip=1 reply='PONG' rid=0x1234abcd` — a genuine cross-process userspace round-trip + EAGAIN +
  EMSGSIZE. CH_BYTE unchanged (P1/p2/p5 selftests pass, terminal renders, `p5bcheck.png` clean, 0
  panic). `gitignore` anchored (`decf0f9`). **P5a+P5b PUSHED** (`2fe28f4`, ls-remote verified).
  **P6a (`9178d89`): the wire schema (schema-only).** `userspace/lib/agent_rpc.h` — type IDs
  `MSG_TOOL_RUN` (0x0101)/`MSG_TOOL_RESULT` (0x0102), `AGENT_RPC_VERSION`=1, fixed-size packed
  `tool_run_t` (392 B)/`tool_result_t` (16 B; `stdout_handle`=0 until P6b), encode/validate with
  explicit `AR_E_*` codes; doc `docs/AGENT_RPC_WIRE.md`. The CH_MSG payload IS one struct; the kernel
  ring stays opaque (schema = userspace policy). Proven by `sbin/rpctest` (encode→validate + every
  rejection): serial `RPCTEST: PASS v=1 run_sz=392 res_sz=16 rej(len=1,ver=1,fld=1,enc=1,resver=1)`;
  no kernel change; MSGTEST + selftests still green; `p6afinal.png` clean. **P6a PUSHED** (`52da0c8`,
  ls-remote verified). **P6b (`f12cecc`): path-only TOOL_RUN runner — DISPATCH.** Self-spawning
  `sbin/toolrun`: agent sends a path-only `TOOL_RUN {/bin/free, args_len=0}` over a `CH_MSG` ctrl →
  runner validates (rejects args!=0/reserved!=0) + spawns `/bin/free` with stdout bound to a `CH_BYTE`
  channel → runner **drains 183 B of stdout** (the "stdout read") → `TOOL_RESULT {exit_code=0,
  stdout_handle}` returned + validated. Serial `RUNNER: PASS ... stdout_bytes=183` + `TOOLRUN: PASS
  sent=1 result=1 type=1 rid=1 valid=1 handle_nz=1`. All hard-no's held (no shell/parser/PATH/env/
  stdin/stderr/concurrency). **TRUST-BOUNDARY NOTE:** no up-transfer in CHANNEL-0, so the RUNNER reads
  stdout; `stdout_handle` is the runner's token; agent-side cross-process stdout-read is P6c. No kernel
  change; RPCTEST/MSGTEST/[CHAN] green; `p6bcheck.png` clean, 0 panic. **Next: P6c** — argv
  (NUL-separated, validated) + cross-process stdout delivery to the agent. P6b committed local, awaiting
  review/push.

## TERMINAL-0 — FROZEN / COMPLETE (T0–T4, pushed to origin `935f54f`) — CHANNEL-0 console output human-stable
- **branch:** `brick/terminal-0` (off `brick/channel-0`), **PUSHED** `git push origin brick/terminal-0`
  verified via `ls-remote` (remote == local HEAD `935f54f`) · **spec:** `docs/superpowers/specs/2026-06-09-terminal-0-design.md`
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
  `docs/dev-memory/bricks/TERMINAL-0.md`. **TERMINAL-0 FROZEN + PUSHED** — the rail is human-stable end
  to end (output-before-prompt → scrollback → line editing → ANSI colour), and `brick/terminal-0` is
  published on origin. **Next brick (chosen): AGENT-RPC-0 / CHANNEL-0 P5** — the human console can now
  display long output, colour, errors, and logs cleanly, so it's ready to host the typed agent rail
  (the chainlayer2 north star). Parked alternatives: `brick/usb-ehci-0` E3, latency/perf track.

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
