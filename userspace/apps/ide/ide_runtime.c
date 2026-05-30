/*
 * ide_runtime.c -- the bottom RUNTIME-FLOW strip (VIZ-3 lower bar).
 *
 * Renders three side-by-side sections inside the supplied Rect r, separated by
 * faint vertical dividers:
 *   left  (~55%): RUNTIME FLOW (<focusfunc>) -- the focused function's flow
 *                 steps drawn as rounded "pill" boxes connected by little
 *                 arrows, wrapping to a 2nd row as needed. Absent gates use a
 *                 dashed TH_RED outline over a dim red fill; connected steps use
 *                 a TH_PANEL fill + TH_BORDER_LT outline. A footer tallies
 *                 "<k> missing gates detected" (orange) or "flow OK" (green).
 *   mid   (~20%): COHERENCE SCORE -- a band-coloured ring/arc gauge with the big
 *                 % in its centre, a Low/Medium/High label, plus a
 *                 "Trend (last 10 runs)" caption and a 10-bar sparkline.
 *   right (~25%): LIVE WARNINGS (N) -- up to 3 risks, each a TH_ORANGE warning
 *                 triangle + clipped title.
 *
 * Freestanding C: no libc/malloc/stdio. Only STATIC helpers (rt_*). All drawing
 * is clipped to the supplied Rect r; loops are bounded and 0-count safe.
 */
#include "ide.h"
#include "ide_gfx.h"
#include "ide_theme.h"

/* ---- layout constants local to this panel ---- */
#define RT_HEAD_H     GFX_FH            /* section title line height          */
#define RT_BOX_H      20               /* flow-step box height                */
#define RT_BOX_MINW   (3 * GFX_FW)     /* min flow-step box width             */
#define RT_BOX_MAXW   (12 * GFX_FW)    /* cap on a single box width           */
#define RT_BOX_PADX   5                /* inner left/right text pad per box   */
#define RT_ARROW_W    12               /* gap reserved for the connector arrow*/
#define RT_ROW_GAPY   6                /* vertical gap between wrapped rows    */
#define RT_SPARK_N    10               /* sparkline bars                      */
#define RT_SPARK_W    4                /* sparkline bar width                 */
#define RT_SPARK_GAP  2                /* gap between sparkline bars          */
#define RT_SPARK_H    16               /* sparkline max bar height            */
#define RT_MAXWARN    3                /* warnings shown                      */
#define RT_ABSENT_FILL 0x33E2574Au     /* dim red fill for absent gates       */

/* number of glyph cells that fit in `wpx` pixels (>= 0). */
static int rt_fit_chars(int wpx) {
    if (wpx <= 0) return 0;
    return wpx / GFX_FW;
}

/* visible byte width of `s` capped to `cap` chars (for box sizing). */
static int rt_text_w(const char* s, int cap) {
    int n = 0;
    if (cap < 0) cap = 0;
    while (s[n] && n < cap) n++;
    return n * GFX_FW;
}

/* clamp helper */
static int rt_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Append literal text to a static buffer at *pi (bounded). */
static void rt_append(char* buf, int cap, int* pi, const char* s) {
    int i = *pi;
    for (; *s && i < cap - 1; s++) buf[i++] = *s;
    buf[i] = 0;
    *pi = i;
}

/* Draw a tiny rightward arrowhead whose tip is at (x, y). */
static void rt_arrowhead(Canvas* cv, int x, int y, uint32_t col) {
    gfx_line(cv, x, y, x - 4, y - 3, col);
    gfx_line(cv, x, y, x - 4, y + 3, col);
    gfx_line(cv, x, y, x - 3, y,     col);   /* small thicken at the tip */
}

/* Draw a small filled warning triangle (apex up) at top-left (x, y), size s,
 * with a dark exclamation mark for contrast. */
static void rt_warn_tri(Canvas* cv, int x, int y, int s, uint32_t col) {
    int denom = (s - 1 > 0) ? s - 1 : 1;
    for (int row = 0; row < s; row++) {
        int half = (row * (s / 2)) / denom;
        int cx = x + s / 2;
        gfx_hline(cv, cx - half, y + row, 2 * half + 1, col);
    }
    /* exclamation: stem + dot, in the panel-dark colour */
    gfx_vline(cv, x + s / 2, y + s / 3, s / 3, TH_BG);
    gfx_fill(cv, x + s / 2, y + s - 3, 1, 1, TH_BG);
}

/* Section title line; returns the y just below the title. */
static int rt_title(Canvas* cv, int x, int y, const char* s, int maxw) {
    if (maxw > GFX_FW)
        gfx_text_clip(cv, x, y, s, TH_TEXT_DIM, x, maxw);
    return y + RT_HEAD_H + 2;
}

/* ---- ring / arc gauge --------------------------------------------------- */
/* Draw a thick ring of radius `rad` centred at (cx, cy); the leading `pct`%
 * (clockwise from 12 o'clock) is `fg`, the remainder is `track`. Done by
 * scanning the bounding box and selecting pixels whose distance to centre is
 * within the ring band, then choosing colour by their angle quadrant-fraction.
 * Pure integer maths, fully bounded -- no trig/float. */
static void rt_ring(Canvas* cv, int cx, int cy, int rad, int thick,
                    int pct, uint32_t fg, uint32_t track) {
    if (rad < 4) return;
    int rout = rad;
    int rin  = rad - thick;
    if (rin < 1) rin = 1;
    int r2o = rout * rout;
    int r2i = rin * rin;
    pct = rt_clamp(pct, 0, 100);
    /* fraction of full circle, scaled by 1024 to stay in ints */
    int fill_q = (pct * 1024) / 100;

    for (int dy = -rout; dy <= rout; dy++) {
        for (int dx = -rout; dx <= rout; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r2o || d2 < r2i) continue;
            /* angle as a 0..1024 fraction, clockwise starting at 12 o'clock.
             * Approximate using the octant + a linear ramp from |dx|,|dy|;
             * good enough for a gauge sweep and cheap. */
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            int sum = adx + ady;
            int oct;          /* coarse fraction within the upper/lower half */
            if (sum == 0) oct = 0;
            else          oct = (adx * 128) / sum;  /* 0..128 */
            int frac;         /* 0..1024 clockwise from top */
            if (dx >= 0 && dy < 0)       frac = oct;                 /* TR  */
            else if (dx >= 0 && dy >= 0) frac = 256 - oct;           /* BR  */
            else if (dx < 0  && dy >= 0) frac = 256 + oct;           /* BL  */
            else                         frac = 512 - oct;           /* TL  */
            /* the above spans 0..512 (a half); mirror into full 0..1024 by
             * folding the right half to 0..512 and left half to 512..1024. */
            int sweep = (dx >= 0) ? frac : (1024 - frac);
            uint32_t col = (sweep <= fill_q) ? fg : track;
            gfx_fill(cv, cx + dx, cy + dy, 1, 1, col);
        }
    }
}

/* ---- FLOW section ------------------------------------------------------- */
static void rt_draw_flow(Ide* a, Canvas* cv, int x, int y, int w, int h) {
    Model* m = &a->model;

    /* title: "RUNTIME FLOW (<focusname>)" assembled into one buffer */
    {
        const char* fname = "-";
        if (m->focus >= 0 && m->focus < m->nfuncs)
            fname = m->funcs[m->focus].name;
        char hdr[80];
        int i = 0;
        rt_append(hdr, (int)sizeof(hdr), &i, "RUNTIME FLOW (");
        rt_append(hdr, (int)sizeof(hdr), &i, fname);
        rt_append(hdr, (int)sizeof(hdr), &i, ")");
        gfx_text_clip(cv, x, y, hdr, TH_TEXT_DIM, x, w);
    }

    int top   = y + RT_HEAD_H + 2;
    int left  = x;
    int right = x + w;
    int bot   = y + h;

    int nflow = m->nflow;
    if (nflow > M_MAXFLOW) nflow = M_MAXFLOW;
    if (nflow < 0) nflow = 0;

    if (nflow == 0) {
        gfx_text_clip(cv, left, top, "No analysis yet", TH_TEXT_FAINT, left, w);
        return;
    }

    /* lay pills left-to-right, wrapping to a 2nd row when out of width */
    int bx = left;
    int by = top;
    int absent = 0;

    for (int i = 0; i < nflow; i++) {
        FlowStep* st = &m->flow[i];
        int max_chars = rt_fit_chars(RT_BOX_MAXW - 2 * RT_BOX_PADX);
        int tw = rt_text_w(st->label, max_chars);
        int bw = tw + 2 * RT_BOX_PADX;
        if (bw < RT_BOX_MINW) bw = RT_BOX_MINW;
        if (bw > RT_BOX_MAXW) bw = RT_BOX_MAXW;
        if (bw > w) bw = w;

        /* wrap: if box would overflow and we're not at the row start */
        if (bx + bw > right && bx > left) {
            bx = left;
            by += RT_BOX_H + RT_ROW_GAPY;
        }
        if (by + RT_BOX_H > bot) break;   /* no vertical room left */

        if (st->absent) {
            absent++;
            /* dim red fill + dashed red outline on a panel base */
            gfx_round(cv, bx, by, bw, RT_BOX_H, 5, TH_PANEL);
            gfx_blend(cv, bx + 1, by + 1, bw - 2, RT_BOX_H - 2, RT_ABSENT_FILL);
            gfx_dashed(cv, bx + 1, by, bx + bw - 2, by, TH_RED, 3);
            gfx_dashed(cv, bx + 1, by + RT_BOX_H - 1, bx + bw - 2,
                       by + RT_BOX_H - 1, TH_RED, 3);
            gfx_dashed(cv, bx, by + 1, bx, by + RT_BOX_H - 2, TH_RED, 3);
            gfx_dashed(cv, bx + bw - 1, by + 1, bx + bw - 1,
                       by + RT_BOX_H - 2, TH_RED, 3);
        } else {
            /* connected: solid rounded pill with a light border */
            gfx_round(cv, bx, by, bw, RT_BOX_H, 5, TH_PANEL);
            gfx_stroke(cv, bx, by, bw, RT_BOX_H, TH_BORDER_LT);
            /* a faint top highlight line for a touch of depth */
            gfx_hline(cv, bx + 3, by + 1, bw - 6, TH_HEADER);
        }

        int lx = bx + RT_BOX_PADX;
        int ly = by + (RT_BOX_H - GFX_FH) / 2;
        gfx_text_clip(cv, lx, ly, st->label,
                      st->absent ? TH_RED : TH_TEXT,
                      lx, bw - 2 * RT_BOX_PADX);

        /* connector arrow to the next box (same row only) */
        if (i + 1 < nflow) {
            int ax0 = bx + bw + 1;
            int ax1 = ax0 + RT_ARROW_W - 3;
            int ay  = by + RT_BOX_H / 2;
            if (ax1 < right) {
                gfx_line(cv, ax0, ay, ax1, ay, TH_TEXT_DIM);
                rt_arrowhead(cv, ax1, ay, TH_TEXT_DIM);
            }
        }

        bx += bw + RT_ARROW_W;
    }

    /* footer tally below the (last used) row */
    int ty = by + RT_BOX_H + RT_ROW_GAPY;
    if (ty + GFX_FH <= bot) {
        if (absent > 0) {
            char buf[48];
            int i = 0;
            char num[12];
            int n = ide_itoa(absent, num);
            num[n] = 0;
            rt_append(buf, (int)sizeof(buf), &i, num);
            rt_append(buf, (int)sizeof(buf), &i, " missing gates detected");
            gfx_text_clip(cv, left, ty, buf, TH_ORANGE, left, w);
        } else {
            gfx_text_clip(cv, left, ty, "flow OK", TH_GREEN, left, w);
        }
    }
}

/* ---- COHERENCE section -------------------------------------------------- */
static void rt_draw_coherence(Ide* a, Canvas* cv, int x, int y, int w, int h) {
    Model* m = &a->model;
    int next = rt_title(cv, x, y, "COHERENCE SCORE", w);

    int bot = y + h;
    int coh = rt_clamp(m->coherence, 0, 100);
    uint32_t band;
    const char* label;
    if (coh >= 80)      { band = TH_GREEN;  label = "High";   }
    else if (coh >= 50) { band = TH_YELLOW; label = "Medium"; }
    else                { band = TH_RED;    label = "Low";    }

    /* big percentage text, built from itoa + '%' */
    char pct[8];
    int n = ide_itoa(coh, pct);
    if (n < (int)sizeof(pct) - 1) { pct[n++] = '%'; pct[n] = 0; }

    /* Gauge area: a ring on the left, % in its centre, label below.
     * Fit the ring to whatever vertical room remains for this section. */
    int gauge_top = next + 2;
    int avail_h   = bot - gauge_top - GFX_FH - RT_SPARK_H - 3 * RT_ROW_GAPY;
    int rad = (avail_h / 2);
    if (rad > w / 3) rad = w / 3;
    if (rad < 4) rad = 4;
    int thick = rad / 3;
    if (thick < 3) thick = 3;

    int draw_ring = (gauge_top + 2 * rad <= bot) && (2 * rad + 2 < w);
    int label_y;

    if (draw_ring) {
        int cx = x + rad + 1;
        int cy = gauge_top + rad;
        rt_ring(cv, cx, cy, rad, thick, coh, band, TH_BORDER);
        /* centre the big % inside the ring */
        int tx = cx - (n * GFX_FW) / 2;
        if (tx < x) tx = x;
        int tyc = cy - GFX_FH / 2;
        gfx_text_clip(cv, tx, tyc, pct, band, tx, x + w - tx);
        gfx_text_clip(cv, tx + 1, tyc, pct, band, tx + 1, x + w - tx); /* bold */
        /* Low/Medium/High label to the right of the ring */
        int lx = cx + rad + 6;
        int la = x + w - lx;
        if (la > GFX_FW)
            gfx_text_clip(cv, lx, cy - GFX_FH / 2, label, TH_TEXT_DIM, lx, la);
        label_y = gauge_top + 2 * rad + RT_ROW_GAPY;
    } else {
        /* fallback: big % + label on one line, then a thin horizontal bar */
        int py = gauge_top;
        gfx_text_clip(cv, x, py, pct, band, x, w);
        gfx_text_clip(cv, x + 1, py, pct, band, x + 1, w);
        int lx = x + (n + 1) * GFX_FW;
        int la = x + w - lx;
        if (la > GFX_FW)
            gfx_text_clip(cv, lx, py, label, TH_TEXT_DIM, lx, la);
        int by = py + GFX_FH + 2;
        if (by + 6 <= bot) {
            gfx_fill(cv, x, by, w, 6, TH_BORDER);
            gfx_fill(cv, x, by, (w * coh) / 100, 6, band);
        }
        label_y = by + 6 + RT_ROW_GAPY;
    }

    /* sparkline caption + bars */
    int cap_y = label_y;
    if (cap_y + GFX_FH <= bot)
        gfx_text_clip(cv, x, cap_y, "Trend (last 10 runs)", TH_TEXT_FAINT,
                      x, w);

    int spark_y = cap_y + GFX_FH + 2;
    if (spark_y + RT_SPARK_H <= bot) {
        /* fixed pseudo-trend (heights 0..RT_SPARK_H), nudged toward `coh`. */
        static const int trend[RT_SPARK_N] = { 4, 7, 5, 9, 6, 11, 8, 13, 10, 15 };
        int base = spark_y + RT_SPARK_H;
        for (int i = 0; i < RT_SPARK_N; i++) {
            int bh = trend[i] + (coh * RT_SPARK_H) / 200;   /* lift by score */
            bh = rt_clamp(bh, 2, RT_SPARK_H);
            int bxx = x + i * (RT_SPARK_W + RT_SPARK_GAP);
            if (bxx + RT_SPARK_W > x + w) break;
            /* the most recent bar (last) is the live one -> band colour; the
             * earlier history sits dimmer for a cleaner sparkline read. */
            uint32_t bc = (i == RT_SPARK_N - 1) ? band : TH_TEXT_FAINT;
            gfx_fill(cv, bxx, base - bh, RT_SPARK_W, bh, bc);
        }
    }
}

/* ---- WARNINGS section --------------------------------------------------- */
static void rt_draw_warnings(Ide* a, Canvas* cv, int x, int y, int w, int h) {
    Model* m = &a->model;

    int nrisks = m->nrisks;
    if (nrisks > M_MAXRISKS) nrisks = M_MAXRISKS;
    if (nrisks < 0) nrisks = 0;

    int shown = nrisks < RT_MAXWARN ? nrisks : RT_MAXWARN;

    /* title: "LIVE WARNINGS (N)" -- N reflects total risks, not just shown */
    {
        char hdr[32];
        int i = 0;
        char num[12];
        int n = ide_itoa(nrisks, num);
        num[n] = 0;
        rt_append(hdr, (int)sizeof(hdr), &i, "LIVE WARNINGS (");
        rt_append(hdr, (int)sizeof(hdr), &i, num);
        rt_append(hdr, (int)sizeof(hdr), &i, ")");
        gfx_text_clip(cv, x, y, hdr, TH_TEXT_DIM, x, w);
    }

    int top = y + RT_HEAD_H + 2;
    int bot = y + h;

    if (nrisks == 0) {
        gfx_text_clip(cv, x, top, "No analysis yet", TH_TEXT_FAINT, x, w);
        return;
    }

    int rowh = GFX_FH + 6;
    int tri  = 12;
    for (int i = 0; i < shown; i++) {
        int ry = top + i * rowh;
        if (ry + GFX_FH > bot) break;
        rt_warn_tri(cv, x, ry + (GFX_FH - tri) / 2, tri, TH_ORANGE);
        int tx = x + tri + 6;
        int avail = x + w - tx;
        if (avail > GFX_FW)
            gfx_text_clip(cv, tx, ry, m->risks[i].title, TH_TEXT, tx, avail);
    }

    /* "+N more" hint if the list overflowed RT_MAXWARN */
    if (nrisks > shown) {
        int ry = top + shown * rowh;
        if (ry + GFX_FH <= bot) {
            char more[24];
            int i = 0;
            char num[12];
            int n = ide_itoa(nrisks - shown, num);
            num[n] = 0;
            rt_append(more, (int)sizeof(more), &i, "+");
            rt_append(more, (int)sizeof(more), &i, num);
            rt_append(more, (int)sizeof(more), &i, " more");
            gfx_text_clip(cv, x + tri + 6, ry, more, TH_TEXT_FAINT,
                          x + tri + 6, x + w - (x + tri + 6));
        }
    }
}

/* ===========================================================================
 * Entry point.
 * ===========================================================================*/
void panel_runtime(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    /* panel background */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL2);
    /* a hairline top edge to seat the strip against the editor above it */
    gfx_hline(cv, r.x, r.y, r.w, TH_BORDER);

    /* three horizontal sections (fractions of r.w) */
    int flow_w = (r.w * 55) / 100;
    int coh_w  = (r.w * 20) / 100;
    int warn_w = r.w - flow_w - coh_w;       /* remainder (~25%) */

    int inner_y = r.y + PAD;
    int inner_h = r.h - 2 * PAD;
    if (inner_h <= 0) return;

    int flow_x = r.x + PAD;
    int coh_x  = r.x + flow_w + PAD;
    int warn_x = r.x + flow_w + coh_w + PAD;

    int flow_iw = flow_w - 2 * PAD;
    int coh_iw  = coh_w - 2 * PAD;
    int warn_iw = warn_w - 2 * PAD;

    /* faint vertical dividers between the three sections */
    gfx_vline(cv, r.x + flow_w, r.y + 4, r.h - 8, TH_BORDER);
    gfx_vline(cv, r.x + flow_w + coh_w, r.y + 4, r.h - 8, TH_BORDER);

    if (flow_iw > 0)
        rt_draw_flow(a, cv, flow_x, inner_y, flow_iw, inner_h);
    if (coh_iw > 0)
        rt_draw_coherence(a, cv, coh_x, inner_y, coh_iw, inner_h);
    if (warn_iw > 0)
        rt_draw_warnings(a, cv, warn_x, inner_y, warn_iw, inner_h);
}
