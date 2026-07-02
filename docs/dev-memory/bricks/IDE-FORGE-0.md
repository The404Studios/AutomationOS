# brick record: IDE-SHOWCASE-0 + IDE-FORGE-0 (frozen together)

> The visual/UX milestone: first the showcase that photographed the Semantic
> LEGO IDE honestly (and caught two real bugs on camera), then the six-agent
> FORGE wave that gave the aphantasia mission its missing organs — knobs
> wired to semantic objects, an automation deck, the project pulse, teaching
> dictionaries, validated application templates, and the blueprint→code
> GENERATE loop proven in-frame.

```yaml
bricks: IDE-SHOWCASE-0 + IDE-FORGE-0
status: complete (frozen together, user-approved)
branch: brick/ide-forge-0 (pushed ee1efd9, ls-remote verified)
commits: [cfad456 showcase, a463d54 forge, ee1efd9 regression refresh]  # unsquashed (user: "the screenshot refresh is part of the proof")
proofs:
  showcase: docs/IDE_SEMANTIC_LEGO.md + screenshots/showcase_*.png (12) + build_test/ide_showcase_shots.sh
  forge:    screenshots/forge_*.png (9) + build_test/ide_forge_shots.sh
headline_frames:
  - forge_dict.png          # 'sys' -> syscall builtin + full teaching pane
  - forge_map_generated.png # G generated claim_slot: spliced call, LINES 76->84, COH 75->100
  - forge_pulse_done.png    # Pulse counts the MARK knob flip; KNOBS sidebar live
```

## IDE-SHOWCASE-0 (cfad456)

The sell-doc (docs/IDE_SEMANTIC_LEGO.md): the aphantasia why, the five design
laws, the 12-shot machine tour, the architecture and the novelty pitch, the
honest-status section. Capturing it doubled as an audit that found and fixed:
- **Esc was a silent data-loss quit** (global ESC=exit above the editor
  routing; the camera photographed the death). Now: editor Esc = popup-close
  else inert; LEGO Esc exits unless dirty (then jumps to the editor).
- **Anonymous diagnostics** → the cc names identifiers, which proved the
  on-device compiler skips ALL preprocessor lines (#define AND #include).

## IDE-FORGE-0 (a463d54) — the six-agent wave

Five parallel agents (audit / dictionaries / templates / graph spec /
aphantasia-UX spec) + one implementer, file-sharded, then serial integration
under the binding constraints (docs/superpowers/specs/2026-06-10-ide-forge-
constraints.md: one selection model · every control's consequence visible
elsewhere · clip rects/ASCII/keyboard-first · no per-frame idle work · T410
budget).

**Landed:**
- `ide_marks.c/.h` — per-symbol DONE/STAR/ISOLATE/MUTE, persisted
  (SYS_PERSIST diskfs + fallback), surfaced as the MARK inspector tab with
  live toggles. Consequences: FUNCTIONS-list OK glyph + starred-first order,
  chrome WATCH chips (clickable), the focus-lock ISOLATE that the old legend
  only pretended existed, muted-warn chip + pulse exclusion.
- **ACTIONS deck** (VIZ-4, was a placeholder duplicate): Build/Run/
  Generate-all/Save-all/Re-analyze/Open-TODO rows with result chips and
  "Ns ago" stamps — the row IS the memory of the last run.
- **PROJECT PULSE** (VIZ-5, same placeholder): Functions/Done/To-do/
  Missing-cards/Warnings/COH + ring gauge + a REAL 10-sample COH history
  (the fake trend is gone) + the clickable !done checklist.
- **Teaching dictionaries** (289 entries: ide_dict_c/asm/os.h) merged into
  the completion engine ahead of bare keywords; every entry renders
  sig+doc+snippet in the preview pane. The OS table IS the header for
  on-device coding: the cc reaches the kernel ONLY via its `syscall(n,a1,a2,
  a3)` builtin (no inline asm on-device) — every number verified.
- **Six application templates** (login/todo/guess/clock/calc/kvstore),
  validated through a HOST HARNESS of the real cc→as→elf pipeline — which
  discovered the cc DROPS global initializers and MIS-COMPILES arrays; the
  templates use code-as-table + bitmask designs that actually work, with the
  why in every banner. Staged into the + NEW gallery.
- **GENERATE fixed and proven**: compilable `(void)` stubs (was invalid
  `(...)`); on camera G generated `claim_slot` — call spliced, stub
  appended, LINES 76→84, COH 75%→100%.
- **The chrome tells the truth**: the 11-verb lying legend → the three real
  keys; sidebar decoupled from the center (never duplicates); map node chips
  (N ln / R W C / holes); LIVE-WARNINGS wording fixed; the icon ghost fixed
  at root (open-fade outlasted the 3-frame full-damage cooldown → 24).

**Regression:** the original 12-shot tour re-run against the FORGE build —
12/12, 0 crashes, tab strip clean in every frame.

## Parked (user-set): COMPOSITOR-ICON-GHOST-0

One outer-titlebar icon residue survives (the window-frame area is
compositor-drawn, never client-committed, so the last open-fade frames can
strand wallpaper-layer pixels there). Tiny dedicated brick: eliminate it,
prove with the IDE-autostart tour, NO IDE changes, NO compositor redesign.
Candidate fixes recorded: cover the entire fade duration, or move icon
cell 0 out of the default window-spawn zone. Does not block this freeze.

## Minor open

Post-GENERATE focus lands on an unexpected symbol (cosmetic; the loop proof
is unaffected). The giant compartment-map spec
(2026-06-10-ide-giant-map-and-generate-spec.md) and the remaining knob ideas
are implementation-ready for a future IDE brick.
