# IDE-FORGE-0 — binding integration constraints (user-set)

Every design coming out of the 5-agent wave is filtered through these three
constraints before any line lands in ide.c. A feature that cannot satisfy
all three is cut or deferred, not bent.

## 1. THE LOOP (the IDE-SYNC-0 law stays absolute)

- ONE selection model. Every new widget — knob strips, action deck rows,
  compartment chips, generated-stub jumps — READS and WRITES the same
  `active file / line / symbol / node / panel` record. No panel may grow
  private selection state. A knob bound to "the selected function" means
  bound to the model's active symbol, so flipping it from the map, the
  inspector, or the editor caret is the same operation.
- Every mutation closes the loop visibly: GENERATE from an absent card must
  end with the re-parse running and the card flipping to a real brick in
  the SAME interaction; a done-switch must move the pulse numbers in the
  same frame. A control whose consequence is not visible somewhere else on
  screen is decoration and is rejected.
- The build→run→see loop stays one keystroke deep from anywhere (B / R
  global in the LEGO workspace; Ctrl+B / Run in the editor).

## 2. UI/UX (the IDE-REPAIR-0 / I3 layout laws stay absolute)

- Every new draw path gets a clip rect; every section stays inside its
  rect; no overlap regressions (the I3 acceptance).
- ASCII-only visible strings (the 8x16 font has no UTF-8).
- Keyboard-first: every new control operable via the existing key
  vocabulary (arrows / Enter / digits / letter mnemonics) because QMP can
  only prove keyboards; mouse paths follow the existing click-router
  patterns and are T410-hands-on items.
- New panels reuse panel_settings' proven widget vocabulary (slider /
  toggle / button row) — no third widget language.
- The status spine (breadcrumb + state chips) remains the single always-
  visible truth strip; new chrome may extend it, never duplicate it.

## 3. T410 (the real-hardware budget stays absolute)

- Single core, cooperative, UC-prone framebuffer: NO new per-frame work
  when idle. Knob strips, pulse counters and action decks render on state
  change only (the existing dirty discipline); nothing polls per frame.
- No full-recomposite storms: new IDE chrome redraws inside the IDE's own
  buffer; compositor-side damage behavior is untouched by this brick.
- Bounded everything: any new loop (template self-tests, generate-all
  sweeps) is iteration-capped; no wall-clock waits in UI paths.
- ASCII + 1180-wide layout discipline (the validated T410 geometry); any
  feature that needs more space collapses gracefully at the validated
  resolution.
- Anything that cannot be proven in QEMU (mouse feel, UC-FB latency of the
  new panels) is listed in the brick record as a T410 hands-on item, per
  the gate-untestable-hardware law.

## Acceptance shape for the integration

QMP screenshot pairs proving loop closure (knob flip -> pulse change;
GENERATE -> card flip; template -> build PASS on-device), zero overlap
regressions in every existing showcase frame, and the default desktop ISO
byte-behavior unchanged outside the IDE binary.
