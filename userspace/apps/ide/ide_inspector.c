/*
 * ide_inspector.c -- the INSPECTOR panel (right column).
 *
 * Tabbed tables for the focused function. Tabs:
 *   0 SYNTAX     -- signature + param list
 *   1 CATEGORY   -- port/read/write/call counts + coherence
 *   2 PORTS      -- PORT | TYPE | DIR | FIT | STATUS         (default)
 *   3 CONNECTIONS-- FROM | TO | TYPE | COMPAT | STATUS  (from m->conns)
 *   4 DETAILS    -- POTENTIALS/RISKS then ACTIONS with [APPLY] buttons
 *
 * Freestanding C: no libc/malloc/stdio. Only STATIC helpers (insp_*). STATIC
 * buffers. All drawing clipped to the supplied Rect r. a->inspector_scroll
 * scrolls long table bodies.
 */
#include "ide.h"
#include "ide_gfx.h"
#include "ide_theme.h"
#include "ide_ast.h"
#include "ide_astprint.h"
#include "ide_library.h"          /* the "complex" library shown in the LIB tab */
#include "ide_editor.h"           /* ide_editor_insert_snippet (click -> insert) */
#include "ide_config.h"           /* persist Settings knobs on change            */
#include "ide_marks.h"            /* IDE-FORGE-0: per-symbol marks (MARK tab)     */
#include "ide_build.h"            /* IDE-FORGE-0: ACTIONS deck backends          */

/* ---- layout constants local to this panel ---- */
#define INSP_HEADER_H   (ROW_H + 2)   /* "INSPECTOR - <func>" header bar  */
#define INSP_TAB_H      (ROW_H + 2)   /* tab strip height                 */
#define INSP_NTABS      7
#define INSP_DOT_W      10            /* gutter for the coloured port dot */
#define INSP_APPLY_W    56            /* [APPLY] button width             */
#define INSP_APPLY_H    (ROW_H - 2)   /* [APPLY] button height            */

/* Settings/MARK row geometry (shared by panel_settings + the MARK knob strip
 * + the ACTIONS deck + the Pulse, so the widget vocabulary is identical). */
#define SET_ROW_H   (ROW_H + 8)
#define SET_HEAD_H  (ROW_H + 2)
#define SET_KNOB_W  8

/* SHORT labels: 5 equal-width cells in RIGHT_W (=27*GFX_FW) give ~48px each at
 * the default scale, so the old full words ("CATEGORY"=8 chars) overflowed and
 * truncated to unreadable fragments. Compact 3-4 char labels fit cleanly at
 * every zoom level. (Hover/active state + the body content disambiguate them.) */
static const char* const INSP_TAB_LBL[INSP_NTABS] = {
    "SYN", "CAT", "PORT", "CONN", "INFO", "LIB", "MARK"
};

/* ---- tiny shared scratch (no libc) ---- */
static char insp_num[16];

/* draw `s` clipped to [x, x+maxw). */
static void insp_text(Canvas* cv, int x, int y, const char* s,
                      uint32_t col, int maxw) {
    if (maxw <= 0) return;
    gfx_text_clip(cv, x, y, s, col, x, maxw);
}

/* format v/100 as "0.NN" (or "1.00" etc.) into `out` (cap >= 8). Signed-safe
 * for the magnitude; caller prepends sign. Returns length. */
static int insp_fix2(int v, char* out) {
    int n = 0;
    if (v < 0) v = -v;
    int whole = v / 100;
    int frac  = v % 100;
    n += ide_itoa(whole, out + n);
    out[n++] = '.';
    out[n++] = (char)('0' + (frac / 10));
    out[n++] = (char)('0' + (frac % 10));
    out[n]   = 0;
    return n;
}

/* port-type name */
static const char* insp_type_name(int t) {
    switch (t) {
        case PORT_INPUT:        return "input";
        case PORT_STATE_READ:   return "state_read";
        case PORT_STATE_WRITE:  return "state_write";
        case PORT_CONTROL:      return "control";
        case PORT_CONTROL_GATE: return "control_gate";
        case PORT_LIFECYCLE:    return "lifecycle";
        default:                return "?";
    }
}

/* PortStatus -> (text,colour) */
static const char* insp_pstatus_name(int s) {
    switch (s) {
        case PS_CONNECTED: return "connected";
        case PS_ABSENT:    return "absent";
        case PS_WEAK:      return "weak";
        default:           return "?";
    }
}
static uint32_t insp_pstatus_col(int s) {
    switch (s) {
        case PS_CONNECTED: return TH_GREEN;
        case PS_ABSENT:    return TH_RED;
        case PS_WEAK:      return TH_YELLOW;
        default:           return TH_TEXT_DIM;
    }
}

/* ConnStatus -> (text,colour) */
static const char* insp_cstatus_name(int s) {
    switch (s) {
        case CS_SAFE:   return "safe";
        case CS_WEAK:   return "weak";
        case CS_ABSENT: return "absent";
        default:        return "?";
    }
}
static uint32_t insp_cstatus_col(int s) {
    switch (s) {
        case CS_SAFE:   return TH_GREEN;
        case CS_WEAK:   return TH_YELLOW;
        case CS_ABSENT: return TH_RED;
        default:        return TH_TEXT_DIM;
    }
}

/* the focused Func, or NULL if no/invalid focus. */
static Func* insp_focus(Ide* a) {
    int fi = a->focus_func;
    if (fi < 0 || fi >= a->model.nfuncs || fi >= M_MAXFUNCS) return 0;
    return &a->model.funcs[fi];
}

/* ----- geometry shared by render + hit-test ----- */

/* tab rect i: equal-width cells across the tab strip. */
static Rect insp_tab_rect(Rect r, int i) {
    Rect t;
    int strip_y = r.y + INSP_HEADER_H;
    int cw = r.w / INSP_NTABS;
    t.x = r.x + i * cw;
    t.y = strip_y;
    t.w = (i == INSP_NTABS - 1) ? (r.x + r.w - t.x) : cw;
    t.h = INSP_TAB_H;
    return t;
}

/* body region below header+tabs */
static Rect insp_body_rect(Rect r) {
    Rect b;
    b.x = r.x;
    b.y = r.y + INSP_HEADER_H + INSP_TAB_H + 1;
    b.w = r.w;
    b.h = (r.y + r.h) - b.y;
    if (b.h < 0) b.h = 0;
    return b;
}

/* apply-button rect for the i-th action, given the action's row top `ry`. */
static Rect insp_apply_rect(Rect b, int ry) {
    Rect btn;
    btn.x = b.x + b.w - PAD - INSP_APPLY_W;
    btn.y = ry + (ROW_H - INSP_APPLY_H) / 2;
    btn.w = INSP_APPLY_W;
    btn.h = INSP_APPLY_H;
    return btn;
}

/* ====================================================================== *
 *  Tab bodies                                                            *
 * ====================================================================== */

/* draw one fixed-column header row at `ty`, return nothing. cols[] are pixel
 * x-offsets relative to b.x; labels parallel array; ncol entries. */
static void insp_colheader(Canvas* cv, Rect b, int ty,
                           const int* colx, const char* const* lbl, int ncol) {
    for (int c = 0; c < ncol; c++) {
        int x = b.x + colx[c];
        int next = (c + 1 < ncol) ? (b.x + colx[c + 1]) : (b.x + b.w - PAD);
        insp_text(cv, x, ty, lbl[c], TH_TEXT_FAINT, next - x);
    }
}

/* ---- PORTS (tab 2) ---- */
static void insp_body_ports(Ide* a, Canvas* cv, Rect b, Func* f) {
    /* PORT | TYPE | DIR | FIT | STATUS */
    static const char* const HDR[5] = { "PORT", "TYPE", "DIR", "FIT", "STATUS" };
    int colx[5];
    colx[0] = PAD + INSP_DOT_W;     /* PORT name (after dot)   */
    colx[1] = 96;                   /* TYPE                    */
    colx[2] = 184;                  /* DIR                     */
    colx[3] = 214;                  /* FIT                     */
    colx[4] = 252;                  /* STATUS                  */

    int bot = b.y + b.h;
    int y = b.y;
    insp_colheader(cv, b, y, colx, HDR, 5);
    gfx_hline(cv, b.x + PAD, y + GFX_FH + 1, b.w - 2 * PAD, TH_BORDER);
    y += ROW_H;

    y -= a->inspector_scroll;

    int np = f->nports;
    if (np > M_MAXPORTS) np = M_MAXPORTS;
    for (int i = 0; i < np; i++) {
        Port* p = &f->ports[i];
        int ry = y;
        y += ROW_H;
        if (ry + ROW_H <= b.y + ROW_H || ry >= bot) continue;
        int ty = ry + (ROW_H - GFX_FH) / 2;

        /* coloured port-type dot */
        gfx_fill(cv, b.x + PAD, ty + 4, 7, 7, th_port_color(p->type));

        insp_text(cv, b.x + colx[0], ty, p->name, TH_TEXT,
                  colx[1] - colx[0] - 2);
        insp_text(cv, b.x + colx[1], ty, insp_type_name(p->type), TH_TEXT_DIM,
                  colx[2] - colx[1] - 2);
        insp_text(cv, b.x + colx[2], ty, p->dir == DIR_OUT ? "out" : "in",
                  TH_TEXT_DIM, colx[3] - colx[2] - 2);

        insp_fix2(p->fit, insp_num);
        insp_text(cv, b.x + colx[3], ty, insp_num, TH_TEXT_DIM,
                  colx[4] - colx[3] - 2);

        insp_text(cv, b.x + colx[4], ty, insp_pstatus_name(p->status),
                  insp_pstatus_col(p->status), b.w - colx[4] - PAD);
    }

    if (np == 0)
        insp_text(cv, b.x + PAD, y + 2, "(no ports)", TH_TEXT_FAINT,
                  b.w - 2 * PAD);
}

/* ---- CONNECTIONS (tab 3) ---- */
static void insp_body_conns(Ide* a, Canvas* cv, Rect b) {
    /* FROM | TO | TYPE | COMPAT | STATUS */
    static const char* const HDR[5] = { "FROM", "TO", "TYPE", "COMPAT", "STATUS" };
    int colx[5];
    colx[0] = PAD;     /* FROM   */
    colx[1] = 70;      /* TO     */
    colx[2] = 140;     /* TYPE   */
    colx[3] = 196;     /* COMPAT */
    colx[4] = 232;     /* STATUS */

    int bot = b.y + b.h;
    int y = b.y;
    insp_colheader(cv, b, y, colx, HDR, 5);
    gfx_hline(cv, b.x + PAD, y + GFX_FH + 1, b.w - 2 * PAD, TH_BORDER);
    y += ROW_H;

    y -= a->inspector_scroll;

    int nc = a->model.nconns;
    if (nc > M_MAXCONNS) nc = M_MAXCONNS;
    for (int i = 0; i < nc; i++) {
        Conn* c = &a->model.conns[i];
        int ry = y;
        y += ROW_H;
        if (ry + ROW_H <= b.y + ROW_H || ry >= bot) continue;
        int ty = ry + (ROW_H - GFX_FH) / 2;

        insp_text(cv, b.x + colx[0], ty, c->from, TH_TEXT, colx[1] - colx[0] - 2);
        insp_text(cv, b.x + colx[1], ty, c->to, TH_TEXT, colx[2] - colx[1] - 2);
        insp_text(cv, b.x + colx[2], ty, insp_type_name(c->type), TH_TEXT_DIM,
                  colx[3] - colx[2] - 2);

        if (c->compat < 0)
            ide_strlcpy(insp_num, "--", sizeof insp_num);
        else
            insp_fix2(c->compat, insp_num);
        insp_text(cv, b.x + colx[3], ty, insp_num, TH_TEXT_DIM,
                  colx[4] - colx[3] - 2);

        insp_text(cv, b.x + colx[4], ty, insp_cstatus_name(c->status),
                  insp_cstatus_col(c->status), b.w - colx[4] - PAD);
    }

    if (nc == 0)
        insp_text(cv, b.x + PAD, y + 2, "(no connections)", TH_TEXT_FAINT,
                  b.w - 2 * PAD);
}

/* ---- DETAILS (tab 4): POTENTIALS/RISKS then ACTIONS ---- */
static void insp_body_details(Ide* a, Canvas* cv, Rect b) {
    int bot = b.y + b.h;
    int avail = b.w - 2 * PAD;
    int y = b.y - a->inspector_scroll;

    /* ---- POTENTIALS / RISKS ---- */
    if (y + GFX_FH > b.y && y < bot)
        insp_text(cv, b.x + PAD, y, "POTENTIALS / RISKS", TH_TEXT_FAINT, avail);
    y += ROW_H;

    int nr = a->model.nrisks;
    if (nr > M_MAXRISKS) nr = M_MAXRISKS;
    for (int i = 0; i < nr; i++) {
        Risk* rk = &a->model.risks[i];

        if (y + GFX_FH > b.y && y < bot)
            insp_text(cv, b.x + PAD, y, rk->title, TH_ORANGE, avail);
        y += ROW_H;

        if (y + GFX_FH > b.y && y < bot) {
            insp_text(cv, b.x + 2 * PAD, y, "Risk: ", TH_TEXT_DIM, avail);
            insp_text(cv, b.x + 2 * PAD + 6 * GFX_FW, y, rk->risk, TH_TEXT,
                      avail - 6 * GFX_FW);
        }
        y += ROW_H;

        if (y + GFX_FH > b.y && y < bot) {
            int n = 0;
            insp_num[n++] = 'S'; insp_num[n++] = 'c'; insp_num[n++] = 'o';
            insp_num[n++] = 'r'; insp_num[n++] = 'e'; insp_num[n++] = ':';
            insp_num[n++] = ' ';
            n += insp_fix2(rk->score, insp_num + n);
            insp_text(cv, b.x + 2 * PAD, y, insp_num, TH_TEXT_DIM, avail);
        }
        y += ROW_H;
    }
    if (nr == 0) {
        if (y + GFX_FH > b.y && y < bot)
            insp_text(cv, b.x + 2 * PAD, y, "(none)", TH_TEXT_FAINT, avail);
        y += ROW_H;
    }

    /* divider */
    y += 2;
    if (y > b.y && y < bot)
        gfx_hline(cv, b.x + PAD, y, avail, TH_BORDER_LT);
    y += ROW_H - 2;

    /* ---- ACTIONS ---- */
    if (y + GFX_FH > b.y && y < bot)
        insp_text(cv, b.x + PAD, y, "ACTIONS", TH_TEXT_FAINT, avail);
    y += ROW_H;

    int na = a->model.nactions;
    if (na > M_MAXACTIONS) na = M_MAXACTIONS;
    for (int i = 0; i < na; i++) {
        Action* ac = &a->model.actions[i];

        /* title row */
        if (y + GFX_FH > b.y && y < bot)
            insp_text(cv, b.x + PAD, y, ac->title, TH_TEXT,
                      avail - INSP_APPLY_W - PAD);
        y += ROW_H;

        /* dCoherence + [APPLY] button on one row */
        int ry = y;
        if (ry + ROW_H > b.y && ry < bot) {
            int ty = ry + (ROW_H - GFX_FH) / 2;
            int n = 0;
            const char* lead = "dCoherence +";
            while (lead[n]) { insp_num[n] = lead[n]; n++; }
            insp_fix2(ac->dcoherence, insp_num + n);
            insp_text(cv, b.x + 2 * PAD, ty, insp_num, TH_GREEN,
                      avail - INSP_APPLY_W - 2 * PAD);

            Rect btn = insp_apply_rect(b, ry);
            gfx_round(cv, btn.x, btn.y, btn.w, btn.h, 4, TH_BLUE);
            int blx = btn.x + (btn.w - 5 * GFX_FW) / 2;
            int bly = btn.y + (btn.h - GFX_FH) / 2;
            insp_text(cv, blx, bly, "APPLY", TH_BG, btn.w);
        }
        y += ROW_H + 2;
    }
    if (na == 0) {
        if (y + GFX_FH > b.y && y < bot)
            insp_text(cv, b.x + 2 * PAD, y, "(none)", TH_TEXT_FAINT, avail);
    }
}

/* static scratch for the AST text tree (no malloc) */
static char insp_ast_buf[4096];

/* ---- SYNTAX fallback: signature + param list (when no AST) ---- */
static void insp_body_syntax_fallback(Ide* a, Canvas* cv, Rect b, Func* f) {
    int avail = b.w - 2 * PAD;
    int y = b.y - a->inspector_scroll;
    int bot = b.y + b.h;

    /* signature: "<ret> <name>(...)" rendered as a few clipped segments */
    if (y + GFX_FH > b.y && y < bot) {
        int x = b.x + PAD;
        insp_text(cv, x, y, f->ret[0] ? f->ret : "void", TH_CYAN, avail);
        x += (ide_strlen(f->ret[0] ? f->ret : "void") + 1) * GFX_FW;
        insp_text(cv, x, y, f->name, TH_BLUE, b.x + b.w - PAD - x);
    }
    y += ROW_H;

    if (y + GFX_FH > b.y && y < bot)
        insp_text(cv, b.x + PAD, y, "PARAMS", TH_TEXT_FAINT, avail);
    y += ROW_H;

    int npar = f->nparams;
    if (npar > M_MAXPARAMS) npar = M_MAXPARAMS;
    for (int i = 0; i < npar; i++) {
        Param* p = &f->params[i];
        if (y + GFX_FH > b.y && y < bot) {
            int x = b.x + 2 * PAD;
            insp_text(cv, x, y, p->type, TH_CYAN, avail);
            x += (ide_strlen(p->type) + 1) * GFX_FW;
            insp_text(cv, x, y, p->name, TH_TEXT, b.x + b.w - PAD - x);
        }
        y += ROW_H;
    }
    if (npar == 0)
        insp_text(cv, b.x + 2 * PAD, y, "(void)", TH_TEXT_FAINT, avail);
}

/* ---- SYNTAX (tab 0): real AST tree of the focused function ---- */
static void insp_body_syntax(Ide* a, Canvas* cv, Rect b, Func* f) {
    int avail = b.w - 2 * PAD;
    int bot   = b.y + b.h;

    /* find the AST_FUNC_DEF for the focused function */
    AstNode* node = ast_find_func(f->name);
    if (!node) {                       /* parse failed / no AST -> fall back */
        insp_body_syntax_fallback(a, cv, b, f);
        return;
    }

    int y = b.y - a->inspector_scroll;

    /* signature header in cyan */
    if (astprint_signature(node, insp_ast_buf, (int)sizeof insp_ast_buf) > 0) {
        if (y + GFX_FH > b.y && y < bot)
            insp_text(cv, b.x + PAD, y, insp_ast_buf, TH_CYAN, avail);
        y += ROW_H;
        if (y > b.y && y < bot)
            gfx_hline(cv, b.x + PAD, y - 2, avail, TH_BORDER);
    }

    /* render the AST as an indented text tree, one line per node */
    int n = astprint_tree(node, insp_ast_buf, (int)sizeof insp_ast_buf);
    if (n <= 0) {
        if (y + GFX_FH > b.y && y < bot)
            insp_text(cv, b.x + PAD, y, "(empty AST)", TH_TEXT_FAINT, avail);
        return;
    }
    if (n >= (int)sizeof insp_ast_buf) n = (int)sizeof insp_ast_buf - 1;
    insp_ast_buf[n] = 0;

    /* walk the buffer line-by-line (split on '\n'), bounded by buffer length */
    int ls = 0;
    for (int i = 0; i <= n; i++) {
        char ch = insp_ast_buf[i];
        if (ch != '\n' && ch != 0) continue;

        insp_ast_buf[i] = 0;           /* terminate this line in place */
        if (insp_ast_buf[ls] && y + GFX_FH > b.y && y < bot) {
            /* dim the leading-space indent, primary text for the node */
            int j = ls;
            while (insp_ast_buf[j] == ' ') j++;
            uint32_t col = (j > ls) ? TH_TEXT_DIM : TH_TEXT;
            insp_text(cv, b.x + PAD, y, insp_ast_buf + ls, col, avail);
        }
        y += ROW_H;
        if (ch == 0) break;
        ls = i + 1;
        if (y >= bot) {                /* past the bottom -> stop drawing */
            /* still must not read past terminator; loop guard handles it */
            if (i >= n) break;
        }
    }
}

/* ---- CATEGORY (tab 1): counts + coherence ---- */
static void insp_kv(Canvas* cv, Rect b, int y, const char* k, int v,
                    int pct) {
    int avail = b.w - 2 * PAD;
    insp_text(cv, b.x + PAD, y, k, TH_TEXT_DIM, avail - 6 * GFX_FW);
    int n = ide_itoa(v, insp_num);
    if (pct) insp_num[n++] = '%';
    insp_num[n] = 0;
    int x = b.x + b.w - PAD - n * GFX_FW;
    insp_text(cv, x, y, insp_num, TH_TEXT, n * GFX_FW);
}

static void insp_body_category(Ide* a, Canvas* cv, Rect b, Func* f) {
    int y = b.y - a->inspector_scroll;
    int npr = f->nreads  > M_MAXREFS  ? M_MAXREFS  : f->nreads;
    int npw = f->nwrites > M_MAXREFS  ? M_MAXREFS  : f->nwrites;
    int npc = f->ncalls  > M_MAXCALLS ? M_MAXCALLS : f->ncalls;
    int npp = f->nports  > M_MAXPORTS ? M_MAXPORTS : f->nports;

    insp_kv(cv, b, y, "Ports",      npp, 0); y += ROW_H;
    insp_kv(cv, b, y, "Reads",      npr, 0); y += ROW_H;
    insp_kv(cv, b, y, "Writes",     npw, 0); y += ROW_H;
    insp_kv(cv, b, y, "Calls",      npc, 0); y += ROW_H;
    insp_kv(cv, b, y, "Coherence",  a->model.coherence, 1); y += ROW_H;
}

/* Accent color for a library category chip. */
static uint32_t insp_libcat_color(int cat) {
    switch (cat) {
    case LIBCAT_IDIOM: return TH_BLUE;
    case LIBCAT_DATA:  return TH_GREEN;
    case LIBCAT_NET:   return TH_CYAN;
    default:           return TH_TEXT_FAINT;
    }
}

/* LIB tab: the "complex" library, a flat scrollable list. Each row is one
 * complex (chip + trigger + label); clicking it inserts the body at the editor
 * caret (panel_inspector_click). Works without a focused function. The click
 * handler replays this exact layout, so keep the geometry in sync. */
static void insp_body_library(Ide* a, Canvas* cv, Rect b) {
    insp_text(cv, b.x + PAD, b.y + 2, "COMPLEX LIBRARY  (click a row to insert at the caret)",
              TH_TEXT_DIM, b.w - 2 * PAD);
    int n = lib_count();
    int rowh = ROW_H;
    int top = b.y + rowh;                       /* first row baseline */
    int bot = b.y + b.h;
    for (int i = 0; i < n; i++) {
        int ry = top + i * rowh - a->inspector_scroll;
        if (ry + rowh <= b.y + rowh || ry >= bot) continue;   /* cull off-screen */
        const Snippet* s = lib_get(i);
        if (!s) continue;
        gfx_round(cv, b.x + PAD, ry + (rowh - 8) / 2, 8, 8, 2,
                  insp_libcat_color(s->category));
        int tx = b.x + PAD + 12;
        int ty = ry + (rowh - GFX_FH) / 2;
        insp_text(cv, tx, ty, s->trigger, TH_TEXT, b.w - (tx - b.x) - PAD);
        int lx = tx + (ide_strlen(s->trigger) + 1) * GFX_FW;
        if (lx < b.x + b.w - PAD)
            insp_text(cv, lx, ty, s->label, TH_TEXT_DIM, b.x + b.w - PAD - lx);
    }
}

/* ---- MARK (tab 6): per-symbol knob strip ----
 *
 * The aphantasia "intent per object" board: Done / Star / Isolate / Mute, each
 * a SET_TOGGLE bound to a SymMark field (persisted on flip). Every knob has a
 * visible consequence ELSEWHERE (funcs list / watch chips / map LOCKED / WARN
 * (muted)) -- a knob with no visible consequence is forbidden by the brick law.
 */
static const char* const MARK_LBL[4] = {
    "Done", "Star (watch)", "Isolate", "Mute warnings"
};
#define MARK_NROWS 4
static int g_mark_sel = 0;   /* keyboard-selected MARK row (0..MARK_NROWS-1) */

/* The int* field for MARK row `idx` in a SymMark (matches MARK_LBL order). */
static int* mark_field_ptr(SymMark* mk, int idx) {
    if (!mk) return 0;
    switch (idx) {
        case 0: return &mk->done;
        case 1: return &mk->star;
        case 2: return &mk->isolate;
        case 3: return &mk->mute;
        default: return 0;
    }
}

/* Extracted from panel_settings (same pixels): the ON/OFF pill + sliding knob +
 * "ON"/"OFF" label, right-aligned in row body `rowb` at row top `ry` (text
 * baseline `ty`). Refactor only -- panel_settings calls this, no visual change. */
static void insp_draw_toggle(Canvas* cv, Rect rowb, int ry, int ty, int on) {
    int ph = GFX_FH, pw = GFX_FH * 2 + 6;
    int px = rowb.x + rowb.w - PAD - pw;
    int py = ry + (SET_ROW_H - ph) / 2;
    gfx_round(cv, px, py, pw, ph, ph / 2, on ? TH_GREEN : TH_BORDER_LT);
    int kd = ph - 4;
    int kx = on ? (px + pw - kd - 2) : (px + 2);
    gfx_round(cv, kx, py + 2, kd, kd, kd / 2, TH_PANEL);
    const char* s = on ? "ON" : "OFF";
    int sw = ide_strlen(s) * GFX_FW;
    int sx = px - PAD - sw;
    insp_text(cv, sx, ty, s, on ? TH_GREEN : TH_TEXT_FAINT, sw);
}

static void insp_body_marks(Ide* a, Canvas* cv, Rect b, Func* f) {
    SymMark* mk = marks_get(f->name);
    int y = b.y - a->inspector_scroll;
    int bot = b.y + b.h;

    /* header "KNOBS - <fname>" in the section-header style */
    {
        char hdr[M_NAME + 12];
        int p = 0;
        const char* lead = "KNOBS - ";
        while (lead[p]) { hdr[p] = lead[p]; p++; }
        for (int i = 0; f->name[i] && p < (int)sizeof(hdr) - 1; i++) hdr[p++] = f->name[i];
        hdr[p] = 0;
        if (y + GFX_FH > b.y && y < bot)
            insp_text(cv, b.x + PAD, y + (SET_HEAD_H - GFX_FH) / 2, hdr, TH_BLUE,
                      b.w - 2 * PAD);
        if (y + SET_HEAD_H - 3 > b.y && y < bot)
            gfx_hline(cv, b.x + PAD, y + SET_HEAD_H - 3, b.w - 2 * PAD, TH_BORDER);
    }
    y += SET_HEAD_H;

    for (int i = 0; i < MARK_NROWS; i++) {
        int ry = y;
        y += SET_ROW_H;
        if (ry + SET_ROW_H <= b.y || ry >= bot) continue;   /* cull off-screen */
        int ty = ry + (SET_ROW_H - GFX_FH) / 2;

        if (i == g_mark_sel)
            gfx_blend(cv, b.x, ry, b.w, SET_ROW_H,
                      (0x30u << 24) | (TH_SELECT & 0x00FFFFFFu));

        insp_text(cv, b.x + PAD, ty, MARK_LBL[i], TH_TEXT, (b.w * 3) / 5);
        int* vp = mark_field_ptr(mk, i);
        insp_draw_toggle(cv, b, ry, ty, vp ? (*vp != 0) : 0);
    }
}

/* ====================================================================== *
 *  panel_inspector                                                       *
 * ====================================================================== */
void panel_inspector(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    /* panel background */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL);

    Func* f = insp_focus(a);

    /* ---- header: "INSPECTOR - <focusname>" ---- */
    gfx_fill(cv, r.x, r.y, r.w, INSP_HEADER_H, TH_HEADER);
    {
        int hx = r.x + PAD;
        int hy = r.y + (INSP_HEADER_H - GFX_FH) / 2;
        int avail = r.w - 2 * PAD;
        insp_text(cv, hx, hy, "INSPECTOR - ", TH_TEXT_DIM, avail);
        int px = hx + 12 * GFX_FW;                 /* len("INSPECTOR - ")==12 */
        insp_text(cv, px, hy, f ? f->name : "(none)", TH_TEXT,
                  r.x + r.w - PAD - px);
    }

    /* ---- tab strip ---- */
    {
        int strip_y = r.y + INSP_HEADER_H;
        gfx_fill(cv, r.x, strip_y, r.w, INSP_TAB_H, TH_PANEL2);
        for (int i = 0; i < INSP_NTABS; i++) {
            Rect t = insp_tab_rect(r, i);
            int active = (a->insp_tab == i);
            int tw = ide_strlen(INSP_TAB_LBL[i]) * GFX_FW;
            int tx = t.x + (t.w - tw) / 2;
            if (tx < t.x + 2) tx = t.x + 2;
            int ty = t.y + (t.h - GFX_FH) / 2;
            insp_text(cv, tx, ty, INSP_TAB_LBL[i],
                      active ? TH_BLUE : TH_TEXT_DIM, t.w - 2);
            if (active)
                gfx_hline(cv, t.x + 2, t.y + t.h - 2, t.w - 4, TH_BLUE);
        }
        gfx_hline(cv, r.x, strip_y + INSP_TAB_H, r.w, TH_BORDER);
    }

    Rect b = insp_body_rect(r);
    if (b.h <= 0) return;

    /* LIB (complex library) works without a focused function -- render it first. */
    if (a->insp_tab == 5) { insp_body_library(a, cv, b); return; }

    if (!f) {
        insp_text(cv, b.x + PAD, b.y + 2, "No function focused.",
                  TH_TEXT_FAINT, b.w - 2 * PAD);
        return;
    }

    switch (a->insp_tab) {
        case 0: insp_body_syntax(a, cv, b, f);    break;
        case 1: insp_body_category(a, cv, b, f);  break;
        case 2: insp_body_ports(a, cv, b, f);     break;
        case 3: insp_body_conns(a, cv, b);        break;
        case 4: insp_body_details(a, cv, b);      break;
        case 6: insp_body_marks(a, cv, b, f);     break;
        default: insp_body_ports(a, cv, b, f);    break;
    }
}

/* ====================================================================== *
 *  panel_inspector_click                                                 *
 * ====================================================================== */
int panel_inspector_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;

    /* tab strip hit-test */
    for (int i = 0; i < INSP_NTABS; i++) {
        Rect t = insp_tab_rect(r, i);
        if (rect_hit(t, mx, my)) {
            a->insp_tab = i;
            a->inspector_scroll = 0;
            return 1;
        }
    }

    /* MARK: flip a knob (Done/Star/Isolate/Mute) on the focused function and
     * persist -- structurally identical to the Settings toggle branch. Replays
     * insp_body_marks()'s header + SET_ROW_H pitch. */
    if (a->insp_tab == 6) {
        Rect b = insp_body_rect(r);
        Func* f = insp_focus(a);
        if (f && b.h > 0 && rect_hit(b, mx, my)) {
            SymMark* mk = marks_get(f->name);
            int y = b.y - a->inspector_scroll + SET_HEAD_H;
            for (int i = 0; i < MARK_NROWS; i++) {
                int ry = y + i * SET_ROW_H;
                if (my >= ry && my < ry + SET_ROW_H) {
                    int* vp = mark_field_ptr(mk, i);
                    if (vp) { *vp = *vp ? 0 : 1; ide_marks_save(); }
                    g_mark_sel = i;
                    break;
                }
            }
        }
        return 1;
    }

    /* LIB: click a complex row to insert it at the editor caret (live re-parse
     * inside ide_editor_insert_snippet registers it in the map). Replays the
     * insp_body_library() layout. */
    if (a->insp_tab == 5) {
        Rect b = insp_body_rect(r);
        if (b.h > 0 && rect_hit(b, mx, my)) {
            int rowh = ROW_H;
            int top = b.y + rowh - a->inspector_scroll;
            int n = lib_count();
            for (int i = 0; i < n; i++) {
                int ry = top + i * rowh;
                if (my >= ry && my < ry + rowh) {
                    const Snippet* s = lib_get(i);
                    if (s) ide_editor_insert_snippet(a, s->body);
                    return 1;
                }
            }
        }
        return 1;
    }

    /* IDE-SYNC-0 S3: CONNECTIONS rows are clickable -- jump the editor (and
     * THE selection) to the row's other-end function: prefer `to`, fall back
     * to `from`. Replays insp_body_conns() row geometry. */
    if (a->insp_tab == 3) {
        Rect b = insp_body_rect(r);
        if (b.h > 0 && rect_hit(b, mx, my)) {
            int top = b.y + ROW_H - a->inspector_scroll;
            int nc = a->model.nconns;
            if (nc > M_MAXCONNS) nc = M_MAXCONNS;
            for (int i = 0; i < nc; i++) {
                int ry = top + i * ROW_H;
                if (my >= ry && my < ry + ROW_H) {
                    Conn* c = &a->model.conns[i];
                    int tgt = -1;
                    for (int j = 0; j < a->model.nfuncs; j++)
                        if (ide_streq(a->model.funcs[j].name, c->to)) { tgt = j; break; }
                    if (tgt < 0)
                        for (int j = 0; j < a->model.nfuncs; j++)
                            if (ide_streq(a->model.funcs[j].name, c->from)) { tgt = j; break; }
                    if (tgt >= 0) ide_sel_jump(a, tgt, PANE_INSPECTOR);
                    return 1;
                }
            }
        }
        return 1;
    }

    /* IDE-SYNC-0 S3: a PORTS row click lands the editor caret on the focused
     * function's definition (ports belong to the focused function). */
    if (a->insp_tab == 2) {
        Rect b = insp_body_rect(r);
        if (b.h > 0 && rect_hit(b, mx, my) && a->focus_func >= 0) {
            ide_sel_jump(a, a->focus_func, PANE_INSPECTOR);
            return 1;
        }
        return 1;
    }

    /* DETAILS: [APPLY] buttons. Replay the same body layout to find which
     * action row (if any) was clicked. */
    if (a->insp_tab == 4) {
        Rect b = insp_body_rect(r);
        if (b.h > 0 && rect_hit(b, mx, my)) {
            int y = b.y - a->inspector_scroll;

            /* POTENTIALS / RISKS header */
            y += ROW_H;
            int nr = a->model.nrisks;
            if (nr > M_MAXRISKS) nr = M_MAXRISKS;
            if (nr == 0) y += ROW_H;
            else         y += nr * (3 * ROW_H);

            /* divider + ACTIONS header */
            y += 2;
            y += ROW_H - 2;     /* divider row advance */
            y += ROW_H;         /* "ACTIONS" header    */

            int na = a->model.nactions;
            if (na > M_MAXACTIONS) na = M_MAXACTIONS;
            for (int i = 0; i < na; i++) {
                y += ROW_H;          /* title row              */
                int ry = y;          /* dCoherence + button row */
                Rect btn = insp_apply_rect(b, ry);
                if (rect_hit(btn, mx, my)) {
                    gen_apply_action(a, i);
                    return 1;
                }
                y += ROW_H + 2;
            }
        }
    }

    /* inside panel but nothing actionable -- still consume the click */
    return 1;
}

/* ====================================================================== *
 *  SETTINGS panel (VIZ-6 / Ctrl+,) -- live knobs & switches.
 *
 *  A flat, scrollable table of rows bound to the runtime variables in
 *  ide_gfx.c. Toggles flip a 0/1 flag; sliders drag an int within a range.
 *  Every change applies INSTANTLY (the row points at the live var) -- no
 *  APPLY button. Render and the 3-phase click pump replay identical geometry.
 * ====================================================================== */

typedef enum { SET_HEADER, SET_TOGGLE, SET_SLIDER } SetKind;
typedef struct {
    SetKind     kind;
    const char* label;
    int*        var;             /* live variable (NULL for headers)        */
    int         vmin, vmax, step;/* slider range + snap step                */
    void      (*apply)(int v);   /* optional side-effect; if set, it OWNS var */
} SetRow;

/* Font-scale slider: gfx_set_scale recomputes the glyph cell AND owns g_ui_pct. */
static void set_apply_fontscale(int v) { gfx_set_scale(v); }

static const SetRow g_set_rows[] = {
    { SET_HEADER, "APPEARANCE",        0,                0,   0,  0, 0 },
    { SET_SLIDER, "Font scale %",      &g_ui_pct,        50, 250,  5, set_apply_fontscale },
    { SET_TOGGLE, "Line numbers",      &g_line_numbers,   0,   1,  1, 0 },
    { SET_TOGGLE, "Annotation gutter", &g_anno_gutter,    0,   1,  1, 0 },
    { SET_HEADER, "EDITOR",            0,                 0,   0,  0, 0 },
    { SET_SLIDER, "Tab width",         &g_tab_width,      1,   8,  1, 0 },
    { SET_TOGGLE, "Auto-indent",       &g_auto_indent,    0,   1,  1, 0 },
    { SET_SLIDER, "Caret blink (ms)",  &g_blink_ms,     100,1000, 50, 0 },
    { SET_HEADER, "AUTOCOMPLETE",      0,                 0,   0,  0, 0 },
    { SET_TOGGLE, "Enable popup",      &g_autocomplete,   0,   1,  1, 0 },
    { SET_SLIDER, "Min prefix",        &g_ac_minpfx,      1,   5,  1, 0 },
    { SET_SLIDER, "Visible rows",      &g_ac_visible,     1,   AC_MAX_MATCHES, 1, 0 },
    { SET_TOGGLE, "Live re-parse",     &g_live_reparse,   0,   1,  1, 0 },
    { SET_HEADER, "MAP",               0,                 0,   0,  0, 0 },
    { SET_SLIDER, "Pan step (px)",     &g_map_pan_step,   5,  60,  5, 0 },
};
#define SET_NROWS   ((int)(sizeof(g_set_rows) / sizeof(g_set_rows[0])))
/* (SET_ROW_H / SET_HEAD_H / SET_KNOB_W are defined up top -- shared widgets.) */

static int g_set_drag = -1;   /* index of the slider being dragged (-1 = none) */
static int g_set_sel  = 1;    /* keyboard-selected row (skips SET_HEADER rows) */

/* Next non-header row from `from` in direction dir (+1/-1); clamps at the ends. */
static int set_next_row(int from, int dir) {
    int i = from + dir;
    while (i >= 0 && i < SET_NROWS) {
        if (g_set_rows[i].kind != SET_HEADER) return i;
        i += dir;
    }
    return from;
}

/* Body rect = panel minus its header bar. */
static Rect set_body_rect(Rect r) {
    Rect b; b.x = r.x; b.y = r.y + SET_HEAD_H; b.w = r.w; b.h = r.h - SET_HEAD_H;
    if (b.h < 0) b.h = 0;
    return b;
}
/* Right-aligned control region within a row (40% of body width, >= 80px). */
static Rect set_ctrl_rect(Rect b, int ry) {
    Rect c;
    int w = (b.w * 2) / 5;
    if (w < 80) w = 80;
    if (w > b.w - 8) w = b.w - 8;
    c.x = b.x + b.w - PAD - w;
    c.y = ry + (SET_ROW_H - GFX_FH) / 2;
    c.w = w; c.h = GFX_FH;
    return c;
}
/* The slider track within a control region (leaves room for the value text). */
static Rect set_track_rect(Rect c) {
    Rect t = c;
    int vw = 5 * GFX_FW;            /* room for up to "1000" + pad */
    if (vw > c.w) vw = c.w / 2;     /* never let the value gutter exceed the region */
    t.w = c.w - vw;
    if (t.w < 1) t.w = 1;           /* clamp to a sane positive width (never re-expand) */
    return t;
}
static int set_knob_x(Rect track, int val, int vmin, int vmax) {
    int span = track.w - SET_KNOB_W;
    if (span < 0) span = 0;
    if (vmax <= vmin) return track.x;
    if (val < vmin) val = vmin; if (val > vmax) val = vmax;
    return track.x + (val - vmin) * span / (vmax - vmin);
}
static int set_val_from_x(Rect track, int mx, int vmin, int vmax, int step) {
    int span = track.w - SET_KNOB_W;
    if (span <= 0) return vmin;
    int rel = mx - track.x - SET_KNOB_W / 2;
    if (rel < 0) rel = 0;
    if (rel > span) rel = span;
    int v = vmin + (rel * (vmax - vmin) + span / 2) / span;     /* rounded */
    if (step > 1) v = ((v - vmin + step / 2) / step) * step + vmin;
    if (v < vmin) v = vmin; if (v > vmax) v = vmax;
    return v;
}
static void set_apply_value(const SetRow* row, int v) {
    if (v < row->vmin) v = row->vmin;
    if (v > row->vmax) v = row->vmax;
    if (row->apply)      row->apply(v);     /* apply owns the var (e.g. font) */
    else if (row->var)   *row->var = v;
    /* Persisted by the caller on drag-release / toggle-click (not every move). */
}

/* Extracted from panel_settings (same pixels): the slider track + knob + value
 * text, right-aligned in row body `rowb` at row top `ry` (text baseline `ty`).
 * Refactor only -- panel_settings calls this, no visual change. */
static void insp_draw_slider(Canvas* cv, Rect rowb, int ry, int ty,
                             int val, int vmin, int vmax) {
    Rect c = set_ctrl_rect(rowb, ry);
    Rect track = set_track_rect(c);
    int tcy = c.y + GFX_FH / 2;
    gfx_fill(cv, track.x, tcy - 1, track.w, 2, TH_BORDER_LT);
    int kx = set_knob_x(track, val, vmin, vmax);
    gfx_round(cv, kx, c.y, SET_KNOB_W, GFX_FH, 2, TH_BLUE);
    char nb[16]; ide_itoa(val, nb);
    int vx = track.x + track.w + GFX_FW / 2;
    if (vx < c.x + c.w)
        insp_text(cv, vx, ty, nb, TH_TEXT_DIM, c.x + c.w - vx);
}

void panel_settings(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL);

    /* header */
    gfx_fill(cv, r.x, r.y, r.w, SET_HEAD_H, TH_HEADER);
    gfx_hline(cv, r.x, r.y + SET_HEAD_H - 1, r.w, TH_BORDER);
    {
        int hy = r.y + (SET_HEAD_H - GFX_FH) / 2;
        insp_text(cv, r.x + PAD, hy, "SETTINGS", TH_TEXT, r.w - 2 * PAD);
        const char* sub = "knobs & switches (apply live)";
        int sx = r.x + PAD + 9 * GFX_FW;
        if (sx < r.x + r.w - PAD)
            insp_text(cv, sx, hy, sub, TH_TEXT_FAINT, r.x + r.w - PAD - sx);
    }

    Rect b = set_body_rect(r);
    if (b.h <= 0) return;

    /* clamp scroll to content */
    {
        int content_h = SET_NROWS * SET_ROW_H;
        int maxs = content_h - b.h;
        if (maxs < 0) maxs = 0;
        if (a->inspector_scroll > maxs) a->inspector_scroll = maxs;
        if (a->inspector_scroll < 0)    a->inspector_scroll = 0;
    }
    /* keep the keyboard-selected knob row on screen */
    if (g_set_sel >= 0 && g_set_sel < SET_NROWS) {
        int sy = g_set_sel * SET_ROW_H;
        if (sy < a->inspector_scroll) a->inspector_scroll = sy;
        else if (sy + SET_ROW_H > a->inspector_scroll + b.h)
            a->inspector_scroll = sy + SET_ROW_H - b.h;
        if (a->inspector_scroll < 0) a->inspector_scroll = 0;
    }

    for (int i = 0; i < SET_NROWS; i++) {
        int ry = b.y + i * SET_ROW_H - a->inspector_scroll;
        if (ry + SET_ROW_H <= b.y || ry >= b.y + b.h) continue;   /* cull */
        const SetRow* row = &g_set_rows[i];
        int ty = ry + (SET_ROW_H - GFX_FH) / 2;

        /* keyboard-selected row highlight */
        if (i == g_set_sel)
            gfx_blend(cv, b.x, ry, b.w, SET_ROW_H,
                      (0x30u << 24) | (TH_SELECT & 0x00FFFFFFu));

        if (row->kind == SET_HEADER) {
            insp_text(cv, b.x + PAD, ty, row->label, TH_BLUE, b.w - 2 * PAD);
            gfx_hline(cv, b.x + PAD, ry + SET_ROW_H - 3, b.w - 2 * PAD, TH_BORDER);
            continue;
        }

        /* label (left) */
        insp_text(cv, b.x + PAD, ty, row->label, TH_TEXT, (b.w * 3) / 5);

        if (row->kind == SET_TOGGLE) {
            insp_draw_toggle(cv, b, ry, ty, row->var ? (*row->var != 0) : 0);
        } else {  /* SET_SLIDER */
            insp_draw_slider(cv, b, ry, ty, row->var ? *row->var : 0,
                             row->vmin, row->vmax);
        }
    }
}

void panel_settings_click(Ide* a, Rect r, int mx, int my, int phase) {
    Rect b = set_body_rect(r);

    if (phase == 2) {                       /* release: persist a finished drag */
        if (g_set_drag >= 0) ide_config_save();
        g_set_drag = -1;
        return;
    }

    if (phase == 1) {                       /* drag move on a grabbed slider */
        if (g_set_drag >= 0 && g_set_drag < SET_NROWS) {
            const SetRow* row = &g_set_rows[g_set_drag];
            if (row->kind == SET_SLIDER) {
                int ry = b.y + g_set_drag * SET_ROW_H - a->inspector_scroll;
                Rect c = set_ctrl_rect(b, ry);
                Rect track = set_track_rect(c);
                set_apply_value(row, set_val_from_x(track, mx, row->vmin, row->vmax, row->step));
            }
        }
        return;
    }

    /* phase 0: press -- toggle a switch or grab a slider. */
    g_set_drag = -1;
    if (b.h <= 0 || !rect_hit(b, mx, my)) return;
    for (int i = 0; i < SET_NROWS; i++) {
        int ry = b.y + i * SET_ROW_H - a->inspector_scroll;
        if (my < ry || my >= ry + SET_ROW_H) continue;
        const SetRow* row = &g_set_rows[i];
        if (row->kind == SET_TOGGLE) {
            if (row->var) { set_apply_value(row, *row->var ? 0 : 1); ide_config_save(); }
        } else if (row->kind == SET_SLIDER) {
            g_set_drag = i;
            Rect c = set_ctrl_rect(b, ry);
            Rect track = set_track_rect(c);
            set_apply_value(row, set_val_from_x(track, mx, row->vmin, row->vmax, row->step));
            /* persisted on release (phase 2) */
        }
        g_set_sel = i;   /* clicking a row also selects it for the keyboard */
        return;
    }
}

/* Keyboard control of the Settings panel (VIZ-6). Up/Down move the selection
 * between knob rows (skipping section headers); Left/Right adjust a slider by its
 * step or set a toggle off/on; Space/Enter toggle (or nudge a slider up). Returns
 * 1 if the key was consumed, 0 otherwise (so digits/'q' fall through to the
 * normal LEGO shortcuts). */
int panel_settings_key(Ide* a, int keycode) {
    (void)a;
    enum { SK_UP = 103, SK_DOWN = 108, SK_LEFT = 105, SK_RIGHT = 106,
           SK_ENTER = 28, SK_SPACE = 57 };
    if (g_set_sel < 0 || g_set_sel >= SET_NROWS) g_set_sel = 1;
    if (g_set_rows[g_set_sel].kind == SET_HEADER)
        g_set_sel = set_next_row(g_set_sel, 1);
    const SetRow* row = &g_set_rows[g_set_sel];
    int step = (row->step > 0) ? row->step : 1;

    switch (keycode) {
    case SK_UP:   g_set_sel = set_next_row(g_set_sel, -1); return 1;
    case SK_DOWN: g_set_sel = set_next_row(g_set_sel, +1); return 1;
    case SK_LEFT:
        if (row->kind == SET_SLIDER && row->var) { set_apply_value(row, *row->var - step); ide_config_save(); }
        else if (row->kind == SET_TOGGLE && row->var) { set_apply_value(row, 0); ide_config_save(); }
        return 1;
    case SK_RIGHT:
        if (row->kind == SET_SLIDER && row->var) { set_apply_value(row, *row->var + step); ide_config_save(); }
        else if (row->kind == SET_TOGGLE && row->var) { set_apply_value(row, 1); ide_config_save(); }
        return 1;
    case SK_SPACE:
    case SK_ENTER:
        if (row->kind == SET_TOGGLE && row->var) { set_apply_value(row, *row->var ? 0 : 1); ide_config_save(); }
        else if (row->kind == SET_SLIDER && row->var) { set_apply_value(row, *row->var + step); ide_config_save(); }
        return 1;
    }
    return 0;
}

/* Keyboard control of the Inspector panel (VIZ-2). Left/Right/Tab cycle the
 * sub-tabs (a->insp_tab, wrapping 0..INSP_NTABS-1, reset scroll on change);
 * Up/Down scroll the body. Returns 1 if consumed. */
int panel_inspector_key(Ide* a, int keycode) {
    enum { IK_UP = 103, IK_DOWN = 108, IK_LEFT = 105, IK_RIGHT = 106, IK_TAB = 15,
           IK_ENTER = 28, IK_SPACE = 57 };

    /* MARK tab: Up/Down move the knob cursor, Space/Enter flip + persist. Other
     * keys (Tab/Left/Right) fall through to the tab-cycling switch below. */
    if (a->insp_tab == 6) {
        switch (keycode) {
        case IK_UP:   if (g_mark_sel > 0) g_mark_sel--; return 1;
        case IK_DOWN: if (g_mark_sel < MARK_NROWS - 1) g_mark_sel++; return 1;
        case IK_SPACE:
        case IK_ENTER: {
            Func* f = insp_focus(a);
            if (f) {
                SymMark* mk = marks_get(f->name);
                int* vp = mark_field_ptr(mk, g_mark_sel);
                if (vp) { *vp = *vp ? 0 : 1; ide_marks_save(); }
            }
            return 1;
        }
        }
    }

    switch (keycode) {
    case IK_TAB:
    case IK_RIGHT:
        a->insp_tab = (a->insp_tab + 1) % INSP_NTABS;
        a->inspector_scroll = 0;
        return 1;
    case IK_LEFT:
        a->insp_tab = (a->insp_tab + INSP_NTABS - 1) % INSP_NTABS;
        a->inspector_scroll = 0;
        return 1;
    case IK_UP:
        if (a->inspector_scroll > ROW_H) a->inspector_scroll -= ROW_H;
        else a->inspector_scroll = 0;
        return 1;
    case IK_DOWN:
        a->inspector_scroll += ROW_H;
        return 1;
    }
    return 0;
}

/* ====================================================================== *
 *  IDE-FORGE-0: the ACTIONS deck (VIZ-4) + the Project Pulse (VIZ-5).
 *
 *  The deck is the project's one-key task list that REMEMBERS what ran, when,
 *  and the result -- so an aphantasic user can leave, come back, and the screen
 *  still says "build passed 12s ago." Each row reuses the Settings widget
 *  vocabulary (label-left / [APPLY]-style button-right). The Pulse (set by the
 *  deck's "Open TODO list") shows the !done checklist (see panel_pulse below).
 * ====================================================================== */

/* set by the deck's "Open TODO list" row; read by panel_pulse (same file). */
static int g_pulse_todo_filter = 0;

/* short message the most-recent action left behind, copied into the row chip. */
static char g_act_msg[48];

static void act_puts(char* d, int* p, const char* s) {
    for (int j = 0; s[j] && *p < 47; j++) d[(*p)++] = s[j];
    d[*p] = 0;
}
static void act_puti(char* d, int* p, int v) {
    char nb[16]; int n = ide_itoa(v, nb);
    for (int j = 0; j < n && *p < 47; j++) d[(*p)++] = nb[j];
    d[*p] = 0;
}

/* --- the six actions, each backed by an existing call (spec 4.2) --- */
static int act_build(Ide* a) {
    ide_do_build(a);
    int ok = ide_build_ok();
    int p = 0;
    act_puts(g_act_msg, &p, ok ? "OK " : "FAIL ");
    act_puti(g_act_msg, &p, ide_build_time_ms());
    act_puts(g_act_msg, &p, "ms");
    return ok;
}
static int act_run(Ide* a) {
    ide_do_run(a);
    int p = 0; g_act_msg[0] = 0;
    act_puts(g_act_msg, &p, ide_run_msg());
    return ide_build_ok();          /* "launchable"; the chip carries the text */
}
static int act_gen_all(Ide* a) {
    int n = 0;
    /* each apply reparses+reanalyzes so index 0 is always the next missing card;
     * capped at 16 iterations (bounded-everything law). */
    while (a->model.nactions > 0 && n < 16) {
        if (!gen_apply_action(a, 0)) break;
        n++;
    }
    if (n > 0) {                     /* one whole-dir re-parse to refresh the map */
        ide_parse_project_model(a);
        ide_set_focus(a, a->focus_func);
    }
    int p = 0; act_puti(g_act_msg, &p, n); act_puts(g_act_msg, &p, " generated");
    return n > 0;
}
static int act_save_all(Ide* a) {
    ide_editor_save(a);              /* flush the active buffer to disk */
    int p = 0; act_puts(g_act_msg, &p, "saved");
    return 1;
}
static int act_reanalyze(Ide* a) {
    ide_parse_project_model(a);
    ide_set_focus(a, a->focus_func);
    int p = 0; act_puts(g_act_msg, &p, "re-analyzed");
    return 1;
}
static int act_open_todo(Ide* a) {
    a->viz = VIZ_POTENTIALS;         /* jump to the Pulse */
    g_pulse_todo_filter = 1;         /* and list the !done functions there */
    int p = 0; act_puts(g_act_msg, &p, "opened TODO");
    return 1;
}

typedef struct {
    const char* label;
    char        key;            /* one-key shortcut shown + handled */
    int       (*run)(Ide*);     /* returns 1=ok, 0=fail */
    int         last_status;    /* -1 never run, 0 fail, 1 ok */
    long        last_ms;        /* ide_ticks_ms() at completion (for "Ns ago") */
    char        last_msg[48];   /* short result text */
} ActRow;
static ActRow g_act_rows[] = {
    { "Build project",      'B', act_build,     -1, 0, "" },
    { "Run",                'R', act_run,       -1, 0, "" },
    { "Generate all stubs", 'G', act_gen_all,   -1, 0, "" },
    { "Save all",           'S', act_save_all,  -1, 0, "" },
    { "Re-analyze project", 'A', act_reanalyze, -1, 0, "" },
    { "Open TODO list",     'T', act_open_todo, -1, 0, "" },
};
#define ACT_NROWS ((int)(sizeof(g_act_rows) / sizeof(g_act_rows[0])))
static int g_act_sel = 0;

/* AT set-1 keycode for the row's letter (matches ide.c's KEY_* defines). */
static int act_keycode(char k) {
    switch (k) {
        case 'B': return 48;  case 'R': return 19;  case 'G': return 34;
        case 'S': return 31;  case 'A': return 30;  case 'T': return 20;
        default:  return -1;
    }
}

/* Run row i and stamp its result (status + timestamp + message). */
static void act_run_row(Ide* a, int i) {
    if (i < 0 || i >= ACT_NROWS) return;
    g_act_msg[0] = 0;
    int ok = g_act_rows[i].run(a);
    g_act_rows[i].last_status = ok ? 1 : 0;
    g_act_rows[i].last_ms     = ide_ticks_ms();
    int j = 0;
    for (; g_act_msg[j] && j < (int)sizeof(g_act_rows[i].last_msg) - 1; j++)
        g_act_rows[i].last_msg[j] = g_act_msg[j];
    g_act_rows[i].last_msg[j] = 0;
}

/* deck body rect = panel minus its header bar. */
static Rect act_body_rect(Rect r) {
    Rect b; b.x = r.x; b.y = r.y + SET_HEAD_H; b.w = r.w; b.h = r.h - SET_HEAD_H;
    if (b.h < 0) b.h = 0;
    return b;
}

void panel_actions(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL);

    /* header in the Settings header style */
    gfx_fill(cv, r.x, r.y, r.w, SET_HEAD_H, TH_HEADER);
    gfx_hline(cv, r.x, r.y + SET_HEAD_H - 1, r.w, TH_BORDER);
    insp_text(cv, r.x + PAD, r.y + (SET_HEAD_H - GFX_FH) / 2,
              "ACTIONS - automation deck", TH_TEXT, r.w - 2 * PAD);

    Rect b = act_body_rect(r);
    if (b.h <= 0) return;
    long now = ide_ticks_ms();

    for (int i = 0; i < ACT_NROWS; i++) {
        int ry = b.y + i * SET_ROW_H;
        if (ry + SET_ROW_H <= b.y || ry >= b.y + b.h) continue;
        int ty = ry + (SET_ROW_H - GFX_FH) / 2;
        ActRow* row = &g_act_rows[i];

        if (i == g_act_sel)
            gfx_blend(cv, b.x, ry, b.w, SET_ROW_H,
                      (0x30u << 24) | (TH_SELECT & 0x00FFFFFFu));

        /* label (left) */
        insp_text(cv, b.x + PAD, ty, row->label, TH_TEXT, (b.w * 2) / 5);

        /* [key] hint just after the label */
        int kx = b.x + PAD + (ide_strlen(row->label) + 1) * GFX_FW;
        char kb[4]; kb[0] = '['; kb[1] = row->key; kb[2] = ']'; kb[3] = 0;
        insp_text(cv, kx, ty, kb, TH_BLUE, 3 * GFX_FW);

        /* run button (right) = the [APPLY] button draw, labelled "RUN" */
        Rect btn = insp_apply_rect(b, ry);
        gfx_round(cv, btn.x, btn.y, btn.w, btn.h, 4, TH_BLUE);
        insp_text(cv, btn.x + (btn.w - 3 * GFX_FW) / 2,
                  btn.y + (btn.h - GFX_FH) / 2, "RUN", TH_BG, btn.w);

        /* result chip (between label and button): status, message, "Ns ago" */
        int chipx = kx + 4 * GFX_FW;
        const char* st = row->last_status < 0 ? "-"
                       : (row->last_status ? "OK" : "FAIL");
        uint32_t sc = row->last_status < 0 ? TH_TEXT_FAINT
                    : (row->last_status ? TH_GREEN : TH_RED);
        insp_text(cv, chipx, ty, st, sc, 5 * GFX_FW);
        chipx += (ide_strlen(st) + 1) * GFX_FW;

        if (row->last_status >= 0) {
            if (row->last_msg[0] && chipx < btn.x - PAD) {
                insp_text(cv, chipx, ty, row->last_msg, TH_TEXT_DIM,
                          btn.x - PAD - chipx);
                chipx += (ide_strlen(row->last_msg) + 1) * GFX_FW;
            }
            long age = now - row->last_ms;
            if (age < 0) age = 0;
            char ab[24]; int p = 0;
            act_puti(ab, &p, (int)(age / 1000));
            act_puts(ab, &p, "s ago");
            int ax = btn.x - PAD - ide_strlen(ab) * GFX_FW;
            if (ax > chipx)
                insp_text(cv, ax, ty, ab, TH_TEXT_FAINT, btn.x - PAD - ax);
        }
    }
}

void panel_actions_click(Ide* a, Rect r, int mx, int my) {
    Rect b = act_body_rect(r);
    if (b.h <= 0 || !rect_hit(b, mx, my)) return;
    for (int i = 0; i < ACT_NROWS; i++) {
        int ry = b.y + i * SET_ROW_H;
        if (my < ry || my >= ry + SET_ROW_H) continue;
        g_act_sel = i;                              /* select on any row click */
        Rect btn = insp_apply_rect(b, ry);
        if (rect_hit(btn, mx, my)) act_run_row(a, i);   /* run on the button   */
        return;
    }
}

int panel_actions_key(Ide* a, int keycode) {
    enum { AK_UP = 103, AK_DOWN = 108, AK_ENTER = 28 };
    /* a row's mnemonic (B/R/G/S/A/T) runs that row directly */
    for (int i = 0; i < ACT_NROWS; i++) {
        if (act_keycode(g_act_rows[i].key) == keycode) {
            g_act_sel = i;
            act_run_row(a, i);
            return 1;
        }
    }
    switch (keycode) {
    case AK_UP:    if (g_act_sel > 0) g_act_sel--; return 1;
    case AK_DOWN:  if (g_act_sel < ACT_NROWS - 1) g_act_sel++; return 1;
    case AK_ENTER: act_run_row(a, g_act_sel); return 1;
    }
    return 0;
}

/* ====================================================================== *
 *  IDE-FORGE-0: the Project Pulse (VIZ-5) -- "how done am I?"
 *
 *  Numbers and words, not shapes: total / done / to-do / missing / warnings /
 *  COH, plus a REAL coherence-history sparkline and a build/run status line.
 *  When opened via the deck's "Open TODO list", it also lists the !done
 *  functions as clickable rows. Reuses insp_kv + rt_ring + the marks store.
 * ====================================================================== */

typedef struct { int y, h, fi; } PulseTodo;
static PulseTodo g_pulse_todo[M_MAXFUNCS];
static int       g_pulse_ntodo;

void panel_pulse(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL);

    /* header "PROJECT PULSE - <project > file>" */
    gfx_fill(cv, r.x, r.y, r.w, SET_HEAD_H, TH_HEADER);
    gfx_hline(cv, r.x, r.y + SET_HEAD_H - 1, r.w, TH_BORDER);
    {
        char bc[IDE_PATH + 40];
        int p = 0;
        const char* lead = "PROJECT PULSE - ";
        while (lead[p]) { bc[p] = lead[p]; p++; }
        ide_breadcrumb_prefix(a, bc + p, (int)sizeof(bc) - p);
        insp_text(cv, r.x + PAD, r.y + (SET_HEAD_H - GFX_FH) / 2, bc, TH_TEXT,
                  r.w - 2 * PAD);
    }

    Rect b = act_body_rect(r);
    if (b.h <= 0) return;

    Model* m = &a->model;
    int total = m->nfuncs; if (total < 0) total = 0;
    int done  = marks_count_done(m);
    int todo  = total - done; if (todo < 0) todo = 0;

    /* Missing cards = functions with ANY absent port (project-wide: ports are
     * built for every function, ide_semantic.c). */
    int missing = 0;
    for (int i = 0; i < m->nfuncs && i < M_MAXFUNCS; i++) {
        Func* f = &m->funcs[i];
        for (int k = 0; k < f->nports && k < M_MAXPORTS; k++)
            if (f->ports[k].status == PS_ABSENT) { missing++; break; }
    }

    /* Warnings are FOCUS-scoped today (honesty note); muting the focused fn
     * zeroes its warned count. Labelled "(focused fn)" -- project-wide = FUTURE. */
    int warned = m->nrisks; if (warned < 0) warned = 0;
    if (a->focus_func >= 0 && a->focus_func < m->nfuncs) {
        SymMark* fmk = marks_find(m->funcs[a->focus_func].name);
        if (fmk && fmk->mute) warned = 0;
    }

    int coh = m->coherence; if (coh < 0) coh = 0; if (coh > 100) coh = 100;
    uint32_t band; const char* bw;
    if (coh >= 80)      { band = TH_GREEN;  bw = "High";   }
    else if (coh >= 50) { band = TH_YELLOW; bw = "Medium"; }
    else                { band = TH_RED;    bw = "Low";    }

    /* scoreboard (left column, narrowed so insp_kv right-aligns sanely) */
    Rect sb = b; if (sb.w > 360) sb.w = 360;
    int y = b.y + PAD;
    insp_kv(cv, sb, y, "Functions",            total,   0); y += ROW_H;
    insp_kv(cv, sb, y, "Done",                 done,    0); y += ROW_H;
    insp_kv(cv, sb, y, "To-do",                todo,    0); y += ROW_H;
    insp_kv(cv, sb, y, "Missing cards",        missing, 0); y += ROW_H;
    insp_kv(cv, sb, y, "Warnings (focused fn)",warned,  0); y += ROW_H;
    insp_kv(cv, sb, y, "Coherence",            coh,     1); y += ROW_H;

    /* COH ring gauge to the right of the scoreboard */
    {
        int rad = 36;
        int rcx = b.x + sb.w + PAD + rad;
        int rcy = b.y + PAD + rad;
        if (rcx + rad < b.x + b.w) {
            rt_ring(cv, rcx, rcy, rad, rad / 3, coh, band, TH_BORDER);
            char pc[8]; int n = ide_itoa(coh, pc);
            if (n < (int)sizeof(pc) - 1) { pc[n++] = '%'; pc[n] = 0; }
            insp_text(cv, rcx - (n * GFX_FW) / 2, rcy - GFX_FH / 2, pc, band,
                      6 * GFX_FW);
            int lw = ide_strlen(bw) * GFX_FW;
            insp_text(cv, rcx - lw / 2, rcy + rad + 4, bw, TH_TEXT_DIM, lw + GFX_FW);
        }
    }

    /* build / run status line */
    int sy = y + PAD;
    {
        const char* bs; uint32_t bsc;
        if (ide_build_ok())          { bs = "BUILD OK";   bsc = TH_GREEN; }
        else if (ide_build_active()) { bs = "BUILD FAIL"; bsc = TH_RED; }
        else                         { bs = "BUILD -";    bsc = TH_TEXT_FAINT; }
        insp_text(cv, b.x + PAD, sy, bs, bsc, sb.w);
        int bx = b.x + PAD + (ide_strlen(bs) + 1) * GFX_FW;
        int nd = ide_build_diag_count();
        if (nd > 0) {
            char db[24]; int p = 0;
            db[p++] = '(';
            { char nb[12]; int nn = ide_itoa(nd, nb);
              for (int j = 0; j < nn && p < 20; j++) db[p++] = nb[j]; }
            const char* tail = " diag)";
            for (int j = 0; tail[j] && p < 23; j++) db[p++] = tail[j];
            db[p] = 0;
            insp_text(cv, bx, sy, db, TH_TEXT_DIM, sb.w);
        }
        sy += ROW_H;
        const char* rm = ide_run_msg();
        if (rm && rm[0]) {
            insp_text(cv, b.x + PAD, sy, rm, TH_CYAN, b.w - 2 * PAD);
            sy += ROW_H;
        }
    }

    /* real COH trend sparkline */
    {
        insp_text(cv, b.x + PAD, sy, "Trend (last 10)", TH_TEXT_FAINT, sb.w);
        sy += GFX_FH + 2;
        int base = sy + 16;
        for (int i = 0; i < 10; i++) {
            int hv = ide_coh_hist(i);
            int bh = (hv < 0) ? 2 : (hv * 16) / 100;
            if (bh < 2) bh = 2;
            int bxx = b.x + PAD + i * 6;
            uint32_t bcc = (i == 9) ? band : TH_TEXT_FAINT;
            gfx_fill(cv, bxx, base - bh, 4, bh, bcc);
        }
        sy = base + PAD;
    }

    /* TODO list (only when the deck's "Open TODO list" set the filter) */
    g_pulse_ntodo = 0;
    if (g_pulse_todo_filter) {
        if (sy + ROW_H <= b.y + b.h)
            insp_text(cv, b.x + PAD, sy, "TODO (click a function to open):",
                      TH_TEXT_FAINT, b.w - 2 * PAD);
        sy += ROW_H;
        for (int i = 0; i < m->nfuncs && i < M_MAXFUNCS; i++) {
            SymMark* mk = marks_find(m->funcs[i].name);
            if (mk && mk->done) continue;            /* only !done functions */
            if (sy + ROW_H > b.y + b.h) break;
            insp_text(cv, b.x + 2 * PAD, sy, m->funcs[i].name, TH_TEXT,
                      b.w - 3 * PAD);
            if (g_pulse_ntodo < M_MAXFUNCS) {
                g_pulse_todo[g_pulse_ntodo].y  = sy;
                g_pulse_todo[g_pulse_ntodo].h  = ROW_H;
                g_pulse_todo[g_pulse_ntodo].fi = i;
                g_pulse_ntodo++;
            }
            sy += ROW_H;
        }
    }
}

int panel_pulse_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;
    for (int i = 0; i < g_pulse_ntodo && i < M_MAXFUNCS; i++) {
        if (my >= g_pulse_todo[i].y && my < g_pulse_todo[i].y + g_pulse_todo[i].h) {
            ide_sel_jump(a, g_pulse_todo[i].fi, PANE_INSPECTOR);
            return 1;
        }
    }
    return 1;   /* consume clicks in the pulse */
}
