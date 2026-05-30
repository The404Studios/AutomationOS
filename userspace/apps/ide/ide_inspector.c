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

/* ---- layout constants local to this panel ---- */
#define INSP_HEADER_H   (ROW_H + 2)   /* "INSPECTOR - <func>" header bar  */
#define INSP_TAB_H      (ROW_H + 2)   /* tab strip height                 */
#define INSP_NTABS      5
#define INSP_DOT_W      10            /* gutter for the coloured port dot */
#define INSP_APPLY_W    56            /* [APPLY] button width             */
#define INSP_APPLY_H    (ROW_H - 2)   /* [APPLY] button height            */

static const char* const INSP_TAB_LBL[INSP_NTABS] = {
    "SYNTAX", "CATEGORY", "PORTS", "CONN", "DETAILS"
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
