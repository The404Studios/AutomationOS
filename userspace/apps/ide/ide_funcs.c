/*
 * ide_funcs.c -- the FUNCTIONS list panel (lower-left).
 *
 * Renders every non-closed function from a->model.funcs as a row with a small
 * glyph + name (focused row highlighted), then an "IDA STYLE" subsection that
 * lists closed (collapsed) functions as compact rounded chips. Clicking an open
 * row focuses it; clicking a closed chip re-opens it and focuses it.
 *
 * Freestanding C: no libc/malloc/stdio. Only STATIC helpers (fn_*). All drawing
 * is clipped to the supplied Rect r.
 */
#include "ide.h"
#include "ide_gfx.h"
#include "ide_theme.h"

/* ---- layout constants local to this panel ---- */
#define FN_HEADER_H   (ROW_H + 2)   /* "FUNCTIONS (file)" header bar          */
#define FN_GLYPH_W    14            /* gutter reserved for the function glyph */
#define FN_CHIP_H     (ROW_H - 4)   /* compact chip height                    */
#define FN_CHIP_GAPX  PAD           /* horizontal gap between chips           */
#define FN_CHIP_GAPY  4             /* vertical gap between chip rows          */
#define FN_CHIP_PADX  PAD           /* inner left/right text padding per chip */
#define FN_CLOSE_W    (GFX_FW + 4)  /* gutter for the per-row close "x"       */

/* basename of a path -- returns pointer just past the last '/' or '\\'. */
static const char* fn_basename(const char* p) {
    const char* b = p;
    for (const char* s = p; *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

/* number of glyph cells that fit in `wpx` pixels (>= 0). */
static int fn_fit_chars(int wpx) {
    if (wpx <= 0) return 0;
    return wpx / GFX_FW;
}

/* draw `s` clipped to a max pixel width inside [x, x+maxw). */
static void fn_text_clipped(Canvas* cv, int x, int y, const char* s,
                            uint32_t col, int maxw) {
    int cap = fn_fit_chars(maxw);
    if (cap <= 0) return;
    gfx_text_clip(cv, x, y, s, col, x, maxw);
    (void)cap;
}

/* visible byte width of `s` capped to `maxw` pixels (for chip sizing). */
static int fn_chip_text_w(const char* s, int maxw) {
    int n = 0, cap = fn_fit_chars(maxw);
    while (s[n] && n < cap) n++;
    return n * GFX_FW;
}

/* render the small "{}" function glyph centred in the gutter at row top ry. */
static void fn_draw_glyph(Canvas* cv, int gx, int ry) {
    int cy = ry + (ROW_H - GFX_FH) / 2;
    gfx_text(cv, gx, cy, "{}", TH_BLUE);
}

/* append literal `s` into out[] at *pos (bounded by cap-1); NUL-terminates. */
static void fn_puts(char* out, int* pos, int cap, const char* s) {
    while (*s && *pos < cap - 1) out[(*pos)++] = *s++;
    out[*pos] = 0;
}

/* append decimal `v` into out[] at *pos (bounded). NUL-terminates. */
static void fn_puti(char* out, int* pos, int cap, int v) {
    char tmp[12];
    int n = ide_itoa(v, tmp);
    for (int i = 0; i < n && *pos < cap - 1; i++) out[(*pos)++] = tmp[i];
    out[*pos] = 0;
}

/* Build a compact connectivity badge for `f` into out[cap].
 * If the function has read/write state, show "r<R> w<W>"; otherwise the
 * port count "<P>p" (a useful single number for pure/leaf functions). */
static void fn_make_badge(const Func* f, char* out, int cap) {
    int p = 0;
    out[0] = 0;
    if (f->nreads || f->nwrites) {
        fn_puts(out, &p, cap, "r");
        fn_puti(out, &p, cap, f->nreads);
        fn_puts(out, &p, cap, " w");
        fn_puti(out, &p, cap, f->nwrites);
    } else {
        fn_puti(out, &p, cap, f->nports);
        fn_puts(out, &p, cap, "p");
    }
}

void panel_funcs(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    /* panel background */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL);

    /* header bar: "FUNCTIONS (<file>)" */
    gfx_fill(cv, r.x, r.y, r.w, FN_HEADER_H, TH_HEADER);
    {
        const char* base = fn_basename(a->model.cur_file[0]
                                       ? a->model.cur_file : a->cur_file);
        int hx = r.x + PAD;
        int hy = r.y + (FN_HEADER_H - GFX_FH) / 2;
        int avail = r.w - 2 * PAD;
        /* "FUNCTIONS (" + base + ")" drawn as two clipped segments */
        gfx_text_clip(cv, hx, hy, "FUNCTIONS (", TH_TEXT_DIM, hx, avail);
        int px = hx + (int)(11 * GFX_FW);            /* len("FUNCTIONS (")==11 */
        int rem = r.x + r.w - PAD - px;
        if (rem > GFX_FW) {
            gfx_text_clip(cv, px, hy, base, TH_TEXT_DIM, px, rem - GFX_FW);
            /* trailing ')' just to the right of the (clipped) name */
            int nlen = ide_strlen(base);
            int nmax = fn_fit_chars(rem - GFX_FW);
            if (nlen > nmax) nlen = nmax;
            int cx = px + nlen * GFX_FW;
            gfx_text_clip(cv, cx, hy, ")", TH_TEXT_DIM, cx, GFX_FW);
        }
    }
    gfx_hline(cv, r.x, r.y + FN_HEADER_H, r.w, TH_BORDER);

    /* content region below header, scrolled by a->funcs_scroll */
    int cx0  = r.x;
    int top  = r.y + FN_HEADER_H + 1;
    int bot  = r.y + r.h;
    int y    = top - a->funcs_scroll;         /* current draw cursor (may be <top) */

    int nf = a->model.nfuncs;
    if (nf > M_MAXFUNCS) nf = M_MAXFUNCS;

    /* ---- open (non-closed) functions ---- */
    for (int i = 0; i < nf; i++) {
        Func* f = &a->model.funcs[i];
        if (!ide_streq(f->file, a->model.cur_file)) continue;  /* IDE-XFILE-0 */
        if (f->closed) continue;

        int ry = y;
        y += ROW_H;
        if (ry + ROW_H <= top || ry >= bot) continue;   /* fully clipped */

        int focused = (i == a->focus_func);
        if (focused)
            gfx_fill(cv, cx0, ry, r.w, ROW_H, TH_SELECT);

        fn_draw_glyph(cv, cx0 + PAD, ry);

        int tx = cx0 + PAD + FN_GLYPH_W;
        int ty = ry + (ROW_H - GFX_FH) / 2;

        /* right edge: reserve a close "x" affordance, then a dim count badge */
        int rowright = cx0 + r.w - PAD;
        int avail    = rowright - tx;            /* px available for name+extras */

        /* close "x" glyph at far right (only when there's comfortable room) */
        int has_close = (avail >= FN_CLOSE_W + 4 * GFX_FW);
        if (has_close) {
            int xx = rowright - FN_CLOSE_W + 2;
            gfx_text(cv, xx, ty, "x", focused ? TH_TEXT_DIM : TH_TEXT_FAINT);
            rowright -= FN_CLOSE_W;
        }

        /* connectivity badge ("r<R> w<W>" or "<P>p") right-aligned, dim */
        char badge[16];
        fn_make_badge(f, badge, (int)sizeof badge);
        int bw   = ide_strlen(badge) * GFX_FW;
        int room = rowright - tx;
        if (bw > 0 && room >= bw + 3 * GFX_FW) {  /* keep some name visible */
            int bx = rowright - bw;
            gfx_text_clip(cv, bx, ty, badge, TH_TEXT_FAINT, bx, bw);
            rowright = bx - GFX_FW / 2;           /* small gap before badge   */
        }

        int namew = rowright - tx;                /* px left for the name     */
        if (namew < 0) namew = 0;
        fn_text_clipped(cv, tx, ty, f->name,
                        focused ? TH_BLUE : TH_TEXT, namew);
    }

    /* ---- divider + "CLOSED FUNCTIONS (IDA STYLE)" subsection ---- */
    if (y + 1 < bot && y + 1 >= top) {
        y += FN_CHIP_GAPY;
        gfx_hline(cv, cx0 + PAD, y, r.w - 2 * PAD, TH_BORDER_LT);
        y += FN_CHIP_GAPY;
    } else {
        y += 2 * FN_CHIP_GAPY;
    }

    {
        int hy = y;
        if (hy + GFX_FH > top && hy < bot)
            gfx_text_clip(cv, cx0 + PAD, hy, "CLOSED FUNCTIONS (IDA STYLE)",
                          TH_TEXT_FAINT, cx0 + PAD, r.w - 2 * PAD);
        y += GFX_FH + FN_CHIP_GAPY;
    }

    /* ---- closed functions as compact rounded chips (flow-wrapped) ---- */
    int chip_x = cx0 + PAD;
    int chip_y = y;
    int right  = cx0 + r.w - PAD;

    /* each chip is:  [ + name ]  -- a "+" reopen affordance then the name. */
    int plus_w = 2 * GFX_FW;   /* "+ " prefix width                          */

    for (int i = 0; i < nf; i++) {
        Func* f = &a->model.funcs[i];
        if (!ide_streq(f->file, a->model.cur_file)) continue;  /* IDE-XFILE-0 */
        if (!f->closed) continue;

        int inner = r.w - 2 * PAD - 2 * FN_CHIP_PADX - plus_w;
        int tw  = fn_chip_text_w(f->name, inner);
        int cw  = tw + plus_w + 2 * FN_CHIP_PADX;
        if (cw > r.w - 2 * PAD) cw = r.w - 2 * PAD;

        /* wrap to next line if this chip would overflow the panel width */
        if (chip_x + cw > right && chip_x > cx0 + PAD) {
            chip_x  = cx0 + PAD;
            chip_y += FN_CHIP_H + FN_CHIP_GAPY;
        }

        if (chip_y + FN_CHIP_H > top && chip_y < bot) {
            gfx_round(cv, chip_x, chip_y, cw, FN_CHIP_H, 4, TH_PANEL2);
            int tx = chip_x + FN_CHIP_PADX;
            int ty = chip_y + (FN_CHIP_H - GFX_FH) / 2;
            gfx_text(cv, tx, ty, "+", TH_BLUE);        /* reopen affordance   */
            int nx = tx + plus_w;
            gfx_text_clip(cv, nx, ty, f->name, TH_TEXT_DIM,
                          nx, cw - 2 * FN_CHIP_PADX - plus_w);
        }

        chip_x += cw + FN_CHIP_GAPX;
    }
}

/* Hit test geometry mirrors panel_funcs() layout exactly. */
int panel_funcs_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;

    int cx0 = r.x;
    int top = r.y + FN_HEADER_H + 1;
    int right = cx0 + r.w - PAD;
    int y = top - a->funcs_scroll;

    int nf = a->model.nfuncs;
    if (nf > M_MAXFUNCS) nf = M_MAXFUNCS;

    /* clicks inside the header bar are consumed but do nothing */
    if (my < top) return 1;

    /* ---- open function rows ---- */
    for (int i = 0; i < nf; i++) {
        Func* f = &a->model.funcs[i];
        if (!ide_streq(f->file, a->model.cur_file)) continue;  /* IDE-XFILE-0 */
        if (f->closed) continue;

        int ry = y;
        y += ROW_H;
        if (my >= ry && my < ry + ROW_H && mx >= cx0 && mx < cx0 + r.w) {
            int tx       = cx0 + PAD + FN_GLYPH_W;
            int rowright = cx0 + r.w - PAD;
            int avail    = rowright - tx;
            int has_close = (avail >= FN_CLOSE_W + 4 * GFX_FW);
            /* far-right "x" closes (collapses) the function instead of focus */
            if (has_close && mx >= rowright - FN_CLOSE_W) {
                a->model.funcs[i].closed = 1;
            } else {
                ide_set_focus(a, i);
            }
            return 1;
        }
    }

    /* skip divider + subsection header (matches render advance) */
    y += FN_CHIP_GAPY;            /* gap before divider                  */
    y += FN_CHIP_GAPY;            /* gap after divider                   */
    y += GFX_FH + FN_CHIP_GAPY;   /* subsection header line              */

    /* ---- closed chips (same flow-wrap as render) ---- */
    int chip_x = cx0 + PAD;
    int chip_y = y;
    int plus_w = 2 * GFX_FW;       /* must match panel_funcs() chip layout    */

    for (int i = 0; i < nf; i++) {
        Func* f = &a->model.funcs[i];
        if (!ide_streq(f->file, a->model.cur_file)) continue;  /* IDE-XFILE-0 */
        if (!f->closed) continue;

        int inner = r.w - 2 * PAD - 2 * FN_CHIP_PADX - plus_w;
        int tw = fn_chip_text_w(f->name, inner);
        int cw = tw + plus_w + 2 * FN_CHIP_PADX;
        if (cw > r.w - 2 * PAD) cw = r.w - 2 * PAD;

        if (chip_x + cw > right && chip_x > cx0 + PAD) {
            chip_x  = cx0 + PAD;
            chip_y += FN_CHIP_H + FN_CHIP_GAPY;
        }

        if (mx >= chip_x && mx < chip_x + cw &&
            my >= chip_y && my < chip_y + FN_CHIP_H) {
            a->model.funcs[i].closed = 0;
            ide_set_focus(a, i);
            return 1;
        }

        chip_x += cw + FN_CHIP_GAPX;
    }

    /* inside panel but no row/chip hit -- still consume the click */
    return 1;
}
