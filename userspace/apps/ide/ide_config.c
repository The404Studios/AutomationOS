/*
 * ide_config.c -- load/save the Settings knobs as a key=value text file.
 * Freestanding: no libc. See ide_config.h.
 */
#include "ide_config.h"
#include "ide_sys.h"      /* ide_read_file / ide_write_file / ide_itoa / ide_strlen / ide_sc */
#include "ide_gfx.h"      /* the g_* runtime knob vars + gfx_set_scale */

/* Durable diskfs syscalls (kernel/core/syscall/handlers.c). Return bytes on
 * success, or a negative errno (ENODEV when no disk -> we fall back to a file). */
#define SYS_PERSIST_READ   94
#define SYS_PERSIST_WRITE  95

/* Read the durable diskfs file into buf; returns bytes, or <0 if unavailable. */
static int persist_read(char* buf, int cap) {
    long r = ide_sc(SYS_PERSIST_READ, (long)IDE_PERSIST_NAME, (long)buf, (long)cap, 0, 0, 0);
    return (int)r;
}
/* Write buf to the durable diskfs file; returns bytes written, or <0. */
static int persist_write(const char* buf, int len) {
    long r = ide_sc(SYS_PERSIST_WRITE, (long)IDE_PERSIST_NAME, (long)buf, (long)len, 0, 0, 0);
    return (int)r;
}

/* One persisted setting: a key string bound to a live int var + clamp range. */
typedef struct {
    const char* key;
    int*        var;
    int         vmin, vmax;
} CfgRow;

/* Keep these keys STABLE across versions (the file is forward/backward tolerant:
 * unknown keys are skipped on load, missing keys keep their defaults). */
static CfgRow cfg_rows[] = {
    { "font_scale",   &g_ui_pct,       50, 250 },
    { "tab_width",    &g_tab_width,     1,   8 },
    { "blink_ms",     &g_blink_ms,    100,1000 },
    { "ac_visible",   &g_ac_visible,    1,   8 },
    { "ac_minpfx",    &g_ac_minpfx,     1,   5 },
    { "map_pan_step", &g_map_pan_step,  5,  60 },
    { "autocomplete", &g_autocomplete,  0,   1 },
    { "anno_gutter",  &g_anno_gutter,   0,   1 },
    { "line_numbers", &g_line_numbers,  0,   1 },
    { "auto_indent",  &g_auto_indent,   0,   1 },
    { "live_reparse", &g_live_reparse,  0,   1 },
    { "theme_mode",   &g_theme_mode,    0,   1 },
};
#define CFG_NROWS  ((int)(sizeof(cfg_rows) / sizeof(cfg_rows[0])))

/* Config files are tiny; a few hundred bytes is plenty. */
#define CFG_BUF_CAP  1024

static int cfg_streq_n(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Parse a non-negative decimal at s[..]; stop at the first non-digit. Returns
 * the value; *consumed gets the digit count (0 if none). */
static int cfg_atoi(const char* s, int len, int* consumed) {
    int v = 0, i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
    if (consumed) *consumed = i;
    return v;
}

/* Apply key[0..klen) = value to the matching row (clamped). */
static void cfg_apply_kv(const char* key, int klen, int value) {
    for (int r = 0; r < CFG_NROWS; r++) {
        int kl = ide_strlen(cfg_rows[r].key);
        if (kl == klen && cfg_streq_n(cfg_rows[r].key, key, klen)) {
            int v = value;
            if (v < cfg_rows[r].vmin) v = cfg_rows[r].vmin;
            if (v > cfg_rows[r].vmax) v = cfg_rows[r].vmax;
            *cfg_rows[r].var = v;
            return;
        }
    }
    /* unknown key: ignore (forward compatible) */
}

void ide_config_load(void) {
    static char buf[CFG_BUF_CAP];
    int n = persist_read(buf, CFG_BUF_CAP);            /* durable diskfs first */
    if (n <= 0) n = ide_read_file(IDE_CONFIG_FALLBACK, buf, CFG_BUF_CAP);  /* session file */
    if (n <= 0) return;                 /* no config yet -> keep defaults */
    if (n > CFG_BUF_CAP) n = CFG_BUF_CAP;

    /* Parse "key=value" lines. Tolerant of blank lines / stray whitespace. */
    int i = 0;
    while (i < n) {
        /* skip leading whitespace / line breaks */
        while (i < n && (buf[i] == ' ' || buf[i] == '\t' ||
                         buf[i] == '\n' || buf[i] == '\r')) i++;
        int ks = i;
        while (i < n && buf[i] != '=' && buf[i] != '\n') i++;
        if (i >= n || buf[i] != '=') {  /* no '=' on this line: skip to EOL */
            while (i < n && buf[i] != '\n') i++;
            continue;
        }
        int klen = i - ks;
        i++;                            /* step past '=' */
        int used = 0;
        int val = cfg_atoi(buf + i, n - i, &used);
        i += used;
        if (klen > 0 && used > 0) cfg_apply_kv(buf + ks, klen, val);
        while (i < n && buf[i] != '\n') i++;   /* to end of line */
    }

    /* Font scale drives the glyph cell -- recompute it after loading. */
    gfx_set_scale(g_ui_pct);
}

void ide_config_save(void) {
    static char buf[CFG_BUF_CAP];
    int p = 0;
    for (int r = 0; r < CFG_NROWS && p < CFG_BUF_CAP - 32; r++) {
        const char* k = cfg_rows[r].key;
        for (int j = 0; k[j] && p < CFG_BUF_CAP - 16; j++) buf[p++] = k[j];
        buf[p++] = '=';
        char nb[16];
        int nn = ide_itoa(*cfg_rows[r].var, nb);
        for (int j = 0; j < nn && p < CFG_BUF_CAP - 2; j++) buf[p++] = nb[j];
        buf[p++] = '\n';
    }
    /* Prefer durable diskfs; fall back to the session file if no disk present. */
    if (persist_write(buf, p) != p)
        ide_write_file(IDE_CONFIG_FALLBACK, buf, p);
}
