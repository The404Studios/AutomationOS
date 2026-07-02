/*
 * ide_chrome.c -- window CHROME for the Semantic LEGO Map IDE:
 *   - panel_topbar()       the top VIZ tab bar (5 evenly spaced tabs)
 *   - panel_topbar_click() hit-test a tab cell and switch a->viz
 *   - panel_status()       the bottom status / shortcuts legend bar
 *
 * Renders FROM a->viz and a->model (m->lexed/parsed/analyzed, m->cur_file,
 * m->total_lines, m->coherence, m->nrisks). No selection state of its own.
 *
 * Freestanding: no libc, no malloc, no stdio. All helpers are file-static and
 * prefixed chr_. Drawing stays inside the supplied Rect r (text is clipped).
 */
#include "ide.h"
#include "ide_theme.h"
#include "ide_marks.h"   /* IDE-FORGE-0: STAR watch chips + MUTE warn chip */

#define CHR_NTABS 6

/* WATCH chip hit-test table: rebuilt every panel_status() render, read by
 * panel_status_click() to jump the selection to a starred function. */
typedef struct { int x, y, w, h, fi; } ChrWatch;
static ChrWatch g_watch[3];
static int      g_watch_n;

/* Tab numbers ("VIZ-1".."VIZ-6") and the words beside them are drawn
 * separately so the number can be BLUE and the word dim. */
static const char* const CHR_TAB_NUM[CHR_NTABS] = {
    "VIZ-1", "VIZ-2", "VIZ-3", "VIZ-4", "VIZ-5", "VIZ-6",
};
static const char* const CHR_TAB_WORD[CHR_NTABS] = {
    "PROJECT MAP", "INSPECTOR", "RUNTIME", "ACTIONS", "POTENTIALS", "SETTINGS",
};

/* Vertically centre a GFX_FH-tall line of text within a bar of height h. */
static inline int chr_text_y(int y, int h) {
    int t = y + (h - GFX_FH) / 2;
    return t < y ? y : t;
}

/* x of the left edge of tab cell `i` within a bar of width w. */
static inline int chr_tab_x(int rx, int w, int i) {
    return rx + (w * i) / CHR_NTABS;
}

/* Coherence band -> chip colour: >=70 green, >=40 yellow, else red. */
static inline uint32_t chr_coh_color(int coh) {
    if (coh >= 70) return TH_GREEN;
    if (coh >= 40) return TH_YELLOW;
    return TH_RED;
}

/* "[+ NEW]" button on the LEGO top bar (mirrors the EDITOR workspace button):
 * opens the New Project modal. Render + hit-test share this geometry. */
static const char CHR_NEW_LABEL[] = "+ NEW";
static inline int chr_new_w(void) { return gfx_textw(CHR_NEW_LABEL) + 2 * GFX_FW; }
/* Left edge of the button: anchored to the right, leaving PAD margin. */
static inline int chr_new_x(Rect r) { return r.x + r.w - PAD - chr_new_w(); }

/* The 6 VIZ tabs pack into the bar MINUS a reserved right area (the coherence
 * chip "COH 100%" + the [+ NEW] button + gaps), so they never collide with
 * either. Render + hit-test both divide THIS width, not the full bar width. */
static inline int chr_tabs_w(Rect r) {
    int reserved = (8 * GFX_FW) + chr_new_w() + 3 * GFX_FW;
    int w = r.w - reserved;
    if (w < r.w / 2) w = r.w / 2;     /* keep the tab strip at >= half the bar */
    if (w < 0) w = 0;
    return w;
}

/* ------------------------------------------------------------------ top bar */
void panel_topbar(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    const Model* m = &a->model;

    /* Bar background + a subtle bottom border under the whole bar. */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_HEADER);
    gfx_hline(cv, r.x, r.y + r.h - 1, r.w, TH_BORDER);

    int ty = chr_text_y(r.y, r.h);
    int tabsw = chr_tabs_w(r);          /* tabs occupy the bar minus the right chips */

    for (int i = 0; i < CHR_NTABS; i++) {
        int cx0 = chr_tab_x(r.x, tabsw, i);
        int cx1 = chr_tab_x(r.x, tabsw, i + 1);
        int cw  = cx1 - cx0;
        if (cw <= 0) continue;

        int active = (i == (int)a->viz);

        if (active) {
            /* Lift the active tab with the panel fill + a 2px blue underline. */
            gfx_fill(cv, cx0, r.y, cw, r.h, TH_PANEL);
            gfx_hline(cv, cx0, r.y + r.h - 2, cw, TH_BLUE);
            gfx_hline(cv, cx0, r.y + r.h - 1, cw, TH_BLUE);
        }
        /* Thin cell separators (skip the very first left edge). */
        if (i > 0) gfx_vline(cv, cx0, r.y + 3, r.h - 6, TH_BORDER);

        /* Clip strictly to this cell. */
        int clip_x = cx0 + PAD;
        int clip_w = cw - 2 * PAD;
        if (clip_w < 0) clip_w = 0;

        /* "VIZ-N" number in BLUE always; the word follows: TEXT when active,
         * TEXT_DIM when inactive. */
        int tx = clip_x;
        gfx_text_clip(cv, tx, ty, CHR_TAB_NUM[i], TH_BLUE, clip_x, clip_w);
        tx += gfx_textw(CHR_TAB_NUM[i]) + GFX_FW;

        uint32_t wcol = active ? TH_TEXT : TH_TEXT_DIM;
        gfx_text_clip(cv, tx, ty, CHR_TAB_WORD[i], wcol, clip_x, clip_w);
    }

    /* Right-aligned "[+ NEW]" button -> opens the New Project modal. */
    {
        int bx = chr_new_x(r);
        int bw = chr_new_w();
        int hov = (a->mouse_y >= r.y && a->mouse_y < r.y + r.h &&
                   a->mouse_x >= bx && a->mouse_x < bx + bw);
        gfx_fill  (cv, bx, r.y + 3, bw, r.h - 6, hov ? TH_SELECT : TH_PANEL);
        gfx_stroke(cv, bx, r.y + 3, bw, r.h - 6, TH_GREEN);
        gfx_text_clip(cv, bx + GFX_FW, ty, CHR_NEW_LABEL, TH_GREEN, bx, bw);
    }

    /* Coherence chip "COH <n>%", placed to the LEFT of the [+ NEW] button so
     * the two never overlap; coloured by band so the score is always visible. */
    {
        char num[16];
        int coh = m->coherence;
        if (coh < 0)   coh = 0;
        if (coh > 100) coh = 100;
        int n = ide_itoa(coh, num);
        num[n] = 0;

        /* width of "COH " + number + "%" */
        int label_w = gfx_textw("COH") + GFX_FW;       /* "COH" + space */
        int chip_w  = label_w + n * GFX_FW + GFX_FW;   /* digits + '%' */
        /* sit just left of the [+ NEW] button (with a gap). */
        int chip_x  = chr_new_x(r) - GFX_FW - chip_w;

        /* Only draw the chip if it FITS to the left of [+ NEW] without crossing
         * the tabs region; when zoomed-in on a narrow bar it simply hides (the
         * coherence score is also shown in the inspector) rather than painting
         * over the [+ NEW] button. */
        if (chip_x >= r.x + PAD) {
            int clip_x = chip_x;
            int clip_w = (chr_new_x(r) - GFX_FW) - chip_x;   /* bounded to the gap */
            if (clip_w < 0) clip_w = 0;

            uint32_t cc = chr_coh_color(coh);
            int cx = chip_x;
            gfx_text_clip(cv, cx, ty, "COH", TH_TEXT_DIM, clip_x, clip_w);
            cx += gfx_textw("COH") + GFX_FW;
            gfx_text_clip(cv, cx, ty, num, cc, clip_x, clip_w);
            cx += n * GFX_FW;
            gfx_text_clip(cv, cx, ty, "%", cc, clip_x, clip_w);
        }
    }
}

int panel_topbar_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;
    if (r.w <= 0) return 1;

    /* [+ NEW] button (right edge) takes priority over the tab cells below it. */
    {
        int bx = chr_new_x(r);
        if (mx >= bx && mx < bx + chr_new_w()) { ide_new_project(a); return 1; }
    }

    int tabsw = chr_tabs_w(r);
    int rel = mx - r.x;
    if (rel >= tabsw) return 1;          /* click in the reserved right area: no tab */
    int i = (tabsw > 0) ? (rel * CHR_NTABS) / tabsw : 0;
    if (i < 0) i = 0;
    if (i >= CHR_NTABS) i = CHR_NTABS - 1;
    a->viz = (VizTab)i;
    return 1;
}

/* --------------------------------------------------------------- status bar */

/* Draw one labelled pipeline dot at (x, midline). The dot is GREEN when `on`,
 * faint otherwise; the label follows in dim text. Returns the x past the
 * label so callers can chain segments. Clips to [clip_x, clip_x+clip_w). */
static int chr_dot(Canvas* cv, int x, int cy, int on, const char* label,
                   int clip_x, int clip_w) {
    enum { DOT = 8 };
    int dx = x;
    int dy = cy - DOT / 2;
    uint32_t dc = on ? TH_GREEN : TH_TEXT_FAINT;
    if (dx + DOT > clip_x && dx < clip_x + clip_w)
        gfx_round(cv, dx, dy, DOT, DOT, DOT / 2, dc);
    int lx = dx + DOT + 4;
    int ty = cy - GFX_FH / 2;
    gfx_text_clip(cv, lx, ty, label, TH_TEXT_DIM, clip_x, clip_w);
    return lx + gfx_textw(label);
}

/* One shortcut chip: a single BLUE key letter, a space, then its dim word.
 * Returns x past the word (plus a trailing gap). Clipped to the right edge. */
static int chr_shortcut(Canvas* cv, int x, int ty, char key, const char* word,
                        int clip_x, int clip_w) {
    char k[2] = { key, 0 };
    gfx_text_clip(cv, x, ty, k, TH_BLUE, clip_x, clip_w);
    int wx = x + GFX_FW + GFX_FW;            /* key + one space */
    gfx_text_clip(cv, wx, ty, word, TH_TEXT_DIM, clip_x, clip_w);
    return wx + gfx_textw(word) + 2 * GFX_FW;  /* trailing gap */
}

void panel_status(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    const Model* m = &a->model;

    /* Bar background + top hairline. */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_HEADER);
    gfx_hline(cv, r.x, r.y, r.w, TH_BORDER);

    int cy = r.y + r.h / 2;                   /* vertical midline */
    int ty = cy - GFX_FH / 2;
    int clip_x = r.x + PAD;
    int clip_w = r.w - 2 * PAD;
    if (clip_w < 0) clip_w = 0;

    /* ---------- LEFT: LINES / FILE + pipeline dots ---------- */
    int x = r.x + PAD;

    /* "LINES: <n>" */
    gfx_text_clip(cv, x, ty, "LINES:", TH_TEXT_DIM, clip_x, clip_w);
    x += gfx_textw("LINES:") + GFX_FW;
    {
        char num[16];
        int n = ide_itoa(m->total_lines, num);
        num[n] = 0;
        gfx_text_clip(cv, x, ty, num, TH_TEXT_DIM, clip_x, clip_w);
        x += n * GFX_FW + 2 * GFX_FW;
    }

    /* IDE-CONTEXT-0 breadcrumb: "project > file > FN  Ln <l>,<c>" from THE
     * selection model (a->sel) -- the always-visible "where am I" the
     * aphantasia north star requires. FN shows the caret's enclosing function
     * (accent blue when resolved), "-" between functions. (The old standalone
     * "FILE:" chip is folded into this breadcrumb.) */
    {
        const char* fn = "-";
        if (a->sel.symbol >= 0 && a->sel.symbol < a->model.nfuncs)
            fn = a->model.funcs[a->sel.symbol].name;
        char bc[IDE_PATH + 96];
        ide_breadcrumb_prefix(a, bc, (int)sizeof(bc));
        gfx_text_clip(cv, x, ty, bc, TH_TEXT_DIM, clip_x, clip_w);
        x += gfx_textw(bc) + GFX_FW;
        gfx_text_clip(cv, x, ty, ">", TH_TEXT_FAINT, clip_x, clip_w);
        x += gfx_textw(">") + GFX_FW;
        gfx_text_clip(cv, x, ty, fn,
                      (a->sel.symbol >= 0) ? TH_BLUE : TH_TEXT_DIM,
                      clip_x, clip_w);
        x += gfx_textw(fn) + 2 * GFX_FW;

        char pos[24]; int p = 0;
        char num[12]; int n;
        pos[p++] = 'L'; pos[p++] = 'n'; pos[p++] = ' ';
        n = ide_itoa(a->sel.line + 1, num);
        for (int i = 0; i < n && p < (int)sizeof(pos) - 1; i++) pos[p++] = num[i];
        pos[p++] = ',';
        n = ide_itoa(a->editor.caret_col + 1, num);
        for (int i = 0; i < n && p < (int)sizeof(pos) - 1; i++) pos[p++] = num[i];
        pos[p] = 0;
        gfx_text_clip(cv, x, ty, pos, TH_TEXT_DIM, clip_x, clip_w);
        x += gfx_textw(pos) + 2 * GFX_FW;
    }

    /* Three pipeline dots. */
    x = chr_dot(cv, x, cy, m->lexed,    "LEXED",    clip_x, clip_w) + GFX_FW;
    x = chr_dot(cv, x, cy, m->parsed,   "PARSED",   clip_x, clip_w) + GFX_FW;
    x = chr_dot(cv, x, cy, m->analyzed, "ANALYZED", clip_x, clip_w) + GFX_FW;

    /* Optional "WARN <n>" chip in ORANGE when the model carries risks. When the
     * FOCUSED function is muted (a MARK knob), render it faint + "(muted)" so the
     * user sees the warning is acknowledged, not gone (IDE-FORGE-0 Knob 4). */
    if (m->nrisks > 0) {
        int muted = 0;
        if (a->focus_func >= 0 && a->focus_func < m->nfuncs) {
            SymMark* fmk = marks_find(m->funcs[a->focus_func].name);
            muted = (fmk && fmk->mute);
        }
        uint32_t wc = muted ? TH_TEXT_FAINT : TH_ORANGE;
        x += GFX_FW;
        gfx_text_clip(cv, x, ty, "WARN", wc, clip_x, clip_w);
        x += gfx_textw("WARN") + GFX_FW;
        {
            char num[16];
            int n = ide_itoa(m->nrisks, num);
            num[n] = 0;
            gfx_text_clip(cv, x, ty, num, wc, clip_x, clip_w);
            x += n * GFX_FW + GFX_FW;
        }
        if (muted) {
            gfx_text_clip(cv, x, ty, "(muted)", TH_TEXT_FAINT, clip_x, clip_w);
            x += gfx_textw("(muted)") + GFX_FW;
        }
        x += GFX_FW;
    }

    /* "WATCH:" chips -- up to 3 starred functions, always visible (the STAR knob
     * consequence). Each is a rounded chip; clicking it jumps the selection
     * (panel_status_click). Reuses the closed-function chip style. */
    g_watch_n = 0;
    {
        int needlbl = 1;
        for (int i = 0; i < m->nfuncs && i < M_MAXFUNCS && g_watch_n < 3; i++) {
            SymMark* mk = marks_find(m->funcs[i].name);
            if (!mk || !mk->star) continue;
            if (needlbl) {
                gfx_text_clip(cv, x, ty, "WATCH:", TH_TEXT_DIM, clip_x, clip_w);
                x += gfx_textw("WATCH:") + GFX_FW;
                needlbl = 0;
            }
            const char* nm = m->funcs[i].name;
            int cw = gfx_textw(nm) + 2 * PAD;
            if (x + cw > clip_x + clip_w) break;
            gfx_round(cv, x, r.y + 2, cw, r.h - 4, 4, TH_PANEL2);
            gfx_text_clip(cv, x + PAD, ty, nm, TH_YELLOW, clip_x, clip_w);
            g_watch[g_watch_n].x = x; g_watch[g_watch_n].y = r.y + 2;
            g_watch[g_watch_n].w = cw; g_watch[g_watch_n].h = r.h - 4;
            g_watch[g_watch_n].fi = i;
            g_watch_n++;
            x += cw + GFX_FW;
        }
        if (!needlbl) x += GFX_FW;     /* gap after the watch strip */
    }

    /* ---------- RIGHT: shortcut legend + MODE ---------- *
     * Drawn left-to-right starting at sx; if it runs past the right edge the
     * clip simply drops the overflow (draw as many as fit). We start it after
     * the left block so the two never overlap. */
    /* IDE-FORGE-0 audit fix: the old legend advertised ELEVEN verbs
     * (EXPLAIN/CONNECT/WARN/BUG/../TRACE) of which only G was real -- B and
     * R were wired to build/run under the WRONG names (BUG/RECYCLE) and the
     * other eight had no handlers at all. For a tool whose law is "the
     * chrome always tells the truth," a lying legend is a critical defect.
     * This legend lists exactly what the LEGO key router binds. */
    static const char  CHR_KEYS[] =
        { 'B','R','G' };
    static const char* const CHR_WORDS[] = {
        "BUILD","RUN","GENERATE"
    };
    enum { CHR_NSHORT = 3 };

    int sx = x + 2 * GFX_FW;                   /* leave a gap after dots */
    for (int i = 0; i < CHR_NSHORT; i++) {
        if (sx >= clip_x + clip_w) break;      /* nothing more fits */
        sx = chr_shortcut(cv, sx, ty, CHR_KEYS[i], CHR_WORDS[i],
                          clip_x, clip_w);
    }

    /* "MODE: SEMANTIC" -- MODE label dim, SEMANTIC green. */
    if (sx < clip_x + clip_w) {
        gfx_text_clip(cv, sx, ty, "MODE:", TH_TEXT_DIM, clip_x, clip_w);
        sx += gfx_textw("MODE:") + GFX_FW;
        gfx_text_clip(cv, sx, ty, "SEMANTIC", TH_GREEN, clip_x, clip_w);
    }
}

/* Click in the status bar: hit-test the WATCH chips and jump the selection to
 * the starred function (the STAR knob's clickable consequence). Returns 1 if
 * the click was inside the bar (always consumed there). */
int panel_status_click(Ide* a, Rect r, int mx, int my) {
    if (!a || !rect_hit(r, mx, my)) return 0;
    for (int i = 0; i < g_watch_n && i < 3; i++) {
        if (mx >= g_watch[i].x && mx < g_watch[i].x + g_watch[i].w &&
            my >= g_watch[i].y && my < g_watch[i].y + g_watch[i].h) {
            ide_sel_jump(a, g_watch[i].fi, PANE_INSPECTOR);
            return 1;
        }
    }
    return 1;   /* consume clicks anywhere in the status bar */
}
