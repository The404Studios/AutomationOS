/*
 * ide.c -- MAIN integrator for the Semantic LEGO Map IDE (freestanding ring 3).
 * =============================================================================
 *
 * Owns the wl window, the layout (fills the r_* rects every frame), the event
 * loop, project scanning + file loading, and dispatches to the analysis engine
 * (model_parse/model_analyze) plus the sibling panel modules (panel_*).
 *
 * This translation unit declares the single large Ide instance (g_ide) in .bss
 * and implements: ide_open_file, ide_set_focus, scan_project, init, layout, the
 * main loop and event routing. Everything else (panel_* render/click,
 * gen_apply_action, gfx_*, model_*, ide_sys helpers) is provided by sibling
 * modules and resolved at link time.
 *
 * Freestanding: no libc / malloc / stdio.
 *
 * Build (object only -- panel externs stay unresolved until full link):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 -Wall \
 *       -c userspace/apps/ide/ide.c -o /tmp/idemain.o
 */

#include "ide.h"
#include "ide_model.h"
#include "ide_gfx.h"
#include "ide_sys.h"
#include "ide_theme.h"
#include "ide_build.h"          /* native toolchain: Build/Run + build panel */
#include "ide_editor.h"         /* editable code editor (EDITOR workspace)   */
#include "ide_term.h"           /* integrated terminal panel                 */
#include "../../lib/wl/wl_client.h"

/* When set (after pressing B in the LEGO workspace), the center-top shows the
 * build/output panel instead of the VIZ view. Any VIZ number key clears it. */
static int g_build_view = 0;

/* ---- syscall numbers we use directly (the rest go via ide_sys.c) ---- */
#define SYS_YIELD   15

/* ---- keycodes (Linux/AT set-1 style, matching kernel/include/input.h) ---- */
#define KEY_ESC      1
#define KEY_1        2
#define KEY_5        6
#define KEY_TAB      15
#define KEY_G        34
#define KEY_Q        16
#define KEY_B        48          /* Build the open file with the native toolchain */
#define KEY_R        19          /* Run the last build (SYS_SPAWN the ELF)        */
#define KEY_S        31          /* Ctrl+S save (editor)                          */
#define KEY_J        36          /* Ctrl+J toggle bottom panel focus              */
#define KEY_E        18          /* Ctrl+E toggle workspace (editor <-> LEGO)     */
#define KEY_GRAVE    41          /* Ctrl+` focus terminal                         */
#define KEY_LEFTCTRL  29
#define KEY_RIGHTCTRL 97
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_UP       103
#define KEY_DOWN     108
#define KEY_LEFT     105
#define KEY_RIGHT    106
/* PageUp/PageDown ARE in the evdev set we receive (104/109); the editor binds
 * them. Arrow-key scrolling also works in the LEGO workspace. */

/* ---- modifier state (the compositor delivers raw keycodes with no modifier
 * byte, so we track Ctrl/Shift press/release ourselves). ---- */
static int g_ctrl_down  = 0;
static int g_shift_down = 0;

/*
 * US-layout raw-keycode -> ASCII. `shift` selects the shifted glyph. Keys with
 * no printable mapping (Enter/Backspace/arrows/etc.) return 0; the editor and
 * terminal handle those by keycode.
 */
static char ide_keycode_ascii(int kc, int shift) {
    switch (kc) {
        case 2:  return shift ? '!' : '1';
        case 3:  return shift ? '@' : '2';
        case 4:  return shift ? '#' : '3';
        case 5:  return shift ? '$' : '4';
        case 6:  return shift ? '%' : '5';
        case 7:  return shift ? '^' : '6';
        case 8:  return shift ? '&' : '7';
        case 9:  return shift ? '*' : '8';
        case 10: return shift ? '(' : '9';
        case 11: return shift ? ')' : '0';
        case 12: return shift ? '_' : '-';
        case 13: return shift ? '+' : '=';
        case 16: return shift ? 'Q' : 'q';
        case 17: return shift ? 'W' : 'w';
        case 18: return shift ? 'E' : 'e';
        case 19: return shift ? 'R' : 'r';
        case 20: return shift ? 'T' : 't';
        case 21: return shift ? 'Y' : 'y';
        case 22: return shift ? 'U' : 'u';
        case 23: return shift ? 'I' : 'i';
        case 24: return shift ? 'O' : 'o';
        case 25: return shift ? 'P' : 'p';
        case 26: return shift ? '{' : '[';
        case 27: return shift ? '}' : ']';
        case 30: return shift ? 'A' : 'a';
        case 31: return shift ? 'S' : 's';
        case 32: return shift ? 'D' : 'd';
        case 33: return shift ? 'F' : 'f';
        case 34: return shift ? 'G' : 'g';
        case 35: return shift ? 'H' : 'h';
        case 36: return shift ? 'J' : 'j';
        case 37: return shift ? 'K' : 'k';
        case 38: return shift ? 'L' : 'l';
        case 39: return shift ? ':' : ';';
        case 40: return shift ? '"' : '\'';
        case 41: return shift ? '~' : '`';
        case 43: return shift ? '|' : '\\';
        case 44: return shift ? 'Z' : 'z';
        case 45: return shift ? 'X' : 'x';
        case 46: return shift ? 'C' : 'c';
        case 47: return shift ? 'V' : 'v';
        case 48: return shift ? 'B' : 'b';
        case 49: return shift ? 'N' : 'n';
        case 50: return shift ? 'M' : 'm';
        case 51: return shift ? '<' : ',';
        case 52: return shift ? '>' : '.';
        case 53: return shift ? '?' : '/';
        case 57: return ' ';
        default: return 0;
    }
}

/* ---- inspector tab indices (see Ide.insp_tab in ide.h) ---- */
#define INSP_SYNTAX   0
#define INSP_DETAILS  4

/* ---- map pan step (pixels per arrow press / drag is 1:1) ---- */
#define MAP_PAN_STEP 20

/* ============================================================================
 * The one big app-state instance. MUST live in .bss (never on the stack): the
 * Ide struct embeds src[131072] plus the model and is far too large for any
 * reasonable stack.
 * ==========================================================================*/
static Ide g_ide;

/* ===========================================================================
 * Tiny local string helpers (kept independent of libc).
 * ==========================================================================*/

/* Append b onto d (which already holds a NUL-terminated string), bounded by
 * cap (total buffer size). */
static void ide_strlcat(char* d, const char* b, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (i < cap - 1 && d[i]) i++;
    int j = 0;
    if (b) while (i < cap - 1 && b[j]) { d[i++] = b[j++]; }
    d[i] = 0;
}

/* True if name ends with ".c" */
static int ends_with_dot_c(const char* name) {
    int n = ide_strlen(name);
    return (n >= 2 && name[n - 2] == '.' && name[n - 1] == 'c');
}

/* Skip "." and "..". */
static int is_dot_entry(const char* name) {
    return (name[0] == '.' &&
            (name[1] == 0 || (name[1] == '.' && name[2] == 0)));
}

/* ===========================================================================
 * File loading + focus plumbing (declared in ide.h; called by panels too).
 * ==========================================================================*/

void ide_open_file(Ide* a, const char* path) {
    if (!a || !path) return;

    int n = ide_read_file(path, a->src, IDE_SRC_CAP);
    if (n < 0) n = 0;                 /* clamp errors to an empty buffer   */
    if (n > IDE_SRC_CAP) n = IDE_SRC_CAP;
    a->src_len = n;

    ide_strlcpy(a->cur_file, path, IDE_PATH);

    model_parse(&a->model, a->src, a->src_len, path);
    /* Default focus = the "richest" function (most reads+writes+calls) so the
     * map opens on the most interesting node instead of the first declaration. */
    a->model.focus = 0;
    a->focus_func  = 0;
    if (a->model.nfuncs <= 0) {
        a->model.focus = -1;
        a->focus_func  = 0;
    } else {
        int best = 0, best_score = -1;
        for (int i = 0; i < a->model.nfuncs; i++) {
            Func* f = &a->model.funcs[i];
            int score = f->nreads + f->nwrites + f->ncalls;
            if (score > best_score) { best_score = score; best = i; }
        }
        a->model.focus = best;
        a->focus_func  = best;
    }
    model_analyze(&a->model);

    /* Reset per-file view state so we don't keep a stale scroll/pan. */
    a->code_scroll      = 0;
    a->inspector_scroll = 0;
    a->funcs_scroll     = 0;
    a->map_ox = 0;
    a->map_oy = 0;

    /* Reset the editable editor's caret/scroll/dirty for the new file. */
    ide_editor_reset(a);
}

void ide_set_focus(Ide* a, int func_idx) {
    if (!a) return;
    int n = a->model.nfuncs;
    if (n <= 0) {                     /* nothing to focus                  */
        a->focus_func  = 0;
        a->model.focus = -1;
        model_analyze(&a->model);
        return;
    }
    if (func_idx < 0)   func_idx = 0;
    if (func_idx >= n)  func_idx = n - 1;
    a->focus_func  = func_idx;
    a->model.focus = func_idx;
    model_analyze(&a->model);
}

/* ===========================================================================
 * Project scan: recursively flatten a->root into a->entries[].
 *
 * For each directory we first emit its immediate children (dirs and files),
 * then recurse into each subdir (depth + 1). Entries carry their indent depth
 * and a fully-resolved path so the explorer can open files directly. Capped at
 * IDE_MAXENT. Uses a scratch dirent buffer per recursion level on the stack;
 * recursion depth is bounded by the directory tree depth and the IDE_MAXENT
 * cap, so the stack stays small.
 * ==========================================================================*/

/* Join dir + "/" + name into out (cap bytes), NUL-terminated. */
static void path_join(char* out, int cap, const char* dir, const char* name) {
    ide_strlcpy(out, dir, cap);
    int len = ide_strlen(out);
    /* Avoid a doubled '/'; the root may or may not have a trailing slash. */
    if (len == 0 || out[len - 1] != '/')
        ide_strlcat(out, "/", cap);
    ide_strlcat(out, name, cap);
}

static void scan_dir(Ide* a, const char* dir, int depth) {
    if (a->nentries >= IDE_MAXENT) return;
    if (depth > 16) return;           /* hard guard against cycles         */

    /* Local scratch -- one chunk of dirents for this level. We read the whole
     * directory once, record the rows, then recurse into the subdirs in a
     * second pass (so a level's files/dirs are listed together). */
    enum { LVL_MAX = 64 };
    IdeDirent ents[LVL_MAX];
    int got = ide_list_dir(dir, ents, LVL_MAX);
    if (got < 0) return;

    /* Remember where this level's rows begin so we can recurse afterwards. */
    int first_row = a->nentries;

    for (int i = 0; i < got; i++) {
        if (a->nentries >= IDE_MAXENT) return;
        const char* nm = ents[i].name;
        if (is_dot_entry(nm)) continue;

        EntryRow* e = &a->entries[a->nentries];
        ide_strlcpy(e->name, nm, (int)sizeof(e->name));
        e->is_dir = (ents[i].type == IDE_DT_DIR);
        e->depth  = depth;
        path_join(e->path, IDE_PATH, dir, nm);
        a->nentries++;
    }

    int last_row = a->nentries;

    /* Second pass: recurse into the directories we just recorded. We re-read
     * each subdir fresh (the shared scratch above is reused per call). */
    for (int r = first_row; r < last_row; r++) {
        if (a->nentries >= IDE_MAXENT) return;
        if (a->entries[r].is_dir) {
            /* Copy the path locally: scan_dir may grow a->entries and the row
             * pointer/path stays valid (fixed array), but be defensive. */
            char sub[IDE_PATH];
            ide_strlcpy(sub, a->entries[r].path, IDE_PATH);
            scan_dir(a, sub, depth + 1);
        }
    }
}

static void scan_project(Ide* a) {
    a->nentries = 0;
    a->sel_entry = 0;
    a->explorer_scroll = 0;
    scan_dir(a, a->root, 0);
}

/* ===========================================================================
 * init: set the root, scan, choose an initial file, set initial view tabs.
 * ==========================================================================*/

static void init(Ide* a) {
    ide_strlcpy(a->root, "/usr/src/towerdefense", IDE_PATH);

    scan_project(a);

    /* Pick the first file named "tower.c"; else the first .c file; else the
     * first regular file found. */
    int pick = -1;
    int first_c = -1;
    int first_file = -1;
    for (int i = 0; i < a->nentries; i++) {
        if (a->entries[i].is_dir) continue;
        if (first_file < 0) first_file = i;
        if (first_c < 0 && ends_with_dot_c(a->entries[i].name)) first_c = i;
        if (ide_streq(a->entries[i].name, "tower.c")) { pick = i; break; }
    }
    if (pick < 0) pick = (first_c >= 0) ? first_c : first_file;

    if (pick >= 0) {
        a->sel_entry = pick;
        ide_open_file(a, a->entries[pick].path);
    } else {
        /* Empty project: present an empty, well-defined model. */
        a->src_len = 0;
        a->cur_file[0] = 0;
        model_parse(&a->model, a->src, 0, "");
        a->model.focus = -1;
        a->focus_func = 0;
        model_analyze(&a->model);
    }

    a->viz = VIZ_MAP;
    a->insp_tab = 2;                  /* PORTS                              */

    /* ---- EDITOR workspace defaults (this is the default face) ---- */
    a->ws = WS_EDITOR;
    a->btab = BTAB_TERMINAL;
    a->bottom_h = 200;                /* initial bottom-dock height (px)     */
    a->term_focus = 0;                /* editor has keys initially           */
    a->editor.focused = 1;
    ide_editor_reset(a);
    ide_term_init(&a->term, a->root); /* terminal starts in the project root */
}

/* ===========================================================================
 * layout: fill the r_* rects from the live window size each frame.
 *
 *   +------------------------------------------------------------+  TOPBAR_H
 *   | topbar (full width)                                        |
 *   +----------+--------------------------------+----------------+
 *   | explorer |                                |                |
 *   | (~58%)   |        map (~58% of center)    |   inspector    |
 *   +----------+--------------------------------+   (RIGHT_W)    |
 *   | funcs    |        code (~42% of center)   |                |
 *   | (rest)   |                                |                |
 *   +----------+--------------------------------+----------------+
 *   | runtime band (full width)                              | RUNTIME_H
 *   +------------------------------------------------------------+
 *   | status (full width)                                    | STATUS_H
 *   +------------------------------------------------------------+
 * ==========================================================================*/

static void layout(Ide* a, wl_window* win) {
    int W = (int)win->w;
    int H = (int)win->h;

    /* Top bar -- full width across the very top. */
    a->r_topbar.x = 0;  a->r_topbar.y = 0;
    a->r_topbar.w = W;  a->r_topbar.h = TOPBAR_H;

    /* Status bar -- full width at the very bottom. */
    a->r_status.x = 0;  a->r_status.y = H - STATUS_H;
    a->r_status.w = W;  a->r_status.h = STATUS_H;

    /* Runtime band -- the RUNTIME_H strip just above the status bar. */
    a->r_runtime.x = 0;             a->r_runtime.y = H - STATUS_H - RUNTIME_H;
    a->r_runtime.w = W;             a->r_runtime.h = RUNTIME_H;

    /* The working region between the top bar and the runtime band. */
    int work_top    = TOPBAR_H;
    int work_bottom = H - STATUS_H - RUNTIME_H;     /* exclusive            */
    int work_h      = work_bottom - work_top;
    if (work_h < 0) work_h = 0;

    /* Left column (explorer over funcs). */
    int left_w = LEFT_W;
    if (left_w > W) left_w = W;
    int expl_h = (work_h * 58) / 100;               /* explorer ~58%        */
    if (expl_h < 0) expl_h = 0;

    a->r_explorer.x = 0;            a->r_explorer.y = work_top;
    a->r_explorer.w = left_w;       a->r_explorer.h = expl_h;

    a->r_funcs.x = 0;               a->r_funcs.y = work_top + expl_h;
    a->r_funcs.w = left_w;          a->r_funcs.h = work_h - expl_h;

    /* Right column (inspector, full working height). */
    int right_w = RIGHT_W;
    if (right_w > W - left_w) right_w = W - left_w;
    if (right_w < 0) right_w = 0;

    a->r_inspector.x = W - right_w; a->r_inspector.y = work_top;
    a->r_inspector.w = right_w;     a->r_inspector.h = work_h;

    /* Center region between the two columns (map over code). */
    int center_x = left_w;
    int center_w = W - left_w - right_w;
    if (center_w < 0) center_w = 0;

    int map_h = (work_h * 58) / 100;                /* map ~58%, code ~42%  */
    if (map_h < 0) map_h = 0;

    a->r_map.x = center_x;          a->r_map.y = work_top;
    a->r_map.w = center_w;          a->r_map.h = map_h;

    a->r_code.x = center_x;         a->r_code.y = work_top + map_h;
    a->r_code.w = center_w;         a->r_code.h = work_h - map_h;
}

/* ===========================================================================
 * layout_editor: the VS-Code-style layout for the EDITOR workspace.
 *
 *   +------------------------------------------------------------+ TOPBAR_H
 *   | topbar (workspace tabs)                                    |
 *   +----------+-------------------------------------------------+
 *   | file     |  editor (center, line numbers + caret)         |
 *   | tree     |                                                 |
 *   | (LEFT_W) +-------------------------------------------------+ btabs
 *   |          |  [TERMINAL][BUILD][PROBLEMS] tab strip          |
 *   |          +-------------------------------------------------+
 *   |          |  bottom panel (terminal / build output)        | bottom_h
 *   +----------+-------------------------------------------------+
 *   | status (full width)                                        | STATUS_H
 *   +------------------------------------------------------------+
 *
 * Responsive: the tree width and bottom-dock height clamp to sane fractions so
 * a maximized window stays usable (tree never eats more than ~30% width; the
 * bottom dock never more than ~60% height).
 * ==========================================================================*/

#define E_BTAB_H   (ROW_H + 4)        /* bottom tab strip height */

static void layout_editor(Ide* a, wl_window* win) {
    int W = (int)win->w;
    int H = (int)win->h;

    /* Top bar (workspace tabs) + status bar. */
    a->r_topbar.x = 0;  a->r_topbar.y = 0;  a->r_topbar.w = W;  a->r_topbar.h = TOPBAR_H;
    a->r_status.x = 0;  a->r_status.y = H - STATUS_H;  a->r_status.w = W;  a->r_status.h = STATUS_H;

    int work_top = TOPBAR_H;
    int work_bottom = H - STATUS_H;
    int work_h = work_bottom - work_top;
    if (work_h < 0) work_h = 0;

    /* Left file tree: LEFT_W but never more than ~30% of a wide window. */
    int tree_w = LEFT_W;
    int max_tree = (W * 30) / 100;
    if (tree_w > max_tree) tree_w = max_tree;
    if (tree_w > W) tree_w = W;
    if (tree_w < 0) tree_w = 0;

    a->r_e_tree.x = 0;        a->r_e_tree.y = work_top;
    a->r_e_tree.w = tree_w;   a->r_e_tree.h = work_h;

    /* Right region = editor over bottom dock. */
    int right_x = tree_w;
    int right_w = W - tree_w;
    if (right_w < 0) right_w = 0;

    /* Clamp bottom-dock height to [E_BTAB_H+GFX_FH, 60% of work_h]. */
    int bh = a->bottom_h;
    int max_bh = (work_h * 60) / 100;
    int min_bh = E_BTAB_H + GFX_FH;     /* at least the tab strip + one row */
    if (bh > max_bh) bh = max_bh;
    if (bh < min_bh) bh = min_bh;
    if (bh > work_h) bh = work_h;

    int editor_h = work_h - bh;
    if (editor_h < 0) editor_h = 0;

    a->r_e_editor.x = right_x;  a->r_e_editor.y = work_top;
    a->r_e_editor.w = right_w;  a->r_e_editor.h = editor_h;

    a->r_e_btabs.x = right_x;   a->r_e_btabs.y = work_top + editor_h;
    a->r_e_btabs.w = right_w;   a->r_e_btabs.h = E_BTAB_H;

    a->r_e_bottom.x = right_x;  a->r_e_bottom.y = work_top + editor_h + E_BTAB_H;
    a->r_e_bottom.w = right_w;  a->r_e_bottom.h = bh - E_BTAB_H;
    if (a->r_e_bottom.h < 0) a->r_e_bottom.h = 0;
}

/* ===========================================================================
 * Rendering: clear + draw every panel into its rect.
 * ==========================================================================*/

/* Fill the CENTER-TOP rect (r_map) according to the active VIZ tab. The right
 * inspector, left columns, code, runtime, topbar and status are ALWAYS drawn
 * below by render() -- only this region's contents change with a->viz.
 *
 * VIZ_INSPECTOR/ACTIONS/POTENTIALS reuse panel_inspector() driven by a
 * temporary insp_tab; we save/restore the real a->insp_tab so the RIGHT-side
 * inspector keeps rendering its own independently-selected tab. */
static void render_center_top(Ide* a, Canvas* cv) {
    if (g_build_view && ide_build_active()) {  /* B pressed: show build output */
        panel_build(a, cv, a->r_map);
        return;
    }
    switch (a->viz) {
    case VIZ_INSPECTOR: {
        int saved = a->insp_tab;
        a->insp_tab = INSP_SYNTAX;            /* enlarged inspector / AST   */
        panel_inspector(a, cv, a->r_map);
        a->insp_tab = saved;
        break;
    }
    case VIZ_RUNTIME:
        panel_runtime(a, cv, a->r_map);
        break;
    case VIZ_ACTIONS:
    case VIZ_POTENTIALS: {
        int saved = a->insp_tab;
        a->insp_tab = INSP_DETAILS;           /* risks / recommended actions*/
        panel_inspector(a, cv, a->r_map);
        a->insp_tab = saved;
        break;
    }
    case VIZ_MAP:
    default:
        panel_map(a, cv, a->r_map);
        break;
    }
}

static void render(Ide* a, Canvas* cv) {
    gfx_fill(cv, 0, 0, cv->w, cv->h, TH_BG);

    panel_topbar   (a, cv, a->r_topbar);
    panel_explorer (a, cv, a->r_explorer);
    panel_funcs    (a, cv, a->r_funcs);
    render_center_top(a, cv);                 /* VIZ-routed center-top      */
    panel_code     (a, cv, a->r_code);
    panel_inspector(a, cv, a->r_inspector);   /* RIGHT inspector: own tab   */
    panel_runtime  (a, cv, a->r_runtime);
    panel_status   (a, cv, a->r_status);
}

/* ===========================================================================
 * EDITOR-workspace chrome: a two-tab top bar (EDITOR / LEGO MAP), the bottom
 * tab strip (TERMINAL / BUILD / PROBLEMS) and a slim status bar.
 * ==========================================================================*/

static const char* const WS_TAB_LABEL[2] = { "EDITOR", "LEGO MAP" };

static void editor_topbar(Ide* a, Canvas* cv, Rect r) {
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_HEADER);
    gfx_hline(cv, r.x, r.y + r.h - 1, r.w, TH_BORDER);
    int ty = r.y + (r.h - GFX_FH) / 2;
    if (ty < r.y) ty = r.y;

    /* two workspace tabs, left-aligned */
    int x = r.x + PAD;
    for (int i = 0; i < 2; i++) {
        int tw = gfx_textw(WS_TAB_LABEL[i]) + 2 * GFX_FW;
        int active = ((int)a->ws == i);
        if (active) {
            gfx_fill(cv, x - GFX_FW / 2, r.y, tw, r.h, TH_PANEL);
            gfx_hline(cv, x - GFX_FW / 2, r.y + r.h - 2, tw, TH_BLUE);
            gfx_hline(cv, x - GFX_FW / 2, r.y + r.h - 1, tw, TH_BLUE);
        }
        gfx_text_clip(cv, x, ty, WS_TAB_LABEL[i],
                      active ? TH_TEXT : TH_TEXT_DIM, r.x, r.w);
        x += tw + GFX_FW;
    }

    /* right-aligned filename + dirty marker */
    const char* f = a->cur_file[0] ? a->cur_file : "(no file)";
    int fw = gfx_textw(f);
    int dirtyw = ide_editor_dirty(a) ? (gfx_textw(" *")) : 0;
    int fx = r.x + r.w - PAD - fw - dirtyw;
    if (fx > x) {
        gfx_text_clip(cv, fx, ty, f, TH_TEXT_DIM, x, r.w);
        if (ide_editor_dirty(a))
            gfx_text_clip(cv, fx + fw, ty, " *", TH_ORANGE, x, r.w);
    }
}

/* Bottom tab strip: clickable [TERMINAL] [BUILD] [PROBLEMS]. */
static const char* const E_BTAB_LABEL[3] = { "TERMINAL", "BUILD", "PROBLEMS" };

static void editor_btabs(Ide* a, Canvas* cv, Rect r) {
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_HEADER);
    gfx_hline(cv, r.x, r.y, r.w, TH_BORDER);
    int ty = r.y + (r.h - GFX_FH) / 2;
    int x = r.x + PAD;
    for (int i = 0; i < 3; i++) {
        int tw = gfx_textw(E_BTAB_LABEL[i]) + 2 * GFX_FW;
        int active = ((int)a->btab == i);
        if (active) {
            gfx_fill(cv, x - GFX_FW / 2, r.y + 1, tw, r.h - 1, TH_PANEL2);
            gfx_hline(cv, x - GFX_FW / 2, r.y + 1, tw, TH_BLUE);
        }
        gfx_text_clip(cv, x, ty, E_BTAB_LABEL[i],
                      active ? TH_TEXT : TH_TEXT_DIM, r.x, r.w);
        x += tw + GFX_FW;
    }
    /* focus hint at the right edge */
    const char* hint = a->term_focus ? "(terminal)" : "(editor)";
    int hw = gfx_textw(hint);
    int hx = r.x + r.w - PAD - hw;
    if (hx > x) gfx_text_clip(cv, hx, ty, hint, TH_TEXT_FAINT, x, r.w);
}

/* The bottom dock body: terminal, build output, or problems. */
static void editor_bottom(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    switch (a->btab) {
    case BTAB_TERMINAL:
        ide_term_render(a, cv, r);
        break;
    case BTAB_BUILD:
        panel_build(a, cv, r);
        break;
    case BTAB_PROBLEMS:
    default: {
        gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL2);
        const char* msg = a->model.nrisks > 0
            ? "See the LEGO MAP workspace inspector for semantic risks."
            : "No problems detected. Press Ctrl+B to build.";
        gfx_text_clip(cv, r.x + PAD, r.y + PAD, msg, TH_TEXT_DIM,
                      r.x + PAD, r.w - 2 * PAD);
        break;
    }
    }
}

/* Slim status bar for the editor workspace. */
static void editor_status(Ide* a, Canvas* cv, Rect r) {
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_HEADER);
    gfx_hline(cv, r.x, r.y, r.w, TH_BORDER);
    int ty = r.y + (r.h - GFX_FH) / 2;
    int x = r.x + PAD;
    int clip_w = r.w - 2 * PAD;

    /* lines */
    gfx_text_clip(cv, x, ty, "LINES:", TH_TEXT_DIM, x, clip_w);
    x += gfx_textw("LINES:") + GFX_FW;
    { char nb[16]; int n = ide_itoa(a->model.total_lines, nb); nb[n] = 0;
      gfx_text_clip(cv, x, ty, nb, TH_TEXT_DIM, r.x + PAD, clip_w);
      x += n * GFX_FW + 2 * GFX_FW; }

    /* dirty/saved */
    if (ide_editor_dirty(a)) {
        gfx_text_clip(cv, x, ty, "UNSAVED", TH_ORANGE, r.x + PAD, clip_w);
        x += gfx_textw("UNSAVED") + 2 * GFX_FW;
    } else {
        gfx_text_clip(cv, x, ty, "SAVED", TH_GREEN, r.x + PAD, clip_w);
        x += gfx_textw("SAVED") + 2 * GFX_FW;
    }

    /* right-aligned shortcut legend */
    const char* leg = "Ctrl+B build  Ctrl+S save  Ctrl+`/J term  Ctrl+E map";
    int lw = gfx_textw(leg);
    int lx = r.x + r.w - PAD - lw;
    if (lx > x) gfx_text_clip(cv, lx, ty, leg, TH_TEXT_FAINT, x, clip_w);
}

static void render_editor(Ide* a, Canvas* cv) {
    gfx_fill(cv, 0, 0, cv->w, cv->h, TH_BG);

    editor_topbar (a, cv, a->r_topbar);
    panel_explorer(a, cv, a->r_e_tree);       /* reuse the project tree     */
    ide_editor_render(a, cv, a->r_e_editor);  /* the editable editor        */
    editor_btabs  (a, cv, a->r_e_btabs);
    editor_bottom (a, cv, a->r_e_bottom);
    editor_status (a, cv, a->r_status);
}

/* ===========================================================================
 * Event routing.
 * ==========================================================================*/

/* Click in the center-top (r_map) region, dispatched by the active VIZ tab.
 * VIZ_MAP -> the LEGO map; the inspector-backed tabs -> inspector clicks (its
 * tab is forced to match what render_center_top() showed so hit-tests line up
 * with the pixels on screen, then restored). */
static void route_center_top_click(Ide* a, int mx, int my) {
    switch (a->viz) {
    case VIZ_INSPECTOR: {
        int saved = a->insp_tab;
        a->insp_tab = INSP_SYNTAX;
        panel_inspector_click(a, a->r_map, mx, my);
        a->insp_tab = saved;
        break;
    }
    case VIZ_ACTIONS:
    case VIZ_POTENTIALS: {
        int saved = a->insp_tab;
        a->insp_tab = INSP_DETAILS;
        panel_inspector_click(a, a->r_map, mx, my);
        a->insp_tab = saved;
        break;
    }
    case VIZ_RUNTIME:
        /* runtime has no interactive handler */
        break;
    case VIZ_MAP:
    default:
        panel_map_click(a, a->r_map, mx, my);
        break;
    }
}

/* Route a left-button press at (mx,my). Priority: topbar first (VIZ tabs),
 * then whichever interactive panel rect contains the point. */
static void route_click(Ide* a, int mx, int my) {
    if (panel_topbar_click(a, a->r_topbar, mx, my)) return;

    if (rect_hit(a->r_explorer, mx, my)) {
        panel_explorer_click(a, a->r_explorer, mx, my);
        return;
    }
    if (rect_hit(a->r_funcs, mx, my)) {
        panel_funcs_click(a, a->r_funcs, mx, my);
        return;
    }
    if (rect_hit(a->r_map, mx, my)) {
        route_center_top_click(a, mx, my);
        return;
    }
    if (rect_hit(a->r_inspector, mx, my)) {
        panel_inspector_click(a, a->r_inspector, mx, my);
        return;
    }
    /* code / runtime / status: no interactive handler for v1. */
}

/* Hit-test the two workspace tabs in the top bar; switch a->ws if clicked.
 * Returns 1 if a tab was hit. */
static int editor_topbar_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;
    int x = r.x + PAD;
    for (int i = 0; i < 2; i++) {
        int tw = gfx_textw(WS_TAB_LABEL[i]) + 2 * GFX_FW;
        if (mx >= x - GFX_FW / 2 && mx < x - GFX_FW / 2 + tw) {
            a->ws = (Workspace)i;
            return 1;
        }
        x += tw + GFX_FW;
    }
    return 1;   /* consumed (top bar) even on empty space */
}

/* Hit-test the bottom tab strip; switch a->btab if clicked. */
static int editor_btabs_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;
    int x = r.x + PAD;
    for (int i = 0; i < 3; i++) {
        int tw = gfx_textw(E_BTAB_LABEL[i]) + 2 * GFX_FW;
        if (mx >= x - GFX_FW / 2 && mx < x - GFX_FW / 2 + tw) {
            a->btab = (BottomTab)i;
            /* clicking the terminal tab also gives it focus */
            if (a->btab == BTAB_TERMINAL) { a->term_focus = 1; a->editor.focused = 0; }
            return 1;
        }
        x += tw + GFX_FW;
    }
    return 1;
}

/* Route a left-button press in the EDITOR workspace. */
static void route_click_editor(Ide* a, int mx, int my) {
    if (editor_topbar_click(a, a->r_topbar, mx, my)) return;

    if (rect_hit(a->r_e_tree, mx, my)) {
        /* The tree uses the same explorer panel; opening a file gives the
         * editor focus so the user can type immediately. */
        panel_explorer_click(a, a->r_e_tree, mx, my);
        a->term_focus = 0;
        a->editor.focused = 1;
        return;
    }
    if (rect_hit(a->r_e_editor, mx, my)) {
        a->term_focus = 0;
        ide_editor_click(a, a->r_e_editor, mx, my);
        return;
    }
    if (editor_btabs_click(a, a->r_e_btabs, mx, my)) return;
    if (rect_hit(a->r_e_bottom, mx, my)) {
        /* clicking the bottom dock body focuses the terminal (when shown) */
        if (a->btab == BTAB_TERMINAL) { a->term_focus = 1; a->editor.focused = 0; }
        return;
    }
}

/* Scroll the panel currently under the mouse by `delta` rows (clamped >= 0).
 * Over the center-top map region we PAN instead of scrolling: vertical arrows
 * adjust map_oy, horizontal arrows adjust map_ox. */
static void scroll_under_mouse(Ide* a, int delta, int horizontal) {
    int mx = a->mouse_x, my = a->mouse_y;
    int* target = 0;

    /* Center-top region: pan the map (only meaningful for VIZ_MAP, but panning
     * the offsets is harmless for the other tabs). */
    if (rect_hit(a->r_map, mx, my)) {
        if (horizontal) a->map_ox += delta * MAP_PAN_STEP;
        else            a->map_oy += delta * MAP_PAN_STEP;
        return;
    }

    if (horizontal) return;   /* the row lists only scroll vertically */

    if (rect_hit(a->r_explorer, mx, my))       target = &a->explorer_scroll;
    else if (rect_hit(a->r_funcs, mx, my))     target = &a->funcs_scroll;
    else if (rect_hit(a->r_code, mx, my))      target = &a->code_scroll;
    else if (rect_hit(a->r_inspector, mx, my)) target = &a->inspector_scroll;

    if (!target) return;
    *target += delta;
    if (*target < 0) *target = 0;
}

/* Shared Ctrl-chord actions available in BOTH workspaces. Returns 1 if the
 * chord was handled. Modifier state is already tracked in g_ctrl_down. */
static int handle_ctrl_chord(Ide* a, int keycode) {
    switch (keycode) {
    case KEY_B:               /* Ctrl+B: build the open file */
        ide_do_build(a);
        if (a->ws == WS_EDITOR) a->btab = BTAB_BUILD;
        else                    g_build_view = 1;
        return 1;
    case KEY_R:               /* Ctrl+R: run the last build */
        ide_do_run(a);
        if (a->ws == WS_EDITOR) a->btab = BTAB_BUILD;
        else                    g_build_view = 1;
        return 1;
    case KEY_S:               /* Ctrl+S: save (editor) */
        ide_editor_save(a);
        return 1;
    case KEY_E:               /* Ctrl+E: toggle workspace */
        a->ws = (a->ws == WS_EDITOR) ? WS_LEGO : WS_EDITOR;
        return 1;
    case KEY_GRAVE:           /* Ctrl+`: focus terminal + show it */
        if (a->ws == WS_EDITOR) {
            a->btab = BTAB_TERMINAL;
            a->term_focus = !a->term_focus;
            a->editor.focused = !a->term_focus;
        }
        return 1;
    case KEY_J:               /* Ctrl+J: toggle bottom-panel focus */
        if (a->ws == WS_EDITOR) {
            a->term_focus = !a->term_focus;
            a->editor.focused = !a->term_focus;
            if (a->term_focus) a->btab = BTAB_TERMINAL;
        }
        return 1;
    default:
        return 0;
    }
}

static void handle_key(Ide* a, int keycode, int pressed) {
    /* ---- modifier tracking (compositor sends no modifier byte) ---- */
    if (keycode == KEY_LEFTCTRL || keycode == KEY_RIGHTCTRL) {
        g_ctrl_down = pressed; return;
    }
    if (keycode == KEY_LEFTSHIFT || keycode == KEY_RIGHTSHIFT) {
        g_shift_down = pressed; return;
    }

    if (!pressed) return;

    /* Ctrl chords first (work in both workspaces). */
    if (g_ctrl_down) {
        if (handle_ctrl_chord(a, keycode)) return;
        /* Ctrl + something we don't bind: swallow so it never types. */
        return;
    }

    /* ESC always exits (matches the LEGO workspace's original behaviour). */
    if (keycode == KEY_ESC) { ide_exit(0); return; }

    /* ============ EDITOR workspace: route typing to editor/terminal ======= */
    if (a->ws == WS_EDITOR) {
        char ch = ide_keycode_ascii(keycode, g_shift_down);
        if (a->term_focus)
            ide_term_key(a, keycode, ch, g_shift_down, 0);
        else
            ide_editor_key(a, keycode, ch, g_shift_down, 0);
        return;
    }

    /* =================== LEGO workspace: analysis shortcuts =============== */

    /* VIZ tab shortcuts '1'..'5' (keycodes 2..6). */
    if (keycode >= KEY_1 && keycode <= KEY_5) {
        a->viz = (VizTab)(keycode - KEY_1);   /* 0..4 == VIZ_MAP..POTENTIALS */
        g_build_view = 0;                     /* leave the build view        */
        return;
    }

    switch (keycode) {
    case KEY_B:
        ide_do_build(a);
        g_build_view = 1;
        break;
    case KEY_R:
        ide_do_run(a);
        g_build_view = 1;
        break;
    case KEY_UP:
        scroll_under_mouse(a, -1, 0);
        break;
    case KEY_DOWN:
        scroll_under_mouse(a, +1, 0);
        break;
    case KEY_LEFT:
        scroll_under_mouse(a, -1, 1);     /* horizontal: pans map_ox        */
        break;
    case KEY_RIGHT:
        scroll_under_mouse(a, +1, 1);
        break;
    case KEY_TAB: {
        int n = a->model.nfuncs;
        if (n > 0) ide_set_focus(a, (a->focus_func + 1) % n);
        break;
    }
    case KEY_G:
        if (a->model.nactions > 0 && gen_apply_action(a, 0)) {
            model_parse(&a->model, a->src, a->src_len, a->cur_file);
            ide_set_focus(a, a->focus_func);
        }
        break;
    case KEY_Q:
        ide_exit(0);
        break;
    default:
        break;
    }
}

/* ===========================================================================
 * _start -- entry point (matches the wl-direct app convention in notes.c:
 * a bare `void _start(void)`, no argc/argv, no return).
 * ==========================================================================*/

void _start(void) {
    Ide* a = &g_ide;

    init(a);

    if (wl_connect() != 0) {
        ide_exit(1);
    }

    wl_window* win = wl_create_window(IDE_W, IDE_H, "ide");
    if (!win) {
        ide_exit(1);
    }

    a->mouse_x = 0;
    a->mouse_y = 0;
    a->buttons = 0;
    a->prev_buttons = 0;

    /* Left-drag panning state for the center-top map. Lives here (not in the
     * Ide struct, which ide.h owns) for the lifetime of the loop:
     *   drag_active  -- a left-press that landed in r_map is being tracked
     *   drag_panning -- movement passed the threshold, so we're now panning
     *                   (and must NOT emit a click on release)
     *   drag_px/py   -- last pointer position used to compute the delta
     *   drag_dx/dy   -- accumulated movement since press (for the threshold) */
    int drag_active = 0, drag_panning = 0;
    int drag_px = 0, drag_py = 0, drag_dx = 0, drag_dy = 0;
    const int DRAG_THRESH = 4;            /* px of travel before it's a drag */

    long last_ms = ide_ticks_ms();

    for (;;) {
        /* Recompute layout (window size may change) and render a fresh frame
         * according to the active workspace. */
        a->win_w = (int)win->w;
        a->win_h = (int)win->h;

        Canvas cv;
        cv.px     = win->pixels;
        cv.stride = (int)(win->stride / 4);   /* stride is BYTES -> PIXELS  */
        cv.w      = (int)win->w;
        cv.h      = (int)win->h;

        /* advance blink animations from wall-clock delta */
        long now_ms = ide_ticks_ms();
        int dt = (int)(now_ms - last_ms);
        if (dt < 0) dt = 0;
        last_ms = now_ms;
        ide_editor_tick(a, dt);
        ide_term_tick(&a->term, dt);

        if (a->ws == WS_EDITOR) {
            layout_editor(a, win);
            render_editor(a, &cv);
        } else {
            layout(a, win);
            render(a, &cv);
        }
        wl_commit(win);

        /* Drain all pending input events for this frame. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            if (kind == WL_EVENT_POINTER) {
                a->mouse_x = ea;
                a->mouse_y = eb;
                a->buttons = ec;

                int left_now  = (ec & 1);
                int left_prev = (a->prev_buttons & 1);

                if (a->ws == WS_EDITOR) {
                    /* EDITOR: route clicks immediately on the press edge; no
                     * map-drag panning in this workspace. */
                    if (left_now && !left_prev)
                        route_click_editor(a, ea, eb);
                } else {
                    /* LEGO: map drag vs click handling. */
                    if (left_now && !left_prev) {
                        if (rect_hit(a->r_map, ea, eb)) {
                            drag_active = 1; drag_panning = 0;
                            drag_px = ea; drag_py = eb; drag_dx = 0; drag_dy = 0;
                        } else {
                            route_click(a, ea, eb);
                        }
                    } else if (left_now && left_prev) {
                        if (drag_active) {
                            int mdx = ea - drag_px;
                            int mdy = eb - drag_py;
                            drag_dx += (mdx < 0 ? -mdx : mdx);
                            drag_dy += (mdy < 0 ? -mdy : mdy);
                            if (!drag_panning &&
                                (drag_dx > DRAG_THRESH || drag_dy > DRAG_THRESH))
                                drag_panning = 1;
                            if (drag_panning) {
                                a->map_ox += mdx;
                                a->map_oy += mdy;
                            }
                            drag_px = ea; drag_py = eb;
                        }
                    } else if (!left_now && left_prev) {
                        if (drag_active && !drag_panning)
                            route_center_top_click(a, ea, eb);
                        drag_active = 0; drag_panning = 0;
                    }
                }

                a->prev_buttons = ec;
            } else if (kind == WL_EVENT_KEY) {
                handle_key(a, ea, eb);
            }
        }

        /* Brief cooperative yield between frames; input still arrives via the
         * poll above. */
        ide_sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}
