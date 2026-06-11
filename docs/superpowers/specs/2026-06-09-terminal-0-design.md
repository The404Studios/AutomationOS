# TERMINAL-0 Design — make CHANNEL-0 console output human-stable

**Status:** approved direction (2026-06-09). **Branch:** `brick/terminal-0` (off `brick/channel-0`).
**Goal:** CHANNEL-0 P4 makes external output appear in the terminal, but it's messy — output lands
*after* the re-printed prompt, there's no scrollback, line editing is half-built, and escape codes
render as boxes. TERMINAL-0 makes that output **human-stable** before we build the typed agent rail
(AGENT-RPC-0) on top of it — because long tool output, compiler errors, and logs must display
cleanly or they'll confuse both the human and the agent's logs.

> Do this BEFORE P5/P6. Parked: AGENT-RPC-0, job control, pipes, `Ctrl-C`, hubs.

## The checkpoints (one commit each, self-test after each)

### T0 — output-before-next-prompt (waitpid-lite)  ← highest value, do first
**Problem (grounded):** in `terminal_m3.c`, the Enter handler calls `shell_execute(line_buf)` then
**immediately re-prints the prompt** (`shell_prompt()`), and the bound child's output only drains in
later frames of the main loop — so it appears *after* the next prompt.
**Fix:** when `shell_execute` spawned a bound child (`g_child_ch > 0`), **defer the prompt**. Set
`g_await_child = 1` instead of re-prompting. In the main loop, after the P4 drain, poll
**`SYS_WAITPID(g_child_pid, &status, WNOHANG)`** (non-blocking — never park the GUI). When the child
has exited *and* the channel is drained empty: do a final drain, `ch_close`, re-print the prompt,
clear `g_await_child`. **Safety:** bound the await (e.g. re-prompt anyway after ~N seconds of frames)
so a long-running child can't freeze the prompt forever; keep draining it in the background.
Needs `SYS_WAITPID` (=6) + `WNOHANG` (=1) in the terminal.

### T1 — scrollback ring
A fixed off-screen line ring (e.g. 500 lines) behind the 25-row viewport; `grid_scroll_up` pushes the
evicted top line into the ring instead of discarding it. Page-up/page-down (and mouse wheel if cheap)
scroll the viewport over the ring. Fixes the "output > 25 rows is lost forever" defect.

### T2 — line editing cleanup
Add an intra-line `cursor_pos`: Left/Right/Home/End/Delete (kernel already forwards these keycodes),
insert/backspace at the cursor (memmove), Ctrl-U/A/E. Remove the per-keystroke `[TERM] key` serial
spam; alias Right-Ctrl / keypad-Enter. (From the 8-agent input findings.)

### T3 — minimal ANSI/VT parser
A small CSI state machine in `grid_putchar`: at minimum **SGR color/bold/reverse** (`\033[...m`),
which maps directly onto the already-present `grid_color[][]`/`g_cur_color` substrate (the cheapest
win), plus erase (`J`/`K`) and basic cursor moves. Bound the param collector (no overflow on a
malicious `cat`). Makes `ls --color`, colored compiler errors, and simple TUIs render.

### T4 — delete dead terminal code
Remove the ~3,000 lines of verified-unlinked duplicates (`apps/terminal/{terminal,renderer,vt_parser,
buffer,pty_impl,tabs,profiles,utils,font_integration}.c`; `userspace/terminal/*`;
`userspace/shell/{textshell,shell_minimal,parser,minimal/}*`) once the stale builder scripts that
reference build-output paths are retired. (Delete/keep table in the 8-agent cleanup report.)

## Acceptance test
1. `/bin/free` output appears **before** the next prompt.
2. `echo hello` (builtin) displays cleanly; a non-builtin like `free`/`lspci` displays cleanly.
3. `cc <bad>.c` error output displays cleanly (no prompt interleaving).
4. Scrollback preserves output beyond 25 rows.
5. Builtins still work.
6. P1–P4 CHANNEL-0 self-tests still PASS (`[CHAN] selftest PASS`, `[CHAN] p2 selftest PASS`).
7. Desktop reaches with 0 panic; default build unchanged.

## Model rule (unchanged)
The terminal is the **holder + renderer**; the kernel stays the dumb ring. TERMINAL-0 is *all
userspace* (`terminal_m3.c`) — it does **not** touch the kernel CHANNEL-0 primitive. Verify each T
with `build_all.sh` + a boot/screenshot; CHANNEL-0's kernel self-tests are the no-regression gate.
