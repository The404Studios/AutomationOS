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
next_checkpoints:
  - T1 scrollback ring | T2 line-editing cleanup | T3 minimal ANSI/VT (SGR color first) | T4 delete dead terminal code
```
