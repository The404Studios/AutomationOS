# IDE GIANT MAP + BLUEPRINT→CODE GENERATE — Design Spec

**Status:** design (not implemented). Owner: GRAPH agent. **Date:** 2026-06-10.
**Scope:** the VIZ-1 SEMANTIC LEGO MAP (`userspace/apps/ide/ide_map.c`), the
generation engine (`ide_gen.c`), and the map-side wiring in `ide.c`. The ACTIONS
panel UX is owned by another agent; this spec only fixes the **map-side semantics**
and the shared data those automations read.

> Aphantasia laws this must obey (`docs/IDE_SEMANTIC_LEGO.md:28-44`): **stable
> layout** (a function/file lives in the SAME place every visit — position is a
> pure function of model index, NEVER force-directed), **nothing only in your
> head** (every hole/flow/health is drawn), **verbal+spatial twins** (every box
> has a breadcrumb/table row), **one selection model** (`a->sel`, panes never own
> private selection state).

---

## 0. Ground truth — what exists today (cited)

### 0.1 The map is single-function-centric, with a flat "file overview"
- `panel_map()` (`ide_map.c:307`) renders **one focus function** as a central card
  + satellites when `a->focus_func >= 0`. When `focus_func < 0` it renders a **flat
  tile grid of every code element** (includes/macros/records/globals/protos/funcs)
  — `ide_map.c:363-485`. There is **no file compartment concept** today; the
  overview mixes all files' elements into one ungrouped grid keyed by
  `tile_idx` in fixed section order (`ide_map.c:388-396`).
- Satellite geometry is recorded in a file-static table `map_sats[]` / `map_nsats`
  (`ide_map.c:271-272`), struct `MapSat` (`ide_map.c:251-262`), pushed by
  `map_sat_push()` (`ide_map.c:280`). Two-pass render: edges then cards
  (`ide_map.c:702-847`). The same table is read back by `panel_map_click()`
  (`ide_map.c:945`) and the keyboard nav.
- Keyboard: arrows → `map_nav(a,dir)` (`ide_map.c:966`), Enter → `map_activate(a)`
  (`ide_map.c:997`), follow logic `map_sat_follow()` (`ide_map.c:878`). Dispatched
  in `ide.c:2756-2794`. **Backspace** today: focus→prev_focus→overview
  (`ide.c:2785-2794`). **Tab** cycles functions (`ide.c:2775-2784`).
- The whole-directory model is built by `ide_parse_project_model()`
  (`ide.c:662-685`): `model_reset` then `model_parse_append` each sibling `.c`,
  the open file last. So `m->funcs[i].file` distinguishes files and **the file
  order is deterministic** (directory order from `ide_list_dir`, open file last).

### 0.2 What G GENERATE does today — REAL but generic, ignores the selected card
`KEY_G` handler — `ide.c:2795-2800`:
```c
case KEY_G:
    if (a->model.nactions > 0 && gen_apply_action(a, 0)) {   // <-- always action[0]
        ide_parse_project_model(a);     // IDE-XFILE-0 whole-dir re-parse
        ide_set_focus(a, a->focus_func);
    }
    break;
```
`gen_apply_action(a, idx)` — `ide_gen.c:105-204` — is a real, span-driven editor, **not** a stub:
1. Reads `a->model.actions[idx].title` (`ide_gen.c:115`).
2. Picks helper name/ret/body by **keyword match on the title** — `claim`→`claim_slot`
   returning `find_free_bullet_slot();`, `cooldown`→`cooldown_gate`, etc.
   (`ide_gen.c:121-141`). Fallback: sanitize the title to an identifier, `int … return 0;`.
3. **EDIT 1** — inserts `"\n    <name>();"` just after the focus body's `{`, at
   `fbody->span.start_off + 1`, via `text_splice` (`ide_gen.c:151-163`). `fbody` is
   found with `ast_find_func()` + `gen_find_body()` (`ide_gen.c:143-149,95-101`).
4. **EDIT 2** — appends `"\n\n/* generated… */\nstatic <ret> <name>(...) {\n    <body>\n}\n"`
   at EOF via `text_splice` (`ide_gen.c:166-180`). **Note the signature is the
   variadic placeholder `(...)` — params are NOT inferred.**
5. Persists `ide_write_file(a->cur_file,…)` (`ide_gen.c:186`); re-parses **single-file**
   `model_parse` + `model_analyze` (`ide_gen.c:189-202`) — the `ide.c:2797` caller
   then re-does the whole-dir parse.

**Verdict on G:** REAL (it really edits + persists + re-parses) but (a) it always
applies `actions[0]`, never the **selected** absent card; (b) it emits a `(...)`
signature, never inferred params; (c) it only ever edits the **focus file**.

### 0.3 The dashed-red ABSENT cards are SYNTHETIC heuristics — there is no call site
This is the load-bearing correction for the mission's premise.
`sem_build_ports()` — `ide_semantic.c:183-198` — emits absent ports **only** when the
function writes a shared (>1-writer) global, and **only two hardcoded names**:
```c
if (writes_shared) {
    if (!sem_calls_any(f, claim_keys))                       // {claim,lock,acquire,gate,reserve}
        sem_add_port(f, "claim_slot",    PORT_LIFECYCLE,    DIR_OUT, 94, PS_ABSENT);
    if (!sem_calls_any(f, gate_keys))                        // {cooldown,gate,ready,can_}
        sem_add_port(f, "cooldown_gate", PORT_CONTROL_GATE, DIR_OUT, 91, PS_ABSENT);
}
```
There is **no `claim_slot(...)` call anywhere in the user's source** — the card is a
suggestion the analyzer *invents*. So "infer the signature from the call site's
argument expressions" **cannot apply to today's absent cards** (there is no call
site). A *genuine* unresolved call (`foo(x,y)` where `foo` is undefined) currently
renders as a **yellow CALL satellite labelled `(extern)`** (`ide_map.c:665-682`;
`map_func_nports` returns −1 at `ide_map.c:143-150`), **not** a red ABSENT card.

**Design consequence:** GENERATE-FROM-ABSENT must handle **two** absent kinds:
- **B1 — template absent** (today's `claim_slot`/`cooldown_gate`): no call site → fill
  signature from a small **template table**, and ALSO insert a call so the heuristic
  flips (this is what makes the card visibly disappear).
- **B2 — true unresolved-call absent** (a NEW analyzer signal, §2.5): a real call to an
  undefined name → infer the signature **from the AST call node's args**, insert only
  the definition (the call already exists).

### 0.4 What the AST knows about a call (verified)
`AST_CALL` is built in `px_postfix()` — `ide_pexpr.c:204-235`:
- `child0` = the callee node; if it's an `AST_IDENT`, `call->name` is the callee
  identifier (`ide_pexpr.c:209-212`).
- `children[1..]` = the **argument expressions**, each a full expression node from
  `px_assignment()` (`ide_pexpr.c:213-227`). Therefore **arg count = `nchildren − 1`**,
  and each arg carries a `Span` (`ide_ast.h:51-55,61`) so its exact source text is
  sliceable, and `AST_IDENT` args carry `name` (`ide_ast.h:67`).
- `ast_find_func(name)` returns the `AST_FUNC_DEF` (`ide_ast.h:89`); walk its subtree
  for `AST_CALL` nodes whose `name == <absent>`.

### 0.5 Per-function data the analyzer already computes (for §3 node detail)
`Func` (`ide_model.h:57-70`): `line_start/line_end` (61), `nparams/params` (63),
`ncalls/calls` (64), `nreads/reads` (65), `nwrites/writes` (66), `nports/ports` (68).
`Global.nwriters/nreaders` (`ide_model.h:77`). **Ports — including ABSENT ports — are
built for EVERY function** in `model_analyze` (`ide_semantic.c:482-484`), so per-fn
absent counts are available for all funcs. The **full** coherence/risks/flow/actions
are computed **only for the focus** (`ide_semantic.c:486-492`); `m->coherence` is a
single int for the focus (`ide_semantic.c:446-458`). **No per-function execution
counts exist** — `rt_hits` is purely flow-step click geometry (`ide_runtime.c:49-51`),
NOT a hit-frequency. Build status is whole-file/global: `ide_build_ok()`,
`ide_build_diag_count()`, `ide_build_line_severity(ln)` (`ide_build.h:30-38`).

### 0.6 The action-key legend is mostly aspirational
The status bar legend (`ide_chrome.c:290-296`) advertises
`E C W B G I R D S P T = EXPLAIN CONNECT WARN BUG GENERATE ISOLATE RECYCLE REMOVE
SEPARATE PROMOTE TRACE`. The **actual** LEGO key switch (`ide.c:2743-2806`) only
handles `B`(build), `R`(**run**, not recycle — `ide.c:2748`), `G`(generate),
arrows/Enter/Tab/Backspace/`Q`. **`E C W I D S P T` and `R`(recycle) have NO handler
— legend-only.** `I ISOLATE` and `R RECYCLE` are unimplemented today.

---

## 1. THE GIANT MAP — file compartments + three stable zoom levels

### 1.1 The three levels (one continuous space, deterministic positions)
| Level | Name | What's drawn | Today's analogue |
|------|------|--------------|------------------|
| **L2** | PROJECT | one rounded **compartment frame per file**, collapsed to a chip with aggregate stats; cross-file edges between frames | none (overview is flat) |
| **L1** | FILE | the compartments **expanded**: function **bricks** inside each frame; cross-file + intra-file edges | the `focus_func<0` overview, but grouped by file |
| **L0** | FUNCTION | today's central card + scored ports + read/write/call/absent satellites | `panel_map` focused view (`ide_map.c:487-863`) — unchanged |

L0 is literally the **zoom-in of one brick** at L1; L1 is the zoom-in of one
compartment at L2. Same space, three magnifications.

### 1.2 Deterministic layout rule (the aphantasia spatial-consistency law)
Position is a **pure function of model index** — never force-directed, never a hash
on a mutable key (preserve the MAP-STABLE-0 contract already documented at
`ide_map.c:537-550`).

- **File order** = first-appearance order of `m->funcs[i].file` while scanning
  `m->funcs` in array order. That order is fixed by `ide_parse_project_model`
  (`ide.c:671-684`: siblings in directory order, open file last). Build a derived
  table once per frame:
  ```c
  // proposed helper in ide_map.c (file-static), rebuilt each panel_map() call
  typedef struct { char file[M_NAME]; int first, count; int nabsent; int coh; } MapFile;
  static MapFile map_files[IDE_XFILE_MAXFILES]; static int map_nfiles;
  // map_files[k].file unique; .first = first func index; .count = funcs in file;
  // .nabsent = sum of PS_ABSENT ports over its funcs; .coh = file coherence proxy (§1.5)
  ```
- **Compartment grid (L2):** `col = k % NCOLS`, `row = k / NCOLS`, where `k` =
  file index and `NCOLS = (body.w) / (FRAME_W + GAP)` clamped ≥1 — identical math
  to the existing overview tile grid (`ide_map.c:384-405`). A file keeps the same
  cell while the directory + window width are unchanged.
- **Brick sub-grid (L1):** inside compartment `k`, function `j` (its index *within*
  the file, 0-based) sits at sub-row `j` (vertical list) or a sub-grid
  `subcol = j % FCOLS`. Pure function of `(k, j)` → stable.
- **Pan/zoom:** reuse `a->map_ox/oy` (`ide.h:160`); `ide_set_focus` already zeroes
  them on every focus change (`ide.c:419-420`) so levels never bleed pan into each
  other. `map_zoom` stays the global preference (`ide.h:161`).

### 1.3 New state (propose — add to `struct Ide`, `ide.h:156-164`)
```c
int map_level;     /* 0 = L0 FUNCTION, 1 = L1 FILE, 2 = L2 PROJECT. default 2 on overview entry */
int map_file;      /* L1/L2: selected compartment index into map_files[] (-1 = none) */
```
`focus_func >= 0` continues to mean L0 (drives the existing focused render verbatim).
`map_level` only matters when `focus_func < 0` (it replaces today's single flat
overview with L2/L1). Keep `map_selected` (`ide.h:164`) as the selected satellite/
brick within the active level.

### 1.4 Draw order (extends the existing two-pass discipline)
In `panel_map()`, when `focus_func < 0`, branch on `a->map_level` instead of the flat
grid at `ide_map.c:363`:
1. backdrop + header strip (reuse `ide_map.c:319-348`).
2. **PASS 1 — layout:** fill `map_files[]`; compute each compartment Rect (L2) and, at
   L1, each brick Rect. Push compartment frames AND bricks into `map_sats[]` (reuse the
   table so `map_nav`/`panel_map_click` work unchanged) with two new `MapKind`s
   `MK_COMPARTMENT`, `MK_BRICK` (extend the enum at `ide_map.c:247-249`).
3. **PASS 2a — cross-compartment edges** (behind frames): for each call where caller
   file ≠ callee file, draw a frame-to-frame edge (L2) or brick-to-brick edge (L1);
   for each global written in file A and read in file B, a state-flow edge (dim).
   Reuse `map_edge()` (`ide_map.c:214`). Edge data is derivable on the fly from
   `m->funcs[].calls` + `m->globals` (no new model fields).
4. **PASS 2b — compartment frames:** `map_card()` (`ide_map.c:167`) with a header band
   = filename + the L2 chip (fn count · COH% · ⚠N) from `map_files[k]`.
5. **PASS 2c — bricks (L1 only):** one small `map_card` per function inside its frame,
   carrying the §3 detail chips. Selected brick gets the existing cyan glow
   (`ide_map.c:810-811`).

### 1.5 Aggregate stats per compartment — data the model HAS vs NEEDS
**HAS (cheap, no new analysis):**
- **fn count** = `map_files[k].count` (count of `m->funcs` with that file).
- **warnings ⚠** = `map_files[k].nabsent` = Σ `PS_ABSENT` ports over the file's funcs
  (ports are built for ALL funcs, `ide_semantic.c:482-484`) **+** count of the file's
  written globals with `Global.nwriters > 1` (`ide_model.h:77`).
- **COH% proxy** = `map_files[k].coh`. **NEEDS a tiny derived helper** because the full
  coherence is focus-only (`ide_semantic.c:446-458`). Specify a cheap proxy reusing the
  same −10/absent rule applied per function, averaged:
  ```c
  // per function: coh_fn = 100 - 10*absent_ports(f), clamped [0,100]
  // file coh   = average of coh_fn over the file's funcs (count==0 -> 100)
  ```
  This is honest (it omits the risk penalty, which is focus-only) and stable. Label it
  `COH%` exactly as the focused view does, so the verbal twin matches.

**NEEDS (optional, future):** true per-function coherence (risks for all funcs) would
require lifting `sem_analyze_focus` risk/flow passes out of the focus-only guard — out
of scope; the proxy is sufficient for the compartment chip.

### 1.6 Keyboard nav across compartments + breadcrumb per level
Extend the LEGO key switch (`ide.c:2756-2794`):
- **Arrows** at L1/L2 → `map_nav(a,dir)` already does nearest-rect-in-direction over
  `map_sats[]` (`ide_map.c:966-993`); since compartments/bricks now live in that table,
  it works unchanged.
- **Enter** (`map_activate`, `ide_map.c:997`) = **zoom IN**:
  - L2 + a compartment selected → `a->map_level = 1; a->map_file = <sel>` (expand that
    file's bricks; keep `focus_func < 0`).
  - L1 + a brick selected → `ide_sel_jump(a, <func idx>, PANE_MAP)` (the existing
    focus-to-function path, `ide.c:494`) → L0.
  Implement by branching in `map_sat_follow()` on the new `MK_COMPARTMENT`/`MK_BRICK`
  kinds (`ide_map.c:880-943`); `MK_BRICK` reuses the existing `MK_CALL` jump arm.
- **Backspace** = **zoom OUT** (replace `ide.c:2785-2794`): L0→L1 (`focus_func=-1;
  map_level=1; map_file=<the function's file index>`); L1→L2 (`map_level=2`); L2 is the
  top (no-op).
- **Breadcrumb** (`ide_breadcrumb_prefix`, `ide.c:519+`, rendered in both status bars):
  - L2: `project` (no file segment).
  - L1: `project > <file.c>`.
  - L0: `project > <file.c> > <fn>  Ln r,c` (today's, unchanged).
  Verbal twin of the spatial level — the user always reads which magnification they're at.

---

## 2. BLUEPRINT→CODE — GENERATE-FROM-ABSENT

### 2.1 Goal
With a dashed-red absent card **selected** (e.g. `claim_slot · fit 0.94`), pressing **G**
creates the function **in the right file** with an **inferred signature**, saves, triggers
the live re-parse, and the card flips from ABSENT to a real brick **on camera**.

### 2.2 The selected-card seam (map → generator)
`map_sats[]` is file-static to `ide_map.c`, so expose a query the `KEY_G` handler can
call. **Add to `ide_map.c` + declare in `ide.h`:**
```c
/* If the keyboard/click selection is an ABSENT card, copy its name into out[cap]
 * and return its fit (0..100); else return -1. */
int map_selected_absent(const Ide* a, char* out, int cap);   // reads map_sats[a->map_selected]
```
Implementation: `if (a->map_selected in range && map_sats[sel].kind==MK_ABSENT) { copy
s->label; return s->fit; } return -1;` (the label holds the bare name, `ide_map.c:694-696`).

### 2.3 New KEY_G dispatch (replace `ide.c:2795-2800`)
```c
case KEY_G: {
    char aname[M_NAME]; int chg = 0;
    if (a->viz == VIZ_MAP && map_selected_absent(a, aname, M_NAME) >= 0)
        chg = gen_from_absent(a, aname);          // NEW path (selected card)
    else if (a->model.nactions > 0)
        chg = gen_apply_action(a, 0);             // legacy fallback (actions[0])
    if (chg) { ide_parse_project_model(a); ide_set_focus(a, a->focus_func); }
    break;
}
```
The `ide_parse_project_model` + `ide_set_focus` tail is the **existing live re-parse**
(`ide.c:2797-2798`) — IDE-SYNC-0's whole-directory rebuild + re-analyze. Reusing it is
what makes the flip appear without any extra plumbing.

### 2.4 `gen_from_absent(Ide* a, const char* name)` — the engine (NEW, in `ide_gen.c`)
Reuse the existing freestanding helpers in `ide_gen.c` (`gen_strcpy`/`gen_append`/
`gen_find_body`, `ide_gen.c:29-101`) and `text_splice` (`ide_astprint.c:118`).

**Step 1 — find the focus function + its body (AST):**
```c
AstNode* fn   = ast_find_func(a->model.funcs[a->focus_func].name);  // ide_gen.c:147 pattern
AstNode* body = gen_find_body(fn);                                  // ide_gen.c:95
```

**Step 2 — locate a call site & infer params (B2 path).** Walk `body`'s subtree
(depth-first over `first_child`/`next`, `ide_ast.h:71-73`) for the first `AST_CALL` whose
`name == name`:
- `argc = call->nchildren - 1` (`ide_pexpr.c:206-223`).
- For each arg child `i` (1..nchildren-1): build `"<type> <pname>"`:
  - if `arg->kind == AST_IDENT`: `pname = arg->name`; **type** = look up `arg->name` in
    `fn`'s params (`Func.params[].name`→`.type`, `ide_model.h:54,63`), else in
    `m->globals[].name`→`.type` (`ide_model.h:73-78`), else `"int"`.
  - else (non-ident expression): `pname = "a<i>"`, `type = "int"` (optionally annotate
    the arg's source span text — sliceable from `a->src[arg->span.start_off..end_off]`,
    `ide_ast.h:52` — as a trailing `/* <expr> */`).
  - Join with `", "`; if `argc == 0` → `"void"`.
- **Return type:** default `"int"` (matches today's `claim_slot`). *Optional polish:* if
  the `AST_CALL`'s parent is `AST_RETURN` of a `void` function or a bare `AST_EXPR_STMT`,
  prefer `"void"` — cheap, but not required for v1.

**Step 3 — template fallback (B1 path, today's synthetic cards).** If **no** call site is
found (the `claim_slot`/`cooldown_gate` synthetic case, §0.3), fill from a small table
(lift the keyword map already in `gen_apply_action`, `ide_gen.c:121-141`):

| name | ret | params | body |
|------|-----|--------|------|
| `claim_slot` | `int` | `void` | `return find_free_bullet_slot();` |
| `cooldown_gate` | `int` | `(tower_t* t)` | `return is_ready_to_fire(t);` |
| *other* | `int` | `void` | `return 0;` |

In B1 we ALSO insert a call so the heuristic flips: reuse today's **EDIT 1** —
`text_splice` `"\n    <name>();"` at `body->span.start_off + 1` (`ide_gen.c:151-163`).
In B2 we do **NOT** insert a call (it already exists).

**Step 4 — choose insertion file + anchor.**
- **Same-file default** (both B1 and B2): insert the definition **above the focus
  function** at `fn->span.start_off` (the function's first byte, `ide_ast.h:52`) so it is
  declared-before-use (the on-device front-end is effectively single-pass; placing the
  helper above the caller avoids an implicit-declaration diagnostic — cf. the honest
  single-TU note in `docs/IDE_SEMANTIC_LEGO.md:210-219`). Stub text:
  ```c
  int <name>(<params>) {\n    /* TODO: generated from blueprint */\n    return 0;\n}\n\n
  ```
  Splice: `text_splice(a->src, &a->src_len, IDE_SRC_CAP, fn->span.start_off, stub)`.
  (Append-at-EOF, as today's EDIT 2 does at `ide_gen.c:177`, also works and is simpler,
  but loses declared-before-use; prefer the above-anchor.)
- **Cross-file case** (absent symbol belongs in a sibling): the home file is only known
  when there is a signal — e.g. a `Proto` for `name` exists (`m->protos[]`,
  `ide_model.h:118-122`) declared in a header, or a future "target file" field on the
  card. **When a target sibling `fb` is known**, reuse the XFILE open-then-splice path:
  1. `ide_open_file(a, <dir>/<fb>)` (`ide.h:251`) — this flips `a->cur_file`/`a->src` to
     the sibling and rebuilds the model (same primitive `ide_sel_jump_xfile` uses,
     `ide.c:697-702`).
  2. `text_splice` the stub into the now-current `a->src`; `ide_write_file`.
  3. `ide_parse_project_model(a)`; then `ide_sel_jump_xfile(a, <focus fn name>,
     <focus file>)` (`ide.c:692`) to land back on the caller so the user sees the flip
     in context. **v1 ships same-file only**; this is the documented seam for when a
     target is resolvable (do not guess a home file without a signal — that would
     violate "stable, predictable" expectations).

**Step 5 — persist + re-parse.** `ide_write_file(a->cur_file, a->src, a->src_len)`
(`ide_gen.c:186`), then return 1. The `KEY_G` tail (§2.3) runs
`ide_parse_project_model` + `ide_set_focus` → the absent port vanishes (B1: the function
now calls `claim_slot` so `sem_calls_any` matches, `ide_semantic.c:191`; B2: the call now
resolves to a defined function) and a real brick exists → **the card flips on camera.**

### 2.5 (Recommended companion) make true unresolved calls render as ABSENT cards
So that B2 (real arg inference) is reachable in normal use, add to `sem_build_ports`
(`ide_semantic.c:177-181`, the `PORT_CONTROL` loop): when a called name resolves to no
`m->funcs[]` def, no `m->protos[]`, and no known extern, mark that control port
`PS_ABSENT` (a real "unresolved call" hole) instead of/in addition to `PS_CONNECTED`.
Then the map's existing `PS_ABSENT` rendering (`ide_map.c:685-700`) draws it dashed-red
with a fit, and GENERATE-FROM-ABSENT infers the signature from the **actual** call site.
This is the honest, non-synthetic absent card the mission imagines. **Gate it** so the
two heuristic gates still fire for the demo (keep `ide_semantic.c:183-198`).

### 2.6 Reverse direction — the "blueprint changed: +N ports" pulse
When code edits change a function's ports, the map **already** re-parses: caret crossing
→ `ide_sel_from_caret` → `ide_set_focus` → `model_analyze` (`ide.c:486-487`); save/build
re-parse too. To make bidirectionality **visible**:
- **New Ide fields** (`ide.h:156-164`): `int bp_pulse_ms; char bp_pulse_msg[32]; int
  prev_focus_nports; int prev_focus_nabsent;`.
- **Capture before / compare after** in `ide_set_focus` (right after `model_analyze`,
  `ide.c:433`): if the focus didn't change but the same function's `nports`/absent count
  did, set `bp_pulse_msg = "blueprint changed: +N ports"` (or `−N`, or
  `"+1 hole filled"` when absent dropped) and `bp_pulse_ms = 1500`. Cleanest: a tiny
  helper `map_note_blueprint_delta(a, old_nports, old_nabsent)` called from the edit/save
  re-parse site so it doesn't fire on ordinary navigation.
- **Render + decay** like the build flash: draw `bp_pulse_msg` in the `panel_map` header
  strip (next to the title, `ide_map.c:347`) tinted, and decay `bp_pulse_ms` each frame
  using the same pattern as `ide_build_tick` (`ide.c:2879-2882`; force `g_ide_redraw` so
  it animates). The pulse is the spatial confirmation that "my code edit moved the
  blueprint" — the reverse twin of GENERATE.

---

## 3. MORE FUNCTIONAL DETAIL PER NODE — three REAL, cheap wins

All grounded in data already computed; each cites where the datum lives.

1. **Lines chip** — `Func.line_end − line_start + 1` (`ide_model.h:61`). Draw `"76 ln"`
   on the central card header (`ide_map.c:742`) and on each L1 brick. Precedent: the
   FUNCTIONS panel already prints per-function size badges (`ide_funcs.c`; visible as
   `1px`/`r2 w1x` in `screenshots/showcase_map.png`). **Effort: S.**
2. **R/W/C fan chip** — `"R<nreads> W<nwrites> C<ncalls>"` from `Func.nreads/nwrites/
   ncalls` (`ide_model.h:64-66`), under the "Ports (N)" sub-heading
   (`ide_map.c:745-752`). On the **write satellite sub-line**, upgrade the existing
   multi-writer warn dot (`ide_map.c:639-661`) to `"×<Global.nwriters> writers"`
   (`Global.nwriters`, `ide_model.h:77`) — the edge now states *how many* writers, not
   just that there's a collision. **Effort: S.**
3. **Absent/“holes” badge per brick** — count of `PS_ABSENT` ports per function (built
   for ALL funcs, `ide_semantic.c:482-484`): a small red dot + number on each L1 brick
   and as the compartment ⚠ aggregate (§1.5). Doubles as the file-level warning roll-up.
   **Effort: S** (the count loop already exists conceptually in `sem_analyze_focus`,
   `ide_semantic.c:449-451`).

**Honest non-wins (do NOT fake these):**
- **"Hot path" from runtime hits — NO DATA.** `rt_hits` is flow-step click geometry,
  not execution frequency (`ide_runtime.c:49-51`); there is no per-function run counter.
  **Real substitute:** a **fan-in badge** = number of functions that call this one,
  already computed in the caller-satellite loop (`ide_map.c:593-619`). High fan-in =
  structurally hot. Honest and cheap. **Effort: S–M.**
- **Last-build status dot — only whole-file.** `ide_build_ok()/diag_count()`
  (`ide_build.h:31-34`) are global; `ide_build_line_severity(ln)` (`ide_build.h:35-38`)
  is per-line, so a per-function dot is derivable (any error line within
  `[line_start,line_end]` → red) **but only after a build**. Show it on the brick header
  when a build result exists (`ide_build_active()`, `ide_build.h:30`). **Effort: M.**
- **Cyclomatic complexity — not computed.** Would need a new AST decision-node count
  (`AST_IF/FOR/WHILE/DO/CASE`, `ide_ast.h:31`). Optional future pass; use `nports` or
  lines as the v1 proxy. **Effort: M** (new pass).

---

## 4. AUTOMATIONAL TASKS — map-side semantics (ACTIONS panel UX owned elsewhere)

The map owns: the **selection**, the **rendering** of automatable state, and the
**generation core**. The ACTIONS panel (another agent) owns the list/preview UX; both
read the shared `m->actions[]` (`ide_model.h:91-97`) and the new `gen_*` cores.

| Key | Today | Proposed map-side semantics | Effort |
|----|-------|-----------------------------|--------|
| **G** GENERATE | `gen_apply_action(a,0)` (`ide.c:2795`) | §2: generate the **selected** absent card with inferred signature | **M** |
| **Shift+G** GENERATE-ALL | — | loop `m->funcs[*]`, call the `gen_from_absent` core for every `PS_ABSENT` port, then a **single** `ide_parse_project_model`; map flips all holes at once | **M** |
| **I** ISOLATE | legend-only, **no handler** (`ide.c`) | "solo" the focused function: a draw filter in `panel_map` that hides all satellites except the central card (set a new `a->map_isolate` flag; skip the satellite passes `ide_map.c:555-700`). The verbal twin: breadcrumb appends `(isolated)`. | **S** |
| **R** RECYCLE | legend says RECYCLE but handler is **RUN** (`ide.c:2748`) — mismatch | **Resolve the conflict first.** Define map-side RECYCLE = "reclaim dead bricks": mark functions with **0 fan-in AND not `main`/`_start`** (derivable from the caller scan, `ide_map.c:593-619`) as gray "0 refs" bricks at L1; R offers removal of the selected dead brick (a future text delete via span). Until a free key exists, keep R=RUN and surface RECYCLE through the ACTIONS panel, not a key. | **M** |

**Coordination note for the ACTIONS-panel agent:** GENERATE-ALL and RECYCLE should appear
as rows in `m->actions[]`-style lists; the map provides (a) `map_selected_absent()` for
the single-card path, (b) `gen_from_absent()` as the reusable core, (c) the dead-brick
flag for RECYCLE's candidate set. The map never owns the list chrome.

---

## 5. Build-first order (what to implement, smallest proven step first)

1. **GENERATE-FROM-ABSENT, B1 same-file** (§2.2-2.4 template path): smallest diff that
   turns today's generic G into a *targeted* one — reuse `gen_apply_action`'s editor,
   just drive name from `map_selected_absent()` and the template table. Proves the
   on-camera flip end-to-end. **Effort: M.**
2. **The three node-detail chips** (§3.1-3.3): pure render additions, no model changes,
   immediate aphantasia value. **Effort: S.**
3. **L2/L1 compartment overview** (§1): the larger structural change; gated behind
   `map_level` so the focused L0 view (the proven showcase) is untouched. **Effort: L.**

**One thing to build first:** GENERATE-FROM-ABSENT B1 same-file (§5.1) — it converts an
existing REAL-but-generic feature into the mission's headline (select the hole → press G
→ watch it fill) with the least new surface, and it exercises every seam (selection query,
`text_splice`, `ide_parse_project_model`, the on-camera re-render) that the rest builds on.
