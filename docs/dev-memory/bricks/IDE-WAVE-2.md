# brick record: IDE-WAVE-2 (IDE-CONTEXT-0 + MAP-STABLE-0 + VIZ1-PARITY-0 + IDE-XFILE-0)

> Four aphantasia-driven bricks designed in parallel (4 read-only agents),
> implemented serially in the user's order. North star: "nothing important
> should have to be imagined" -- the map/inspector/breadcrumb ARE the user's
> externalized working memory.

```yaml
brick: IDE-WAVE-2
status: complete
branches: >
  brick/ide-context-0 -> brick/map-stable-0 -> brick/viz1-parity-0 -> brick/ide-xfile-0
  (a serial chain off the frozen brick/ide-sync-0 HEAD 0f321fe; each brick's branch
  starts at the previous brick's HEAD)
request: >
  "perfect its working good. also keep going and ultrathink run 4 agents and continue
  working on the IDE" -- after the T410-confirmed IDE-SYNC-0. Order: CONTEXT -> STABLE ->
  PARITY, with XFILE slotted by its scope assessment (it became its own two-checkpoint brick).
checkpoints:
  - id: IDE-CONTEXT-0
    title: persistent breadcrumb + what-changed indicator
    commits: [06f7787, 9502f2d]
    result: >
      ONE breadcrumb spine (ide_breadcrumb_prefix: "project > file") rendered by BOTH
      workspaces' status bars, appended with "> FN Ln l,c"; edits_since_save wired at the
      ed_insert_byte/ed_delete_byte choke points (reset on open/save) renders the
      what-changed star. Proof ctxfix_ed.png: "src > tower.c > tower_tick Ln 28, Col 1".
      EN-ROUTE KERNEL-CLASS BUG (06f7787): scan_dir's per-recursion-level
      `IdeDirent ents[64]` (~18 KB/level) crossed the ~64 KB mapped user stack at depth
      1-2; sys_readdir's copy_to_user EFAULTed and ide_list_dir read it as end-of-dir ->
      a deterministic BLANK IDE whose appearance flipped with ±450 B of unrelated code
      (frame-layout sensitive -- why the bisect probes were maddening). Fix = static
      buffer (safe: pass 1 drains ents into a->entries before pass 2 recurses). Same
      family as the earlier count_boxes cliff; rule stands: NO large stack locals in
      recursive userspace code.
  - id: MAP-STABLE-0
    title: deterministic map -- same function, same place
    commits: [c68c310]
    result: >
      The design agent's audit found layout ALREADY deterministic (parse order = source
      order; satellite geometry pure functions of the model). The one real bug: the
      pan/zoom offsets (map_ox/map_oy) are shared mutable state that survived focus
      changes, so "same function" could appear anywhere. Fix = 2 lines in ide_set_focus
      (reset pan on focus change) + order-contract comments at the layout loops so a
      future sort doesn't silently break the law.
  - id: VIZ1-PARITY-0
    title: the map matches the user's mockup
    commits: [5859843]
    result: >
      Satellites become 2-line cards (MAP_SAT_H 46, MapSat.sub[56]): name + "Type: <ty>"
      for globals / "Ports (N)" for resolved functions / "(extern)" for unresolved;
      connector studs on the wires; "CLICK TO CLOSE (IDA STYLE)" hint on the central
      card; the read/write/call/absent chip legend. viz1_lego.png matches the mockup
      frame the user drew.
  - id: IDE-XFILE-0a
    title: whole-directory model, current-file filters, guarded follows
    commits: [334fb52]
    result: >
      model_parse split into model_reset + model_parse_append (legacy wrapper kept);
      ide_parse_project_model parses every sibling .c (cap 24 files / M_MAXFUNCS funcs,
      static scratch + static dirents -- the stack-cliff lesson applied) then the OPEN
      file LAST so cur_file describes it. Editor-coupled views (funcs panel, caret->
      symbol, edit line-shift) filter Func.file == cur_file so the UX is unchanged;
      sibling functions resolve on the map ("Ports (N)" instead of "(extern)") and
      INCOMING cross-file callers appear (game_main() -> on tower_tick's map). Follows
      guarded: cross-file targets no-op rather than desync the editor.
  - id: IDE-XFILE-0b
    title: cross-file follow -- open the sibling, then jump
    commits: [7dbd819]
    result: >
      ide_sel_jump_xfile(name, fbase): copy inputs to locals (callers pass pointers into
      the model, which the reopen WIPES), open dirname(cur_file)/fbase, re-find by
      name+file in the rebuilt model, ide_sel_jump. map_sat_follow flips its guards:
      CALL prefers a same-file definition (static-name collisions stay local), else
      opens the sibling; READ/WRITE = two passes (same-file producer first, sibling
      fallback). PROOF xfile0b2_xjump.png: follow the game_main() caller satellite from
      tower_tick -> breadcrumb flips "src > tower.c > tower_tick" to "src > main.c >
      game_main Ln 12,1", funcs panel/explorer/map/inspector all rebuilt around main.c.
  - id: harness
    title: per-brick QMP verification, extended
    result: >
      build_test/ide_sync_check.sh ran per checkpoint (kernel quick_build + IDE=1
      build_all + boot + key injection + 4 screendumps). HARNESS FINDING: the original
      "right+ret" follow step had ALWAYS selected the g_enemies READ satellite, whose
      follow finds no producer -> the jump frame was a no-op in EVERY prior run (the S2
      proof had come from the central-card/breadcrumb frames, not that step). Added the
      xjump step: "left" selects the only node left of the reads column (the incoming
      caller satellite) and "ret" exercises the real cross-file follow.
review:
  default_build_changed: false      # userspace IDE only; kernel untouched all wave
  all_waits_bounded: true
  hardware_init_gated: n/a
  touches_userspace: true
  touches_kernel: false
  preserves_known_good_t410: true
  smoke_proves_claim: true          # failing-then-passing on XFILE (0a guard no-op frame, 0b flip frame)
  raw_pointers_or_truncation: >
    the 0b copy-before-reopen rule is load-bearing: every pointer into the model dies at
    reopen; ide_sel_jump_xfile copies name+file to locals first.
verdict: pass
done: >
  IDE-WAVE-2 COMPLETE. Where-am-I never leaves the screen (breadcrumb + what-changed);
  the map is deterministic (same function, same place); the map matches the mockup; and
  the model stopped being single-file -- satellites resolve across the directory and
  following one OPENS the sibling and lands the caret, with every pane rebuilt in sync.
next:
  - T410 hands-on for the wave (mouse paths the QMP harness cannot drive), then push the
    four branches on the user's word.
  - deferred from the XFILE design: globals-file mislabeling (extern-first dedupe),
    static-name collisions across files, recursive subdirectories, .asm siblings.
  - then (user order): net-stack Phase 1 -> E1000-PCH-0.
```
