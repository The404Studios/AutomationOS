# brick record: IDE-REPAIR-0

> Four checkpointed fixes, no rewrite: the desktop debug overlay + raw-path
> titles (I0), the editor line-bleed that made typing impossible (I1), the
> autocomplete/snippet Tab conflict (I2), and the panel duplication/overlap
> vs the design mockup (I3). Diagnosed by 5 parallel read-only agents from
> the user's T410 photos; fixed serially, one commit + clean build +
> screenshot per checkpoint.

```yaml
brick: IDE-REPAIR-0
status: complete
branch: brick/ide-repair-0
base: brick/desktop-toast-0           # carries the welcome-toast removal (438e0c6)
request: >
  The IDE is "extremely bugged": UX/UI overlapping panels, the ability to code broken (autocomplete +
  rendering), structure wrong vs the design mockup; plus the desktop-wide "sbin/compositor" text and
  the flashing debug box. User-set checkpoints I0-I3, strictly ordered, ONE COMMIT + CLEAN BUILD +
  SCREENSHOT each. HARD NO's: no full IDE rewrite; no compiler-semantics changes; every draw path
  clipped; every visible debug string gated or removed; ASCII-only UI strings.
checkpoints:
  - id: I0
    title: hide debug overlays and normalize app titles
    commits: [7991d5b]   # + 438e0c6 (toast) on the base branch
    result: >
      THE sbin/compositor MYSTERY SOLVED: the big white changing text at (744,14) and the 48x48
      colour-cycling square at (1180,12) are the SCHED_DEBUG liveness heartbeat drawn by the TIMER
      ISR straight onto the framebuffer (pit.c) -- and quick_build.sh defaulted SCHED_DEBUG **ON**,
      so every kernel built without explicitly disabling it carried both artifacts over the desktop.
      SCHED_DEBUG is now DEFAULT OFF (opt-in SCHED_DEBUG=1). Titles normalized as defense-in-depth:
      the panel never displays a path-like title (product-name fallback), titlebars sanitize paths
      to their basename, the IDE is titled "Semantic IDE". All visible strings ASCII. Proof:
      screenshots/i0check.png -- clean top-right, panel reads Semantic IDE, 0 crashes.
  - id: I1
    title: editor renders one glyph per cell (the can't-code bug)
    commits: [1a17c1d]
    result: >
      a->src is a flat NOT-NUL-terminated buffer; both code render loops (ide_editor.c editor view +
      ide_codeview.c LEGO code view) passed the INTERIOR pointer to gfx_text_clip, which draws until
      a NUL and renders control bytes as advancing blanks -- every row painted the whole buffer
      suffix from its own line start (the diagonal smear; newlines/tabs as wide gaps; typed chars on
      multiple rows). The buffer was always correct; the drawing lied. Fix: a 1-char NUL'd stack
      local per cell, both loops. Proof: screenshots/i0check.png -- tower.c renders with exact
      per-line text. The autocomplete agent independently confirmed insertion offsets were always
      correct: the smear was purely this render bug.
  - id: I2
    title: autocomplete yields Tab/Enter to live snippet tab-stops
    commits: [20da381]
    result: >
      The engine was otherwise verified sound (single dispatch, exact single insertion with correct
      casing, caret-anchored opaque popup, Esc closes, arrows only navigate while open, bare Enter =
      newline). The one real defect: ed_ac_refresh re-opened the popup while a snippet's ${N} fields
      were live, so Tab accepted a completion instead of jumping fields, mangling the snippet.
      Completion is now suppressed while snippet_active. Manual T410 check: insert forr, type into
      field 1, Tab -> caret jumps to field 2.
  - id: I3
    title: panels stay inside their rects (LIB sidebar-only; compact runtime strip)
    commits: [4009e8e]
    result: >
      INSPECTOR: center + right sidebar both rendered panel_inspector with the SHARED insp_tab ->
      selecting LIB painted the full-width COMPLEX LIBRARY list across the center AND the sidebar.
      The center now save/restores insp_tab (the pattern ACTIONS/POTENTIALS already used); the click
      router mirrors it (also killing a latent click-inserts-snippet bug). RUNTIME: one renderer
      drawn twice (detailed view in the center AND crammed into the 5-row strip, where the coherence
      column's height math went negative -> the boundary crowding on every tab). panel_runtime now
      branches on rect height: strip = compact one-line summary, VIZ-3 center = full detail; the
      strip no longer clobbers the rt_hits table so center pill clicks hit correctly. Proof:
      clean build + i3check.png (editor unregressed); LEGO-mode visuals confirmed hands-on on T410.
  - id: diagnosis
    title: 5 parallel read-only agents from the T410 photos
    result: >
      editor-render / autocomplete / inspector-layout / runtime-duplication / title-text agents,
      each delivering root cause + exact minimal patches; cross-confirming (the autocomplete agent
      independently re-derived the I1 render bug and exonerated the insertion engine). QMP
      mouse-injection for LEGO-mode screenshots did not register (cursor never moved) -- not worth
      puppeteering; the T410 hands-on is the visual confirmation for I3's map-mode panels.
review:
  default_build_changed: true        # SCHED_DEBUG default flip (kernel flag, overlay code untouched)
  all_waits_bounded: true
  hardware_init_gated: n/a
  touches_userspace: true
  touches_kernel: false              # pit.c overlay still exists behind the flag; only the default moved
  preserves_known_good_t410: true
  smoke_proves_claim: true           # per-checkpoint builds + screenshots; typing proofs = T410 hands-on
  raw_pointers_or_truncation: none
verdict: pass
done: >
  IDE-REPAIR-0 COMPLETE: the desktop is free of debug overlays by default, titles are names not
  paths, the editor renders (and therefore types) correctly, autocomplete cannot mangle snippets,
  and the panels respect the mockup's boundaries. Five commits, each independently buildable.
next:
  - T410 hands-on: flash the refreshed ISO; verify typing (10 lines, Backspace/Enter), the forr
    snippet Tab-flow, VIZ-2 LIB sidebar-only, VIZ-3 single detail view + compact strip.
  - then (user order): net-stack Phase 1 -> E1000-PCH-0; parked: KERNEL-DIRECTMAP-AUDIT-0.
```
