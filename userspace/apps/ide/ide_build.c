/*
 * ide_build.c -- the BUILD panel + Build/Run actions.
 *
 * ide_do_build() runs the native toolchain (tc_build) on the open file and
 * caches the TcResult in static storage. ide_do_run() SYS_SPAWNs the produced
 * ELF. panel_build() renders the cached result: a status line, output info,
 * the toolchain message, a run message, a diagnostics list and an asm preview.
 *
 * Improvements over v1:
 *   - Error-line highlighting: ide_build_line_severity() lets the editor paint
 *     error/warning backgrounds. Diagnostics are colorized red vs yellow.
 *   - Click-to-jump: panel_build_click() jumps the editor caret to a clicked
 *     diagnostic's source line.
 *   - Scrolling: the panel scrolls via mouse wheel (panel_build_scroll).
 *   - Build flash: the BUILD tab briefly flashes green/red after a build.
 *   - Build time: measured in ide_do_build(), displayed in the status line.
 *   - Warning/error colorization: diagnostic lines prefixed "error"/"warning"
 *     get TH_RED / TH_ORANGE respectively.
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
#ifndef SYS_WAITPID
#define SYS_WAITPID 6
#endif
#ifndef SYS_YIELD
#define SYS_YIELD 15
#endif

/* ---- cached build state ------------------------------------------------- */
static TcResult g_res;
static int      g_have;
static char     g_runmsg[96];

/* ---- child process tracking (for exit-code polling) -------------------- */
static long     g_child_pid  = -1;   /* PID of the running child, or -1     */
static int      g_child_done = 0;    /* 1 once we've reaped the child       */

/* ---- build timing ------------------------------------------------------ */
static int      g_build_ms   = 0;    /* duration of last build (ms)         */

/* ---- build flash (tab tint after build) -------------------------------- */
static int      g_flash_ms   = 0;    /* remaining flash time (ms), 0=off    */
static uint32_t g_flash_col  = 0;    /* TH_GREEN or TH_RED                  */
#define FLASH_DURATION 600            /* total flash time in ms              */

/* ---- scroll offset for the panel body ---------------------------------- */
static int      g_scroll     = 0;    /* top visible row (in BP_LINE_H units)*/

/* ---- layout constants local to this panel ------------------------------- */
#define BP_HEAD_H     (ROW_H + 2)        /* "BUILD" header bar height          */
#define BP_LINE_H     GFX_FH             /* one text row                       */
#define BP_DIAG_MAX   64                 /* show all diagnostics (scrollable)  */

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

/* Case-insensitive check: does `msg` contain "warning" anywhere? */
static int bp_is_warning(const char* msg) {
    if (!msg) return 0;
    for (int i = 0; msg[i]; i++) {
        const char* w = "warning";
        int j = 0;
        while (w[j]) {
            char c = msg[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != w[j]) break;
            j++;
        }
        if (!w[j]) return 1;
    }
    return 0;
}

/* Classify a diagnostic: 2=warning, 1=error (default). */
static int bp_diag_severity(const TcDiag* d) {
    if (!d) return 1;
    return bp_is_warning(d->msg) ? 2 : 1;
}

/* Colour for a diagnostic line based on severity. */
static uint32_t bp_diag_color(int severity) {
    return severity == 2 ? TH_ORANGE : TH_RED;
}

/* Draw one clipped text line at row y inside [x, x+w); returns the next y.
 * `row_idx` is checked against g_scroll to decide whether to actually paint. */
static int bp_line(Canvas* cv, int x, int y, int w, int bot,
                   const char* s, uint32_t col, int* row_idx) {
    int ri = *row_idx;
    (*row_idx)++;
    if (ri < g_scroll) return y;           /* above the scroll window */
    if (y + BP_LINE_H > bot) return y;     /* no vertical room        */
    if (w > 0 && s && s[0])
        gfx_text_clip(cv, x, y, s, col, x, w);
    return y + BP_LINE_H;
}

/* ---- per-diagnostic geometry cache for click-to-jump -------------------- */
#define BP_CLICK_MAX TC_MAXDIAG
static struct { int y, h, diag_idx; } g_click_rows[BP_CLICK_MAX];
static int g_click_nrows = 0;

/* ===========================================================================
 * Actions
 * ===========================================================================*/

/* Derive "/sbin/<base>" from a source path (basename without extension), e.g.
 * "/usr/src/derby/derby.c" -> "/sbin/derby". Returns 1 on success. */
static int ide_prebuilt_path(const char* src, char* out, int cap) {
    if (!src || !out || cap < 8) return 0;
    int i, last = -1;
    for (i = 0; src[i]; i++) if (src[i] == '/') last = i;
    const char* base = src + last + 1;
    int bl = 0; while (base[bl] && base[bl] != '.') bl++;
    if (bl <= 0) return 0;
    if (bl > cap - 8) bl = cap - 8;   /* leave room for "/sbin/" + NUL */
    const char* pre = "/sbin/";
    int p = 0;
    for (i = 0; pre[i] && p < cap - 1; i++) out[p++] = pre[i];
    for (i = 0; i < bl && p < cap - 1; i++) out[p++] = base[i];
    out[p] = 0;
    return 1;
}

/* True if `path` is an existing ELF binary (first 4 bytes == \x7f E L F). This
 * is stricter than "openable" so the prebuilt fallback never points Run at a
 * non-executable file that merely happens to share the name. */
static int ide_is_elf(const char* path) {
    char m[4];
    int n = ide_read_file(path, m, (int)sizeof(m));
    if (n < 4) return 0;
    return (unsigned char)m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
}

/* True if `path` begins with `prefix`. */
static int ide_starts_with(const char* path, const char* prefix) {
    int i = 0;
    while (prefix[i]) { if (path[i] != prefix[i]) return 0; i++; }
    return 1;
}

/* When the single-file on-device compile fails AND a matching prebuilt /sbin
 * binary exists (e.g. derby, which links g3d/wl/bitfont -- beyond a single-file
 * compiler), present that binary as "built" so the user can Run the real program
 * from the IDE. The /sbin twin existing is the gate: only shipped apps have one,
 * so a scratch file with a fixable error never triggers this. Returns 1 if it
 * converted the failure into a runnable prebuilt result. */
static int ide_try_prebuilt(Ide* a) {
    char sbinp[160];
    /* Only shipped samples (under /usr/src) fall back to a prebuilt binary, so a
     * user's scratch copy elsewhere named like a shipped app is never shadowed. */
    if (!ide_starts_with(a->cur_file, "/usr/src/")) return 0;
    if (!ide_prebuilt_path(a->cur_file, sbinp, (int)sizeof(sbinp))) return 0;
    if (!ide_is_elf(sbinp)) return 0;
    ide_strlcpy(g_res.out_path, sbinp, (int)sizeof(g_res.out_path));
    ide_strlcpy(g_res.out_dir, "/sbin", (int)sizeof(g_res.out_dir));
    g_res.ok = 1;
    g_res.elf_len = 0;
    g_res.ndiags = 0;
    g_res.message[0] = 0;
    ide_strlcpy(g_res.message,
                "prebuilt app (links external libraries the single-file compiler "
                "can't) -- press R to Run ", (int)sizeof(g_res.message));
    int ml = ide_strlen(g_res.message);
    ide_strlcpy(g_res.message + ml, sbinp, (int)sizeof(g_res.message) - ml);
    return 1;
}

void ide_do_build(Ide* a) {
    if (!a || !a->cur_file[0]) {
        g_have = 1;
        g_res.ok = 0;
        g_res.message[0] = 0;
        ide_strlcpy(g_res.message, "no file open -- open or create a file first",
                     (int)sizeof(g_res.message));
        g_res.lang = LANG_C;
        g_res.ndiags = 0;
        g_res.code_len = 0;
        g_res.elf_len = 0;
        g_res.out_path[0] = 0;
        g_res.out_dir[0] = 0;
        g_res.asm_preview[0] = 0;
        g_runmsg[0] = 0;
        g_build_ms = 0;
        g_flash_ms  = FLASH_DURATION;
        g_flash_col = TH_RED;
        g_scroll = 0;
        return;
    }

    /* Project build: flush the editor, compile the project ENTRY (src/main.c),
     * and emit the artifact into <root>/build/<Name>.elf so Run launches the
     * project's own binary. Loose-file builds compile the open file straight to
     * /Desktop/<base>.elf (override OFF). */
    char build_src[IDE_PATH];
    const char* src_to_build = a->cur_file;
    int proj = a->project.active;
    if (proj) {
        ide_editor_save(a);                          /* flush editor -> disk      */
        int n = ide_strlen(a->project.root);
        ide_strlcpy(build_src, a->project.root, IDE_PATH);
        if (n < IDE_PATH - 1) build_src[n++] = '/';
        ide_strlcpy(build_src + n, a->project.entry, IDE_PATH - n);
        src_to_build = build_src;

        char out_dir[IDE_PATH];
        int m = ide_strlen(a->project.root);
        ide_strlcpy(out_dir, a->project.root, IDE_PATH);
        if (m < IDE_PATH - 1) out_dir[m++] = '/';
        ide_strlcpy(out_dir + m, "build", IDE_PATH - m);
        tc_set_output_override(out_dir, a->project.name);
    }

    long t0 = ide_ticks_ms();
    tc_build(src_to_build, &g_res);
    long t1 = ide_ticks_ms();
    g_build_ms = (int)(t1 - t0);
    if (g_build_ms < 0) g_build_ms = 0;

    if (proj) tc_set_output_override(0, 0);          /* one-shot: clear override  */

    /* If the on-device single-file compile failed but this is a shipped app with
     * a prebuilt /sbin binary (e.g. derby), present it as runnable instead. */
    if (!g_res.ok)
        ide_try_prebuilt(a);

    g_have = 1;
    g_runmsg[0] = 0;
    g_scroll = 0;

    /* Flash green on success, red on failure. */
    g_flash_ms  = FLASH_DURATION;
    g_flash_col = g_res.ok ? TH_GREEN : TH_RED;

    if (g_res.ok)
        ide_reveal_dir(a, g_res.out_dir, g_res.out_path);
}

void ide_do_run(Ide* a) {
    (void)a;
    if (!g_have || !g_res.ok) {
        ide_strlcpy(g_runmsg, "nothing built -- press B first", (int)sizeof(g_runmsg));
        return;
    }

    long pid = ide_sc(SYS_SPAWN, (long)g_res.out_path, 0, 0, 0, 0, 0);

    int i = 0;
    if (pid > 0) {
        g_child_pid  = pid;
        g_child_done = 0;
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, "Running... pid ");
        bp_append_int(g_runmsg, (int)sizeof(g_runmsg), &i, (int)pid);

        for (int y = 0; y < 4; y++)
            ide_sc(SYS_YIELD, 0, 0, 0, 0, 0, 0);

        int status = 0;
        long w = ide_sc(SYS_WAITPID, (long)pid, (long)&status, 1 /*WNOHANG*/, 0, 0, 0);
        if (w > 0) {
            g_child_done = 1;
            g_child_pid  = -1;
            i = 0;
            bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, "Process exited with code ");
            bp_append_int(g_runmsg, (int)sizeof(g_runmsg), &i, status);
        }
    } else {
        g_child_pid  = -1;
        g_child_done = 0;
        int ev = (int)(-pid);
        const char *reason;
        switch (ev) {
            case  2: reason = "file not found"; break;
            case  8: reason = "not a valid executable"; break;
            case 12: reason = "out of memory"; break;
            case 13: reason = "permission denied"; break;
            default: reason = "spawn error"; break;
        }
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, reason);
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, " (rc=");
        bp_append_int(g_runmsg, (int)sizeof(g_runmsg), &i, (int)pid);
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, ")");
    }
}

int ide_run_poll(void) {
    if (g_child_pid <= 0 || g_child_done) return 0;

    int status = 0;
    long w = ide_sc(SYS_WAITPID, g_child_pid, (long)&status, 1 /*WNOHANG*/, 0, 0, 0);
    if (w > 0) {
        g_child_done = 1;
        g_child_pid  = -1;
        int i = 0;
        bp_append(g_runmsg, (int)sizeof(g_runmsg), &i, "Process exited with code ");
        bp_append_int(g_runmsg, (int)sizeof(g_runmsg), &i, status);
        return 1;
    }
    return 0;
}

int ide_build_active(void) {
    return g_have;
}

/* IDE-CONTEXT-0: what-changed accessors for the status bars. */
int ide_build_ok(void)         { return g_have && g_res.ok; }
int ide_build_diag_count(void) { return g_have ? g_res.ndiags : 0; }

/* IDE-FORGE-0: the last Run message (g_runmsg) for the ACTIONS deck + Pulse. */
const char* ide_run_msg(void)  { return g_runmsg; }

/* ===========================================================================
 * Query APIs for other modules
 * ===========================================================================*/

int ide_build_line_severity(int ln) {
    if (!g_have || g_res.ok) return 0;
    int nd = g_res.ndiags;
    if (nd < 0) nd = 0;
    if (nd > TC_MAXDIAG) nd = TC_MAXDIAG;
    int ln1 = ln + 1;                     /* diagnostics store 1-based lines */
    int worst = 0;
    for (int d = 0; d < nd; d++) {
        if (g_res.diags[d].line == ln1) {
            int sev = bp_diag_severity(&g_res.diags[d]);
            if (sev > worst) worst = sev;
            if (worst == 1) return 1;     /* error is worst; short-circuit */
        }
    }
    return worst;
}

uint32_t ide_build_flash_color(void) {
    if (g_flash_ms <= 0) return 0;
    int alpha = (g_flash_ms * 0x60) / FLASH_DURATION;
    if (alpha < 0) alpha = 0;
    if (alpha > 0x60) alpha = 0x60;
    return ((uint32_t)alpha << 24) | (g_flash_col & 0x00FFFFFFu);
}

void ide_build_tick(int dt_ms) {
    if (g_flash_ms > 0) {
        g_flash_ms -= dt_ms;
        if (g_flash_ms < 0) g_flash_ms = 0;
    }
}

int ide_build_time_ms(void) {
    return g_build_ms;
}

void panel_build_scroll(int delta) {
    g_scroll += delta;
    if (g_scroll < 0) g_scroll = 0;
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

    /* Flash tint over the header when active. */
    {
        uint32_t fc = ide_build_flash_color();
        if (fc)
            gfx_blend(cv, r.x, r.y, r.w, BP_HEAD_H, fc);
    }

    {
        int hx = r.x + PAD;
        int hw = r.w - 2 * PAD;
        int hy = r.y + (BP_HEAD_H - GFX_FH) / 2;
        if (hw > 0) gfx_text_clip(cv, hx, hy, "BUILD", TH_TEXT, hx, hw);

        /* Build time right-aligned in the header. */
        if (g_have && g_build_ms > 0) {
            char tbuf[32];
            int ti = 0;
            bp_append(tbuf, (int)sizeof(tbuf), &ti, "(");
            bp_append_int(tbuf, (int)sizeof(tbuf), &ti, g_build_ms);
            bp_append(tbuf, (int)sizeof(tbuf), &ti, " ms)");
            int tw = gfx_textw(tbuf);
            int tx = r.x + r.w - PAD - tw;
            if (tx > hx + gfx_textw("BUILD") + PAD)
                gfx_text_clip(cv, tx, hy, tbuf, TH_TEXT_DIM, hx, hw);
        }
    }

    int x   = r.x + PAD;
    int w   = r.w - 2 * PAD;
    int y   = r.y + BP_HEAD_H + PAD;
    int bot = r.y + r.h;
    if (w <= 0) return;

    g_click_nrows = 0;

    int row_idx = 0;

    /* no result yet: centred hint */
    if (!g_have) {
        const char* hint = "Press Ctrl+B to build the open file";
        int tw = gfx_textw(hint);
        int hx = x + (w - tw) / 2;
        if (hx < x) hx = x;
        int hy = r.y + (r.h - GFX_FH) / 2;
        if (hy < y) hy = y;
        gfx_text_clip(cv, hx, hy, hint, TH_TEXT_DIM, x, w);
        return;
    }

    /* status line: OK/FAILED + [lang] + build time */
    {
        char buf[80];
        int i = 0;
        const char* st = g_res.ok ? "OK" : "FAILED";
        uint32_t stc = g_res.ok ? TH_GREEN : TH_RED;
        bp_append(buf, (int)sizeof(buf), &i, st);
        bp_append(buf, (int)sizeof(buf), &i, " [");
        bp_append(buf, (int)sizeof(buf), &i, bp_lang_name(g_res.lang));
        bp_append(buf, (int)sizeof(buf), &i, "]");
        if (g_build_ms > 0) {
            bp_append(buf, (int)sizeof(buf), &i, " ");
            bp_append_int(buf, (int)sizeof(buf), &i, g_build_ms);
            bp_append(buf, (int)sizeof(buf), &i, " ms");
        }
        y = bp_line(cv, x, y, w, bot, buf, stc, &row_idx);
    }

    /* output path */
    {
        char buf[176];
        int i = 0;
        bp_append(buf, (int)sizeof(buf), &i, "out: ");
        bp_append(buf, (int)sizeof(buf), &i, g_res.out_path);
        y = bp_line(cv, x, y, w, bot, buf, TH_TEXT_DIM, &row_idx);
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
        y = bp_line(cv, x, y, w, bot, buf, TH_TEXT_DIM, &row_idx);
    }

    /* toolchain message */
    if (g_res.message[0])
        y = bp_line(cv, x, y, w, bot, g_res.message, TH_TEXT, &row_idx);

    /* run message */
    if (g_runmsg[0])
        y = bp_line(cv, x, y, w, bot, g_runmsg, TH_CYAN, &row_idx);

    /* run hint */
    if (g_res.ok && !g_runmsg[0])
        y = bp_line(cv, x, y, w, bot,
                    "Press Ctrl+R to run  (also appears as a Desktop icon)",
                    TH_CYAN, &row_idx);
    else if (g_res.ok && g_runmsg[0] && g_child_pid > 0 && !g_child_done)
        y = bp_line(cv, x, y, w, bot,
                    "(waiting for process to exit...)", TH_TEXT_DIM, &row_idx);

    /* diagnostics */
    {
        int nd = g_res.ndiags;
        if (nd < 0) nd = 0;
        if (nd > TC_MAXDIAG) nd = TC_MAXDIAG;

        /* Count warnings vs errors for the header. */
        int nerrors = 0, nwarnings = 0;
        for (int d = 0; d < nd; d++) {
            if (bp_diag_severity(&g_res.diags[d]) == 2)
                nwarnings++;
            else
                nerrors++;
        }

        char hdr[64];
        int i = 0;
        bp_append(hdr, (int)sizeof(hdr), &i, "Diagnostics: ");
        bp_append_int(hdr, (int)sizeof(hdr), &i, nerrors);
        bp_append(hdr, (int)sizeof(hdr), &i, " error");
        if (nerrors != 1) bp_append(hdr, (int)sizeof(hdr), &i, "s");
        if (nwarnings > 0) {
            bp_append(hdr, (int)sizeof(hdr), &i, ", ");
            bp_append_int(hdr, (int)sizeof(hdr), &i, nwarnings);
            bp_append(hdr, (int)sizeof(hdr), &i, " warning");
            if (nwarnings != 1) bp_append(hdr, (int)sizeof(hdr), &i, "s");
        }
        y = bp_line(cv, x, y, w, bot, hdr, TH_TEXT_DIM, &row_idx);

        /* Render ALL diagnostics (scrollable). */
        int shown = nd < BP_DIAG_MAX ? nd : BP_DIAG_MAX;
        for (int d = 0; d < shown; d++) {
            if (y + BP_LINE_H > bot) break;
            char buf[160];
            int j = 0;
            int sev = bp_diag_severity(&g_res.diags[d]);
            bp_append(buf, (int)sizeof(buf), &j,
                      sev == 2 ? "warn " : "err  ");
            bp_append(buf, (int)sizeof(buf), &j, "line ");
            bp_append_int(buf, (int)sizeof(buf), &j, g_res.diags[d].line);
            bp_append(buf, (int)sizeof(buf), &j, ": ");
            bp_append(buf, (int)sizeof(buf), &j, g_res.diags[d].msg);

            int pre_y = y;
            int pre_ri = row_idx;
            y = bp_line(cv, x, y, w, bot, buf, bp_diag_color(sev), &row_idx);

            /* Record geometry for click-to-jump if the row was actually painted. */
            if (pre_ri >= g_scroll && y > pre_y && g_click_nrows < BP_CLICK_MAX) {
                g_click_rows[g_click_nrows].y = pre_y;
                g_click_rows[g_click_nrows].h = BP_LINE_H;
                g_click_rows[g_click_nrows].diag_idx = d;
                g_click_nrows++;
            }
        }
        if (nd > shown && y + BP_LINE_H <= bot) {
            char trunc[48];
            int j = 0;
            bp_append(trunc, (int)sizeof(trunc), &j, "... ");
            bp_append_int(trunc, (int)sizeof(trunc), &j, nd - shown);
            bp_append(trunc, (int)sizeof(trunc), &j, " more");
            y = bp_line(cv, x, y, w, bot, trunc, TH_TEXT_FAINT, &row_idx);
        }
    }

    /* asm preview */
    if (g_res.asm_preview[0] && y + BP_LINE_H <= bot) {
        y = bp_line(cv, x, y, w, bot, "ASM:", TH_TEXT_DIM, &row_idx);

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
            if (p < cap && s[p] == '\n') p++;
            y = bp_line(cv, x, y, w, bot, line, TH_TEXT_FAINT, &row_idx);
        }
    }

    /* Clamp scroll so it can't go past the last row. */
    {
        int total_rows = row_idx;
        int vis_rows = (bot - (r.y + BP_HEAD_H + PAD)) / BP_LINE_H;
        if (vis_rows < 1) vis_rows = 1;
        int max_scroll = total_rows - vis_rows;
        if (max_scroll < 0) max_scroll = 0;
        if (g_scroll > max_scroll) g_scroll = max_scroll;
    }

    /* Scroll hint when scrolled down. */
    if (g_scroll > 0) {
        char sbuf[24];
        int si = 0;
        bp_append(sbuf, (int)sizeof(sbuf), &si, "^ scroll ");
        bp_append_int(sbuf, (int)sizeof(sbuf), &si, g_scroll);
        int sw = gfx_textw(sbuf);
        int sx = r.x + r.w - PAD - sw;
        int sy = r.y + BP_HEAD_H + 1;
        if (sx > x)
            gfx_text_clip(cv, sx, sy, sbuf, TH_TEXT_FAINT, x, w);
    }
}

/* ===========================================================================
 * Click-to-jump handler
 * ===========================================================================*/

int panel_build_click(Ide* a, Rect r, int mx, int my) {
    if (!a || !g_have) return 0;
    if (mx < r.x || mx >= r.x + r.w || my < r.y || my >= r.y + r.h) return 0;

    for (int i = 0; i < g_click_nrows; i++) {
        if (my >= g_click_rows[i].y &&
            my <  g_click_rows[i].y + g_click_rows[i].h) {
            int di = g_click_rows[i].diag_idx;
            if (di >= 0 && di < g_res.ndiags) {
                int target_line = g_res.diags[di].line - 1;  /* 1-based -> 0-based */
                if (target_line < 0) target_line = 0;
                a->editor.caret_line = target_line;
                a->editor.caret_col  = 0;
                a->editor.want_col   = 0;
                a->editor.focused    = 1;
                a->term_focus        = 0;
                a->explorer_focused  = 0;
                return 1;
            }
        }
    }
    return 1;  /* consumed even if no diagnostic was hit */
}
