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

#define CHR_NTABS 5

/* Tab numbers ("VIZ-1".."VIZ-5") and the words beside them are drawn
 * separately so the number can be BLUE and the word dim. */
static const char* const CHR_TAB_NUM[CHR_NTABS] = {
    "VIZ-1", "VIZ-2", "VIZ-3", "VIZ-4", "VIZ-5",
};
static const char* const CHR_TAB_WORD[CHR_NTABS] = {
    "PROJECT MAP", "INSPECTOR", "RUNTIME", "ACTIONS", "POTENTIALS",
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

/* ------------------------------------------------------------------ top bar */
void panel_topbar(Ide* a, Canvas* cv, Rect r) {
    if (r.w <= 0 || r.h <= 0) return;

    const Model* m = &a->model;

    /* Bar background + a subtle bottom border under the whole bar. */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_HEADER);
    gfx_hline(cv, r.x, r.y + r.h - 1, r.w, TH_BORDER);

    int ty = chr_text_y(r.y, r.h);

    for (int i = 0; i < CHR_NTABS; i++) {
        int cx0 = chr_tab_x(r.x, r.w, i);
        int cx1 = chr_tab_x(r.x, r.w, i + 1);
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

    /* Right-aligned coherence chip: "COH <n>%" coloured by band so the score
     * is always visible regardless of the active VIZ. */
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
        int chip_x  = r.x + r.w - PAD - chip_w;
        if (chip_x < r.x + PAD) chip_x = r.x + PAD;

        int clip_x = r.x + PAD;
        int clip_w = r.w - 2 * PAD;
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

int panel_topbar_click(Ide* a, Rect r, int mx, int my) {
    if (!rect_hit(r, mx, my)) return 0;
    if (r.w <= 0) return 1;

    int rel = mx - r.x;
    int i = (rel * CHR_NTABS) / r.w;
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

    /* "FILE: <file>" */
    gfx_text_clip(cv, x, ty, "FILE:", TH_TEXT_DIM, clip_x, clip_w);
    x += gfx_textw("FILE:") + GFX_FW;
    {
        const char* f = m->cur_file[0] ? m->cur_file : "-";
        gfx_text_clip(cv, x, ty, f, TH_TEXT_DIM, clip_x, clip_w);
        x += gfx_textw(f) + 2 * GFX_FW;
    }

    /* Three pipeline dots. */
    x = chr_dot(cv, x, cy, m->lexed,    "LEXED",    clip_x, clip_w) + GFX_FW;
    x = chr_dot(cv, x, cy, m->parsed,   "PARSED",   clip_x, clip_w) + GFX_FW;
    x = chr_dot(cv, x, cy, m->analyzed, "ANALYZED", clip_x, clip_w) + GFX_FW;

    /* Optional "WARN <n>" chip in ORANGE when the model carries risks. */
    if (m->nrisks > 0) {
        x += GFX_FW;
        gfx_text_clip(cv, x, ty, "WARN", TH_ORANGE, clip_x, clip_w);
        x += gfx_textw("WARN") + GFX_FW;
        {
            char num[16];
            int n = ide_itoa(m->nrisks, num);
            num[n] = 0;
            gfx_text_clip(cv, x, ty, num, TH_ORANGE, clip_x, clip_w);
            x += n * GFX_FW + 2 * GFX_FW;
        }
    }

    /* ---------- RIGHT: shortcut legend + MODE ---------- *
     * Drawn left-to-right starting at sx; if it runs past the right edge the
     * clip simply drops the overflow (draw as many as fit). We start it after
     * the left block so the two never overlap. */
    static const char  CHR_KEYS[] =
        { 'E','C','W','B','G','I','R','D','S','P','T' };
    static const char* const CHR_WORDS[] = {
        "EXPLAIN","CONNECT","WARN","BUG","GENERATE","ISOLATE",
        "RECYCLE","REMOVE","SEPARATE","PROMOTE","TRACE"
    };
    enum { CHR_NSHORT = 11 };

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
