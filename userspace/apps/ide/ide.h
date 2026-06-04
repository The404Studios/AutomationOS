/*
 * ide.h -- top-level app state + the panel contract.
 *
 * ide.c owns the Ide struct, the wl window, the event loop, layout (it fills
 * the r_* rects every frame), file loading, and calls model_parse/model_analyze.
 * Each panel module implements panel_<x>() to render into a Canvas within its
 * Rect, and (where interactive) panel_<x>_click() to handle a click.
 */
#ifndef IDE_APP_H
#define IDE_APP_H

#include "ide_model.h"
#include "ide_gfx.h"
#include "ide_sys.h"

typedef struct Rect { int x, y, w, h; } Rect;
static inline int rect_hit(Rect r, int mx, int my) {
    return mx >= r.x && my >= r.y && mx < r.x + r.w && my < r.y + r.h;
}

/* Editor caret/scroll/dirty state (see ide_editor.h). Embedded by value in the
 * Ide struct so it persists across renders and is reset on file open. */
#include "ide_editor.h"

/* Embedded integrated-terminal state (see ide_term.h). */
#include "ide_term.h"

/* Top-level workspace: which IDE face is showing. EDITOR is the default VS-Code
 * style layout (file tree + editor + bottom terminal/build tabs); LEGO is the
 * original Semantic LEGO Map analysis workspace (map/inspector/runtime). */
typedef enum {
    WS_EDITOR = 0,   /* VS-Code-lite editing workspace */
    WS_LEGO          /* Semantic LEGO Map analysis      */
} Workspace;

/* Which tab the bottom dock shows in the EDITOR workspace. */
typedef enum {
    BTAB_TERMINAL = 0,
    BTAB_BUILD,
    BTAB_PROBLEMS
} BottomTab;

typedef enum {
    VIZ_MAP = 0,     /* VIZ-1 PROJECT MAP (semantic LEGO map)  */
    VIZ_INSPECTOR,   /* VIZ-2 INSPECTOR                        */
    VIZ_RUNTIME,     /* VIZ-3 RUNTIME                          */
    VIZ_ACTIONS,     /* VIZ-4 ACTIONS                          */
    VIZ_POTENTIALS   /* VIZ-5 POTENTIALS                       */
} VizTab;

#define IDE_SRC_CAP   131072   /* 128 KB max open file              */
#define IDE_MAXENT    256      /* explorer directory entries        */
#define IDE_MAXCOLLAPSE 48     /* max simultaneously-collapsed folders */
#define IDE_PATH      192

typedef struct {
    char name[128];
    int  is_dir;
    int  depth;       /* indent level in the tree                 */
    int  collapsed;   /* dir only: 1 = children hidden (render hint) */
    char path[IDE_PATH];
} EntryRow;

/* ---------------------------------------------------------------------------
 * "New Project" modal. Ctrl+N (or the topbar [+ NEW] button) opens a small
 * centered overlay that (1) lists the project templates found under
 * /usr/src/templates/ so the user picks one, then (2) prompts for a project
 * name. Confirming clones the chosen template dir into /usr/src/<name>/ and
 * opens its main .c in the editor.
 * ------------------------------------------------------------------------- */
#define IDE_TEMPLATES_DIR "/usr/src/templates"
#define IDE_PROJECTS_DIR  "/usr/src"
#define NP_MAXTPL  16          /* templates listed in the picker            */
#define NP_NAMELEN 64          /* max typed project-name length             */

typedef enum {
    NP_CLOSED = 0,   /* modal hidden (normal IDE)                  */
    NP_PICK,         /* choosing a template from the list          */
    NP_NAME          /* typing the new project's name              */
} NewProjPhase;

typedef struct {
    NewProjPhase phase;
    char tpl[NP_MAXTPL][64];   /* template directory names               */
    int  ntpl;                 /* count discovered                       */
    int  sel;                  /* highlighted template row               */
    char name[NP_NAMELEN];     /* project name being typed               */
    int  name_len;
    char status[96];           /* one-line result / error message        */
} NewProj;

typedef struct Ide {
    /* project / file */
    char     root[IDE_PATH];        /* project root dir              */
    char     cur_file[IDE_PATH];    /* currently open file           */
    char     src[IDE_SRC_CAP];      /* source of cur_file            */
    int      src_len;

    /* analysis */
    Model    model;
    int      focus_func;            /* selected function (mirrors model.focus) */
    int      prev_focus;            /* previous focus (Backspace = back in map)  */

    /* explorer */
    EntryRow entries[IDE_MAXENT];
    int      nentries;
    int      sel_entry;
    char     collapsed_paths[IDE_MAXCOLLAPSE][IDE_PATH]; /* folders the user collapsed */
    int      n_collapsed;

    /* UI */
    Workspace ws;                   /* EDITOR (default) or LEGO       */
    VizTab   viz;
    int      explorer_scroll, funcs_scroll, code_scroll, inspector_scroll;
    int      map_ox, map_oy;        /* map pan offset                */
    int      map_zoom;              /* scale x100: 100 = 1.00 (max), 1 = 0.01 (min); clamped 0.01..1.00 */
    int      insp_tab;              /* 0 SYNTAX 1 CATEGORY 2 PORTS 3 CONN 4 DETAILS */

    /* editor workspace state */
    Editor    editor;               /* caret/scroll/dirty (ide_editor.c) */
    IdeTerm   term;                 /* integrated terminal (ide_term.c)  */
    BottomTab btab;                 /* TERMINAL / BUILD / PROBLEMS       */
    int       bottom_h;             /* current bottom-dock height (px)   */
    int       term_focus;           /* 1 = terminal owns keys, 0 = editor*/
    int       explorer_focused;     /* 1 = explorer owns keys (arrows/enter) */
    int       goto_active;          /* 1 = go-to-line prompt active */
    char      goto_buf[8];          /* line number input buffer (max 7 digits + NUL) */
    int       goto_len;             /* current length of goto_buf */
    int       find_active;          /* 1 = find (Ctrl+F) prompt active */
    char      find_buf[96];         /* search query */
    int       find_len;             /* current length of find_buf */
    int       find_replace;         /* 1 = replace mode (Ctrl+H): two fields */
    int       find_repl_focus;      /* 0 = editing query, 1 = editing replacement */
    char      repl_buf[96];         /* replacement text */
    int       repl_len;             /* current length of repl_buf */
    int       win_w, win_h;         /* last known window size (responsive)*/

    /* layout rects (recomputed each frame by ide.c) */
    Rect     r_topbar, r_explorer, r_funcs, r_map, r_code,
             r_inspector, r_runtime, r_status;
    /* editor-workspace rects */
    Rect     r_e_tree, r_e_editor, r_e_bottom, r_e_btabs;

    /* "New Project" modal (Ctrl+N / topbar [+ NEW]); see NewProj above. */
    NewProj  np;

    int      mouse_x, mouse_y, buttons, prev_buttons;
} Ide;

/* ---- panel render entry points (implemented by their own modules) ---- */
void panel_explorer  (Ide* a, Canvas* cv, Rect r);   /* ide_explorer.c */
void panel_funcs     (Ide* a, Canvas* cv, Rect r);   /* ide_funcs.c    */
void panel_map       (Ide* a, Canvas* cv, Rect r);   /* ide_map.c      */
void panel_code      (Ide* a, Canvas* cv, Rect r);   /* ide_codeview.c */
void panel_inspector (Ide* a, Canvas* cv, Rect r);   /* ide_inspector.c*/
void panel_runtime   (Ide* a, Canvas* cv, Rect r);   /* ide_runtime.c  */
void panel_topbar    (Ide* a, Canvas* cv, Rect r);   /* ide_chrome.c   */
void panel_status    (Ide* a, Canvas* cv, Rect r);   /* ide_chrome.c   */

/* ---- click handlers: return 1 if consumed; may mutate *a ---- */
int  panel_explorer_click (Ide* a, Rect r, int mx, int my);
int  panel_funcs_click    (Ide* a, Rect r, int mx, int my);
int  panel_map_click      (Ide* a, Rect r, int mx, int my);
int  panel_inspector_click(Ide* a, Rect r, int mx, int my);
int  panel_topbar_click   (Ide* a, Rect r, int mx, int my);   /* switch VIZ tab */

/* ---- generation (ide_gen.c): apply ACTIONS / emit skeleton ---- */
/* Apply model.actions[idx] to a->src (and persist via ide_write_file). Returns
 * 1 if the source changed (caller should re-parse + re-analyze). */
int  gen_apply_action(Ide* a, int idx);

/* ---- helpers ide.c provides for panels (selection plumbing) ---- */
/* Load file `path` into a->src, set cur_file, re-parse+re-analyze, focus 0. */
void ide_open_file(Ide* a, const char* path);
/* Re-scan the project tree and reveal `dir` (the build folder), auto-selecting
 * `sel_path` (the produced ELF) if present. Used after a successful build so the
 * artifact is visible + openable in the explorer. (ide.c) */
void ide_reveal_dir(Ide* a, const char* dir, const char* sel_path);
/* Explorer folder collapse/expand (ide.c). collapsed state is kept by path so
 * it survives the tree re-scan; rebuild_visible_entries re-scans hiding the
 * children of collapsed folders and restores the selection. */
int  ide_is_collapsed(Ide* a, const char* path);
void ide_toggle_collapsed(Ide* a, const char* path);
void rebuild_visible_entries(Ide* a);
/* Set the focused function (re-runs analyze for that focus). */
void ide_set_focus(Ide* a, int func_idx);
/* Open the "New Project" templates modal (Ctrl+N / topbar [+ NEW] button).
 * Public so chrome in ide_chrome.c can trigger it too. Implemented in ide.c. */
void ide_new_project(Ide* a);

#endif /* IDE_H */
