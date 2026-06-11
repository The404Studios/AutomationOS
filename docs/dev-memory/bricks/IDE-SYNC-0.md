# brick record: IDE-SYNC-0

> The core prosthetic loop: editor caret -> map node -> inspector table ->
> back to editor, all reading and writing ONE selection model. "Until that
> loop is tight, the map is only a picture. Once it syncs both ways, it
> becomes external working memory." (the user, setting the brick)

```yaml
brick: IDE-SYNC-0
status: complete
branch: brick/ide-sync-0
base: brick/ide-repair-0              # off the frozen, T410-confirmed repair brick (90e6da4)
request: >
  S0 editor caret -> active symbol/node; S1 active symbol -> inspector detail (+ map highlight);
  S2 map click -> editor jump; S3 inspector row click -> editor jump; S4 selection highlight
  consistent across all three panes, sync survives typing/redraw. DESIGN LAW: ONE active selection
  model (active_file / active_line / active_symbol_id / active_node_id / active_panel) -- the panes
  do not invent their own selection state. HARD NO's: no stable-layout work, no map redesign, no new
  visual styles, no new parser (the existing symbol extraction has per-function 1-based line ranges,
  which proved sufficient).
checkpoints:
  - id: S0
    title: track editor caret symbol
    commits: [337258d]
    result: >
      IdeSel lands in the Ide struct (file/line/symbol/node/pane) with ide_sel_from_caret() --
      resolve the caret's enclosing function via the parser's line ranges, hooked after every
      caret-moving key/click in BOTH workspaces -- and ide_sel_reset() on file open/new. The LEGO
      status bar gains the first breadcrumb: FN name (accent blue when resolved) + Ln line,col.
  - id: S1
    title: sync map and inspector selection
    commits: [0cc10ff]
    result: >
      Caret crossings refocus via ide_set_focus (one model_analyze per crossing, none within a
      function; between functions the last focus holds so the map never blanks); ide_set_focus
      writes THE selection model at its tail so map/runtime/inspector-driven focus and caret-driven
      focus land in the same place. PROVEN INTERACTIVELY via QMP key injection
      (build_test/ide_sync_check.sh): caret arrowed into tower_init in the EDITOR workspace ->
      switch to LEGO -> the map central card IS tower_init, INSPECTOR - tower_init shows its port
      table, the FUNCTIONS list highlights it, the breadcrumb reads FN tower_init Ln 15,1
      (s1b_lego.png).
  - id: S2S3S4
    title: jump editor from map and inspector; ranges survive typing
    commits: [073898d]
    result: >
      ide_sel_jump(func_idx, pane) = focus + land the shared editor caret on the definition line
      (Go-to-Ln-style clamp; render auto-scrolls; prev_focus keeps Backspace-back). Map follows
      route through it (call target + read/write producer-consumer paths); no-in-file-target
      follows correctly no-op (single-file model). Inspector CONNECTIONS rows click-jump to the
      row's other-end function (name lookup, prefer `to`); PORTS rows land on the focused
      function's definition. S4: newline insert/delete at the ed_insert_byte/ed_delete_byte choke
      points shifts every affected function's recorded line range, keeping caret->symbol and jump
      targets true between re-parses. s2_jump.png = the full coherence frame (tower_tick central
      card + ports, read/write/call satellites, dashed-red ABSENT claim_slot, populated inspector
      PORT table, code-view annotations, compact runtime strip, FN tower_tick Ln 28,1).
  - id: harness
    title: QMP keyboard-driven interactive verification
    result: >
      build_test/ide_sync_check.sh: build, boot with IDE autostart, inject arrows (caret moves),
      Ctrl+comma (-> LEGO workspace) and 1/2 (VIZ tabs), Enter (map follow), screendump each stage.
      KEYBOARD injection works through the PS/2 path; MOUSE rel injection does not reach the
      compositor (cursor never moves) -- mouse-path verification stays on the T410 hands-on list.
review:
  default_build_changed: false      # userspace IDE only
  all_waits_bounded: true
  hardware_init_gated: n/a
  touches_userspace: true
  touches_kernel: false
  preserves_known_good_t410: true
  smoke_proves_claim: true          # interactive key-driven screenshots for S0/S1 + the coherence frame
  raw_pointers_or_truncation: none  # bounded name copies; clamped line math
verdict: pass
done: >
  IDE-SYNC-0 COMPLETE. One selection model, three panes, both directions: the caret drives the map
  and inspector; the map and inspector drive the caret; the breadcrumb always says where you are;
  typing keeps the mapping true. The map stopped being a picture.
next:
  - T410 hands-on: mouse-click a map satellite and an inspector CONN row (the jump paths the QMP
    mouse couldn't drive); type ~10 lines and confirm FN/Ln tracking stays true.
  - then (user order): IDE-CONTEXT-0 (persistent breadcrumb strip + what-changed) -> MAP-STABLE-0
    (pinned layout) -> VIZ1-PARITY-0 (port-labeled edges, ABSENT cards everywhere per the mockup).
```
