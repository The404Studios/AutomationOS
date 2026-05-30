/*
 * ide_build.c -- the BUILD panel + Build/Run actions.
 *
 * ide_do_build() runs the native toolchain (tc_build) on the open file and
 * caches the TcResult in static storage. ide_do_run() SYS_SPAWNs the produced
 * ELF. panel_build() renders the cached result: a status line, output info,
 * the toolchain message, a run message, a diagnostics list and an asm preview.
 *
 * Freestanding C: no libc/malloc/stdio. All state is static; every draw is
 * clipped to the supplied Rect and bounded; all pointer use is NULL-safe.
 */
#include "ide_build.h"
#include "ide_gfx.h"
#include "ide_theme.h"
#include "ide_sys.h"

#ifndef SYS_SPAWN
#define SYS_SPAWN 16
#endif

/* ---- cached build state ------------------------------------------------- */
static TcResult g_res;
static int      g_have;
static char     g_runmsg[96];

/* ---- layout constants local to this panel ------------------------------- */
#define BP_HEAD_H     (ROW_H + 2)        /* "BUILD" header bar height          */
#define BP_LINE_H     GFX_FH             /* one text row                       */
#define BP_DIAG_MAX   8                  /* diagnostics rows shown             */

/* ---- tiny static string helpers (bounded, NUL-terminated) --------------- */

/* Append literal text to buf at *pi; never overruns cap. */
static void bp_append(char* buf, int cap, int* pi, const char* s) {
    int i = *pi;
    if (!s) { buf[i] = 0; return; }
    for (; *s && i < cap - 1; s++) buf[i++] = *s;
    buf[i] = 0;
    *pi = i;
}

/* Append a signed integer (decimal) to buf at *pi. */
static void bp_append_int(char* buf, int cap, int* pi, int v) {
    char num[16];
    int n = ide_itoa(v, num);
    if (n < 0) n = 0;
    if (n > (int)sizeof(num) - 1) n = (int)sizeof(num) - 1;
    num[n] = 0;
    bp_append(buf, cap, pi, num);
}

/* Human name for the detected toolchain language. */
static const char* bp_lang_name(TcLang l) {
    switch (l) {
        case LANG_C:      return "C";
        case LANG_ASM:    return "ASM";
        case LANG_CPP:    return "C++";
        case LANG_CSHARP: return "C#";
        default:          return "?";
    }
}

/* Draw one clipped text line at row y inside [x, x+w); returns the next y. */
static int bp_line(Canvas* cv, int x, int y, int w, int bot,
                   const char* s, uint32_t col) {
    if (y + BP_LINE_H > bot) return y;          /* no vertical room */
    if (w > 0 && s && s[0])
        gfx_text_clip(cv, x, y, s, col, x, w);
    return y + BP_LINE_H;
}

/* ===========================================================================
 * Actions
 * ===========================================================================*/

void ide_do_build(Ide* a) {
    if (!a || !a->cur_file[0]) return;
    tc_build(a->cur_file, &g_res);
    g_have = 1;
    g_runmsg[0] = 0;
}

void ide_do_run(Ide* a) {
    (void)a;
    if (!g_have || !g_res.ok) {
        ide_strlcpy(g_runmsg, "nothing built", (int)sizeof(g_runmsg));
        return;
    }

    long pid = ide_sc(SYS_SPAWN, (long)g_res.out_path, 0, 0, 0, 0, 0);

    int i = 0;
    if (pid >= 0) {
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, "spawned ");
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, g_res.out_path);
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, " pid ");
        bp_append_int(g_runmsg, (int)sizeof(g_runmsg), &i, (int)pid);
    } else {
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, "spawn failed (");
        bp_append_int(g_runmsg, (int)sizeof(g_runmsg), &i, (int)pid);
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, ")");
    }
}

int ide_build_active(void) {
    return g_have;
}

/* ===========================================================================
 * Panel
 * ===========================================================================*/

void panel_build(Ide* a, Canvas* cv, Rect r) {
    (void)a;
    if (!cv || r.w <= 0 || r.h <= 0) return;

    /* background */
    gfx_fill(cv, r.x, r.y, r.w, r.h, TH_PANEL2);

    /* header bar */
    gfx_fill(cv, r.x, r.y, r.w, BP_HEAD_H, TH_HEADER);
    gfx_hline(cv, r.x, r.y + BP_HEAD_H - 1, r.w, TH_BORDER);
    {
        int hx = r.x + PAD;
        int hw = r.w - 2 * PAD;
        int hy = r.y + (BP_HEAD_H - GFX_FH) / 2;
        if (hw > 0) gfx_text_clip(cv, hx, hy, "BUILD", TH_TEXT, hx, hw);
    }

    int x   = r.x + PAD;
    int w   = r.w - 2 * PAD;
    int y   = r.y + BP_HEAD_H + PAD;
    int bot = r.y + r.h;
    if (w <= 0) return;

    /* no result yet: centred hint */
    if (!g_have) {
        const char* hint = "Press B to build the open file";
        int tw = gfx_textw(hint);
        int hx = x + (w - tw) / 2;
        if (hx < x) hx = x;
        int hy = r.y + (r.h - GFX_FH) / 2;
        if (hy < y) hy = y;
        gfx_text_clip(cv, hx, hy, hint, TH_TEXT_DIM, x, w);
        return;
    }

    /* status line: OK/FAILED + [lang] */
    {
        char buf[48];
        int i = 0;
        const char* st = g_res.ok ? "OK" : "FAILED";
        uint32_t stc = g_res.ok ? TH_GREEN : TH_RED;
        bp_append(buf, (int)sizeof(buf), &i, st);
        bp_append(buf, (int)sizeof(buf), &i, " [");
        bp_append(buf, (int)sizeof(buf), &i, bp_lang_name(g_res.lang));
        bp_append(buf, (int)sizeof(buf), &i, "]");
        y = bp_line(cv, x, y, w, bot, buf, stc);
    }

    /* output path */
    {
        char buf[176];
        int i = 0;
        bp_append(buf, (int)sizeof(buf), &i, "out: ");
        bp_append(buf, (int)sizeof(buf), &i, g_res.out_path);
        y = bp_line(cv, x, y, w, bot, buf, TH_TEXT_DIM);
    }

    /* sizes */
    {
        char buf[48];
        int i = 0;
        bp_append(buf, (int)sizeof(buf), &i, "code ");
        bp_append_int(buf, (int)sizeof(buf), &i, g_res.code_len);
        bp_append(buf, (int)sizeof(buf), &i, "b  elf ");
        bp_append_int(buf, (int)sizeof(buf), &i, g_res.elf_len);
        bp_append(buf, (int)sizeof(buf), &i, "b");
        y = bp_line(cv, x, y, w, bot, buf, TH_TEXT_DIM);
    }

    /* toolchain message */
    if (g_res.message[0])
        y = bp_line(cv, x, y, w, bot, g_res.message, TH_TEXT);

    /* run message */
    if (g_runmsg[0])
        y = bp_line(cv, x, y, w, bot, g_runmsg, TH_CYAN);

    /* diagnostics */
    {
        int nd = g_res.ndiags;
        if (nd < 0) nd = 0;
        if (nd > TC_MAXDIAG) nd = TC_MAXDIAG;

        char hdr[32];
        int i = 0;
        bp_append(hdr, (int)sizeof(hdr), &i, "Diagnostics (");
        bp_append_int(hdr, (int)sizeof(hdr), &i, nd);
        bp_append(hdr, (int)sizeof(hdr), &i, "):");
        y = bp_line(cv, x, y, w, bot, hdr, TH_TEXT_DIM);

        int shown = nd < BP_DIAG_MAX ? nd : BP_DIAG_MAX;
        for (int d = 0; d < shown; d++) {
            if (y + BP_LINE_H > bot) break;
            char buf[160];
            int j = 0;
            bp_append(buf, (int)sizeof(buf), &j, "line ");
            bp_append_int(buf, (int)sizeof(buf), &j, g_res.diags[d].line);
            bp_append(buf, (int)sizeof(buf), &j, ": ");
            bp_append(buf, (int)sizeof(buf), &j, g_res.diags[d].msg);
            y = bp_line(cv, x, y, w, bot, buf, TH_ORANGE);
        }
    }

    /* asm preview: split on '\n', fill remaining rows */
    if (g_res.asm_preview[0] && y + BP_LINE_H <= bot) {
        y = bp_line(cv, x, y, w, bot, "ASM:", TH_TEXT_DIM);

        const char* s = g_res.asm_preview;
        int cap = (int)sizeof(g_res.asm_preview);
        int p = 0;
        char line[160];
        while (p < cap && s[p] && y + BP_LINE_H <= bot) {
            int li = 0;
            while (p < cap && s[p] && s[p] != '\n') {
                if (li < (int)sizeof(line) - 1) line[li++] = s[p];
                p++;
            }
            line[li] = 0;
            if (p < cap && s[p] == '\n') p++;   /* skip newline */
            y = bp_line(cv, x, y, w, bot, line, TH_TEXT_FAINT);
        }
    }
}
