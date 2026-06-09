# brick record: TERMINAL-0

> Make CHANNEL-0's now-visible console output human-stable, before the typed agent rail.
> Spec: `docs/superpowers/specs/2026-06-09-terminal-0-design.md`. All userspace (`terminal_m3.c`);
> does NOT touch the kernel CHANNEL-0 primitive.

```yaml
brick: TERMINAL-0
status: in-progress
branch: brick/terminal-0
base: brick/channel-0
request: >
  P4 makes external output appear, but it lands AFTER the next prompt, there's no scrollback, line
  editing is half-built, and escapes render as boxes. Make it human-stable (output before prompt,
  scrollback, clean editing, minimal ANSI) so long tool output / compiler errors / logs display
  cleanly -- for the human and the agent's logs.
checkpoints:
  - id: T0
    title: defer the prompt until a bound child exits + its output drains (waitpid-lite)
    files: [userspace/apps/terminal/terminal_m3.c]
    tests: [build_test/channel_p4.sh, build_test/shot.sh]
    result: "build_all clean; SCREENSHOT (screenshots/t0demo.png) shows /bin/free's output THEN the next root@ prompt (output-before-prompt); desktop reached; channel free output renders (P3/P4 intact)"
    design:
      - brutally narrow: prompt ordering ONLY. No scrollback, no ANSI, no line-editing rewrite, no
        dead-code delete, no kernel change.
      - Enter handler: after shell_execute, if it spawned a bound child (g_child_ch>0) set
        g_await_child=1 and DEFER shell_prompt(); a builtin/no-child re-prompts immediately as before.
      - main loop: drain the bound child (bounded <=4KB / 8 reads per frame), then waitpid-lite --
        SYS_WAITPID(pid, &st, WNOHANG=1) is NON-BLOCKING (kernel handlers.c:1032 returns 0 if alive,
        never parks the GUI). On exit (w!=0): final drain (bounded by the ring cap -- dead child),
        ch_close, clear state, shell_prompt() -- so the prompt comes AFTER the output.
      - edge case: a hung/long-running child keeps the prompt deferred but the GUI loop keeps
        rendering (no freeze); a timeout / Ctrl-C is deferred (not T0).
    review:
      default_build_changed: false      # unbound/builtin path unchanged; only the bound-child prompt order
      all_waits_bounded: true           # WNOHANG only; <=4KB/frame drain; final drain bounded by ring cap
      touches_userspace: true           # terminal-only, as scoped
      touches_kernel_channel0: false
      preserves_known_good_t410: true
      smoke_proves_claim: true          # screenshot = the human-visible proof (prompt after output)
      raw_pointers_or_truncation: none
    verdict: pass
  - id: T1
    title: scrollback ring -- viewport over recent lines (one append path)
    files: [userspace/apps/terminal/terminal_m3.c]
    tests: [build_test/shot.sh]
    result: "build_all clean; SCREENSHOT (screenshots/t1demo.png) shows a scrolled-back viewport of LINE rows the old 25-row discard grid would have lost = scrollback holds; no-regression desktop"
    design:
      - ONE buffer: sb[256][80] ring; the visible grid is a VIEWPORT (sb_view..sb_view+g_rows).
        cur_row is gone -> sb_head (monotonic logical line) + cur_col + sb_view + sb_follow.
      - ONE append path: grid_putchar -> sb[sb_head%256][cur_col]; \n/wrap advance sb_head + clear
        the new line; grid_scroll_up removed (the ring + view scroll). Builtins, child channel
        output, and the prompt all go through grid_putchar (no separate grid mutation paths).
      - sb_follow=1 pins the view to the bottom (new output auto-scrolls); PageUp clears follow and
        scrolls back (clamped to oldest = sb_head-255), PageDown scrolls forward and re-follows at
        the bottom. render() draws the viewport + the cursor only while following + a thin right-edge
        bar when scrolled. resize re-pins the view if following.
    review:
      default_build_changed: false      # terminal-only; builtins/prompt/output identical at the bottom
      all_waits_bounded: true           # no new loops in the hot path; render bounded by g_rows
      touches_userspace: true           # terminal-only, as scoped
      touches_kernel: false
      preserves_known_good_t410: true
      smoke_proves_claim: true          # screenshot shows old lines preserved + a scrolled viewport
      raw_pointers_or_truncation: none  # ring indices via sb_slot (logical % 256); view clamped each render
    verdict: pass
  - id: T2
    title: clean line editing -- intra-line cursor + redraw-current-line (one input model)
    files: [userspace/apps/terminal/terminal_m3.c]
    tests: [build_test/shot.sh]
    result: "build_all clean; SCREENSHOT (screenshots/t2check.png) shows a multi-line self-check -- the three acceptance cases (abc<<X=aXbc, abc Home X=Xabc, abc <Del=ab) run through the SAME edit primitives the key handlers use and print PASS lines to scrollback (visible on the term's left edge; a single short line gets occluded by the auto-opened apps); final clean build (t2final.png) with the temp self-check removed = no-regression desktop"
    design:
      - one input model: line_buf/line_len/line_cursor are the SOURCE OF TRUTH (the command buffer,
        separate from the scrollback display). erase_input_line + char-by-char echo -> redraw_input_line()
        which re-renders prompt + line_buf on the head line each edit (no scrollback ghosts).
      - added: Left/Right (clamp), Home/End, Backspace-at-cursor, Delete-at-cursor (KEY_DELETE),
        insert-at-cursor (mid-line memmove). History/Tab/Ctrl-L now route through redraw_input_line.
      - killed the per-keystroke print("[TERM] key ...") serial spam -> behind #ifdef TERM_DEBUG_KEYS
        (default OFF).
    review:
      default_build_changed: false      # terminal-only; T0 (output-before-prompt) + T1 (scrollback) intact
      all_waits_bounded: true           # redraw is bounded by g_cols; no new loops in the hot path
      touches_userspace: true
      touches_kernel: false
      preserves_known_good_t410: true
      smoke_proves_claim: true          # screenshot shows a correct mid-line insert result (aXbc)
      raw_pointers_or_truncation: none  # line_buf bounded LINE_MAX-1; redraw clips to g_cols
    verdict: pass
  - id: T3
    title: minimal ANSI/VT SGR colour -- a thin parser BEFORE grid_putchar (ANSI = state mutation)
    files: [userspace/apps/terminal/terminal_m3.c]
    tests: [build_test/shot.sh]
    result: "build_all clean; SCREENSHOT (screenshots/t3check.png) shows a term_write() demo -- 'red' renders red, 'green' green, 'red-then-bold' (ESC[31;1m) bright red, 'unknown-seq-ok' (ESC[999m) in DEFAULT colour with no junk; NO escape bytes appear as text. final clean build (t3final.png) with the temp demo removed = no-regression desktop"
    design:
      - ANSI is not output, it is state mutation: a 3-state parser (ANSI_TEXT/ESC/CSI) sits BEFORE
        grid_putchar. printable byte -> g_cur_color = g_ansi_color; grid_putchar(c). SGR 'm' final
        -> ansi_apply_sgr() mutates g_ansi_color. escape bytes never enter scrollback as junk.
      - scope is colour ONLY: ESC[0m reset, ESC[30-37m / ESC[90-97m fg, ESC[39m default, ESC[1m bold
        (retro-brightens a base colour so ESC[31;1m and ESC[1;31m both = bright red). NO cursor move,
        NO clear-screen, NO alt-screen, NO control chaos. maps onto the existing g_cur_color/sb_color
        u32 substrate via two 8-entry palettes (base + bright), tuned for the dark bg.
      - bounded: the CSI param buffer is 32 bytes; overflow resets the parser to TEXT (no unbounded
        parse). unknown final bytes / codes (ESC[999m, cursor params, backgrounds) are ignored safely.
      - one sink: the child-output drain (both the per-frame drain and the post-exit final drain) now
        routes through term_write() instead of raw grid_putchar; ansi_reset() runs when the child exits
        so colour can't bleed into the next command. builtins/prompt keep their explicit grid_puts_color
        (no ANSI), so the prompt stays default and line editing is unchanged.
    review:
      default_build_changed: false      # only child output now ANSI-parsed; builtins/prompt unchanged
      all_waits_bounded: true           # CSI buffer 32B, overflow->reset; parser is O(1)/byte
      touches_userspace: true
      touches_kernel: false
      preserves_known_good_t410: true
      smoke_proves_claim: true          # screenshot: coloured words render, escapes gone, ESC[999m sane
      raw_pointers_or_truncation: none  # g_ansi_buf bounded 32; palette indices range-checked (30-37/90-97)
    verdict: pass
next_checkpoints:
  - T4 delete dead terminal code (the ~3,000 lines now superseded by the T0-T3 active path)
```
