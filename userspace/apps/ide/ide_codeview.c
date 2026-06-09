/*
 * ide_codeview.c -- CODE VIEW panel for the Semantic LEGO Map IDE.
 *
 * Renders a->src with a left line-number gutter, per-character syntax
 * highlighting (via lex_classify_line), and -- the LEGO twist -- per-line
 * "port" annotation chips on the right edge for lines inside the focused
 * function: "Read: <g>" (cyan), "Write: <g>" (green), "Control[: <call>]"
 * (yellow). The focus function's gutter rows get a subtle TH_SELECT tint.
 *
 * Freestanding: no libc, no malloc, no stdio. All helpers are file-static and
 * prefixed cv_. Every draw clips to the supplied Rect r; we never read past
 * a->src_len and never crash on empty source.
 */
#include "ide.h"
#include "ide_theme.h"
#include "ide_lex.h"

/* ---- tunables ---- */
#define CV_HEADER_H   (ROW_H + 2)        /* title row + hairline divider     */
#define CV_GUTTER_CH  5                  /* gutter width in characters       */
#define CV_LINECAP    1024               /* max chars classified per line    */
#define CV_TAGCAP     80                 /* max chars in a built tag string  */
#define CV_TABW       (g_tab_width < 1 ? 1 : g_tab_width)  /* tab stop width (Settings knob) */
#define CV_SBAR_W     3                  /* scrollbar indicator width (px)   */
#define CV_FOCUS_PAD  2                  /* lines above focus_start on recentre */

/* Gutter pixel width: digits + one space of padding on each side. */
static inline int cv_gutter_w(void) {
    return CV_GUTTER_CH * GFX_FW + 2 * PAD;
}

/* Body rect (panel minus header). Code + gutter live here. */
static inline Rect cv_body(Rect r) {
    Rect b;
    b.x = r.x;
    b.y = r.y + CV_HEADER_H;
    b.w = r.w;
    b.h = r.h - CV_HEADER_H;
    if (b.h < 0) b.h = 0;
    return b;
}

/* ---- tiny local string helpers (no libc) ---- */

static int cv_slen(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

/* Append src to dst[*pos] (cap-bounded, NUL-terminated). */
static void cv_append(char* dst, int* pos, int cap, const char* src) {
    int i = 0;
    if (!src) return;
    while (src[i] && *pos < cap - 1)
        dst[(*pos)++] = src[i++];
    dst[*pos] = '\0';
}

/* Right-aligned decimal of v into out (width w, space-padded). Returns out. */
static void cv_itoa_pad(int v, int w, char* out, int cap) {
    char tmp[16];
    int n = 0, k = 0, p = 0;
    if (w > cap - 1) w = cap - 1;
    if (v < 0) v = 0;
    /* build reversed digits */
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    /* leading spaces */
    for (k = 0; k < w - n; k++) out[p++] = ' ';
    /* digits forward */
    for (k = n - 1; k >= 0; k--) out[p++] = tmp[k];
    out[p] = '\0';
}

/* ---- substring / word matching over the raw line text ---- */

/* identifier char test (matches lexer's notion of ident continuation) */
static int cv_is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/*
 * Find the first occurrence of NUL-terminated word `w` in line[0..len) on a
 * whole-identifier boundary (so "g_x" does not match inside "g_xyz"). Returns
 * the start index, or -1 if not found. nlen is cv_slen(w).
 */
static int cv_find_word(const char* line, int len, const char* w, int nlen) {
    int i;
    if (nlen <= 0 || nlen > len) return -1;
    for (i = 0; i + nlen <= len; i++) {
        int j, ok = 1;
        for (j = 0; j < nlen; j++) {
            if (line[i + j] != w[j]) { ok = 0; break; }
        }
        if (!ok) continue;
        /* boundary check: char before and after must not be ident chars */
        if (i > 0 && cv_is_ident(line[i - 1])) continue;
        if (i + nlen < len && cv_is_ident(line[i + nlen])) continue;
        return i;
    }
    return -1;
}

/*
 * After a name found at [at, at+nlen), is the next non-space char an '='
 * that is an assignment (not '==', '<=', '>=', '!=')? Used to distinguish a
 * write from a read.
 */
static int cv_assigned_after(const char* line, int len, int at, int nlen) {
    int k = at + nlen;
    while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
    if (k >= len) return 0;
    if (line[k] != '=') return 0;
    if (k + 1 < len && line[k + 1] == '=') return 0;   /* == comparison */
    return 1;
}

/* Advance a 0-based visual column past one source character. Tabs jump to the
 * next multiple of CV_TABW; everything else advances one cell. */
static int cv_col_advance(int col, char ch) {
    if (ch == '\t') return (col / CV_TABW + 1) * CV_TABW;
    return col + 1;
}

/* Tab-aware visual column for character index `charcol` within line[0..llen).
 * Mirrors the editor caret model (char index) onto the code view's tab-stop
 * rendering, so the shared a->editor caret lands in the right cell here. */
static int cv_caret_viscol(const char* line, int llen, int charcol) {
    int col = 0, i;
    if (charcol > llen) charcol = llen;
    if (charcol < 0) charcol = 0;
    for (i = 0; i < charcol; i++) col = cv_col_advance(col, line[i]);
    return col;
}

/* Inverse of cv_caret_viscol: the character index whose cell contains visual
 * column `viscol` (clamped to llen). Used to place the caret from a click. */
static int cv_charcol_from_vis(const char* line, int llen, int viscol) {
    int col = 0, i;
    if (viscol <= 0) return 0;
    for (i = 0; i < llen; i++) {
        int adv = cv_col_advance(col, line[i]);
        if (viscol < adv) return i;     /* click lands within this char's span */
        col = adv;
    }
    return llen;
}

/* Count newline-delimited lines in src[0..slen). An empty buffer is 0 lines;
 * a buffer with no trailing '\n' still counts its final partial line. */
static int cv_count_lines(const char* src, int slen) {
    int n = 0, i;
    if (!src || slen <= 0) return 0;
    for (i = 0; i < slen; i++)
        if (src[i] == '\n') n++;
    if (slen > 0 && src[slen - 1] != '\n') n++;   /* trailing partial line */
    return n;
}

/* class value -> text colour (kept in sync with ide_editor.c ed_class_color) */
static uint32_t cv_class_color(unsigned char c) {
    switch (c) {
        case LEXCLS_KEYWORD: return TH_PURPLE;
        case LEXCLS_TYPE:    return TH_CYAN;
        case LEXCLS_STRING:  return TH_ORANGE;
        case LEXCLS_COMMENT: return TH_TEXT_FAINT;
        case LEXCLS_NUMBER:  return TH_GREEN;
        case LEXCLS_PREPROC: return TH_MAGENTA;
        case LEXCLS_CALL:    return TH_YELLOW;
        default:             return TH_TEXT;
    }
}

/*
 * Draw a rounded "chip" tag right-aligned within the body, on row `ry`,
 * without overrunning the code area. text is NUL-terminated. The chip is only
 * drawn if it fits to the right of `min_x` (the right edge of the code). col
 * is the accent/text colour; the fill is a dim tint of the panel.
 */
/* Draw a semantic annotation in the RIGHT-MARGIN gutter, connected to the code
 * by a short dashed line + a colored bullet -- VS Code "inlay hint" style. This
 * replaces the old right-aligned chip, which shared the right edge with the code
 * text and so overlapped it on longer lines ("towers[i Control: tower_init").
 *   code_end : right pixel of the drawn code on this row (connector start)
 *   gutter_x : left edge of the reserved annotation gutter (label start)
 * Code is clipped to stop before gutter_x by the caller, so there is no overlap. */
static void cv_draw_tag(Canvas* cv, Rect body, int ry, int code_end,
                        int gutter_x, const char* text, uint32_t col) {
    int liney = ry + GFX_FH / 2;
    int lbl_x = gutter_x + 6;
    int avail = body.x + body.w - PAD - lbl_x;
    if (avail <= GFX_FW) return;                  /* no room for the label */
    if (ry < body.y || ry + GFX_FH > body.y + body.h) return;

    /* dashed connector from end-of-code to the gutter */
    if (code_end + 2 < gutter_x - 2)
        gfx_dashed(cv, code_end + 2, liney, gutter_x - 2, liney, TH_TEXT_FAINT, 3);
    /* colored bullet at the gutter edge, then the label text */
    gfx_fill(cv, gutter_x, liney - 1, 3, 3, col);
    gfx_text_clip(cv, lbl_x, ry, text, col, lbl_x, avail);
}

void panel_code(Ide* a, Canvas* cv, Rect r) {
    static unsigned char cls[CV_LINECAP];
    static char numbuf[16];
    static char tagbuf[CV_TAGCAP];
    /* last focus function we re-centred on: only auto-scroll on a *change*,
     * so the user keeps free scrolling afterwards. -2 = "no value yet". */
    static int last_focus = -2;

    int line_h = GFX_FH;
    int gutter_w;
    Rect body;
    int scroll, vis, row;
    int focus_ls = -1, focus_le = -1;
    int total_lines;
    Func* ff = 0;
    int code_x, code_clip_w;
    int anno_x, anno_w;               /* reserved right-margin annotation gutter */

    if (!a || r.w <= 0 || r.h <= 0) return;

    /* ---- background + header ---- */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL2);
    gfx_fill(cv, r.x, r.y, r.w, CV_HEADER_H, TH_HEADER);
    gfx_hline(cv, r.x, r.y + CV_HEADER_H - 1, r.w, TH_BORDER);

    /* focus function (if any) -- bounds-checked against the model */
    if (a->focus_func >= 0 && a->focus_func < a->model.nfuncs) {
        ff = &a->model.funcs[a->focus_func];
        focus_ls = ff->line_start;
        focus_le = ff->line_end;
    }

    /* header text: "<file>: <focus function name>" */
    {
        char hdr[IDE_PATH + M_NAME + 4];
        int p = 0;
        const char* file = a->cur_file[0] ? a->cur_file : "(no file)";
        cv_append(hdr, &p, (int)sizeof(hdr), file);
        if (ff) {
            cv_append(hdr, &p, (int)sizeof(hdr), ": ");
            cv_append(hdr, &p, (int)sizeof(hdr), ff->name);
        }
        gfx_text_clip(cv, r.x + PAD, r.y + (CV_HEADER_H - GFX_FH) / 2,
                      hdr, TH_TEXT_DIM, r.x + PAD, r.w - 2 * PAD);
    }

    body = cv_body(r);
    if (body.h <= 0) return;

    gutter_w   = g_line_numbers ? cv_gutter_w() : 0;   /* Settings knob */
    code_x     = body.x + gutter_w + PAD;

    /* Reserve a RIGHT-MARGIN gutter for semantic annotations so they never
     * overlap the code. Room for ~"Control: <name>"; capped to 2/5 of the body,
     * and dropped entirely if that would leave fewer than 16 code columns (e.g.
     * a very narrow code pane) -- in which case code uses the full width and the
     * annotations simply don't draw. The annotation gutter is also a Settings knob. */
    anno_w = g_anno_gutter ? (22 * GFX_FW + 8) : 0;
    if (anno_w > (body.w * 2) / 5) anno_w = (body.w * 2) / 5;
    anno_x = body.x + body.w - anno_w;
    if (anno_w <= 0 || anno_x < code_x + 16 * GFX_FW) {
        anno_w = 0;
        anno_x = body.x + body.w;            /* no gutter: code uses full width */
    }
    code_clip_w = anno_x - PAD - code_x;
    if (code_clip_w < 0) code_clip_w = 0;

    /* gutter background + divider (only when line numbers are shown) */
    if (g_line_numbers) {
        gfx_fill(cv, body.x, body.y, gutter_w, body.h, TH_PANEL);
        gfx_vline(cv, body.x + gutter_w, body.y, body.h, TH_BORDER);
    }
    /* faint divider marking the annotation gutter (when reserved) */
    if (anno_w > 0)
        gfx_vline(cv, anno_x - PAD, body.y, body.h, TH_BORDER);

    vis = body.h / line_h;            /* fully visible code rows */
    if (vis < 1) vis = 1;

    /* total line count for scroll clamping + scrollbar geometry */
    {
        int slen = a->src_len;
        if (slen < 0) slen = 0;
        if (slen > IDE_SRC_CAP) slen = IDE_SRC_CAP;
        total_lines = cv_count_lines(a->src, slen);
    }

    /* ---- auto-scroll on focus change ----
     * When focus_func differs from the value we last re-centred on, and the
     * focused function's first line is not already comfortably visible, set the
     * scroll so line_start sits near the top. We update last_focus every frame
     * so a single change triggers exactly one re-centre; after that the user
     * can scroll freely. */
    scroll = a->code_scroll;
    if (scroll < 0) scroll = 0;

    if (a->codeview_focus) {
        /* Editing in the code view: keep the CARET line on screen rather than
         * auto-recentering on the focused function. Mirrors ed_scroll_to_caret. */
        int vrows = body.h / line_h; if (vrows < 1) vrows = 1;
        int cl = a->editor.caret_line;
        if (cl < scroll) scroll = cl;
        else if (cl > scroll + vrows - 1) scroll = cl - vrows + 1;
        int maxs = total_lines - vrows; if (maxs < 0) maxs = 0;
        if (scroll > maxs) scroll = maxs;
        if (scroll < 0) scroll = 0;
        a->code_scroll = scroll;
        last_focus = a->focus_func;   /* don't snap-recenter when focus released */
    } else if (a->focus_func != last_focus) {
        last_focus = a->focus_func;
        if (focus_ls >= 1) {
            int top = scroll + 1;            /* first visible 1-based line */
            int bot = scroll + vis;          /* last fully visible line    */
            if (focus_ls < top || focus_ls > bot) {
                int want = focus_ls - 1 - CV_FOCUS_PAD;  /* 0-based scroll */
                int maxs = total_lines - vis;
                if (maxs < 0) maxs = 0;
                if (want < 0) want = 0;
                if (want > maxs) want = maxs;
                scroll = want;
                a->code_scroll = want;
            }
        }
    }

    /* one extra partial row may peek in at the bottom */
    vis = (body.h + line_h - 1) / line_h;

    /* ---- walk lines: skip `scroll`, render `vis` visible ones ---- */
    {
        int lineno = 1;       /* 1-based current line number          */
        int pos = 0;          /* byte offset into a->src              */
        int slen = a->src_len;
        const char* src = a->src;

        if (slen < 0) slen = 0;
        if (slen > IDE_SRC_CAP) slen = IDE_SRC_CAP;

        /* skip lines before the scroll position */
        while (lineno <= scroll && pos < slen) {
            while (pos < slen && src[pos] != '\n') pos++;
            if (pos < slen) pos++;       /* consume the '\n'           */
            lineno++;
        }

        for (row = 0; row < vis && pos <= slen; row++) {
            int lstart = pos;
            int lend;                    /* exclusive end (no '\n')    */
            int llen, n;
            int ry = body.y + row * line_h;

            /* a row that does not start within the buffer => nothing left */
            if (pos >= slen && lstart >= slen) {
                /* allow drawing the final (possibly empty) line once */
                if (lineno > 1 && (slen == 0 || src[slen - 1] == '\n')) break;
            }

            /* scan to end of this line */
            while (pos < slen && src[pos] != '\n') pos++;
            lend = pos;
            if (pos < slen) pos++;       /* step past '\n' for next row */

            llen = lend - lstart;
            if (llen < 0) llen = 0;

            /* clip the final partial row to the body bottom */
            if (ry + line_h > body.y + body.h && ry >= body.y + body.h) break;

            int in_focus = (focus_ls >= 0 &&
                            lineno >= focus_ls && lineno <= focus_le);
            int text_right = code_x;     /* px right edge of drawn code text */

            /* ---- focus row tint: full line width (gutter + code), then the
             * right-aligned line number on top ---- */
            {
                /* TH_SELECT (0xFF1D2D45) tinted to a subtle 0x40 alpha. */
                if (in_focus)
                    gfx_blend(cv, body.x, ry, body.w, line_h,
                              (0x40u << 24) | (TH_SELECT & 0x00FFFFFFu));

                if (g_line_numbers) {
                    cv_itoa_pad(lineno, CV_GUTTER_CH, numbuf, (int)sizeof(numbuf));
                    gfx_text_clip(cv, body.x + PAD, ry, numbuf, TH_TEXT_FAINT,
                                  body.x + PAD, CV_GUTTER_CH * GFX_FW);
                }
            }

            /* ---- selection highlight (code-view editing), under the text ---- */
            if (a->codeview_focus) {
                int slo, shi;
                if (ide_editor_sel_range(a, &slo, &shi)) {
                    int lend2 = lstart + llen;
                    if (shi > lstart && slo <= lend2) {
                        int c0 = (slo > lstart ? slo : lstart) - lstart;  /* char idx */
                        int c1 = (shi < lend2 ? shi : lend2) - lstart;    /* char idx */
                        int eol_extra = (shi > lend2) ? 1 : 0;  /* spans the newline */
                        int v0 = cv_caret_viscol(src + lstart, llen, c0);
                        int v1 = cv_caret_viscol(src + lstart, llen, c1) + eol_extra;
                        if (v1 > v0) {
                            int hx = code_x + v0 * GFX_FW;
                            int hw = (v1 - v0) * GFX_FW;
                            int sel_right = code_x + code_clip_w;
                            if (hx < sel_right) {
                                if (hx + hw > sel_right) hw = sel_right - hx;
                                gfx_blend(cv, hx, ry, hw, line_h,
                                          (0x66u << 24) | (TH_BLUE & 0x00FFFFFFu));
                            }
                        }
                    }
                }
            }

            /* ---- classify + draw the code text, char-by-char, tab-aware ---- */
            n = llen;
            if (n > CV_LINECAP) n = CV_LINECAP;
            if (n > 0) {
                int i, col = 0;
                int code_right = code_x + code_clip_w;
                lex_classify_line(src + lstart, n, cls);
                for (i = 0; i < n; i++) {
                    char ch = src[lstart + i];
                    int cx = code_x + col * GFX_FW;
                    /* stop once this column has run off the visible region */
                    if (cx >= code_right) break;
                    if (ch != '\t' && (unsigned char)ch >= 0x20 &&
                        (unsigned char)ch < 0x7F) {
                        /* IDE-REPAIR-0 I1: ONE glyph per cell -- `src` is the
                         * un-terminated flat buffer; the interior pointer made
                         * gfx_text_clip draw the whole buffer suffix per row
                         * (same line-bleed as ide_editor.c). */
                        char glyph[2]; glyph[0] = ch; glyph[1] = 0;
                        gfx_text_clip(cv, cx, ry, glyph,
                                      cv_class_color(cls[i]),
                                      code_x, code_clip_w);
                        if (cx + GFX_FW > text_right) text_right = cx + GFX_FW;
                    }
                    col = cv_col_advance(col, ch);   /* tabs jump to next stop */
                }
                if (text_right > code_right) text_right = code_right;
            }

            /* ---- caret (code-view editing), drawn on top of the text ---- */
            if (a->codeview_focus && a->editor.caret_line == lineno - 1) {
                int vc = cv_caret_viscol(src + lstart, llen, a->editor.caret_col);
                int cx = code_x + vc * GFX_FW;
                if (cx >= code_x && cx < code_x + code_clip_w)
                    gfx_fill(cv, cx, ry, 2, line_h, TH_BLUE);
            }

            /* ---- annotation chip (focus-function lines only) ----
             * Only drawn when it fits to the right of text_right without
             * overlapping the code (cv_draw_tag enforces the gap). */
            if (ff && in_focus && n > 0 && anno_w > 0) {
                const char* ln = src + lstart;
                int tag_done = 0;
                int k;

                /* WRITE: a written global with '=' after it (highest signal) */
                for (k = 0; k < ff->nwrites && !tag_done; k++) {
                    const char* g = ff->writes[k];
                    int gl = cv_slen(g);
                    int at = cv_find_word(ln, n, g, gl);
                    if (at >= 0 && cv_assigned_after(ln, n, at, gl)) {
                        int p = 0;
                        cv_append(tagbuf, &p, CV_TAGCAP, "Write: ");
                        cv_append(tagbuf, &p, CV_TAGCAP, g);
                        cv_draw_tag(cv, body, ry, text_right, anno_x, tagbuf, TH_GREEN);
                        tag_done = 1;
                    }
                }

                /* READ: a read global present on the line */
                for (k = 0; k < ff->nreads && !tag_done; k++) {
                    const char* g = ff->reads[k];
                    int gl = cv_slen(g);
                    if (cv_find_word(ln, n, g, gl) >= 0) {
                        int p = 0;
                        cv_append(tagbuf, &p, CV_TAGCAP, "Read: ");
                        cv_append(tagbuf, &p, CV_TAGCAP, g);
                        cv_draw_tag(cv, body, ry, text_right, anno_x, tagbuf, TH_CYAN);
                        tag_done = 1;
                    }
                }

                /* CONTROL: an outgoing call present on the line */
                for (k = 0; k < ff->ncalls && !tag_done; k++) {
                    const char* c = ff->calls[k];
                    int cl = cv_slen(c);
                    if (cv_find_word(ln, n, c, cl) >= 0) {
                        int p = 0;
                        cv_append(tagbuf, &p, CV_TAGCAP, "Control: ");
                        cv_append(tagbuf, &p, CV_TAGCAP, c);
                        cv_draw_tag(cv, body, ry, text_right, anno_x, tagbuf, TH_YELLOW);
                        tag_done = 1;
                    }
                }
            }

            lineno++;

            /* if we consumed the whole buffer and it had no trailing '\n',
             * the line we just drew was the last one. */
            if (pos >= slen && (slen == 0 || src[slen - 1] != '\n')) {
                row++;
                break;
            }
        }
    }

    /* ---- scrollbar indicator (right edge of body) ----
     * A small thumb proportional to the visible fraction, positioned by the
     * scroll offset. Drawn only when there is something to scroll. */
    if (total_lines > 0) {
        int track_x = body.x + body.w - CV_SBAR_W;
        int track_h = body.h;
        int vrows   = body.h / line_h;          /* fully visible rows */
        if (vrows < 1) vrows = 1;
        if (track_h > 0 && total_lines > vrows) {
            int denom = total_lines;            /* lines spanned by track  */
            int thumb_h = (vrows * track_h) / denom;
            int max_off = total_lines - vrows;  /* > 0 here                */
            int thumb_y;
            int s = scroll;
            if (s > max_off) s = max_off;
            if (thumb_h < 8) thumb_h = 8;       /* keep it grabbable       */
            if (thumb_h > track_h) thumb_h = track_h;
            thumb_y = body.y + (max_off > 0
                                ? (s * (track_h - thumb_h)) / max_off : 0);
            if (thumb_y < body.y) thumb_y = body.y;
            if (thumb_y + thumb_h > body.y + track_h)
                thumb_y = body.y + track_h - thumb_h;
            /* faint track + brighter thumb */
            gfx_blend(cv, track_x, body.y, CV_SBAR_W, track_h, 0x30000000u);
            gfx_fill(cv, track_x, thumb_y, CV_SBAR_W, thumb_h, TH_BORDER_LT);
        }
    }
}

/*
 * Left-click in the code view: give it keyboard focus and place the shared
 * a->editor caret at the clicked cell. Geometry mirrors panel_code exactly
 * (cv_body + cv_gutter_w + a->code_scroll), and the visual->char mapping is
 * tab-aware (cv_charcol_from_vis) so the caret lands where the glyph is drawn.
 * shift extends the existing selection instead of dropping it.
 */
int panel_code_click(Ide* a, Rect r, int mx, int my, int shift) {
    Editor* e;
    Rect body;
    int line_h, gutter_w, code_x, scroll, total, row, ln, lstart, llen, viscol, ci;
    int slen;

    if (!a || r.w <= 0 || r.h <= 0) return 0;
    e = &a->editor;
    body = cv_body(r);
    if (body.h <= 0) return 0;

    /* Take keyboard focus for the code view (drop the editor's own focus). */
    a->codeview_focus = 1;
    e->focused = 0;
    e->ac_active = 0;

    if (my < body.y) return 1;          /* header click: focus only, no caret move */

    line_h   = GFX_FH;
    gutter_w = g_line_numbers ? cv_gutter_w() : 0;   /* mirror panel_code */
    code_x   = body.x + gutter_w + PAD;

    scroll = a->code_scroll; if (scroll < 0) scroll = 0;

    slen = a->src_len;
    if (slen < 0) slen = 0;
    if (slen > IDE_SRC_CAP) slen = IDE_SRC_CAP;
    total = cv_count_lines(a->src, slen);
    if (total < 1) total = 1;

    row = (my - body.y) / line_h;
    ln  = scroll + row;
    if (ln < 0) ln = 0;
    if (ln >= total) ln = total - 1;

    /* Shift+click extends the selection from the existing caret (anchor once). */
    if (shift) { if (e->sel_anchor_off < 0) e->sel_anchor_off = ide_editor_caret_off(a); }
    else         e->sel_anchor_off = -1;

    lstart = ide_editor_line_start(a, ln);
    llen   = ide_editor_line_len(a, ln);
    viscol = (mx - code_x) / GFX_FW;
    if (viscol < 0) viscol = 0;
    ci = cv_charcol_from_vis(a->src + lstart, llen, viscol);

    e->caret_line = ln;
    e->caret_col  = ci;
    e->want_col   = ci;
    return 1;
}
