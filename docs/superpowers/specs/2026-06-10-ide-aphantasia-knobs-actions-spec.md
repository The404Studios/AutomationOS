# Spec: Per-Symbol Knobs, the Automation Deck, and the Project Pulse

**Date:** 2026-06-10
**Agent:** APHANTASIA-UX
**Scope:** Semantic LEGO IDE — `userspace/apps/ide/`
**Status:** Design spec. The main session implements. Every claim about existing
code is cited `file:line`; every proposed widget names the exact existing
function it imitates and where to hook it.

---

## 0. The aphantasia thought experiment (the "why" that drives every decision)

Picture yourself with no mind's eye. You cannot "hold" the project. You opened
`tower_tick`, saw it needs `claim_slot`, and decided "I'll come back to that
after I finish `wave_spawn`." Three minutes later that decision is **gone** —
it never had a place to live except your head, and your head doesn't keep
pictures. The IDE's design laws (`docs/IDE_SEMANTIC_LEGO.md:28-44`) already
externalize *where am I* (breadcrumb), *what's connected* (map/ports), and
*how bad is it* (COH). They do **not** yet externalize the three things you
need to actually **finish** a project rather than navigate one:

1. **Intent per object** — "I've handled this function / I still owe this one /
   watch this one / silence this one." Today there is no per-symbol state at
   all except `Func.closed` (`ide_model.h:69`), an IDA collapse flag. Nothing
   survives a reparse or a restart.
2. **A task list that remembers what ran** — Build/Run/Generate exist as
   one-shot keypresses (`ide.c:2744-2800`) that leave **no trace**. An aphantasic
   user cannot remember "did the build pass? when? what did Run say?" The result
   scrolls away in the BUILD tab and is gone.
3. **A single "how done am I" board** — COH answers *health*, not *completion*.
   "5 functions, 2 done, 1 missing a stub, build green 12s ago" is the sentence
   that lets you stop and resume without re-deriving the whole project in your
   head.

This spec builds exactly those three, on top of one new foundation (a persistent
per-symbol mark store), reusing the existing Settings widget vocabulary verbatim.

### 0.1 Current reality the implementer must know (two load-bearing facts)

- **The bottom command legend is a fiction.** `panel_status()` draws eleven
  shortcut chips — `E EXPLAIN  C CONNECT  W WARN  B BUG  G GENERATE  I ISOLATE
  R RECYCLE  D REMOVE  S SEPARATE  P PROMOTE  T TRACE` (`ide_chrome.c:290-303`)
  — but they are **decorative `chr_shortcut()` labels with no key bindings.**
  The only LEGO-workspace keys actually wired are `B`=build, `R`=run,
  `G`=`gen_apply_action(a,0)`, `Q`=quit, arrows/Enter/Tab/Backspace for map nav
  (`ide.c:2743-2806`). Worse, the wired keys **contradict** the legend: `B`
  builds (legend says "BUG"), `R` runs (legend says "RECYCLE"). **So
  `I ISOLATE` does not exist today — there is no isolate behavior anywhere in
  the codebase.** The mission's "what does ISOLATE do today?" answer is:
  *nothing; it is a label.* This spec gives ISOLATE a real, visible meaning
  (§3.4) and replaces the misleading legend with the real deck (§4).

- **VIZ-4 ACTIONS and VIZ-5 POTENTIALS are placeholder duplicates.** Both tabs
  render the *same* thing: `panel_inspector()` with `insp_tab` forced to
  `INSP_DETAILS` (`ide.c:1520-1527`), i.e. the POTENTIALS/RISKS + ACTIONS list
  from `insp_body_details()` (`ide_inspector.c:271-359`). In the screenshots
  `showcase_actions.png` and `showcase_potentials.png` they are pixel-identical
  and both read "(none)" because `game_main` has COH 100% / no risks. **These
  two tabs are the empty real estate this spec fills:** VIZ-4 becomes the
  automation deck, VIZ-5 becomes the project pulse.

---

## 1. The Settings widget vocabulary (what we reuse, exactly)

Everything below imitates `panel_settings` in `ide_inspector.c`. The vocabulary:

| Concept | Definition | Cite |
|---|---|---|
| Row model | `SetRow { SetKind kind; const char* label; int* var; int vmin,vmax,step; void(*apply)(int); }` | `ide_inspector.c:705-712` |
| Row kinds | `SET_HEADER, SET_TOGGLE, SET_SLIDER` | `ide_inspector.c:705` |
| Row table | `static const SetRow g_set_rows[]` drives draw+hit+key | `ide_inspector.c:717-733` |
| Draw entry | `panel_settings(Ide*,Canvas*,Rect)` | `ide_inspector.c:804-888` |
| Header draw | section label `TH_BLUE` + `gfx_hline` | `ide_inspector.c:852-855` |
| **Toggle draw** | pill `gfx_round(..., on?TH_GREEN:TH_BORDER_LT)` + knob `gfx_round(...,TH_PANEL)` + `"ON"/"OFF"` text | `ide_inspector.c:861-873` |
| **Slider draw** | track `gfx_fill` + knob `gfx_round(...,TH_BLUE)` + value text | `ide_inspector.c:874-886` |
| Hit entry (3-phase) | `panel_settings_click(Ide*,Rect,mx,my,phase)`; 0=press,1=drag,2=release | `ide_inspector.c:890-931` |
| Toggle hit | flip `*var` + `ide_config_save()` | `ide_inspector.c:919-920` |
| Slider hit | grab → `g_set_drag`, set value; persist on release | `ide_inspector.c:921-926, 893-897` |
| Key entry | `panel_settings_key(Ide*,keycode)` Up/Down/Left/Right/Space/Enter | `ide_inspector.c:938-966` |
| Geometry helpers | `set_body_rect:753`, `set_ctrl_rect:759`, `set_track_rect:770`, `set_knob_x:778`, `set_val_from_x:785`, `set_apply_value:796`, `set_next_row:743` | `ide_inspector.c` |
| Selection/drag state | `g_set_drag:739`, `g_set_sel:740` | `ide_inspector.c` |
| Constants | `SET_ROW_H = ROW_H+8`, `SET_HEAD_H = ROW_H+2`, `SET_KNOB_W = 8` | `ide_inspector.c:735-737` |
| Button affordance | `[APPLY]` = `gfx_round(...,TH_BLUE)` in `insp_apply_rect(b,ry)` | `ide_inspector.c:147, 347-351` |

`ROW_H = GFX_FH+2`, `PAD = 6` (`ide_theme.h:54-55`); colors `TH_BLUE/GREEN/ORANGE/RED`
(`ide_theme.h:24-29`).

### 1.1 The one constraint that shapes the whole design

Every `SetRow.var` is `int*` pointing at a **global** runtime var
(`ide_inspector.c:717-733` → `g_ui_pct`, `g_tab_width`, … all `extern int` in
`ide_gfx.h:17-36`). Likewise `CfgRow.var` is `int*` to a global
(`ide_config.c:26-47`). **Per-symbol state cannot be a global** — it belongs to
*the focused function*, and the focused function changes and is rebuilt on every
reparse. So the foundation (§2) is: a small store of per-symbol marks keyed by
**function name** (stable across reparse), exposing `int*` fields so the existing
toggle/slider widgets bind to them with zero change to their draw/hit/key code.

---

## 2. FOUNDATION — the per-symbol mark store (`ide_marks.c` / `.h`, NEW)

Nothing in §3–§5 works without persistent, reparse-surviving, per-symbol state.
This is the only genuinely new subsystem; it mirrors `ide_config.c` exactly.

### 2.1 Data model

```c
/* ide_marks.h */
#include "ide_model.h"           /* M_NAME, M_MAXFUNCS */

typedef struct {
    char name[M_NAME];           /* function name = the stable key            */
    int  done;                   /* 0/1  user marked this function finished   */
    int  star;                   /* 0/1  watch/pin -> always-visible chip     */
    int  isolate;                /* 0/1  focus-lock + map solo (see 3.4)      */
    int  mute;                   /* 0/1  suppress this fn's warnings/penalty   */
} SymMark;

/* Get the mark record for `name`, creating a zeroed one on first touch.
 * Returns NULL only if the table is full. Fields are int so panel_settings'
 * `int*`-based SetRow widgets bind to &m->done etc. with no widget changes. */
SymMark* marks_get(const char* name);
SymMark* marks_find(const char* name);   /* NULL if absent (read-only paths) */

int  marks_count_done(const Model* m);   /* funcs in *m that are marked done  */
int  marks_any_isolated(void);           /* >=0 index pattern; see 3.4        */

void ide_marks_load(void);               /* call once at init (see 2.3)       */
void ide_marks_save(void);               /* call after every mark change      */
```

```c
/* ide_marks.c -- static table, no malloc (mirrors ide_config.c's pattern). */
#define MARKS_CAP (M_MAXFUNCS * 2)   /* room for renames within a session */
static SymMark g_marks[MARKS_CAP];
static int     g_nmarks;
```

`marks_get` does a linear scan by `ide_streq` (≤128 entries, identical cost to
`sem_find_global`'s scan at `ide_semantic.c:126-134`) and appends a zeroed row
on miss. Keying by **name** is the same identity the runtime strip already uses
to match flow labels to functions (`rt_streq` against `m->funcs[j].name`,
`ide_runtime.c:575-576`), so it is a proven-stable key.

### 2.2 Persistence — copy `ide_config.c` verbatim, new file name

Reuse the durable-diskfs-then-fallback pattern exactly:
- syscalls `SYS_PERSIST_READ 94` / `SYS_PERSIST_WRITE 95` (`ide_config.c:11-23`),
- file format = `key=value` lines, tolerant parser (`ide_config.c:82-112`),
- writer = concatenate rows (`ide_config.c:114-129`).

The only difference: the key set is **dynamic** (one line per marked function),
not a fixed table. Encode each non-zero mark as one line:

```
done.tower_tick=1
star.tower_tick=1
isolate.tower_tick=1
mute.wave_spawn=1
```

Persist file name `IDE_MARKS_NAME "ide.marks"` + fallback
`"/Desktop/.ide_marks"` (mirror `ide_config.h:21-22`). On load, split each line
at the first `.` into `field`/`name`, `marks_get(name)`, set the field. Only
non-zero fields are written, so the file stays tiny and the format is
forward-tolerant (unknown `field.` prefixes ignored, exactly like
`cfg_apply_kv`'s unknown-key skip at `ide_config.c:79`).

`marks_save()` is called from every toggle handler (§3.3), the same way
`panel_settings_click` calls `ide_config_save()` on toggle
(`ide_inspector.c:920`).

### 2.3 Wiring the load (one line, with a citation for where)

`ide_config_load()` is called once during `init()` at `ide.c:1116`. Add
`ide_marks_load();` on the next line. **This is the failing-then-passing proof
point** (§6 acceptance #4): without this line, marks set in a session vanish on
restart; with it, they survive. (Reboot-durable when a SATA disk is present,
session-durable otherwise — same guarantee as the config file,
`ide_config.h:16-20`.)

---

## 3. PER-SYMBOL KNOBS — the headline ask

### 3.1 Where it lives

A **knob strip** for the active selection. Per the mission ("inspector or below
the map"), put it as a **new 7th inspector tab** so it rides the existing
right-column inspector that already follows the one selection model, AND mirror
it as a compact strip pinned to the top of the VIZ-4 deck (§4) so it is visible
while you automate.

Add tab label `"MARK"` to `INSP_TAB_LBL[]` and bump `INSP_NTABS 6 → 7`
(`ide_inspector.c:27, 36-38`). The tab strip render (`ide_inspector.c:542-559`)
and hit-test (`ide_inspector.c:590-597`) already loop `0..INSP_NTABS`, so they
pick up the new tab for free. Route it in the body switch
(`ide_inspector.c:573-580`): `case 6: insp_body_marks(a, cv, b, f); break;`.

The knob strip targets **the focused function** — `insp_focus(a)`
(`ide_inspector.c:115`) returns it, driven by `a->sel.symbol` / `a->focus_func`
(`ide.h:129, 146`; written by `ide_set_focus:406`, `ide_sel_jump:494`,
`ide_sel_from_caret:461`). When no function is focused, draw the existing
"No function focused." line (`ide_inspector.c:567-571`).

### 3.2 Rendering — reuse the toggle/slider blocks verbatim

`insp_body_marks()` fetches `SymMark* mk = marks_get(f->name);` then draws rows
**using the identical toggle block** from `panel_settings`
(`ide_inspector.c:861-873`). To avoid copy-paste drift, the implementer should
factor that 13-line toggle block and the slider block into two reusable helpers
inside `ide_inspector.c`:

```c
/* Extracted from panel_settings (ide_inspector.c:861-873) — same pixels. */
static void insp_draw_toggle(Canvas* cv, Rect rowb, int ry, const char* label, int on);
/* Extracted from panel_settings (ide_inspector.c:874-886). */
static void insp_draw_slider(Canvas* cv, Rect rowb, int ry, const char* label,
                             int val, int vmin, int vmax);
```

`panel_settings` then calls these helpers (no visual change — refactor only),
and `insp_body_marks` calls `insp_draw_toggle` once per knob with
`on = mk->done` etc. Row pitch = `SET_ROW_H` (`ide_inspector.c:735`); selected-row
highlight = the `gfx_blend(... TH_SELECT ...)` from `ide_inspector.c:848-850`.

Header row "KNOBS — `<fname>`" via the section-header style
(`ide_inspector.c:852-855`).

### 3.3 The knobs (every one has a backend that exists or is one function away)

For each: **widget · backing field · persistence · the visible consequence
elsewhere** (a knob with no visible consequence is forbidden).

#### Knob 1 — DONE  (mark-done / mark-todo)   ★ highest value
- **Widget:** `SET_TOGGLE`, label "Done", on = `mk->done`. Draw =
  `ide_inspector.c:861-873`.
- **Backing field:** `SymMark.done` (§2.1).
- **Persistence:** `marks_save()` on flip → `done.<name>=1` line (§2.2).
- **Visible consequence (mandatory):** the FUNCTIONS list. In `panel_funcs`,
  the open-function row loop (`ide_funcs.c:132-177`) draws a `{}` glyph
  (`fn_draw_glyph:56-59`) and a connectivity badge (`fn_make_badge:78-90`).
  Add: when `marks_find(f->name)->done`, draw a green check glyph in the glyph
  gutter (swap `"{}"`→`"OK"`/check in `fn_draw_glyph`) and dim the name to
  `TH_TEXT_FAINT`. **Second consequence:** the Pulse "done N / total" count
  (§5) changes in the same frame (`marks_count_done`). This is the
  acceptance-test knob (§6 #1).

#### Knob 2 — STAR / PIN  (watch list)   ★ second highest value
- **Widget:** `SET_TOGGLE`, label "Star (watch)", on = `mk->star`.
- **Backing field:** `SymMark.star`.
- **Persistence:** `marks_save()` → `star.<name>=1`.
- **Visible consequence (mandatory):** an **always-visible watch chip strip in
  the chrome.** `panel_status` (`ide_chrome.c:205-311`) already lays chips
  left-to-right with `chr_shortcut`-style helpers; add a "WATCH:" segment that
  renders up to 3 starred names as `gfx_round` chips (reuse the closed-function
  chip style from `ide_funcs.c:220-228`). Clicking a watch chip = `ide_sel_jump`
  to that function (`ide.c:494`). This realizes design-law #2 "always-visible
  context" (`docs/IDE_SEMANTIC_LEGO.md:32-34`): the function you're watching is
  on screen permanently, not in your head. **Second consequence:** starred
  functions sort to the top of the FUNCTIONS list (a stable-partition pass in
  `panel_funcs`'s row loop, `ide_funcs.c:132`).

#### Knob 3 — ISOLATE  (focus-lock + map solo)   ★ the "I ISOLATE" that never existed
- **Widget:** `SET_TOGGLE`, label "Isolate", on = `mk->isolate`.
- **Backing field:** `SymMark.isolate`.
- **Persistence:** `marks_save()` → `isolate.<name>=1`.
- **Visible consequence (mandatory) — define ISOLATE for the first time:**
  Today `ide_sel_from_caret` re-centers the map on every function crossing:
  `if (sym >= 0 && sym != a->focus_func) ide_set_focus(a, sym)`
  (`ide.c:486-487`). When the focused function is isolated, **gate that
  re-focus** so the map stays pinned to the isolated brick even as the caret
  moves into other functions or files. Visible proof: a "LOCKED" chip in the map
  header (`panel_map` header area, `ide_map.c`) and the breadcrumb FN field
  stops tracking the caret. For an aphantasic user this is the single most
  valuable control: "keep `tower_tick` and its missing `claim_slot` on screen
  while I go edit `wave_spawn` to add it." It is ~one conditional plus a header
  chip — genuinely one function away, and honest (no fake behavior). This is
  acceptance-test knob #3 (§6).

#### Knob 4 — WARN-MUTE  (silence this function's warnings)
- **Widget:** `SET_TOGGLE`, label "Mute warnings", on = `mk->mute`.
- **Backing field:** `SymMark.mute`.
- **Persistence:** `marks_save()` → `mute.<name>=1`.
- **Visible consequence (mandatory):** risks/coherence are computed for the
  focus function in `sem_analyze_focus` (`ide_semantic.c:322-459`). The WARN
  chip in the topbar (`ide_chrome.c:273-284`) and status bar
  (`ide_chrome.c:272-284`) read `m->nrisks`. When the focused function is muted,
  render the WARN chip as `"WARN n (muted)"` in `TH_TEXT_FAINT` and **exclude
  this function from the Pulse "warned" count** (§5). Lower value than 1–3 but
  cheap and on-theme (you decide a warning is acceptable and it stops nagging).

#### FUTURE — RENAME-SYMBOL  (no infrastructure; do not attempt now)
A semantic rename must rewrite every reference across all files and reparse.
The IDE has only textual find/replace (`a->find_buf`/`a->repl_buf`, Ctrl+H,
`ide.c:2440-2449`) and no cross-file reference index (the model stores call/read/
write **names**, `ide_model.h:64-66`, but not their source spans). Mark RENAME
**FUTURE**: it needs an AST-backed reference table that does not exist. A knob
that only renamed the definition would silently break callers — forbidden
(consequence is *invisible breakage*, the opposite of the design law).

### 3.4 Knob strip hit-test + keyboard (reuse the 3-phase pump + key entry)

- **Click:** add `insp_body_marks` hit-testing to `panel_inspector_click`
  alongside the existing per-tab blocks (`ide_inspector.c:599-690`). For
  `insp_tab == 6`, walk the same row pitch and flip the matching `SymMark`
  field, then `marks_save()` — structurally identical to the Settings toggle
  branch (`ide_inspector.c:919-920`).
- **Keyboard:** `panel_inspector_key` already handles the inspector tab
  (`ide_inspector.c:971-991`); extend it so on `insp_tab == 6` Up/Down move a
  `g_mark_sel` cursor and Space/Enter flip the field + `marks_save()`, copying
  `panel_settings_key`'s structure (`ide_inspector.c:948-964`). It is already
  gated `a->viz == VIZ_INSPECTOR && panel_inspector_key(...)` at `ide.c:2733`.

---

## 4. THE ACTIONS PANEL AS AN AUTOMATION DECK  (VIZ-4)

Replace the placeholder `INSP_DETAILS` reuse (`ide.c:1520-1527`) with a real
deck: the project's one-key task list where an aphantasic user sees **what ran,
when, and what happened — permanently.**

### 4.1 Row model (the SetRow analog for buttons)

```c
/* ide_actions.c (NEW), or a static block in ide_inspector.c next to panel_settings */
typedef struct {
    const char* label;        /* "Build", "Run", ...                         */
    char        key;          /* one-key shortcut shown + handled            */
    int       (*run)(Ide*);   /* returns 1=ok, 0=fail; runs the action       */
    /* live result, stamped by the deck after each run: */
    int         last_status;  /* -1 never run, 0 fail, 1 ok                   */
    long        last_ms;      /* ide_ticks_ms() at completion (for "Ns ago")  */
    char        last_msg[48]; /* short result text                           */
} ActRow;
static ActRow g_act_rows[] = {
    { "Build project",        'B', act_build,    -1, 0, "" },
    { "Run",                  'R', act_run,      -1, 0, "" },
    { "Generate all stubs",   'G', act_gen_all,  -1, 0, "" },
    { "Save all",             'S', act_save_all, -1, 0, "" },
    { "Re-analyze project",   'A', act_reanalyze,-1, 0, "" },
    { "Open TODO list",       'T', act_open_todo,-1, 0, "" },
};
```

### 4.2 The six actions, each backed by an existing call

| Row | `run()` body | Backend cite |
|---|---|---|
| Build project | `ide_do_build(a); g_build_view=1;` then read `ide_build_ok()` | `ide_build.c:207-275, 345` |
| Run | `ide_do_run(a);` then read `g_runmsg` via new `ide_run_msg()` accessor | `ide_build.c:277-322, 41` |
| Generate all stubs | loop `gen_apply_action(a, 0)` while `a->model.nactions>0` (each apply reparses+reanalyzes so index 0 is always the next missing; cap at, say, 16 iters); coordinates with the GRAPH agent's GENERATE (assume `ide_generate_all(a)` callable) | `ide_gen.c:105-204`; `ide.c:2795-2799` is the single-shot precedent |
| Save all | `ide_editor_save(a)` for the active buffer + flush open tabs | `ide_editor.c:670`; tabs `ide.h:196-198` |
| Re-analyze project | `ide_parse_project_model(a); ide_set_focus(a, a->focus_func);` | `ide.c:662, 2797-2798` |
| Open TODO list | `a->viz = VIZ_POTENTIALS;` (jump to Pulse) and set a `g_pulse_todo_filter=1` so the Pulse lists `!done` functions | `ide.h:50`; Pulse §5 |

**New accessors needed** (trivial, in `ide_build.c`): `const char* ide_run_msg(void)`
returning `g_runmsg` (`ide_build.c:41`), and reuse `ide_build_ok()`
(`ide_build.c:345`), `ide_build_time_ms()` (`ide_build.c:384`).

### 4.3 Draw — reuse the label-left / button-right row, plus a result chip

`panel_actions(Ide*, Canvas*, Rect r)` renders a header ("ACTIONS — automation
deck") in the Settings header style (`ide_inspector.c:809-819`), then one row per
`g_act_rows[]` at `SET_ROW_H` pitch (`ide_inspector.c:735`). Per row:

1. **Label** left: `insp_text(... row->label, TH_TEXT ...)` (`ide_inspector.c:859`).
2. **Key hint** `[B]` in `TH_BLUE` next to the label (the `chr_shortcut` idiom,
   `ide_chrome.c:196-203`).
3. **Run button** right: `gfx_round(..., TH_BLUE)` + centered "RUN" / the
   action's verb — literally `insp_apply_rect(b,ry)` + the `[APPLY]` button draw
   (`ide_inspector.c:147, 347-351`).
4. **Last-result chip** between label and button: colored by `last_status`
   (`-1`→`TH_TEXT_FAINT "—"`, `0`→`TH_RED "FAIL"`, `1`→`TH_GREEN "OK"`), then
   `last_msg` dim, then **a relative timestamp** "Ns ago" computed from
   `ide_ticks_ms() - last_ms` (`ide_build.c:252-255` shows the `ide_ticks_ms`
   delta idiom). This is the permanence an aphantasic user needs: the row itself
   *is* the memory of the last build.

### 4.4 Hit-test + keyboard

- **Click:** `panel_actions_click(Ide*, Rect, mx, my)` — find the row under `my`
  (same scan as `panel_settings_click` phase 0, `ide_inspector.c:914-930`); if
  the click is on the run button rect (`insp_apply_rect`), call `row->run(a)`,
  then stamp `last_status / last_ms = ide_ticks_ms() / last_msg`. Wire it in the
  center-top click dispatch where VIZ_ACTIONS currently routes to the inspector
  (`ide.c:2160-2167`).
- **Keyboard:** `panel_actions_key(Ide*, keycode)` gated `a->viz == VIZ_ACTIONS`,
  added beside the other per-VIZ key gates (`ide.c:2732-2734`). Map each
  `row->key` (B/R/G/S/A/T) to running that row + stamping it. Because this gate
  runs **before** the global `case KEY_B/KEY_R/KEY_G` (`ide.c:2743-2800`) and
  returns on consume, the deck's keys win while VIZ-4 is open (and the global
  build/run still work from other tabs). This also lets us **delete the
  misleading legend** (`ide_chrome.c:290-303`) and replace it with the real
  six, so the chrome stops lying.

### 4.5 What this fixes for the user

Instead of "press B and hope," the deck shows: `Build project [B]  OK  847ms
12s ago`. You can leave, come back, and the screen still says the build passed
12s ago — the decision-state that used to evaporate now persists on screen.

---

## 5. THE PROJECT PULSE  (VIZ-5 redesign — "how done am I?")

Replace the placeholder `INSP_DETAILS` reuse (`ide.c:1521-1527`) with a
**scoreboard**: numbers and words, not shapes, answering completion not just
health.

### 5.1 The numbers and exactly where each already lives

| Pulse line | Value | Source (file:line) |
|---|---|---|
| Functions (total) | `a->model.nfuncs` | `ide_model.h:126`, `ide.c` model |
| Done | `marks_count_done(&a->model)` | §2.1 (new), counts `mk->done` over model funcs |
| To-do | `nfuncs - done` | derived |
| Missing (absent cards) | count funcs with any `ports[k].status==PS_ABSENT` | `ide_model.h:44,52`; ports built for **every** func at `ide_semantic.c:483-484` (so this is project-wide, not just focus) |
| Warned | `m->nrisks` for the focus, **minus muted** (§3.3 Knob 4) | `ide_semantic.c:486-492` (risks are focus-only — see honesty note) |
| Build status | `ide_build_ok()` / `ide_build_diag_count()` | `ide_build.c:345-346` |
| Last run result | `ide_run_msg()` (new accessor over `g_runmsg`) | `ide_build.c:41` |
| Coherence | `m->coherence` (0..100) + band word Low/Med/High | `ide_semantic.c:458`; band logic `ide_runtime.c:299-301` |
| COH trend | a real 10-sample history (see honesty note) | replaces the **fake** static `trend[]` at `ide_runtime.c:363` |

**Honesty notes the implementer must respect:**
- *Warned is focus-scoped today.* `sem_analyze_focus` only fills `m->risks` for
  the focused function (`ide_semantic.c:486-492`); there is no project-wide risk
  tally. Label the Pulse line "Warnings (focused fn)" and mark project-wide warn
  count **FUTURE** (needs a per-func risk pass in `model_analyze`). Do not print
  a number that pretends to be project-wide.
- *The COH sparkline is currently fake.* `rt_draw_coherence` uses a hardcoded
  `static const int trend[10]` nudged by the live score (`ide_runtime.c:362-374`)
  — it is **not** a real history. For the Pulse to be honest, add a real ring
  buffer `static int g_coh_hist[10]; static int g_coh_head;` pushed once per
  `model_analyze()` (`ide_semantic.c:465`, at the end after `m->coherence` is
  set, or in `ide_set_focus` after `model_analyze` returns, `ide.c:433`). The
  Pulse and the VIZ-3 sparkline then both read real history.

### 5.2 Layout (reuse existing primitives)

`panel_pulse(Ide*, Canvas*, Rect r)`:
- Header "PROJECT PULSE — `<project or file>`" via `ide_breadcrumb_prefix`
  (`ide.c:519`) in the Settings header style (`ide_inspector.c:809-819`).
- A **key/value scoreboard** using `insp_kv(cv, b, y, label, value, pct)`
  verbatim (`ide_inspector.c:457-466`) — one row each for Functions / Done /
  To-do / Missing / Warnings / COH%. `insp_kv` already right-aligns the number
  and supports a `%` suffix, exactly what a verbal-twin scoreboard needs.
- A **COH ring gauge** reusing `rt_ring(cv, cx, cy, rad, thick, coh, band,
  track)` (`ide_runtime.c:122-159`) with the band color logic
  (`ide_runtime.c:299-301`) and the big "% + Low/Med/High" label
  (`ide_runtime.c:303-335`).
- A **build/run status line**: the three pipeline dots
  `chr_dot(cv,x,cy,on,label,...)` (`ide_chrome.c:180-192`) repurposed as
  Build(green/red) + a `ide_run_msg()` echo in `TH_CYAN` (the run-message color,
  `ide_build.c:502`).
- If `g_pulse_todo_filter` (set by the deck's "Open TODO list", §4.2), append a
  **TODO list**: every model func with `!marks_find(f->name)->done`, each a
  clickable row → `ide_sel_jump` (`ide.c:494`). This is the checklist that
  "Open TODO list" opens.

### 5.3 Why this is the completion aid

COH says "75% healthy." The Pulse says "5 functions, 2 done, 1 missing a stub,
2 warnings on the function you're looking at, build green 12s ago, COH 75% and
falling." That second sentence is what an aphantasic user otherwise has to
reconstruct from nothing every time they sit down — now it is just *there*.

---

## 6. QUALITY CONTROL — acceptance the main session must prove via QMP screenshots

Use the existing repeatable QMP tour harness (`build_test/ide_showcase_shots.sh`,
referenced `docs/IDE_SEMANTIC_LEGO.md:50-52, 248-249`). Each check is
**same-frame** (no reload) unless it says "restart."

1. **DONE → Pulse + funcs list, same frame.** Focus `tower_tick` (COH 75, has
   risks — `showcase_map.png`). Open inspector MARK tab, toggle **Done** on.
   *Assert:* VIZ-5 Pulse "Done" goes `N → N+1` and "To-do" `M → M-1` in the same
   frame; the FUNCTIONS list shows `tower_tick` with the done glyph + dimmed
   name.
2. **STAR → watch chip in chrome.** Toggle **Star** on `tower_tick`. *Assert:* a
   "WATCH: tower_tick" chip appears in the status chrome; clicking it jumps the
   selection to `tower_tick`.
3. **ISOLATE → map stays pinned.** Toggle **Isolate** on `tower_tick`. Move the
   caret into `wave_spawn` (or open `main.c`). *Assert:* the map header shows
   "LOCKED" and the central card **stays** `tower_tick` (does not re-center) —
   proving the `ide.c:486-487` gate fires.
4. **Persistence across restart (failing-then-passing).** With Done+Star+Isolate
   set, quit (`Q`) and relaunch the IDE. *Assert (passing build):* all three
   marks on `tower_tick` are restored. *Assert (failing build = the proof):*
   with the `ide_marks_load();` line at `ide.c:1117` removed, the same steps
   lose all marks on restart. Capture both.
5. **ACTIONS deck stamps results.** Open VIZ-4, press the **Build** row.
   *Assert:* the row's chip flips to `OK`/`FAIL` matching the BUILD tab
   (`ide_build.c:345`), with a fresh "Ns ago" timestamp; pressing it again later
   updates the timestamp. The misleading legend (`ide_chrome.c:290-303`) is gone,
   replaced by the six real rows.
6. **Generate → Pulse moves.** With `tower_tick` focused (1 missing card), press
   the deck's **Generate all stubs** row. *Assert:* the Pulse "Missing" count
   drops and "COH" rises in the same frame (`gen_apply_action` already
   reparses+reanalyzes, `ide_gen.c:188-203`).

---

## 7. Implementation order (smallest provable steps first)

1. **`ide_marks.c/.h`** (§2) + `ide_marks_load()` at `ide.c:1117`. Prove with a
   throwaway: set `g_marks[0]` and confirm the file round-trips. *(Foundation;
   nothing else compiles meaning without it.)*
2. **Per-symbol DONE knob** (§3, Knob 1 only) + the FUNCTIONS-list glyph
   consequence. This is the thinnest end-to-end slice that touches store +
   widget + visible consequence + persistence — acceptance #1 and #4.
3. **STAR + ISOLATE knobs** (§3.3 Knobs 2–3) + their chrome/map consequences —
   acceptance #2, #3. ISOLATE is the headline ("I ISOLATE" finally means
   something).
4. **Project Pulse** (§5) — once marks exist, the scoreboard is mostly reused
   primitives (`insp_kv`, `rt_ring`, `chr_dot`). Add the real COH history ring
   buffer here (retire the fake `trend[]`).
5. **ACTIONS deck** (§4) — last, because Generate-all and "Open TODO list"
   depend on the Pulse + GRAPH agent's GENERATE callable. Delete the legend.
6. **WARN-MUTE knob** (§3.3 Knob 4) — smallest payoff; do it when the WARN
   plumbing is already open from step 4/5.

Do **not** attempt RENAME (FUTURE, §3.3) — no reference index exists.

---

## 8. Appendix — "imitate exactly this" citation table

| You are building | Copy the shape of | At |
|---|---|---|
| `SymMark` store + persistence | `CfgRow` + `ide_config_load/save` | `ide_config.c:26-129` |
| `marks_get` linear scan | `sem_find_global` | `ide_semantic.c:126-134` |
| name-as-stable-key | `rt_streq` flow→func match | `ide_runtime.c:575-576` |
| toggle knob draw | toggle block in `panel_settings` | `ide_inspector.c:861-873` |
| slider knob draw | slider block in `panel_settings` | `ide_inspector.c:874-886` |
| knob hit-test (toggle) | `panel_settings_click` phase 0 | `ide_inspector.c:914-930` |
| knob keyboard | `panel_settings_key` | `ide_inspector.c:938-966` |
| new inspector tab | `INSP_TAB_LBL` + tab loop | `ide_inspector.c:36-38, 542-597` |
| save-on-change | `ide_config_save()` after toggle | `ide_inspector.c:920` |
| run-button draw | `[APPLY]` button | `ide_inspector.c:147, 347-351` |
| key-hint chip | `chr_shortcut` | `ide_chrome.c:196-203` |
| relative timestamp | `ide_ticks_ms()` delta | `ide_build.c:252-255` |
| scoreboard rows | `insp_kv` | `ide_inspector.c:457-466` |
| COH ring gauge | `rt_ring` + band logic | `ide_runtime.c:122-159, 299-301` |
| pipeline dots | `chr_dot` | `ide_chrome.c:180-192` |
| watch chip render | closed-function chip | `ide_funcs.c:220-228` |
| jump-to-symbol on click | `ide_sel_jump` | `ide.c:494-514` |
| focused-function fetch | `insp_focus` | `ide_inspector.c:115` |
| ISOLATE gate point | re-focus on caret crossing | `ide.c:486-487` |
| where to load marks | beside `ide_config_load()` in `init()` | `ide.c:1116` |
| tabs that become deck/pulse | VIZ_ACTIONS/POTENTIALS routing | `ide.c:1520-1527, 2160-2167` |
| legend to delete | decorative shortcut chips | `ide_chrome.c:290-303` |
