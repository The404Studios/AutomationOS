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

/* ===========================================================================
 * File loading + focus plumbing (declared in ide.h; called by panels too).
 * ==========================================================================*/

void ide_open_file(Ide* a, const char* path) {
    if (!a || !path) return;

    /* A built artifact is a PROGRAM, not source: RUN it (SYS_SPAWN) rather than load
     * its binary bytes into the text editor (which showed garbage -- the user's "the
     * elfs don't open" complaint). Both the explorer click/Enter and the post-build
     * reveal route through here, so double-clicking a .elf now launches it. */
    if (ends_with_dot_elf(path)) {
        ide_sc(16 /* SYS_SPAWN */, (long)path, 0, 0, 0, 0, 0);
        return;
    }

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
    a->map_zoom = 100;              /* reset to 100% on file load */

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

/* Clone template dir `src_dir` into `dst_dir`, copying every regular file.
 * Records the best "main .c" path into open_path (cap bytes). Returns the
 * number of files copied, or <0 on a fatal error (dst mkdir failed). */
static int np_clone(Ide* a, const char* src_dir, const char* dst_dir,
                    char* open_path, int open_cap) {
    if (open_cap > 0) open_path[0] = 0;

    /* Create the destination project directory (recursive mkdir; ignore an
     * "already exists" style error -- the write below would still succeed). */
    ide_sc(SYS_MKDIR_NUM, (long)dst_dir, 0755, 0, 0, 0, 0);

    IdeDirent ents[64];
    int got = ide_list_dir(src_dir, ents, 64);
    if (got < 0) return -1;

    int copied = 0;
    int have_main = 0;          /* 1 once a preferred main .c is chosen      */
    for (int i = 0; i < got; i++) {
        if (ents[i].type != IDE_DT_REG) continue;     /* flat dir of files  */
        if (is_dot_entry(ents[i].name)) continue;

        char src_path[IDE_PATH];
        char dst_path[IDE_PATH];
        path_join(src_path, IDE_PATH, src_dir, ents[i].name);
        path_join(dst_path, IDE_PATH, dst_dir, ents[i].name);

        /* Copy bytes verbatim through the shared source buffer. */
        int n = ide_read_file(src_path, a->src, IDE_SRC_CAP);
        if (n < 0) continue;                          /* skip unreadable    */
        if (n > IDE_SRC_CAP) n = IDE_SRC_CAP;
        if (ide_write_file(dst_path, a->src, n) < 0) continue;
        copied++;

        /* Track which copied file to open: a preferred main wins outright;
         * otherwise remember the first .c as a fallback. */
        if (open_cap > 0 && ends_with_dot_c(ents[i].name)) {
            if (!have_main && np_is_preferred_main(ents[i].name)) {
                ide_strlcpy(open_path, dst_path, open_cap);
                have_main = 1;
            } else if (open_path[0] == 0) {
                ide_strlcpy(open_path, dst_path, open_cap);
            }
        }
    }
    return copied;
}

/* Confirm the typed name: validate, build the src/dst dirs, clone, rescan the
 * tree and open the new project's main .c. Updates np->status either way. */
static void np_confirm(Ide* a) {
    NewProj* np = &a->np;

    if (np->ntpl <= 0) { np_close(a); return; }
    if (np->name_len <= 0) {
        ide_strlcpy(np->status, "type a project name, then Enter", 96);
        return;
    }
    if (np->sel < 0 || np->sel >= np->ntpl) np->sel = 0;

    char src_dir[IDE_PATH];
    char dst_dir[IDE_PATH];
    path_join(src_dir, IDE_PATH, IDE_TEMPLATES_DIR, np->tpl[np->sel]);
    path_join(dst_dir, IDE_PATH, IDE_PROJECTS_DIR,  np->name);

    char open_path[IDE_PATH];
    int copied = np_clone(a, src_dir, dst_dir, open_path, IDE_PATH);
    if (copied < 0) {
        ide_strlcpy(np->status, "create failed (mkdir/readdir)", 96);
        return;
    }
    if (copied == 0) {
        ide_strlcpy(np->status, "template was empty", 96);
        return;
    }

    /* Refresh the explorer so the new /usr/src/<name>/ appears, then open the
     * project's main source (if we found one) and land in the editor. */
    scan_project(a);
    np_close(a);
    if (open_path[0]) {
        ide_open_file(a, open_path);
        a->ws = WS_EDITOR;
        a->term_focus = 0;
        a->editor.focused = 1;
        /* Select the opened file's row in the tree if we can find it. */
        for (int i = 0; i < a->nentries; i++) {
            if (!a->entries[i].is_dir &&
                ide_streq(a->entries[i].path, open_path)) {
                a->sel_entry = i;
                break;
            }
        }
    }
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
        a->focus_func = 0;
        model_analyze(&a->model);
    }

    a->viz = VIZ_MAP;
    a->insp_tab = 2;                  /* PORTS                              */
    a->map_zoom = 100;                /* 100% zoom initially                 */

    /* ---- EDITOR workspace defaults (this is the default face) ---- */
    a->ws = WS_EDITOR;
    a->btab = BTAB_TERMINAL;
    a->bottom_h = 200;                /* initial bottom-dock height (px)     */
    a->term_focus = 0;                /* editor has keys initially           */
    a->explorer_focused = 0;          /* explorer unfocused initially        */
    a->goto_active = 0;               /* go-to-line inactive initially       */
    a->goto_len = 0;
    a->n_collapsed = 0;               /* all folders expanded initially      */
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
 * "New Project" modal rendering + click hit-testing.
 *
 * Drawn LAST (over whatever workspace is active) as a centered card. Phase
 * NP_PICK lists the templates; NP_NAME shows the typed name with a caret. The
 * card geometry is recomputed from the live window size so it always centers.
 * ==========================================================================*/

#define NP_CARD_W   360
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
    const char* leg = "Ctrl+N new  B build  R run  S save  F find  H replace  E map";
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

    render_newproj(a, cv);                     /* modal overlay (if open)    */

    /* Go-to-line prompt overlay (bottom-center, inline) */
    if (a->goto_active) {
        int prompt_w = 200;
        int prompt_h = 30;
        int px = (cv->w - prompt_w) / 2;
        int py = cv->h - 60;  /* 60px from bottom */

        /* Background box */
        gfx_fill(cv, px, py, prompt_w, prompt_h, TH_PANEL2);
        gfx_stroke(cv, px, py, prompt_w, prompt_h, TH_CYAN);

        /* Prompt text */
        int tx = px + 8;
        int ty = py + (prompt_h - GFX_FH) / 2;
        gfx_text(cv, tx, ty, "Go to line:", TH_TEXT);

        /* User input */
        int input_x = tx + 11 * GFX_FW + 8;
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
     * is accent-coloured. The match is highlighted in the editor body. */
    if (a->find_active) {
        int rows = a->find_replace ? 2 : 1;
        int prompt_w = 420;
        int row_h = 26;
        int prompt_h = rows * row_h + 8;
        int px = (cv->w - prompt_w) / 2;
        int py = cv->h - 40 - prompt_h;
        gfx_fill  (cv, px, py, prompt_w, prompt_h, TH_PANEL2);
        gfx_stroke(cv, px, py, prompt_w, prompt_h, TH_BLUE);
        int tx = px + 8;
        int input_x = tx + 8 * GFX_FW;
        int hint_w  = 4 * GFX_FW;
        int avail   = (px + prompt_w - 8 - hint_w) - input_x;
        /* row 0: Find */
        int ty0 = py + 4 + (row_h - GFX_FH) / 2;
        int find_foc = !(a->find_replace && a->find_repl_focus);
        gfx_text(cv, tx, ty0, "Find:", find_foc ? TH_BLUE : TH_TEXT_DIM);
        if (avail > 0) gfx_text_clip(cv, input_x, ty0, a->find_buf, TH_GREEN, input_x, avail);
        if (a->find_len > 0) {
            int has = (a->editor.sel_anchor_off >= 0);
            gfx_text(cv, px + prompt_w - 8 - hint_w, ty0, has ? "ok" : "no",
                     has ? TH_GREEN : TH_ORANGE);
        }
        /* row 1: Replace (replace mode only) */
        if (a->find_replace) {
            int ty1 = py + 4 + row_h + (row_h - GFX_FH) / 2;
            int repl_foc = a->find_repl_focus;
            gfx_text(cv, tx, ty1, "Repl:", repl_foc ? TH_BLUE : TH_TEXT_DIM);
            if (avail > 0) gfx_text_clip(cv, input_x, ty1, a->repl_buf, TH_CYAN, input_x, avail);
            gfx_text(cv, px + prompt_w - 8 - 8 * GFX_FW, ty1, "Tab/Ent", TH_TEXT_FAINT);
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
    /* (g_build_view is cleared in the LEGO pointer-press branch before this is
     * reached, so any click dismisses the transient BUILD overlay -- see the
     * TAB-TRAP FIX comment in the event loop.) */
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
        ide_editor_click(a, a->r_e_editor, mx, my);
        return;
    }
    if (editor_btabs_click(a, a->r_e_btabs, mx, my)) return;
    if (rect_hit(a->r_e_bottom, mx, my)) {
        /* clicking the bottom dock body focuses the terminal (when shown) */
        if (a->btab == BTAB_TERMINAL) {
            a->term_focus = 1;
            a->explorer_focused = 0;
            a->editor.focused = 0;
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
     * the offsets is harmless for the other tabs). */
    if (rect_hit(a->r_map, mx, my)) {
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
static int handle_ctrl_chord(Ide* a, int keycode) {
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
        gfx_set_scale(120);
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
    case KEY_A:               /* Ctrl+A: select all (editor) */
        if (a->ws == WS_EDITOR && a->editor.focused) ide_editor_select_all(a);
        return 1;
    case KEY_C:               /* Ctrl+C: copy selection / current line (editor) */
        if (a->ws == WS_EDITOR && a->editor.focused) ide_editor_copy(a);
        return 1;
    case KEY_X:               /* Ctrl+X: cut selection / current line (editor) */
        if (a->ws == WS_EDITOR && a->editor.focused) ide_editor_cut(a);
        return 1;
    case KEY_V:               /* Ctrl+V: paste (editor) */
        if (a->ws == WS_EDITOR && a->editor.focused) ide_editor_paste(a);
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
    case KEY_D:               /* Ctrl+D: duplicate line (editor only) */
        if (a->ws == WS_EDITOR && a->editor.focused)
            ide_editor_duplicate_line(a);
        return 1;
    case KEY_K:               /* Ctrl+Shift+K: delete line (editor only) */
        if (g_shift_down && a->ws == WS_EDITOR && a->editor.focused)
            ide_editor_delete_line(a);
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
    case KEY_BACKSPC:                 /* go BACK to the previously-focused node */
        ide_set_focus(a, a->prev_focus);
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

    wl_window* win = wl_create_window(IDE_W, IDE_H, "ide");
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

        /* Caret blink: force a redraw at a throttled ~400ms cadence only when a
         * text caret is visible+focused, so the cursor blinks without busy-
         * redrawing. Idle + unfocused => ZERO redraws/commits (the lag fix). */
        int blink_active = (a->ws == WS_EDITOR && a->editor.focused) || a->term_focus;
        if (blink_active && (now_ms - last_blink_ms) >= 400) {
            g_ide_redraw = 1;
            last_blink_ms = now_ms;
        }

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
                            /* Scroll editor */
                            a->editor.top_line += scroll_lines;
                            if (a->editor.top_line < 0) a->editor.top_line = 0;
                        } else if (rect_hit(a->r_explorer, ea, eb)) {
                            /* Scroll explorer */
                            a->explorer_scroll += scroll_lines;
                            if (a->explorer_scroll < 0) a->explorer_scroll = 0;
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
