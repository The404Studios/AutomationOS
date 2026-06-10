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
#include "ide_library.h"
#include "ide_build.h"          /* native toolchain: Build/Run + build panel */
#include "ide_editor.h"         /* editable code editor (EDITOR workspace)   */
#include "ide_term.h"           /* integrated terminal panel                 */
#include "ide_library.h"        /* "complex" snippet library (disk loader)   */
#include "ide_config.h"         /* persist Settings knobs (load on init)     */
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
#define KEY_N        49          /* Ctrl+N new project (templates picker)         */
#define KEY_S        31          /* Ctrl+S save (editor)                          */
#define KEY_A        30          /* Ctrl+A select all (editor)                    */
#define KEY_X        45          /* Ctrl+X cut (editor)                           */
#define KEY_C        46          /* Ctrl+C copy (editor)                          */
#define KEY_V        47          /* Ctrl+V paste (editor)                         */
#define KEY_F        33          /* Ctrl+F find (editor)                          */
#define KEY_H        35          /* Ctrl+H find & replace (editor)                */
#define KEY_D        32          /* Ctrl+D duplicate line (editor)                */
#define KEY_K        37          /* Ctrl+Shift+K delete line (editor)             */
#define KEY_ENTER    28          /* confirm in the New Project modal              */
#define KEY_BACKSPC  14          /* edit the typed project name                   */
#define KEY_J        36          /* Ctrl+J toggle bottom panel focus              */
#define KEY_E        18          /* Ctrl+E toggle workspace (editor <-> LEGO)     */
#define KEY_W        17          /* Ctrl+W toggle word wrap (editor)              */
#define KEY_M        50          /* Ctrl+M toggle minimap (editor)                */
#define KEY_GRAVE    41          /* Ctrl+` focus terminal                         */
#define KEY_LEFTCTRL  29
#define KEY_RIGHTCTRL 97
#define KEY_LEFTSHIFT 42
#define KEY_RIGHTSHIFT 54
#define KEY_UP       103
#define KEY_DOWN     108
#define KEY_LEFT     105
#define KEY_RIGHT    106
#define KEY_EQUAL    13          /* Ctrl+= zoom in (LEGO map)                     */
#define KEY_MINUS    12          /* Ctrl+- zoom out (LEGO map)                    */
#define KEY_0        11          /* Ctrl+0 reset zoom to 100% (LEGO map)          */
#define KEY_Z        44          /* Ctrl+Z undo (editor)                          */
#define KEY_Y        21          /* Ctrl+Y redo (editor)                          */
#define KEY_COMMA    51          /* Ctrl+, open Settings (VIZ-6)                   */
/* PageUp/PageDown ARE in the evdev set we receive (104/109); the editor binds
 * them. Arrow-key scrolling also works in the LEGO workspace. */

/* ---- modifier state (the compositor delivers raw keycodes with no modifier
 * byte, so we track Ctrl/Shift press/release ourselves). ---- */
static int g_ctrl_down  = 0;
static int g_shift_down = 0;

/* Dirty flag: the IDE only re-renders + wl_commits when something actually
 * changed (input, zoom, resize, or a periodic caret-blink tick). Previously the
 * main loop redrew + committed full-surface EVERY iteration, forcing the
 * compositor to recomposite the whole screen continuously -- the "super laggy"
 * report, 4x worse at 2x text. Seeded 1 so the first frame always draws. */
static int g_ide_redraw = 1;

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
#define INSP_LIB      5   /* "complex library" palette -- RIGHT-sidebar only */

/* ---- map pan step (pixels per arrow press / drag is 1:1) ---- */
#define MAP_PAN_STEP (g_map_pan_step < 1 ? 1 : g_map_pan_step)   /* Settings knob */

/* ============================================================================
 * The one big app-state instance. MUST live in .bss (never on the stack): the
 * Ide struct embeds src[131072] plus the model and is far too large for any
 * reasonable stack.
 * ==========================================================================*/
static Ide g_ide;

/* Multi-file tab source backing stores (.bss, 8*128KB = 1MB). */
static char g_tab_src[IDE_MAX_TABS][IDE_SRC_CAP];

static void tab_save_active(Ide* a) {
    int i = a->tab_active;
    if (i < 0 || i >= IDE_MAX_TABS || !a->tabs[i].used) return;
    ide_strlcpy(a->tabs[i].path, a->cur_file, IDE_PATH);
    a->tabs[i].src_len    = a->src_len;
    a->tabs[i].editor     = a->editor;
    a->tabs[i].focus_func = a->focus_func;
    a->tabs[i].prev_focus = a->prev_focus;
    for (int j = 0; j < a->src_len && j < IDE_SRC_CAP; j++)
        g_tab_src[i][j] = a->src[j];
}

static void tab_restore(Ide* a, int idx) {
    if (idx < 0 || idx >= IDE_MAX_TABS || !a->tabs[idx].used) return;
    int len = a->tabs[idx].src_len;
    if (len > IDE_SRC_CAP) len = IDE_SRC_CAP;
    if (len < 0) len = 0;
    for (int j = 0; j < len; j++)
        a->src[j] = g_tab_src[idx][j];
    a->src_len    = len;
    a->editor     = a->tabs[idx].editor;
    a->focus_func = a->tabs[idx].focus_func;
    a->prev_focus = a->tabs[idx].prev_focus;
    ide_strlcpy(a->cur_file, a->tabs[idx].path, IDE_PATH);
    a->tab_active = idx;
    model_parse(&a->model, a->src, a->src_len, a->cur_file);
    if (a->model.nfuncs > 0) {
        if (a->focus_func >= a->model.nfuncs) a->focus_func = 0;
        a->model.focus = a->focus_func;
    } else { a->model.focus = -1; }
    model_analyze(&a->model);
}

static int tab_find(Ide* a, const char* path) {
    for (int i = 0; i < IDE_MAX_TABS; i++)
        if (a->tabs[i].used && ide_streq(a->tabs[i].path, path)) return i;
    return -1;
}

static int tab_alloc(Ide* a) {
    for (int i = 0; i < IDE_MAX_TABS; i++)
        if (!a->tabs[i].used) { a->tabs[i].used = 1; a->tab_count++; return i; }
    return -1;
}

static const char* tab_basename(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') base = p + 1;
    return base;
}

void ide_tab_switch(Ide* a, int idx) {
    if (idx < 0 || idx >= IDE_MAX_TABS || !a->tabs[idx].used) return;
    if (idx == a->tab_active) return;
    tab_save_active(a);
    tab_restore(a, idx);
}

void ide_tab_close(Ide* a, int idx) {
    if (idx < 0 || idx >= IDE_MAX_TABS || !a->tabs[idx].used) return;
    if (idx == a->tab_active && a->editor.dirty && a->cur_file[0])
        ide_editor_save(a);
    a->tabs[idx].used = 0;
    a->tabs[idx].path[0] = 0;
    a->tabs[idx].src_len = 0;
    a->tab_count--;
    if (a->tab_count < 0) a->tab_count = 0;
    if (idx == a->tab_active) {
        int next = -1;
        for (int i = idx + 1; i < IDE_MAX_TABS; i++)
            if (a->tabs[i].used) { next = i; break; }
        if (next < 0)
            for (int i = idx - 1; i >= 0; i--)
                if (a->tabs[i].used) { next = i; break; }
        if (next >= 0) {
            a->tab_active = -1;
            tab_restore(a, next);
        } else {
            a->tab_active = -1;
            a->src_len = 0;
            a->cur_file[0] = 0;
            ide_editor_reset(a);
            model_parse(&a->model, a->src, 0, "");
            a->model.focus = -1; a->focus_func = -1;
            model_analyze(&a->model);
        }
    }
}

void ide_tab_next(Ide* a) {
    if (a->tab_count <= 1) return;
    int start = a->tab_active;
    if (start < 0) start = 0;
    int i = start;
    for (;;) {
        i = (i + 1) % IDE_MAX_TABS;
        if (a->tabs[i].used) { ide_tab_switch(a, i); return; }
        if (i == start) return;
    }
}

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

/* True if name ends with ".elf" (a built, runnable artifact). */
static int ends_with_dot_elf(const char* name) {
    int n = ide_strlen(name);
    return (n >= 4 && name[n - 4] == '.' && name[n - 3] == 'e' &&
            name[n - 2] == 'l' && name[n - 1] == 'f');
}

/* Skip "." and "..". */
static int is_dot_entry(const char* name) {
    return (name[0] == '.' &&
            (name[1] == 0 || (name[1] == '.' && name[2] == 0)));
}

/* Forward declaration: auto-expand parent dirs of opened file (defined below
 * rebuild_visible_entries, after the collapse helpers it depends on). */
static void ide_expand_to_file(Ide* a, const char* file_path);

/* ===========================================================================
 * File loading + focus plumbing (declared in ide.h; called by panels too).
 * ==========================================================================*/

void ide_open_file(Ide* a, const char* path) {
    if (!a || !path) return;

    /* A built artifact is a PROGRAM, not source: RUN it (SYS_SPAWN). */
    if (ends_with_dot_elf(path)) {
        ide_sc(16 /* SYS_SPAWN */, (long)path, 0, 0, 0, 0, 0);
        return;
    }

    /* Multi-file tab integration: if the file is already open in a tab, switch
     * to it. Otherwise save the current tab and create a new one. */
    {
        int existing = tab_find(a, path);
        if (existing >= 0) {
            ide_tab_switch(a, existing);
            ide_expand_to_file(a, path);
            return;
        }
        tab_save_active(a);
        int slot = tab_alloc(a);
        if (slot < 0) {
            /* All tabs full: reuse the active slot. */
            slot = a->tab_active;
            if (slot < 0) slot = 0;
            if (!a->tabs[slot].used) { a->tabs[slot].used = 1; a->tab_count++; }
        }
        a->tab_active = slot;
        ide_strlcpy(a->tabs[slot].path, path, IDE_PATH);
    }

    int n = ide_read_file(path, a->src, IDE_SRC_CAP);
    if (n < 0) n = 0;                 /* clamp errors to an empty buffer   */
    if (n > IDE_SRC_CAP) n = IDE_SRC_CAP;

    /* Binary content guard: scan the first 256 bytes (or the whole file if
     * shorter) for NUL characters. Text files never contain NUL; ELF headers,
     * .o relocatables, and other binary formats do. When detected, replace the
     * buffer with a clear message instead of showing raw binary garbage. */
    {
        int probe = n < 256 ? n : 256;
        int is_bin = 0;
        for (int i = 0; i < probe; i++) {
            if (a->src[i] == '\0') { is_bin = 1; break; }
        }
        if (is_bin) {
            const char* msg = "(binary file -- cannot display in the editor)";
            int ml = 0; while (msg[ml]) ml++;
            for (int i = 0; i < ml && i < IDE_SRC_CAP; i++) a->src[i] = msg[i];
            n = ml < IDE_SRC_CAP ? ml : IDE_SRC_CAP;
        }
    }

    a->src_len = n;

    ide_strlcpy(a->cur_file, path, IDE_PATH);

    model_parse(&a->model, a->src, a->src_len, path);
    /* Default focus = overview (all elements visible as LEGO tiles).
     * The user clicks a function tile to drill into the dependency graph. */
    a->model.focus = -1;
    a->focus_func  = -1;
    a->prev_focus  = -1;
    model_analyze(&a->model);
    ide_sel_reset(a);                 /* IDE-SYNC-0: new file, fresh selection */

    /* Reset per-file view state so we don't keep a stale scroll/pan. */
    a->code_scroll      = 0;
    a->codeview_focus   = 0;   /* code view starts unfocused on a new file */
    a->inspector_scroll = 0;
    a->funcs_scroll     = 0;
    a->map_ox = 0;
    a->map_oy = 0;
    a->map_zoom = 100;              /* reset to 100% on file load */

    /* Reset the editable editor's caret/scroll/dirty for the new file. */
    ide_editor_reset(a);

    /* Sync the new tab's metadata with what we just loaded. */
    tab_save_active(a);

    /* Auto-expand parent directories so the opened file is visible in the
     * explorer tree, and scroll/select its row. */
    ide_expand_to_file(a, path);
}

void ide_set_focus(Ide* a, int func_idx) {
    if (!a) return;
    a->flow_step_focus = -1;          /* a focus change clears any runtime-flow trace */
    a->map_selected    = -1;          /* ...and any map-node selection (satellites change) */
    int n = a->model.nfuncs;
    if (n <= 0) {                     /* nothing to focus                  */
        a->focus_func  = -1;
        a->model.focus = -1;
        model_analyze(&a->model);
        return;
    }
    /* func_idx == -1 => go to overview (no function focused) */
    if (func_idx < -1)  func_idx = -1;
    if (func_idx >= n)  func_idx = n - 1;
    a->focus_func  = func_idx;
    a->model.focus = func_idx;
    model_analyze(&a->model);
}

/* ===========================================================================
 * IDE-SYNC-0: THE unified selection model (a->sel).
 * One selection, three panes. ide_sel_from_caret() is the caret-side writer;
 * ide_set_focus() callers (map/runtime/inspector clicks) are the other side.
 * ===========================================================================*/

void ide_sel_reset(Ide* a) {
    if (!a) return;
    int i = 0;
    while (a->cur_file[i] && i < IDE_PATH - 1) { a->sel.file[i] = a->cur_file[i]; i++; }
    a->sel.file[i] = 0;
    a->sel.line    = 0;
    a->sel.symbol  = -1;
    a->sel.node    = -1;
    a->sel.pane    = PANE_EDITOR;
}

/* Resolve the caret's enclosing function by the parser's recorded line ranges
 * (Func.line_start/line_end are 1-based; caret_line is 0-based) and write THE
 * selection model. Cheap (linear over <=M_MAXFUNCS) -- safe to call after
 * every caret-moving key/click. */
void ide_sel_from_caret(Ide* a, int pane) {
    if (!a) return;
    int line = a->editor.caret_line;
    int sym  = -1;
    for (int i = 0; i < a->model.nfuncs; i++) {
        const Func* f = &a->model.funcs[i];
        if (line + 1 >= f->line_start && line + 1 <= f->line_end) { sym = i; break; }
    }
    a->sel.pane   = pane;
    a->sel.line   = line;
    a->sel.symbol = sym;
    a->sel.node   = a->map_selected;
    {
        int i = 0;
        while (a->cur_file[i] && i < IDE_PATH - 1) { a->sel.file[i] = a->cur_file[i]; i++; }
        a->sel.file[i] = 0;
    }
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

/* Is `path` currently in the user's collapsed-folders set? */
int ide_is_collapsed(Ide* a, const char* path) {
    for (int i = 0; i < a->n_collapsed; i++)
        if (ide_streq(a->collapsed_paths[i], path)) return 1;
    return 0;
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
        e->collapsed = e->is_dir ? ide_is_collapsed(a, e->path) : 0;
        a->nentries++;
    }

    int last_row = a->nentries;

    /* Second pass: recurse into the directories we just recorded. We re-read
     * each subdir fresh (the shared scratch above is reused per call). */
    for (int r = first_row; r < last_row; r++) {
        if (a->nentries >= IDE_MAXENT) return;
        if (a->entries[r].is_dir && !ide_is_collapsed(a, a->entries[r].path)) {
            /* Copy the path locally: scan_dir may grow a->entries and the row
             * pointer/path stays valid (fixed array), but be defensive.
             * Collapsed folders are listed but NOT recursed into. */
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
    a->n_collapsed = 0;          /* a freshly opened project starts fully expanded */
    scan_dir(a, a->root, 0);
}

/* Toggle a folder's collapsed state (by path) in the small collapsed set. */
void ide_toggle_collapsed(Ide* a, const char* path) {
    for (int i = 0; i < a->n_collapsed; i++) {
        if (ide_streq(a->collapsed_paths[i], path)) {
            for (int j = i; j < a->n_collapsed - 1; j++)
                ide_strlcpy(a->collapsed_paths[j], a->collapsed_paths[j + 1], IDE_PATH);
            a->n_collapsed--;
            return;
        }
    }
    if (a->n_collapsed < IDE_MAXCOLLAPSE)
        ide_strlcpy(a->collapsed_paths[a->n_collapsed++], path, IDE_PATH);
}

/* Re-scan the tree (collapsed folders not recursed into) and restore the
 * selection by path, since hiding/showing children shifts indices. */
void rebuild_visible_entries(Ide* a) {
    char selpath[IDE_PATH];
    selpath[0] = 0;
    if (a->sel_entry >= 0 && a->sel_entry < a->nentries)
        ide_strlcpy(selpath, a->entries[a->sel_entry].path, IDE_PATH);

    a->nentries = 0;
    scan_dir(a, a->root, 0);

    a->sel_entry = 0;
    if (selpath[0]) {
        for (int i = 0; i < a->nentries; i++)
            if (ide_streq(a->entries[i].path, selpath)) { a->sel_entry = i; break; }
    }
    if (a->explorer_scroll > a->nentries - 1) a->explorer_scroll = a->nentries - 1;
    if (a->explorer_scroll < 0) a->explorer_scroll = 0;
}

/* Auto-expand parent directories so `file_path` is visible in the tree.
 * Walks the path from root to leaf, un-collapsing every ancestor directory
 * that is currently collapsed, then rebuilds the visible tree and scrolls
 * the target entry into view. Called after ide_open_file so the newly opened
 * file is always visible in the explorer. */
static void ide_expand_to_file(Ide* a, const char* file_path) {
    if (!a || !file_path || !file_path[0]) return;

    /* Build each ancestor prefix of file_path and ensure it is NOT in the
     * collapsed set. For "/usr/src/game/main.c" we check "/usr", "/usr/src",
     * "/usr/src/game" -- skipping the leaf (the file itself). */
    int plen = ide_strlen(file_path);
    int changed = 0;
    for (int i = 1; i < plen; i++) {
        if (file_path[i] == '/') {
            /* Extract prefix [0..i) */
            char prefix[IDE_PATH];
            int j;
            for (j = 0; j < i && j < IDE_PATH - 1; j++)
                prefix[j] = file_path[j];
            prefix[j] = 0;

            /* If this directory is collapsed, un-collapse it */
            if (ide_is_collapsed(a, prefix)) {
                ide_toggle_collapsed(a, prefix);
                changed = 1;
            }
        }
    }

    if (changed)
        rebuild_visible_entries(a);

    /* Now find the file in the visible entries and select + scroll to it. */
    for (int i = 0; i < a->nentries; i++) {
        if (ide_streq(a->entries[i].path, file_path)) {
            a->sel_entry = i;
            /* Scroll so the file is visible, centered-ish in the panel. */
            int vis_approx = 12;  /* conservative; actual depends on panel size */
            int target_scroll = i - vis_approx / 3;
            if (target_scroll < 0) target_scroll = 0;
            if (target_scroll > a->nentries - 1) target_scroll = a->nentries - 1;
            a->explorer_scroll = target_scroll;
            break;
        }
    }
}

/* Re-scan the project (fully expanded) and reveal a build artifact: select the
 * row whose path == sel_path (the produced ELF), or failing that the build
 * folder `dir`, scrolling it into view. Called after a successful build so the
 * new build/ folder + ELF are immediately visible and openable. The tree is
 * enumerated live (real readdir), so a freshly-created folder/file shows up. */
void ide_reveal_dir(Ide* a, const char* dir, const char* sel_path) {
    if (!a) return;
    scan_project(a);                       /* full rescan, all folders expanded */

    int hit = -1;
    if (sel_path && sel_path[0]) {
        for (int i = 0; i < a->nentries; i++)
            if (ide_streq(a->entries[i].path, sel_path)) { hit = i; break; }
    }
    if (hit < 0 && dir && dir[0]) {
        for (int i = 0; i < a->nentries; i++)
            if (ide_streq(a->entries[i].path, dir)) { hit = i; break; }
    }
    if (hit >= 0) {
        a->sel_entry = hit;
        /* Scroll so the selected row sits a few lines down from the top. */
        a->explorer_scroll = hit > 3 ? hit - 3 : 0;
        if (a->explorer_scroll > a->nentries - 1) a->explorer_scroll = a->nentries - 1;
        if (a->explorer_scroll < 0) a->explorer_scroll = 0;
    }
}

/* ===========================================================================
 * "New Project" flow: pick a template under /usr/src/templates/, name it, then
 * clone the template directory into /usr/src/<name>/ and open its main .c.
 *
 * Cloning is done entirely through the AOS syscalls already wrapped in
 * ide_sys.c: SYS_MKDIR creates the destination dir, SYS_READDIR enumerates the
 * template's files, and each file is copied verbatim with ide_read_file +
 * ide_write_file. No libc, no recursion (templates are a single flat dir of a
 * clean .c + README).
 * ==========================================================================*/

#define SYS_MKDIR_NUM 67          /* matches kernel/include/syscall.h          */

/* Reuse the big source buffer as a scratch copy area so we don't add another
 * 128 KB to .bss. ide_open_file() overwrites a->src afterwards anyway. */

/* Open the modal: discover template dirs under /usr/src/templates/. */
static void np_open(Ide* a) {
    NewProj* np = &a->np;
    np->ntpl = 0;
    np->sel  = 0;
    np->name_len = 0;
    np->name[0]  = 0;
    np->status[0] = 0;

    IdeDirent ents[NP_MAXTPL * 2];
    int got = ide_list_dir(IDE_TEMPLATES_DIR, ents,
                           (int)(sizeof(ents) / sizeof(ents[0])));
    for (int i = 0; i < got && np->ntpl < NP_MAXTPL; i++) {
        if (ents[i].type != IDE_DT_DIR) continue;     /* only directories  */
        if (is_dot_entry(ents[i].name)) continue;
        ide_strlcpy(np->tpl[np->ntpl], ents[i].name, 64);
        np->ntpl++;
    }

    if (np->ntpl <= 0) {
        /* No templates on disk: still show the modal with a clear message so
         * the user isn't left wondering why nothing happened. */
        ide_strlcpy(np->status, "no templates in /usr/src/templates", 96);
    }
    np->phase = NP_PICK;
}

static void np_close(Ide* a) { a->np.phase = NP_CLOSED; }

/* Public entry point (declared in ide.h) so ide_chrome.c can open the modal. */
void ide_new_project(Ide* a) { if (a) np_open(a); }

/* Choose the file to open after a clone: prefer the template's canonical main
 * source (game.c / app.c / service.c / main.c), else the first .c file. The
 * dst path (/usr/src/<name>) is prepended. Writes into out (cap bytes); leaves
 * out empty if nothing suitable was copied. */
static int np_is_preferred_main(const char* name) {
    return ide_streq(name, "game.c")   || ide_streq(name, "app.c") ||
           ide_streq(name, "service.c")|| ide_streq(name, "main.c");
}

/* Clone template dir `src_dir` into `dst_dir` (the project's src/), copying every
 * regular file. The chosen main .c is written as "main.c" so the manifest entry
 * stays src/main.c; other files keep their names. open_path receives the path of
 * the written main.c. Returns the number of files copied, or <0 on a fatal error. */
static int np_clone(Ide* a, const char* src_dir, const char* dst_dir,
                    char* open_path, int open_cap) {
    if (open_cap > 0) open_path[0] = 0;

    ide_sc(SYS_MKDIR_NUM, (long)dst_dir, 0755, 0, 0, 0, 0);

    IdeDirent ents[64];
    int got = ide_list_dir(src_dir, ents, 64);
    if (got < 0) return -1;

    /* Pick the main source: a preferred main (game/app/service/main.c) wins,
     * else the first .c. */
    int main_idx = -1;
    for (int i = 0; i < got; i++) {
        if (ents[i].type != IDE_DT_REG || is_dot_entry(ents[i].name)) continue;
        if (ends_with_dot_c(ents[i].name)) {
            if (main_idx < 0) main_idx = i;
            if (np_is_preferred_main(ents[i].name)) { main_idx = i; break; }
        }
    }

    int copied = 0;
    for (int i = 0; i < got; i++) {
        if (ents[i].type != IDE_DT_REG || is_dot_entry(ents[i].name)) continue;
        /* Don't let a non-main file literally named main.c clobber the renamed one. */
        if (i != main_idx && ide_streq(ents[i].name, "main.c")) continue;

        char src_path[IDE_PATH];
        char dst_path[IDE_PATH];
        path_join(src_path, IDE_PATH, src_dir, ents[i].name);
        path_join(dst_path, IDE_PATH, dst_dir,
                  (i == main_idx) ? "main.c" : ents[i].name);

        /* Copy bytes verbatim through the shared source buffer. */
        int n = ide_read_file(src_path, a->src, IDE_SRC_CAP);
        if (n < 0) continue;                          /* skip unreadable    */
        if (n > IDE_SRC_CAP) n = IDE_SRC_CAP;
        if (ide_write_file(dst_path, a->src, n) < 0) continue;
        copied++;

        if (i == main_idx && open_cap > 0) ide_strlcpy(open_path, dst_path, open_cap);
    }
    return copied;
}

/* Confirm the typed name: validate, scaffold /Desktop/Projects/<name>/{src,build,
 * res}, clone the template's main into src/main.c (or seed one), write
 * project.json, rescan, and open the project's main source. Updates np->status. */
static void np_confirm(Ide* a) {
    NewProj* np = &a->np;

    if (np->name_len <= 0) {
        ide_strlcpy(np->status, "type a project name, then Enter", 96);
        return;
    }
    if (np->name_len > PROJECT_NAME_MAX) {
        ide_strlcpy(np->status, "name too long (max 32 chars)", 96);
        return;
    }
    if (np->ntpl > 0 && (np->sel < 0 || np->sel >= np->ntpl)) np->sel = 0;

    /* Project root on the desktop: /Desktop/Projects/<name>. */
    char root[IDE_PATH];
    path_join(root, IDE_PATH, IDE_PROJECTS_DIR, np->name);

    /* Refuse to clobber an existing project of the same name. */
    {
        IdeDirent probe[1];
        if (ide_list_dir(root, probe, 1) >= 0) {
            ide_strlcpy(np->status, "a project with that name already exists", 96);
            return;
        }
    }

    /* Scaffold the project tree (recursive mkdir creates intermediates). */
    char sub[IDE_PATH];
    char src_dir[IDE_PATH];
    ide_sc(SYS_MKDIR_NUM, (long)IDE_PROJECTS_DIR, 0755, 0, 0, 0, 0);  /* ensure /Desktop/Projects */
    ide_sc(SYS_MKDIR_NUM, (long)root,             0755, 0, 0, 0, 0);
    path_join(src_dir, IDE_PATH, root, "src");
    ide_sc(SYS_MKDIR_NUM, (long)src_dir, 0755, 0, 0, 0, 0);
    path_join(sub, IDE_PATH, root, "build"); ide_sc(SYS_MKDIR_NUM, (long)sub, 0755, 0, 0, 0, 0);
    path_join(sub, IDE_PATH, root, "res");   ide_sc(SYS_MKDIR_NUM, (long)sub, 0755, 0, 0, 0, 0);

    /* Clone the chosen template's sources into <root>/src (main -> main.c). */
    char open_path[IDE_PATH];
    open_path[0] = 0;
    if (np->ntpl > 0) {
        char tpl_dir[IDE_PATH];
        path_join(tpl_dir, IDE_PATH, IDE_TEMPLATES_DIR, np->tpl[np->sel]);
        np_clone(a, tpl_dir, src_dir, open_path, IDE_PATH);
    }
    /* No template (or none copied) -> seed a minimal, compilable src/main.c. */
    if (!open_path[0]) {
        path_join(open_path, IDE_PATH, src_dir, "main.c");
        ide_project_seed_main(open_path);
    }

    /* Populate the project model + write project.json. */
    IdeProject* p = &a->project;
    p->active = 1;
    ide_strlcpy(p->root, root, (int)sizeof(p->root));
    ide_strlcpy(p->name, np->name, (int)sizeof(p->name));
    ide_strlcpy(p->lang, "c", (int)sizeof(p->lang));
    ide_strlcpy(p->entry, "src/main.c", (int)sizeof(p->entry));
    {
        char rt[96]; int n = 0;
        const char* pre = "build/";
        for (int i = 0; pre[i] && n < 91; i++) rt[n++] = pre[i];
        for (int i = 0; p->name[i] && n < 91; i++) rt[n++] = p->name[i];
        const char* ext = ".elf";
        for (int i = 0; ext[i] && n < 95; i++) rt[n++] = ext[i];
        rt[n] = 0;
        ide_strlcpy(p->run_target, rt, (int)sizeof(p->run_target));
    }
    ide_project_write_manifest(p);

    /* Refresh the explorer, close the modal, open the project's main source.
     * (The new project lives under /Desktop/Projects, not the /usr/src explorer
     * root, so it appears on the DESKTOP + opens directly in the editor here.) */
    scan_project(a);
    np_close(a);
    ide_open_file(a, open_path);
    a->ws = WS_EDITOR;
    a->term_focus = 0;
    a->editor.focused = 1;
}

/* Modal key handling. Returns 1 if the modal consumed the key (so the editor /
 * terminal never see it while the overlay is up). `ch` is the ASCII translation
 * (0 for non-printables); keycode distinguishes Enter/Backspace/Esc/arrows. */
static int np_key(Ide* a, int keycode, char ch) {
    NewProj* np = &a->np;
    if (np->phase == NP_CLOSED) return 0;

    if (keycode == KEY_ESC) { np_close(a); return 1; }

    if (np->phase == NP_PICK) {
        switch (keycode) {
        case KEY_UP:
            if (np->ntpl > 0) np->sel = (np->sel + np->ntpl - 1) % np->ntpl;
            return 1;
        case KEY_DOWN:
            if (np->ntpl > 0) np->sel = (np->sel + 1) % np->ntpl;
            return 1;
        case KEY_ENTER:
            if (np->ntpl > 0) { np->phase = NP_NAME; np->status[0] = 0; }
            return 1;
        default:
            return 1;             /* swallow everything else while picking */
        }
    }

    /* NP_NAME: edit the project name. */
    switch (keycode) {
    case KEY_ENTER:
        np_confirm(a);
        return 1;
    case KEY_BACKSPC:
        if (np->name_len > 0) np->name[--np->name_len] = 0;
        return 1;
    default:
        break;
    }

    /* Accept a conservative filename charset: letters, digits, '_', '-'. */
    if (ch && np->name_len < NP_NAMELEN - 1) {
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (ok) { np->name[np->name_len++] = ch; np->name[np->name_len] = 0; }
    }
    return 1;
}

/* ===========================================================================
 * init: set the root, scan, choose an initial file, set initial view tabs.
 * ==========================================================================*/

static void init(Ide* a) {
    /* Root the explorer at /usr/src so EVERY packaged project (towerdefense,
     * bubbledefense, native, ...) is browsable + editable from the file tree,
     * not just the single towerdefense demo. scan_dir() recurses into the
     * subdirs, so clicking any file under any project opens it in the editor. */
    ide_strlcpy(a->root, "/usr/src", IDE_PATH);
    a->prev_focus = 0;

    /* Load the editable "complex" library from /usr/lib/snippets (extends the
     * built-in core). Non-fatal if the dir is absent. */
    lib_load_disk(a);

    /* Initialize multi-file tab state. */
    a->tab_count = 0;
    a->tab_active = -1;
    for (int ti = 0; ti < IDE_MAX_TABS; ti++) a->tabs[ti].used = 0;

    scan_project(a);

    /* Pick a friendly starting file: prefer "tower.c" (small + parser-safe);
     * else the first .c file; else the first regular file in the tree. Zombie
     * Bastion still ships under /usr/src/zombiebastion/ and opens from the tree,
     * but is NOT the auto-opened default (don't parse a big game file at launch). */
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
        a->focus_func = -1;
        model_analyze(&a->model);
    }

    a->viz = VIZ_MAP;
    a->insp_tab = 2;                  /* PORTS                              */
    a->flow_step_focus = -1;          /* no runtime-flow step traced yet     */
    a->map_selected = -1;             /* no map node selected yet            */
    a->map_zoom = 100;                /* 100% zoom initially                 */

    /* ---- EDITOR workspace defaults (this is the default face) ---- */
    a->ws = WS_EDITOR;
    a->btab = BTAB_TERMINAL;
    a->bottom_h = 8 * GFX_FH;         /* initial bottom-dock height (~8 rows / ~150px) */
    a->term_focus = 0;                /* editor has keys initially           */
    a->zen_mode = 0;                  /* zen mode off initially              */
    a->explorer_focused = 0;          /* explorer unfocused initially        */
    a->goto_active = 0;               /* go-to-line inactive initially       */
    a->goto_len = 0;
    a->n_collapsed = 0;               /* all folders expanded initially      */
    a->editor.focused = 1;
    ide_editor_reset(a);
    ide_term_init(&a->term, a->root); /* terminal starts in the project root */

    /* Apply persisted Settings knobs LAST (they override the hardcoded defaults
     * above) and before the first layout(), so saved zoom/flags take effect with
     * no first-frame flash. Safe + silent if no config file exists yet. */
    ide_config_load();
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

    /* VERTICAL fit: reserve the topbar first, then squeeze the runtime + status
     * bands into whatever height remains, so they can NEVER paint over the topbar
     * / explorer on a small window (the cause of the runtime-overlaps-everything
     * artifact). Below a point the runtime band collapses to 0 (hidden). */
    int avail = H - TOPBAR_H; if (avail < 0) avail = 0;
    int status_h  = STATUS_H;  if (status_h  > avail)            status_h  = avail;
    int runtime_h = RUNTIME_H; if (runtime_h > avail - status_h) runtime_h = avail - status_h;
    if (runtime_h < 0) runtime_h = 0;

    a->r_status.x = 0;  a->r_status.y = H - status_h;
    a->r_status.w = W;  a->r_status.h = status_h;

    a->r_runtime.x = 0;  a->r_runtime.y = H - status_h - runtime_h;
    a->r_runtime.w = W;  a->r_runtime.h = runtime_h;

    /* The working region between the top bar and the runtime band. */
    int work_top    = TOPBAR_H;
    int work_bottom = H - status_h - runtime_h;     /* exclusive, never < TOPBAR_H */
    int work_h      = work_bottom - work_top;
    if (work_h < 0) work_h = 0;

    /* HORIZONTAL fit: clamp the left + right columns TOGETHER (each to a fraction)
     * and reserve a minimum center, so explorer and inspector can never touch /
     * overlap and the map/code center never collapses unless the window is tiny --
     * in which case a column is HIDDEN (width 0) rather than overlapped. */
    int min_center = GFX_FW * 12;
    int left_w  = LEFT_W;  if (left_w  > (W * 30) / 100) left_w  = (W * 30) / 100;
    int right_w = RIGHT_W; if (right_w > (W * 32) / 100) right_w = (W * 32) / 100;
    if (left_w + right_w > W - min_center) {
        int over = (left_w + right_w) - (W - min_center);
        left_w  -= over / 2;
        right_w -= (over - over / 2);
    }
    if (W < min_center + 80) right_w = 0;            /* drop inspector first */
    if (W < 80)            { left_w = 0; right_w = 0; }
    if (left_w  < 0) left_w  = 0;
    if (right_w < 0) right_w = 0;

    int expl_h = (work_h * 58) / 100;               /* explorer ~58%        */
    if (expl_h < 0) expl_h = 0;

    a->r_explorer.x = 0;            a->r_explorer.y = work_top;
    a->r_explorer.w = left_w;       a->r_explorer.h = expl_h;

    a->r_funcs.x = 0;               a->r_funcs.y = work_top + expl_h;
    a->r_funcs.w = left_w;          a->r_funcs.h = work_h - expl_h;

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
#define E_TOOLBAR_H (ROW_H + 4)      /* editor toolbar height   */

static void layout_editor(Ide* a, wl_window* win) {
    int W = (int)win->w;
    int H = (int)win->h;

    /* Top bar (workspace tabs) + status bar. */
    a->r_topbar.x = 0;  a->r_topbar.y = 0;  a->r_topbar.w = W;  a->r_topbar.h = TOPBAR_H;
    a->r_status.x = 0;  a->r_status.y = H - STATUS_H;  a->r_status.w = W;  a->r_status.h = STATUS_H;

    /* Toolbar sits immediately below the top bar. */
    a->r_e_toolbar.x = 0;  a->r_e_toolbar.y = TOPBAR_H;
    a->r_e_toolbar.w = W;  a->r_e_toolbar.h = E_TOOLBAR_H;

    /* File tab bar: one row below the toolbar when tabs are open. */
    int ftab_h = (a->tab_count > 0) ? (ROW_H + 2) : 0;

    int work_top = TOPBAR_H + E_TOOLBAR_H + ftab_h;
    int work_bottom = H - STATUS_H;
    int work_h = work_bottom - work_top;
    if (work_h < 0) work_h = 0;

    a->r_e_filetabs.x = 0;  a->r_e_filetabs.y = TOPBAR_H + E_TOOLBAR_H;
    a->r_e_filetabs.w = W;  a->r_e_filetabs.h = ftab_h;

    /* Zen mode (Ctrl+Shift+E): hide file tree and bottom panel entirely,
     * giving the editor the full window width minus only topbar/toolbar/status. */
    if (a->zen_mode) {
        a->r_e_tree.x = 0; a->r_e_tree.y = work_top;
        a->r_e_tree.w = 0; a->r_e_tree.h = 0;

        a->r_e_editor.x = 0;  a->r_e_editor.y = work_top;
        a->r_e_editor.w = W;  a->r_e_editor.h = work_h;

        a->r_e_btabs.x = 0;  a->r_e_btabs.y = work_top + work_h;
        a->r_e_btabs.w = 0;  a->r_e_btabs.h = 0;

        a->r_e_bottom.x = 0; a->r_e_bottom.y = work_top + work_h;
        a->r_e_bottom.w = 0; a->r_e_bottom.h = 0;
        return;
    }

    /* Left file tree: LEFT_W but never more than ~22% of the window so the
     * editor gets the lion's share on 1280x800 (~200px tree, ~800px editor). */
    int tree_w = LEFT_W;
    int max_tree = (W * 22) / 100;
    if (tree_w > max_tree) tree_w = max_tree;
    if (tree_w > W) tree_w = W;
    if (tree_w < 0) tree_w = 0;

    a->r_e_tree.x = 0;        a->r_e_tree.y = work_top;
    a->r_e_tree.w = tree_w;   a->r_e_tree.h = work_h;

    /* Right region = editor over bottom dock. */
    int right_x = tree_w;
    int right_w = W - tree_w;
    if (right_w < 0) right_w = 0;

    /* Bottom dock: cap at 40% (was 60%) so the editor keeps more vertical
     * space on 1280x800. Default bottom_h (~150px) yields ~8 lines of output. */
    int bh = a->bottom_h;
    int max_bh = (work_h * 40) / 100;
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
 * "New Project" modal rendering + click hit-testing.
 *
 * Drawn LAST (over whatever workspace is active) as a centered card. Phase
 * NP_PICK lists the templates; NP_NAME shows the typed name with a caret. The
 * card geometry is recomputed from the live window size so it always centers.
 * ==========================================================================*/

#define NP_CARD_W   (40 * GFX_FW)   /* ~40 chars wide, scales with font */
#define NP_ROW_H    ROW_H

/* Compute the centered card rect for the current window size. */
static Rect np_card_rect(Ide* a) {
    int rows = a->np.ntpl > 0 ? a->np.ntpl : 1;
    int body = rows * NP_ROW_H;
    int h = TOPBAR_H            /* title bar              */
          + GFX_FH + PAD        /* hint line              */
          + body + PAD          /* template list          */
          + GFX_FH + 2 * PAD    /* name field             */
          + GFX_FH + 2 * PAD;   /* footer hint / status   */
    int w = NP_CARD_W;
    if (w > a->win_w - 2 * PAD)  w = a->win_w - 2 * PAD;
    if (h > a->win_h - 2 * PAD)  h = a->win_h - 2 * PAD;
    Rect r;
    r.w = w; r.h = h;
    r.x = (a->win_w - w) / 2;
    r.y = (a->win_h - h) / 2;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    return r;
}

/* The list area inside the card (where each template row is hit-tested). */
static Rect np_list_rect(Ide* a, Rect card) {
    int rows = a->np.ntpl > 0 ? a->np.ntpl : 1;
    Rect r;
    r.x = card.x + PAD;
    r.y = card.y + TOPBAR_H + GFX_FH + PAD;
    r.w = card.w - 2 * PAD;
    r.h = rows * NP_ROW_H;
    return r;
}

static void render_newproj(Ide* a, Canvas* cv) {
    NewProj* np = &a->np;
    if (np->phase == NP_CLOSED) return;

    /* Dim the whole screen behind the card so it reads as modal. */
    gfx_blend(cv, 0, 0, cv->w, cv->h, 0xB0000000u);

    Rect c = np_card_rect(a);
    gfx_fill  (cv, c.x, c.y, c.w, c.h, TH_PANEL);
    gfx_stroke(cv, c.x, c.y, c.w, c.h, TH_BORDER_LT);

    /* Title bar. */
    gfx_fill (cv, c.x, c.y, c.w, TOPBAR_H, TH_HEADER);
    gfx_hline(cv, c.x, c.y + TOPBAR_H - 1, c.w, TH_BORDER);
    {
        int ty = c.y + (TOPBAR_H - GFX_FH) / 2;
        gfx_text_clip(cv, c.x + PAD, ty, "NEW PROJECT", TH_TEXT,
                      c.x + PAD, c.w - 2 * PAD);
        const char* esc = "Esc";
        int ew = gfx_textw(esc);
        gfx_text_clip(cv, c.x + c.w - PAD - ew, ty, esc, TH_TEXT_FAINT,
                      c.x + PAD, c.w - 2 * PAD);
    }

    int clip_x = c.x + PAD;
    int clip_w = c.w - 2 * PAD;

    /* Hint line. */
    {
        int hy = c.y + TOPBAR_H + (PAD / 2);
        const char* hint = (np->phase == NP_PICK)
            ? "Pick a template (Up/Down, Enter):"
            : "Project name (type, Enter to create):";
        gfx_text_clip(cv, clip_x, hy, hint, TH_TEXT_DIM, clip_x, clip_w);
    }

    /* Template list. */
    Rect lst = np_list_rect(a, c);
    if (np->ntpl <= 0) {
        gfx_text_clip(cv, lst.x, lst.y, "(no templates found)",
                      TH_ORANGE, clip_x, clip_w);
    } else {
        int hover = -1;
        if (np->phase == NP_PICK && rect_hit(lst, a->mouse_x, a->mouse_y))
            hover = (a->mouse_y - lst.y) / NP_ROW_H;
        for (int i = 0; i < np->ntpl; i++) {
            int ry = lst.y + i * NP_ROW_H;
            int sel = (i == np->sel);
            if (sel)
                gfx_fill(cv, lst.x, ry, lst.w, NP_ROW_H, TH_SELECT);
            else if (i == hover)
                gfx_fill(cv, lst.x, ry, lst.w, NP_ROW_H, TH_HOVER);
            int ty = ry + (NP_ROW_H - GFX_FH) / 2;
            /* small folder glyph + name */
            gfx_fill(cv, lst.x + 2, ty + 4, 3, 2, TH_YELLOW);
            gfx_fill(cv, lst.x + 2, ty + 6, GFX_FW, 7, TH_YELLOW);
            gfx_text_clip(cv, lst.x + GFX_FW + 8, ty, np->tpl[i],
                          sel ? TH_TEXT : TH_TEXT_DIM,
                          lst.x + GFX_FW + 8, lst.w - GFX_FW - 8);
        }
    }

    /* Name field. */
    {
        int fy = lst.y + lst.h + PAD;
        int fh = GFX_FH + 2;
        uint32_t border = (np->phase == NP_NAME) ? TH_BLUE : TH_BORDER;
        gfx_fill  (cv, clip_x, fy, clip_w, fh, TH_PANEL2);
        gfx_stroke(cv, clip_x, fy, clip_w, fh, border);
        int ty = fy + (fh - GFX_FH) / 2;
        if (np->name_len > 0) {
            gfx_text_clip(cv, clip_x + 4, ty, np->name, TH_TEXT,
                          clip_x + 4, clip_w - 8);
            if (np->phase == NP_NAME) {
                int cx = clip_x + 4 + np->name_len * GFX_FW;
                if (cx < clip_x + clip_w - 2)
                    gfx_fill(cv, cx, ty, 2, GFX_FH, TH_TEXT);
            }
        } else {
            const char* ph = (np->phase == NP_NAME)
                ? "my-game" : "(select a template above)";
            gfx_text_clip(cv, clip_x + 4, ty, ph, TH_TEXT_FAINT,
                          clip_x + 4, clip_w - 8);
        }

        /* Footer: full destination path preview OR the status message. */
        int sy = fy + fh + (PAD / 2);
        if (np->status[0]) {
            gfx_text_clip(cv, clip_x, sy, np->status, TH_ORANGE,
                          clip_x, clip_w);
        } else if (np->name_len > 0) {
            char prev[IDE_PATH];
            ide_strlcpy(prev, "-> " IDE_PROJECTS_DIR "/", (int)sizeof(prev));
            ide_strlcat(prev, np->name, (int)sizeof(prev));
            gfx_text_clip(cv, clip_x, sy, prev, TH_GREEN, clip_x, clip_w);
        }
    }
}

/* Hit-test a click while the modal is open. Always returns 1 (the modal is
 * fully captured -- clicks outside the card are ignored, not passed through). */
static int np_click(Ide* a, int mx, int my) {
    NewProj* np = &a->np;
    if (np->phase == NP_CLOSED) return 0;

    Rect c = np_card_rect(a);

    /* Clicking the list selects a template (and, in PICK phase, advances to the
     * name step on the same click for a fast one-two flow). */
    if (np->ntpl > 0) {
        Rect lst = np_list_rect(a, c);
        if (rect_hit(lst, mx, my)) {
            int row = (my - lst.y) / NP_ROW_H;
            if (row >= 0 && row < np->ntpl) {
                np->sel = row;
                if (np->phase == NP_PICK) { np->phase = NP_NAME; np->status[0] = 0; }
            }
            return 1;
        }
    }
    /* Click anywhere else inside the card: no-op (keeps it open). Outside the
     * card: also a no-op so a stray click can't dismiss a half-typed name. */
    return 1;
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
    case VIZ_INSPECTOR:
        /* Interactive inspector: render whatever sub-tab the user selected
         * (SYN/CAT/PORT/CONN/INFO). Previously this force-set INSP_SYNTAX every
         * frame which -- together with the matching force in the click router --
         * made the sub-tabs impossible to switch (every click was reverted).
         *
         * IDE-REPAIR-0 I3 -- EXCEPT LIB: the "complex library" is a palette
         * that belongs ONLY in the RIGHT inspector sidebar (render() draws it
         * into r_inspector). Drawing it here too duplicated the full-width
         * snippet list across the center, over the node-detail area. When LIB
         * is active, show the node-detail (SYNTAX) view in the center. */
        if (a->insp_tab == INSP_LIB) {
            a->insp_tab = INSP_SYNTAX;
            panel_inspector(a, cv, a->r_map);
            a->insp_tab = INSP_LIB;
        } else {
            panel_inspector(a, cv, a->r_map);
        }
        break;
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
    case VIZ_SETTINGS:
        panel_settings(a, cv, a->r_map);      /* knobs & switches (VIZ-6)   */
        break;
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

    render_newproj (a, cv);                   /* modal overlay (if open)    */
}

/* ===========================================================================
 * EDITOR-workspace chrome: a two-tab top bar (EDITOR / LEGO MAP), the bottom
 * tab strip (TERMINAL / BUILD / PROBLEMS) and a slim status bar.
 * ==========================================================================*/

static const char* const WS_TAB_LABEL[2] = { "EDITOR", "LEGO MAP" };

/* The "[+ NEW]" toolbar button label (clickable -> opens the New Project
 * modal). Kept here so the renderer and the hit-test share one string. */
static const char NP_BTN_LABEL[] = "+ NEW";

/* X of the left edge of the [+ NEW] button: just past the two workspace tabs.
 * Mirrors the tab-advance math in editor_topbar so render + click agree. */
static int np_btn_x(Rect r) {
    int x = r.x + PAD;
    for (int i = 0; i < 2; i++)
        x += gfx_textw(WS_TAB_LABEL[i]) + 2 * GFX_FW + GFX_FW;
    return x;
}
static inline int np_btn_w(void) { return gfx_textw(NP_BTN_LABEL) + 2 * GFX_FW; }

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

    /* [+ NEW] button: a small framed, green-accented pill that opens the New
     * Project modal. Hovering tints it. */
    int btn_x = np_btn_x(r);
    int btn_w = np_btn_w();
    int btn_vis = (btn_x + btn_w < r.x + r.w);
    if (btn_vis) {
        int hov = (a->mouse_y >= r.y && a->mouse_y < r.y + r.h &&
                   a->mouse_x >= btn_x && a->mouse_x < btn_x + btn_w);
        gfx_fill  (cv, btn_x, r.y + 3, btn_w, r.h - 6, hov ? TH_SELECT : TH_PANEL);
        gfx_stroke(cv, btn_x, r.y + 3, btn_w, r.h - 6, TH_GREEN);
        gfx_text_clip(cv, btn_x + GFX_FW, ty, NP_BTN_LABEL, TH_GREEN, btn_x, btn_w);
    }

    /* right-aligned filename + dirty marker. Its left edge must clear the [+NEW]
     * button's RIGHT edge (or the tabs, if +NEW is hidden at narrow widths) so
     * the filename never paints over them when the UI is zoomed in. */
    int left_bound = btn_vis ? (btn_x + btn_w + GFX_FW) : x;
    const char* f = a->cur_file[0] ? a->cur_file : "(no file)";
    int fw = gfx_textw(f);
    int dirtyw = ide_editor_dirty(a) ? (gfx_textw(" *")) : 0;
    int fx = r.x + r.w - PAD - fw - dirtyw;
    if (fx > left_bound) {
        int clipw = r.x + r.w - left_bound;
        gfx_text_clip(cv, fx, ty, f, TH_TEXT_DIM, left_bound, clipw);
        if (ide_editor_dirty(a))
            gfx_text_clip(cv, fx + fw, ty, " *", TH_ORANGE, left_bound, clipw);
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
        /* Flash the BUILD tab green/red briefly after a build completes. */
        if (i == 1 /* BTAB_BUILD */) {
            uint32_t fc = ide_build_flash_color();
            if (fc)
                gfx_blend(cv, x - GFX_FW / 2, r.y + 1, tw, r.h - 1, fc);
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

/* Slim status bar for the editor workspace. Shows Ln/Col, file name, lines,
 * dirty state, and a right-aligned shortcut legend. */
static void editor_status(Ide* a, Canvas* cv, Rect r) {
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_HEADER);
    gfx_hline(cv, r.x, r.y, r.w, TH_BORDER);
    int ty = r.y + (r.h - GFX_FH) / 2;
    int x = r.x + PAD;
    int clip_x0 = r.x + PAD;
    int clip_w = r.w - 2 * PAD;
    int compact = (r.w < 80 * GFX_FW);   /* compact at narrow widths */

    /* Ln, Col -- compact: "42:8" */
    {
        char lc[40]; int p = 0;
        char nb[16]; int nn;
        if (compact) {
            nn = ide_itoa(a->editor.caret_line + 1, nb);
            for (int i = 0; i < nn; i++) lc[p++] = nb[i];
            lc[p++] = ':';
            nn = ide_itoa(a->editor.caret_col + 1, nb);
            for (int i = 0; i < nn; i++) lc[p++] = nb[i];
        } else {
            lc[p++] = 'L'; lc[p++] = 'n'; lc[p++] = ' ';
            nn = ide_itoa(a->editor.caret_line + 1, nb);
            for (int i = 0; i < nn; i++) lc[p++] = nb[i];
            lc[p++] = ','; lc[p++] = ' ';
            lc[p++] = 'C'; lc[p++] = 'o'; lc[p++] = 'l'; lc[p++] = ' ';
            nn = ide_itoa(a->editor.caret_col + 1, nb);
            for (int i = 0; i < nn; i++) lc[p++] = nb[i];
        }
        lc[p] = 0;
        gfx_text_clip(cv, x, ty, lc, TH_TEXT, clip_x0, clip_w);
        x += gfx_textw(lc) + 2 * GFX_FW;
    }

    /* current file name (basename only for brevity) */
    if (a->cur_file[0]) {
        const char* base = a->cur_file;
        for (const char* p = a->cur_file; *p; p++)
            if (*p == '/') base = p + 1;
        gfx_text_clip(cv, x, ty, base, TH_TEXT_DIM, clip_x0, clip_w);
        x += gfx_textw(base) + 2 * GFX_FW;
    }

    /* dirty/saved -- compact: "*" or "-" */
    if (compact) {
        const char* ind = ide_editor_dirty(a) ? "*" : "-";
        uint32_t col = ide_editor_dirty(a) ? TH_ORANGE : TH_GREEN;
        gfx_text_clip(cv, x, ty, ind, col, clip_x0, clip_w);
        x += gfx_textw(ind) + GFX_FW;
    } else {
        if (ide_editor_dirty(a)) {
            gfx_text_clip(cv, x, ty, "UNSAVED", TH_ORANGE, clip_x0, clip_w);
            x += gfx_textw("UNSAVED") + 2 * GFX_FW;
        } else {
            gfx_text_clip(cv, x, ty, "SAVED", TH_GREEN, clip_x0, clip_w);
            x += gfx_textw("SAVED") + 2 * GFX_FW;
        }
    }

    /* lines -- skip in compact mode to save horizontal space */
    if (!compact) {
        gfx_text_clip(cv, x, ty, "LINES:", TH_TEXT_DIM, clip_x0, clip_w);
        x += gfx_textw("LINES:") + GFX_FW;
        { extern int ed_line_count(const char* src, int len);
          int live_lines = ed_line_count(a->src, a->src_len);
          char nb[16]; int n = ide_itoa(live_lines, nb); nb[n] = 0;
          gfx_text_clip(cv, x, ty, nb, TH_TEXT_DIM, clip_x0, clip_w);
          x += n * GFX_FW + 2 * GFX_FW; }
    }

    /* Zen mode indicator */
    if (a->zen_mode) {
        gfx_text_clip(cv, x, ty, "ZEN", TH_PURPLE, clip_x0, clip_w);
        x += gfx_textw("ZEN") + GFX_FW;
    }

    /* right-aligned shortcut legend -- omit in compact mode */
    if (!compact) {
        const char* leg = "^S save ^B build ^F find ^Tab tab ^` term";
        int lw = gfx_textw(leg);
        int lx = r.x + r.w - PAD - lw;
        if (lx > x) gfx_text_clip(cv, lx, ty, leg, TH_TEXT_FAINT, x, clip_w);
    }
}


/* ===========================================================================
 * "New File" action: clear the editor buffer for a fresh untitled file.
 * If the buffer is dirty, auto-save first (no discard dialog in freestanding).
 * ==========================================================================*/
static void ide_new_file(Ide* a) {
    if (a->cur_file[0] && ide_editor_dirty(a))
        ide_editor_save(a);
    /* Save the current tab before clearing for the new file. */
    tab_save_active(a);
    {
        int slot = tab_alloc(a);
        if (slot < 0) { slot = a->tab_active; if (slot < 0) slot = 0;
            if (!a->tabs[slot].used) { a->tabs[slot].used = 1; a->tab_count++; } }
        a->tab_active = slot;
        a->tabs[slot].path[0] = 0;
    }
    a->src_len = 0;
    a->cur_file[0] = 0;
    model_parse(&a->model, a->src, 0, "");
    a->model.focus = -1;
    a->focus_func = -1;
    model_analyze(&a->model);
    ide_sel_reset(a);                 /* IDE-SYNC-0: fresh selection */
    ide_editor_reset(a);
    a->editor.focused = 1;
    /* Dismiss any open overlay prompts so they don't persist over the new file. */
    a->find_active = 0;
    a->goto_active = 0;
}

/* ===========================================================================
 * Editor toolbar: clickable pill buttons for common actions.
 * Buttons: [New] [Save] [Build] [Run] | [Find] [Replace] [Go to Ln]
 * ==========================================================================*/
typedef struct { const char* label; const char* compact; uint32_t color; } TbBtn;
static const TbBtn TB_BUTTONS[] = {
    { "New",       "N", 0xFF54D17Au },
    { "Save",      "S", 0xFF4D9BE6u },
    { "Build",     "B", 0xFFE6C24Au },
    { "Run",       "R", 0xFF49C5D6u },
    { "Find",      "F", 0xFF8A98AAu },
    { "Replace",   "H", 0xFF8A98AAu },
    { "Go to Ln",  "G", 0xFF8A98AAu },
};
#define TB_NBUTTONS ((int)(sizeof(TB_BUTTONS) / sizeof(TB_BUTTONS[0])))
#define TB_GAP      (GFX_FW)
#define TB_PAD_X    (GFX_FW)
#define TB_PILL_H   (GFX_FH + 4)

/* Return 1 if compact mode is needed (full labels don't fit in the toolbar). */
static int tb_need_compact(Rect r) {
    int x = r.x + PAD;
    for (int i = 0; i < TB_NBUTTONS; i++) {
        int tw = gfx_textw(TB_BUTTONS[i].label) + 2 * TB_PAD_X;
        x += tw + TB_GAP;
        if (i == 3) x += TB_GAP;
    }
    return (x > r.x + r.w - PAD);
}

static void tb_btn_geometry(Rect r, int* out_x, int* out_w, int compact) {
    int x = r.x + PAD;
    for (int i = 0; i < TB_NBUTTONS; i++) {
        const char* lbl = compact ? TB_BUTTONS[i].compact : TB_BUTTONS[i].label;
        int tw = gfx_textw(lbl) + 2 * TB_PAD_X;
        out_x[i] = x;
        out_w[i] = tw;
        x += tw + TB_GAP;
        if (i == 3) x += TB_GAP;
    }
}

static void editor_toolbar(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL);
    gfx_hline(cv, r.x, r.y + r.h - 1, r.w, TH_BORDER);
    int ty = r.y + (r.h - GFX_FH) / 2;
    int pill_y = r.y + (r.h - TB_PILL_H) / 2;
    int compact = tb_need_compact(r);
    int bx[TB_NBUTTONS], bw[TB_NBUTTONS];
    tb_btn_geometry(r, bx, bw, compact);
    for (int i = 0; i < TB_NBUTTONS; i++) {
        if (bx[i] + bw[i] > r.x + r.w - PAD) break;
        int hov = (a->mouse_y >= r.y && a->mouse_y < r.y + r.h &&
                   a->mouse_x >= bx[i] && a->mouse_x < bx[i] + bw[i]);
        uint32_t bg = hov ? TH_SELECT : TH_PANEL2;
        gfx_round(cv, bx[i], pill_y, bw[i], TB_PILL_H, 3, bg);
        gfx_stroke(cv, bx[i], pill_y, bw[i], TB_PILL_H, TB_BUTTONS[i].color);
        int tx2 = bx[i] + TB_PAD_X;
        const char* lbl = compact ? TB_BUTTONS[i].compact : TB_BUTTONS[i].label;
        gfx_text_clip(cv, tx2, ty, lbl,
                      hov ? TH_TEXT : TB_BUTTONS[i].color, bx[i], bw[i]);
    }
    {
        const char* hint = compact
            ? "^S ^B ^R ^F ^H ^G"
            : "Ctrl: S save  B build  F find  W wrap  M minimap  D multi";
        int hw = gfx_textw(hint);
        int hx = r.x + r.w - PAD - hw;
        int last_r = TB_NBUTTONS > 0 ? bx[TB_NBUTTONS-1] + bw[TB_NBUTTONS-1] + TB_GAP : r.x + PAD;
        if (hx > last_r)
            gfx_text_clip(cv, hx, ty, hint, TH_TEXT_FAINT, last_r, r.w);
    }
    (void)pill_y;
}

static int editor_toolbar_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;
    int compact = tb_need_compact(r);
    int bx[TB_NBUTTONS], bw[TB_NBUTTONS];
    tb_btn_geometry(r, bx, bw, compact);
    for (int i = 0; i < TB_NBUTTONS; i++) {
        /* Skip buttons that extend past the visible toolbar (same clip test as
         * editor_toolbar's render loop) so invisible buttons can't be clicked. */
        if (bx[i] + bw[i] > r.x + r.w - PAD) break;
        if (mx >= bx[i] && mx < bx[i] + bw[i]) {
            switch (i) {
            case 0: ide_new_file(a); break;
            case 1: ide_editor_save(a); break;
            case 2:
                if (a->cur_file[0] && ide_editor_dirty(a)) ide_editor_save(a);
                ide_do_build(a); a->btab = BTAB_BUILD; break;
            case 3: ide_do_run(a); a->btab = BTAB_BUILD; break;
            case 4:
                a->find_active = 1; a->find_replace = 0;
                a->find_len = 0; a->find_buf[0] = '\0'; a->goto_active = 0; break;
            case 5:
                a->find_active = 1; a->find_replace = 1; a->find_repl_focus = 0;
                a->find_len = 0; a->find_buf[0] = '\0';
                a->repl_len = 0; a->repl_buf[0] = '\0'; a->goto_active = 0; break;
            case 6:
                a->goto_active = 1; a->goto_len = 0; a->goto_buf[0] = '\0'; break;
            }
            return 1;
        }
    }
    return 1;
}

/* ===========================================================================
 * File tab bar: render + click for multi-file tabs.
 * Renders one tab per open file (basename only). Active tab is highlighted.
 * Each tab has a close [x] button. Dirty tabs show a dot indicator.
 * ==========================================================================*/

#define FTAB_CLOSE_W  (GFX_FW + 4)   /* width of the close-x area */

static void editor_filetabs(Ide* a, Canvas* cv, Rect r) {
    if (r.h <= 0 || r.w <= 0 || a->tab_count <= 0) return;
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL);
    gfx_hline(cv, r.x, r.y + r.h - 1, r.w, TH_BORDER);

    int ty = r.y + (r.h - GFX_FH) / 2;
    int x = r.x + PAD;
    for (int i = 0; i < IDE_MAX_TABS; i++) {
        if (!a->tabs[i].used) continue;
        const char* name;
        int dirty;
        if (i == a->tab_active) {
            name = tab_basename(a->cur_file[0] ? a->cur_file : "(new)");
            dirty = a->editor.dirty;
        } else {
            name = a->tabs[i].path[0] ? tab_basename(a->tabs[i].path) : "(new)";
            dirty = a->tabs[i].editor.dirty;
        }

        int tw = gfx_textw(name);
        int dot_w = dirty ? (GFX_FW + 2) : 0;
        int tab_w = PAD + dot_w + tw + PAD + FTAB_CLOSE_W + PAD;
        int active = (i == a->tab_active);

        if (x + tab_w > r.x + r.w - PAD) break;

        if (active) {
            gfx_fill(cv, x, r.y, tab_w, r.h - 1, TH_PANEL2);
            gfx_hline(cv, x, r.y, tab_w, TH_BLUE);
        } else {
            if (a->mouse_y >= r.y && a->mouse_y < r.y + r.h &&
                a->mouse_x >= x && a->mouse_x < x + tab_w)
                gfx_fill(cv, x, r.y, tab_w, r.h - 1, TH_HOVER);
        }
        gfx_vline(cv, x + tab_w - 1, r.y + 2, r.h - 4, TH_BORDER);

        int tx = x + PAD;

        if (dirty) {
            int dot_x = tx + GFX_FW / 2 - 2;
            int dot_y = ty + GFX_FH / 2 - 2;
            gfx_fill(cv, dot_x, dot_y, 4, 4, TH_ORANGE);
            tx += GFX_FW + 2;
        }

        gfx_text_clip(cv, tx, ty, name,
                      active ? TH_TEXT : TH_TEXT_DIM, x, tab_w);
        tx += tw;

        int cx = x + tab_w - FTAB_CLOSE_W - PAD / 2;
        int close_hov = (a->mouse_y >= r.y && a->mouse_y < r.y + r.h &&
                         a->mouse_x >= cx && a->mouse_x < cx + FTAB_CLOSE_W);
        if (close_hov)
            gfx_fill(cv, cx, r.y + 2, FTAB_CLOSE_W, r.h - 5, TH_SELECT);
        gfx_text_clip(cv, cx + 2, ty, "x",
                      close_hov ? TH_RED : TH_TEXT_FAINT, cx, FTAB_CLOSE_W);

        x += tab_w;
    }
}

static int editor_filetabs_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;
    if (a->tab_count <= 0) return 1;

    int x = r.x + PAD;
    for (int i = 0; i < IDE_MAX_TABS; i++) {
        if (!a->tabs[i].used) continue;
        const char* name;
        int dirty;
        if (i == a->tab_active) {
            name = tab_basename(a->cur_file[0] ? a->cur_file : "(new)");
            dirty = a->editor.dirty;
        } else {
            name = a->tabs[i].path[0] ? tab_basename(a->tabs[i].path) : "(new)";
            dirty = a->tabs[i].editor.dirty;
        }
        int tw = gfx_textw(name);
        int dot_w = dirty ? (GFX_FW + 2) : 0;
        int tab_w = PAD + dot_w + tw + PAD + FTAB_CLOSE_W + PAD;

        if (x + tab_w > r.x + r.w - PAD) break;

        if (mx >= x && mx < x + tab_w) {
            int cx = x + tab_w - FTAB_CLOSE_W - PAD / 2;
            if (mx >= cx && mx < cx + FTAB_CLOSE_W) {
                ide_tab_close(a, i);
            } else {
                ide_tab_switch(a, i);
            }
            return 1;
        }
        x += tab_w;
    }
    return 1;
}

static void render_editor(Ide* a, Canvas* cv) {

    gfx_fill(cv, 0, 0, cv->w, cv->h, TH_BG);

    editor_topbar (a, cv, a->r_topbar);
    editor_toolbar(a, cv, a->r_e_toolbar);    /* action toolbar             */
    editor_filetabs(a, cv, a->r_e_filetabs);  /* open file tabs             */
    panel_explorer(a, cv, a->r_e_tree);       /* reuse the project tree     */
    ide_editor_render(a, cv, a->r_e_editor);  /* the editable editor        */
    editor_btabs  (a, cv, a->r_e_btabs);
    editor_bottom (a, cv, a->r_e_bottom);
    editor_status (a, cv, a->r_status);

    render_newproj(a, cv);                     /* modal overlay (if open)    */

    /* Go-to-line prompt overlay (bottom-center, inline).
     * Dimensions derived from the runtime font cell so the prompt scales. */
    if (a->goto_active) {
        int prompt_w = 22 * GFX_FW;           /* ~22 chars wide        */
        int prompt_h = GFX_FH + 2 * PAD;      /* one row + padding     */
        int px = (cv->w - prompt_w) / 2;
        int py = cv->h - STATUS_H - prompt_h - PAD;

        /* Background box */
        gfx_fill(cv, px, py, prompt_w, prompt_h, TH_PANEL2);
        gfx_stroke(cv, px, py, prompt_w, prompt_h, TH_CYAN);

        /* Prompt text */
        int tx = px + PAD;
        int ty = py + (prompt_h - GFX_FH) / 2;
        gfx_text(cv, tx, ty, "Go to line:", TH_TEXT);

        /* User input */
        int input_x = tx + gfx_textw("Go to line:") + GFX_FW;
        gfx_text(cv, input_x, ty, a->goto_buf, TH_GREEN);

        /* Blinking cursor */
        static int blink_counter = 0;
        blink_counter++;
        if ((blink_counter / 30) % 2 == 0) {  /* blink every ~0.5s at 60fps */
            int cursor_x = input_x + a->goto_len * GFX_FW;
            gfx_vline(cv, cursor_x, ty, GFX_FH, TH_GREEN);
        }
    }

    /* Find (Ctrl+F) / Find&Replace (Ctrl+H) overlay. One row for find (+ ok/no);
     * a second row for the replacement in replace mode. The focused field's label
     * is accent-coloured. The match is highlighted in the editor body.
     * All dimensions derived from the runtime font cell so the prompt scales. */
    if (a->find_active) {
        int rows = a->find_replace ? 2 : 1;
        int prompt_w = 46 * GFX_FW;           /* ~46 chars wide        */
        int row_h = GFX_FH + PAD;             /* one text row + pad    */
        int prompt_h = rows * row_h + PAD + 2;
        int px = (cv->w - prompt_w) / 2;
        int py = cv->h - STATUS_H - prompt_h - PAD;
        gfx_fill  (cv, px, py, prompt_w, prompt_h, TH_PANEL2);
        gfx_stroke(cv, px, py, prompt_w, prompt_h, TH_BLUE);
        int tx = px + PAD;
        int input_x = tx + 8 * GFX_FW;
        int hint_w  = 4 * GFX_FW;
        int avail   = (px + prompt_w - PAD - hint_w) - input_x;
        /* row 0: Find */
        int ty0 = py + PAD / 2 + (row_h - GFX_FH) / 2;
        int find_foc = !(a->find_replace && a->find_repl_focus);
        gfx_text(cv, tx, ty0, "Find:", find_foc ? TH_BLUE : TH_TEXT_DIM);
        if (avail > 0) gfx_text_clip(cv, input_x, ty0, a->find_buf, TH_GREEN, input_x, avail);
        if (a->find_len > 0) {
            int has = (a->editor.sel_anchor_off >= 0);
            gfx_text(cv, px + prompt_w - PAD - hint_w, ty0, has ? "ok" : "no",
                     has ? TH_GREEN : TH_ORANGE);
        }
        /* row 1: Replace (replace mode only) */
        if (a->find_replace) {
            int ty1 = py + PAD / 2 + row_h + (row_h - GFX_FH) / 2;
            int repl_foc = a->find_repl_focus;
            gfx_text(cv, tx, ty1, "Repl:", repl_foc ? TH_BLUE : TH_TEXT_DIM);
            if (avail > 0) gfx_text_clip(cv, input_x, ty1, a->repl_buf, TH_CYAN, input_x, avail);
            gfx_text(cv, px + prompt_w - PAD - 8 * GFX_FW, ty1, "Tab/Ent", TH_TEXT_FAINT);
        }
    }
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
        /* Let the inspector's own click handler switch its sub-tab / select a
         * row. (No more force-to-SYNTAX + restore, which silently ate the
         * SYN/CAT/PORT/CONN/INFO tab clicks the user reported.)
         *
         * IDE-REPAIR-0 I3: mirror render_center_top() -- the center never
         * shows the LIB palette, so while LIB is active hit-test the center
         * against the SYNTAX view, NOT the LIB row list (otherwise clicking
         * the AST text would insert a snippet at the caret). A click on the
         * tab strip still selects any tab, including LIB. */
        int was_lib = (a->insp_tab == INSP_LIB);
        if (was_lib) a->insp_tab = INSP_SYNTAX;
        panel_inspector_click(a, a->r_map, mx, my);
        if (was_lib && a->insp_tab == INSP_SYNTAX) a->insp_tab = INSP_LIB; /* no tab clicked */
        break;
    }
    case VIZ_ACTIONS:
    case VIZ_POTENTIALS: {
        /* ACTIONS/POTENTIALS are the DETAILS view; force DETAILS only for the
         * duration of the hit-test so the [APPLY] buttons line up, but if the
         * user clicked a sub-tab honor it (don't blindly revert). */
        int saved = a->insp_tab;
        a->insp_tab = INSP_DETAILS;
        panel_inspector_click(a, a->r_map, mx, my);
        if (a->insp_tab == INSP_DETAILS) a->insp_tab = saved;  /* no tab clicked */
        break;
    }
    case VIZ_RUNTIME:
        panel_runtime_click(a, a->r_map, mx, my);
        break;
    case VIZ_SETTINGS:
        panel_settings_click(a, a->r_map, mx, my, 0);   /* phase 0: grab/toggle */
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
    /* (g_build_view is cleared in the LEGO pointer-press branch before this is
     * reached, so any click dismisses the transient BUILD overlay -- see the
     * TAB-TRAP FIX comment in the event loop.) */
    /* Any click in the LEGO workspace drops code-view keyboard focus first; a
     * click landing in r_code re-takes it via panel_code_click below. This frees
     * the arrow keys for map navigation when the user clicks elsewhere. */
    a->codeview_focus = 0;
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
    if (rect_hit(a->r_runtime, mx, my)) {
        /* the persistent bottom RUNTIME-FLOW strip: click a step to trace it
         * and cross-focus the corresponding function in the map. */
        panel_runtime_click(a, a->r_runtime, mx, my);
        return;
    }
    if (rect_hit(a->r_code, mx, my)) {
        /* code view is now editable: place the caret + take keyboard focus */
        panel_code_click(a, a->r_code, mx, my, g_shift_down);
        ide_sel_from_caret(a, PANE_CODEVIEW);   /* IDE-SYNC-0 S0 */
        return;
    }
    /* status: no interactive handler. */
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
    /* [+ NEW] button -> open the New Project modal. */
    {
        int bx = np_btn_x(r);
        if (mx >= bx && mx < bx + np_btn_w()) { np_open(a); return 1; }
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
    if (editor_toolbar_click(a, a->r_e_toolbar, mx, my)) return;
    if (editor_filetabs_click(a, a->r_e_filetabs, mx, my)) return;

    if (rect_hit(a->r_e_tree, mx, my)) {
        /* The tree uses the same explorer panel. If clicking a file, open it
         * and focus editor. If clicking to select, focus explorer for keyboard nav. */
        panel_explorer_click(a, a->r_e_tree, mx, my);
        /* Clicking a FILE (even the already-open one) focuses the editor so the
         * arrow keys move the caret; only a FOLDER keeps the explorer focused for
         * arrow-key tree nav. (Previously, re-clicking the auto-opened file
         * latched explorer_focused=1 and the explorer key-block ate the arrows.) */
        a->term_focus = 0;
        if (a->sel_entry >= 0 && a->sel_entry < a->nentries &&
            a->entries[a->sel_entry].is_dir) {
            a->explorer_focused = 1;
            a->editor.focused = 0;
        } else {
            a->explorer_focused = 0;
            a->editor.focused = 1;
        }
        return;
    }
    if (rect_hit(a->r_e_editor, mx, my)) {
        a->term_focus = 0;
        a->explorer_focused = 0;
        a->editor.focused = 1;
        ide_editor_click(a, a->r_e_editor, mx, my, g_shift_down);
        ide_sel_from_caret(a, PANE_EDITOR);     /* IDE-SYNC-0 S0 */
        return;
    }
    if (editor_btabs_click(a, a->r_e_btabs, mx, my)) return;
    if (rect_hit(a->r_e_bottom, mx, my)) {
        if (a->btab == BTAB_TERMINAL) {
            /* clicking the terminal body focuses it */
            a->term_focus = 1;
            a->explorer_focused = 0;
            a->editor.focused = 0;
        } else if (a->btab == BTAB_BUILD) {
            /* click-to-jump: clicking a diagnostic row jumps the editor there */
            panel_build_click(a, a->r_e_bottom, mx, my);
        }
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
     * the offsets is harmless for the other tabs). The SETTINGS view (VIZ-6)
     * scrolls its knob list (inspector_scroll, in px) instead of panning. */
    if (rect_hit(a->r_map, mx, my)) {
        if (a->viz == VIZ_SETTINGS) {
            if (!horizontal) {
                a->inspector_scroll += delta * ROW_H;
                if (a->inspector_scroll < 0) a->inspector_scroll = 0;
            }
            return;
        }
        if (horizontal) a->map_ox += delta * MAP_PAN_STEP;
        else            a->map_oy += delta * MAP_PAN_STEP;
        return;
    }

    if (!horizontal) {
        if (rect_hit(a->r_explorer, mx, my))       target = &a->explorer_scroll;
        else if (rect_hit(a->r_funcs, mx, my))     target = &a->funcs_scroll;
        else if (rect_hit(a->r_code, mx, my))      target = &a->code_scroll;
        else if (rect_hit(a->r_inspector, mx, my)) target = &a->inspector_scroll;
    }

    if (target) {
        *target += delta;
        if (*target < 0) *target = 0;
        return;
    }

    /* Nothing scrollable under the cursor: pan the map so arrow keys ALWAYS do
     * something visible on the LEGO face (previously they were silently dropped
     * unless the mouse happened to hover a panel -- "arrow keys don't work"). */
    if (horizontal) a->map_ox += delta * MAP_PAN_STEP;
    else            a->map_oy += delta * MAP_PAN_STEP;
}

/* Shared Ctrl-chord actions available in BOTH workspaces. Returns 1 if the
 * chord was handled. Modifier state is already tracked in g_ctrl_down. */
/* True when keystrokes should drive the shared text editor: the EDITOR
 * workspace with the editor focused, OR the LEGO workspace with the code view
 * focused (Phase 2: code in every view). Lets the core editing chords (undo,
 * copy/cut/paste, select-all, dup/delete-line) work in both places. */
static int ed_input_active(Ide* a) {
    return (a->ws == WS_EDITOR && a->editor.focused) ||
           (a->ws == WS_LEGO   && a->codeview_focus);
}

static int handle_ctrl_chord(Ide* a, int keycode) {
    /* Dismiss autocomplete popup on any Ctrl chord -- Ctrl+S (save), Ctrl+B
     * (build), etc. should not leave the popup visible. The editor's own key
     * handler never sees Ctrl chords (they are consumed here), so the popup
     * must be cleared here rather than in ide_editor_key. */
    a->editor.ac_active = 0;
    switch (keycode) {
    case KEY_N:               /* Ctrl+N: open the New Project templates picker */
        np_open(a);
        return 1;
    case KEY_EQUAL:           /* Ctrl+= : zoom the whole IDE text IN (the layout reflows) */
        gfx_set_scale(g_ui_pct + 25);
        return 1;
    case KEY_MINUS:           /* Ctrl+- : zoom the whole IDE text OUT */
        gfx_set_scale(g_ui_pct - 25);
        return 1;
    case KEY_0:               /* Ctrl+0 : reset the IDE text zoom to the default */
        gfx_set_scale(100);
        return 1;
    case KEY_B:               /* Ctrl+B: build the open file */
        /* Build what's on screen: flush unsaved editor edits to disk first so
         * tc_build (which re-reads the file) compiles the live buffer, not a
         * stale on-disk copy. Harmless when nothing is dirty / no file open. */
        if (a->cur_file[0] && ide_editor_dirty(a)) ide_editor_save(a);
        ide_do_build(a);
        /* Always surface build output: switch to/show the BUILD view. */
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
    case KEY_A:               /* Ctrl+A: select all (editor / code view) */
        if (ed_input_active(a)) ide_editor_select_all(a);
        return 1;
    case KEY_C:               /* Ctrl+C: copy selection / current line */
        if (ed_input_active(a)) ide_editor_copy(a);
        return 1;
    case KEY_X:               /* Ctrl+X: cut selection / current line */
        if (ed_input_active(a)) ide_editor_cut(a);
        return 1;
    case KEY_V:               /* Ctrl+V: paste */
        if (ed_input_active(a)) ide_editor_paste(a);
        return 1;
    case KEY_Z:               /* Ctrl+Z: undo  /  Ctrl+Shift+Z: redo */
        if (ed_input_active(a)) {
            if (g_shift_down) ide_editor_redo(a);
            else              ide_editor_undo(a);
        }
        return 1;
    case KEY_Y:               /* Ctrl+Y: redo */
        if (ed_input_active(a)) ide_editor_redo(a);
        return 1;
    case KEY_COMMA:           /* Ctrl+, : open the Settings panel (VIZ-6) */
        a->ws  = WS_LEGO;
        a->viz = VIZ_SETTINGS;
        a->codeview_focus = 0;
        g_build_view = 0;
        return 1;
    case KEY_F:               /* Ctrl+F: open the find prompt (editor) */
        if (a->ws == WS_EDITOR) {
            a->find_active = 1;
            a->find_replace = 0;
            a->find_len = 0;
            a->find_buf[0] = '\0';
            a->goto_active = 0;       /* find + goto are mutually exclusive */
        }
        return 1;
    case KEY_H:               /* Ctrl+H: open find & replace (editor) */
        if (a->ws == WS_EDITOR) {
            a->find_active = 1;
            a->find_replace = 1;
            a->find_repl_focus = 0;   /* start in the Find field */
            a->find_len = 0; a->find_buf[0] = '\0';
            a->repl_len = 0; a->repl_buf[0] = '\0';
            a->goto_active = 0;
        }
        return 1;
    case KEY_G:               /* Ctrl+G: go to line (editor only) */
        if (a->ws == WS_EDITOR && a->editor.focused) {
            a->goto_active = 1;
            a->goto_len = 0;
            a->goto_buf[0] = '\0';
        }
        return 1;
    case KEY_D:               /* Ctrl+D: multi-cursor (with selection) or duplicate line */
        if (ed_input_active(a))
            ide_editor_multi_cursor_add(a);
        return 1;
    case KEY_K:               /* Ctrl+Shift+K: delete line */
        if (g_shift_down && ed_input_active(a))
            ide_editor_delete_line(a);
        return 1;
    case KEY_W:               /* Ctrl+W: toggle word wrap (editor only) */
        if (a->ws == WS_EDITOR && a->editor.focused)
            ide_editor_toggle_wrap(a);
        return 1;
    case KEY_M:               /* Ctrl+M: toggle minimap (editor only) */
        if (a->ws == WS_EDITOR && a->editor.focused)
            ide_editor_toggle_minimap(a);
        return 1;
    case KEY_E:
        if (g_shift_down) {
            /* Ctrl+Shift+E: toggle zen mode (hide tree + bottom) */
            a->zen_mode = !a->zen_mode;
            if (a->zen_mode) {
                a->editor.focused = 1;
                a->term_focus = 0;
                a->explorer_focused = 0;
            }
        } else {
            /* Ctrl+E: toggle workspace */
            a->ws = (a->ws == WS_EDITOR) ? WS_LEGO : WS_EDITOR;
        }
        return 1;
    case KEY_GRAVE:           /* Ctrl+`: focus terminal + show it */
        if (a->ws == WS_EDITOR) {
            a->btab = BTAB_TERMINAL;
            a->term_focus = !a->term_focus;
            a->editor.focused = !a->term_focus;
            a->explorer_focused = 0;
        }
        return 1;
    case KEY_J:               /* Ctrl+J: toggle bottom-panel focus */
        if (a->ws == WS_EDITOR) {
            a->term_focus = !a->term_focus;
            a->editor.focused = !a->term_focus;
            a->explorer_focused = 0;
            if (a->term_focus) a->btab = BTAB_TERMINAL;
        }
        return 1;
    case KEY_TAB:             /* Ctrl+Tab: cycle to the next open file tab */
        if (a->ws == WS_EDITOR)
            ide_tab_next(a);
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

    /* New Project modal captures ALL keys while open (Esc closes it, Enter
     * advances/confirms, Backspace edits, arrows move the selection, printable
     * keys type the name). It runs BEFORE the global ESC-exit so Esc dismisses
     * the modal instead of quitting the whole IDE. */
    if (a->np.phase != NP_CLOSED) {
        char ch = ide_keycode_ascii(keycode, g_shift_down);
        np_key(a, keycode, ch);
        return;
    }

    /* Go-to-line prompt captures keys (digits, Enter, Esc, Backspace) */
    if (a->goto_active) {
        if (keycode == KEY_ESC) {
            a->goto_active = 0;
            return;
        }
        if (keycode == KEY_BACKSPC) {
            if (a->goto_len > 0) {
                a->goto_len--;
                a->goto_buf[a->goto_len] = '\0';
            }
            return;
        }
        if (keycode == KEY_ENTER) {
            /* Parse line number and jump */
            int line = 0;
            for (int i = 0; i < a->goto_len; i++) {
                if (a->goto_buf[i] >= '0' && a->goto_buf[i] <= '9') {
                    line = line * 10 + (a->goto_buf[i] - '0');
                }
            }
            if (line > 0) {
                /* Jump to line (1-indexed for user, 0-indexed internally) */
                a->editor.caret_line = line - 1;
                a->editor.caret_col = 0;
                a->editor.want_col = 0;
                /* Clamp to valid range */
                extern int ed_line_count(const char* src, int len);
                int total = ed_line_count(a->src, a->src_len);
                if (a->editor.caret_line >= total) a->editor.caret_line = total - 1;
                if (a->editor.caret_line < 0) a->editor.caret_line = 0;
            }
            a->goto_active = 0;
            return;
        }
        /* Type digits 0-9 */
        char ch = ide_keycode_ascii(keycode, g_shift_down);
        if (ch >= '0' && ch <= '9' && a->goto_len < 7) {
            a->goto_buf[a->goto_len++] = ch;
            a->goto_buf[a->goto_len] = '\0';
        }
        return;
    }

    /* Find prompt (Ctrl+F) / Find&Replace (Ctrl+H). In plain find: incremental
     * search, Enter = next match. In replace mode: Tab toggles the Find/Replace
     * field, Enter = replace ALL. Esc closes either. */
    if (a->find_active && !g_ctrl_down) {
        int rep = a->find_replace;
        int on_repl = rep && a->find_repl_focus;
        if (keycode == KEY_ESC) { a->find_active = 0; return; }
        if (keycode == KEY_TAB && rep) { a->find_repl_focus = !a->find_repl_focus; return; }
        if (keycode == KEY_ENTER) {
            if (rep) {                              /* replace ALL find -> repl */
                a->find_buf[a->find_len] = '\0';
                a->repl_buf[a->repl_len] = '\0';
                if (a->find_len > 0) ide_editor_replace_all(a, a->find_buf, a->repl_buf);
                a->find_active = 0;
            } else {
                ide_editor_find(a, a->find_buf, 1); /* find next */
            }
            return;
        }
        if (keycode == KEY_BACKSPC) {
            if (on_repl) { if (a->repl_len > 0) a->repl_buf[--a->repl_len] = '\0'; }
            else {
                if (a->find_len > 0) a->find_buf[--a->find_len] = '\0';
                if (a->find_len > 0) ide_editor_find(a, a->find_buf, 0);
                else a->editor.sel_anchor_off = -1;
            }
            return;
        }
        char fch = ide_keycode_ascii(keycode, g_shift_down);
        if (fch >= 32 && fch < 127) {
            if (on_repl) {
                if (a->repl_len < (int)sizeof(a->repl_buf) - 1) {
                    a->repl_buf[a->repl_len++] = fch;
                    a->repl_buf[a->repl_len] = '\0';
                }
            } else if (a->find_len < (int)sizeof(a->find_buf) - 1) {
                a->find_buf[a->find_len++] = fch;
                a->find_buf[a->find_len] = '\0';
                ide_editor_find(a, a->find_buf, 0);  /* live-preview the match */
            }
        }
        return;
    }

    /* Ctrl chords first (work in both workspaces). handle_ctrl_chord only
     * returns 1 for keys we actually bind (B/R/S/E/J/`). If g_ctrl_down is set
     * but the key is NOT a bound chord, we deliberately FALL THROUGH to normal
     * typing instead of dead-swallowing the key: on real hardware a Ctrl
     * *release* can be missed (focus change while Ctrl is held, or a dropped
     * event), which would otherwise latch g_ctrl_down=1 and silently kill ALL
     * subsequent typing. Falling through means a desynced modifier can never
     * permanently disable the keyboard -- at worst one keystroke types a literal
     * char, and the next real Ctrl press/release resyncs the state. */
    if (g_ctrl_down) {
        if (handle_ctrl_chord(a, keycode)) return;
    }

    /* CODE VIEW (LEGO workspace) keyboard editing: when the code panel has
     * focus, route typing/arrows/backspace to the shared editor (mirrored in
     * both views). Esc releases focus -- handled here, BEFORE the global
     * ESC=exit below, so editing the map's code panel never quits the IDE. */
    if (a->ws == WS_LEGO && a->codeview_focus) {
        if (keycode == KEY_ESC) { a->codeview_focus = 0; return; }
        char ch = ide_keycode_ascii(keycode, g_shift_down);
        ide_editor_key(a, keycode, ch, g_shift_down, 0);
        ide_sel_from_caret(a, PANE_CODEVIEW);   /* IDE-SYNC-0 S0 */
        /* The autocomplete popup is drawn only by the main editor; suppress it
         * in the code view so an invisible popup can't capture Tab/Enter (v1). */
        a->editor.ac_active = 0;
        return;
    }

    /* ESC always exits (matches the LEGO workspace's original behaviour). */
    if (keycode == KEY_ESC) { ide_exit(0); return; }

    /* ============ EDITOR workspace: route typing to editor/terminal/explorer ======= */
    if (a->ws == WS_EDITOR) {
        /* Explorer keyboard navigation (arrows + Enter) */
        if (a->explorer_focused) {
            if (keycode == KEY_UP) {
                if (a->sel_entry > 0) a->sel_entry--;
                /* Scroll to keep selection visible */
                if (a->sel_entry < a->explorer_scroll)
                    a->explorer_scroll = a->sel_entry;
                return;
            }
            if (keycode == KEY_DOWN) {
                if (a->sel_entry < a->nentries - 1) a->sel_entry++;
                /* Scroll to keep selection visible (approximate visible rows) */
                int vis = (a->r_e_tree.h - (ROW_H + 2)) / ROW_H;
                if (a->sel_entry >= a->explorer_scroll + vis)
                    a->explorer_scroll = a->sel_entry - vis + 1;
                return;
            }
            if (keycode == KEY_ENTER) {
                /* Open selected file or toggle folder */
                if (a->sel_entry >= 0 && a->sel_entry < a->nentries) {
                    EntryRow* e = &a->entries[a->sel_entry];
                    if (e->is_dir) {
                        /* Toggle folder collapse (same as clicking it) */
                        ide_toggle_collapsed(a, e->path);
                        rebuild_visible_entries(a);
                    } else {
                        /* Open file and focus editor for typing */
                        ide_open_file(a, e->path);
                        a->explorer_focused = 0;
                        a->editor.focused = 1;
                    }
                }
                return;
            }
            /* Esc unfocuses explorer, focuses editor */
            if (keycode == KEY_ESC) {
                a->explorer_focused = 0;
                a->editor.focused = 1;
                return;
            }
            /* Any other key while explorer focused: ignore (don't type in editor) */
            return;
        }

        char ch = ide_keycode_ascii(keycode, g_shift_down);
        if (a->term_focus)
            ide_term_key(a, keycode, ch, g_shift_down, 0);
        else {
            ide_editor_key(a, keycode, ch, g_shift_down, 0);
            ide_sel_from_caret(a, PANE_EDITOR); /* IDE-SYNC-0 S0 */
        }
        return;
    }

    /* =================== LEGO workspace: analysis shortcuts =============== */

    /* Per-VIZ panel keyboard control (input works in every center panel). Each is
     * gated on the active VIZ tab (only one is shown at a time, so arrows never
     * conflict); each returns 0 for keys it doesn't use, so digits/'q'/etc. fall
     * through to the global LEGO shortcuts below. */
    if (a->viz == VIZ_SETTINGS  && panel_settings_key (a, keycode)) return;
    if (a->viz == VIZ_INSPECTOR && panel_inspector_key(a, keycode)) return;
    if (a->viz == VIZ_RUNTIME   && panel_runtime_key  (a, keycode)) return;

    /* VIZ tab shortcuts '1'..'6' (keycodes 2..7) -> VIZ_MAP..VIZ_SETTINGS. */
    if (keycode >= KEY_1 && keycode <= KEY_1 + (int)VIZ_SETTINGS) {
        a->viz = (VizTab)(keycode - KEY_1);
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
    /* Arrow keys: in the MAP (both the focused-function view AND the file
     * overview) they move the node selection -- keyboard navigation across the
     * lego nodes (map_nav no-ops safely if no layout). Other VIZ tabs scroll/pan.
     * The first arrow press in overview selects the first node. */
    case KEY_UP:
        if (a->viz == VIZ_MAP) map_nav(a, 0);
        else scroll_under_mouse(a, -1, 0);
        break;
    case KEY_DOWN:
        if (a->viz == VIZ_MAP) map_nav(a, 1);
        else scroll_under_mouse(a, +1, 0);
        break;
    case KEY_LEFT:
        if (a->viz == VIZ_MAP) map_nav(a, 2);
        else scroll_under_mouse(a, -1, 1);     /* horizontal: pans map_ox     */
        break;
    case KEY_RIGHT:
        if (a->viz == VIZ_MAP) map_nav(a, 3);
        else scroll_under_mouse(a, +1, 1);
        break;
    case KEY_ENTER:                  /* activate the selected map node (= click it) */
        if (a->viz == VIZ_MAP) map_activate(a);
        break;
    case KEY_TAB: {
        int n = a->model.nfuncs;
        if (n > 0) {
            if (a->focus_func < 0)
                ide_set_focus(a, 0);          /* from overview -> first func */
            else
                ide_set_focus(a, (a->focus_func + 1) % n);
        }
        break;
    }
    case KEY_BACKSPC:                 /* go BACK to the previously-focused node or overview */
        if (a->focus_func >= 0 && a->prev_focus == a->focus_func) {
            /* No distinct previous focus -- go to overview */
            a->prev_focus = a->focus_func;
            ide_set_focus(a, -1);
        } else if (a->focus_func >= 0) {
            ide_set_focus(a, a->prev_focus);
        }
        /* if already in overview (focus_func < 0), do nothing */
        break;
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

    wl_window* win = wl_create_window(IDE_W, IDE_H, "Semantic IDE");
    if (!win) {
        ide_exit(1);
    }

    /* Defensive: guarantee the landing view is the editable EDITOR workspace
     * with the editor (not the bottom terminal) owning the keyboard, so the
     * very first keystroke after launch lands in the editor. init() already
     * sets these, but reassert here so nothing can leave keys mis-routed. */
    a->ws          = WS_EDITOR;
    a->term_focus  = 0;
    a->editor.focused = 1;
    g_ctrl_down    = 0;
    g_shift_down   = 0;

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
    int settings_drag = 0;   /* dragging a Settings slider (VIZ-6) */
    const int DRAG_THRESH = 4;            /* px of travel before it's a drag */

    long last_ms = ide_ticks_ms();
    long last_blink_ms = last_ms;

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
        /* Decay the BUILD tab flash (green=ok / red=fail). If the flash is
         * still active, force a redraw so the fading tint animates smoothly. */
        if (ide_build_flash_color()) {
            ide_build_tick(dt);
            g_ide_redraw = 1;
        }

        /* Caret blink: force a redraw at a throttled ~400ms cadence only when a
         * text caret is visible+focused, so the cursor blinks without busy-
         * redrawing. Idle + unfocused => ZERO redraws/commits (the lag fix). */
        int blink_active = (a->ws == WS_EDITOR && a->editor.focused) || a->term_focus ||
                           (a->ws == WS_LEGO && a->codeview_focus);
        if (blink_active && (now_ms - last_blink_ms) >= 400) {
            g_ide_redraw = 1;
            last_blink_ms = now_ms;
        }

        /* Poll for child-process exit (non-blocking WNOHANG). When the spawned
         * program terminates, the run message updates to show its exit code and
         * we force a redraw so the build panel reflects it immediately. */
        if (ide_run_poll())
            g_ide_redraw = 1;

        /* Only re-render + commit when something actually changed (input, zoom,
         * resize, or the blink tick above). No more full-surface recomposite of
         * the whole screen every single frame. */
        if (g_ide_redraw) {
            if (a->ws == WS_EDITOR) {
                layout_editor(a, win);
                render_editor(a, &cv);
            } else {
                layout(a, win);
                render(a, &cv);
            }
            wl_commit(win);
            g_ide_redraw = 0;
        }

        /* Drain all pending input events for this frame. Each event is a state
         * change -> mark the frame dirty so the next render reflects it. */
        int kind, ea, eb, ec;
        while (wl_poll_event(win, &kind, &ea, &eb, &ec)) {
            g_ide_redraw = 1;                 /* any event changes state -> redraw */
            if (kind == WL_EVENT_POINTER) {
                a->mouse_x = ea;
                a->mouse_y = eb;
                /* Extract buttons (low 16 bits) and wheel (high 16 bits, signed) */
                a->buttons = ec & 0xFFFF;
                int wheel_packed = (ec >> 16) & 0xFFFF;
                int wheel = (wheel_packed & 0x8000) ? (int)(wheel_packed | 0xFFFF0000) : (int)wheel_packed;

                int left_now  = (a->buttons & 1);
                int left_prev = (a->prev_buttons & 1);

                /* Ctrl + mouse-wheel: ZOOM the whole IDE text in/out (the layout
                 * reflows from the runtime cell size). Takes priority over scroll. */
                if (wheel != 0 && g_ctrl_down) {
                    gfx_set_scale(g_ui_pct + (wheel > 0 ? 25 : -25));
                    a->prev_buttons = a->buttons;
                    continue;                 /* don't also scroll */
                }

                /* Handle mouse wheel scrolling (negative=scroll down, positive=scroll up) */
                if (wheel != 0) {
                    int scroll_lines = -wheel;  /* invert: wheel up = scroll up = decrease scroll */
                    if (a->ws == WS_EDITOR) {
                        if (rect_hit(a->r_e_editor, ea, eb)) {
                            /* Scroll editor -- clamp to [0, total_lines-1] so
                             * the user cannot scroll past the end of the file. */
                            extern int ed_line_count(const char* src, int len);
                            a->editor.top_line += scroll_lines;
                            if (a->editor.top_line < 0) a->editor.top_line = 0;
                            int max_top = ed_line_count(a->src, a->src_len) - 1;
                            if (max_top < 0) max_top = 0;
                            if (a->editor.top_line > max_top)
                                a->editor.top_line = max_top;
                        } else if (rect_hit(a->r_e_tree, ea, eb)) {
                            /* Scroll file tree in editor workspace */
                            a->explorer_scroll += scroll_lines;
                            if (a->explorer_scroll < 0) a->explorer_scroll = 0;
                            if (a->nentries > 0 && a->explorer_scroll > a->nentries - 1)
                                a->explorer_scroll = a->nentries - 1;
                        } else if (rect_hit(a->r_e_bottom, ea, eb)) {
                            if (a->btab == BTAB_BUILD) {
                                /* Scroll the build output panel */
                                panel_build_scroll(scroll_lines);
                            } else if (a->btab == BTAB_TERMINAL) {
                                /* Scroll terminal scrollback (wheel up = back in history) */
                                ide_term_scroll(&a->term, -scroll_lines);
                            }
                        }
                    } else {
                        /* LEGO workspace: scroll map */
                        if (rect_hit(a->r_map, ea, eb)) {
                            a->map_oy += scroll_lines * 20;  /* 20px per notch */
                        }
                    }
                }

                if (a->np.phase != NP_CLOSED) {
                    /* Modal up: it captures the pointer. Only the press edge
                     * matters (select a template / advance); drags are ignored. */
                    if (left_now && !left_prev)
                        np_click(a, ea, eb);
                    a->prev_buttons = a->buttons;
                    continue;
                }

                if (a->ws == WS_EDITOR) {
                    /* EDITOR: route clicks immediately on the press edge; no
                     * map-drag panning in this workspace. */
                    if (left_now && !left_prev)
                        route_click_editor(a, ea, eb);
                } else {
                    /* LEGO: map drag vs click handling. */
                    if (left_now && !left_prev) {
                        /* TAB-TRAP FIX: any left-press in the LEGO workspace dismisses
                         * the transient BUILD overlay. Ctrl+B/Ctrl+R set g_build_view and
                         * render_center_top() draws panel_build over the WHOLE center
                         * region while it is set; previously ONLY the '1'..'5' keys cleared
                         * it, so clicking the visible build report (which fills r_map) did
                         * nothing and the user was trapped ("can't view the other tabs").
                         * Clearing it HERE -- before the r_map drag test -- covers BOTH the
                         * map-drag path (route_center_top_click on release) and the side-
                         * panel path (route_click). The result is kept (ide_build_active)
                         * so Ctrl+B re-shows it on demand. */
                        g_build_view = 0;
                        if (a->viz == VIZ_SETTINGS && rect_hit(a->r_map, ea, eb)) {
                            /* Settings (VIZ-6): a press grabs/toggles a row; if it
                             * landed on a slider knob, subsequent moves drag it. No
                             * map panning in this view. */
                            panel_settings_click(a, a->r_map, ea, eb, 0);
                            settings_drag = 1;
                        } else if (rect_hit(a->r_map, ea, eb)) {
                            drag_active = 1; drag_panning = 0;
                            drag_px = ea; drag_py = eb; drag_dx = 0; drag_dy = 0;
                        } else {
                            route_click(a, ea, eb);
                        }
                    } else if (left_now && left_prev) {
                        if (settings_drag) {
                            panel_settings_click(a, a->r_map, ea, eb, 1);  /* drag move */
                        } else if (drag_active) {
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
                        if (settings_drag) {
                            panel_settings_click(a, a->r_map, ea, eb, 2);  /* release */
                            settings_drag = 0;
                        } else if (drag_active && !drag_panning) {
                            route_center_top_click(a, ea, eb);
                        }
                        drag_active = 0; drag_panning = 0;
                    }
                }

                a->prev_buttons = a->buttons;
            } else if (kind == WL_EVENT_KEY) {
                handle_key(a, ea, eb);
            }
        }

        /* Brief cooperative yield between frames; input still arrives via the
         * poll above. */
        ide_sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);
    }
}

