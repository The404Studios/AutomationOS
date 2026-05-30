/*
 * ttf.c -- Anti-aliased, scalable TrueType rendering via vendored stb_truetype.
 * =============================================================================
 *
 * Freestanding (ring 3, no stdio). A .ttf is mapped zero-copy with SYS_MAP_FILE
 * (libc map_file()); glyphs are rasterized by stb_truetype to 8-bit coverage
 * and alpha-blended onto an ARGB32 buffer for true anti-aliasing.
 *
 * stb_truetype's STBTT_* hooks are wired to the userspace libc/libm BELOW,
 * *before* the header is included, so the header never pulls in any host
 * <math.h>/<stdlib.h>/<string.h>. Every external symbol it references
 * (floor/ceil/sqrt/pow/fmod/cos/acos/fabs, malloc/free, memcpy/memset/strlen)
 * is provided by linking math.c + stdlib.c + string.c (or libc.a + libm).
 *
 * Build (flags DIRECT on the command line):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 *       -fno-pic -fno-pie -mno-red-zone -O2 -c ttf.c -o ttf.o
 */

#include "ttf.h"

/*
 * <stddef.h> is a freestanding-conforming, compiler-provided header (legal with
 * -ffreestanding): it gives stb_truetype the size_t / NULL / ptrdiff_t it needs
 * without dragging in any hosted libc.
 */
#include <stddef.h>

/* ----------------------------------------------------------------------------
 * Minimal external declarations for the libc/libm symbols we depend on.
 * (We avoid including the libc headers here to keep this TU self-contained and
 *  immune to macro clashes with stb_truetype; the linker resolves these from
 *  string.c / stdlib.c / math.c — i.e. libc.a + libm.)
 * ------------------------------------------------------------------------- */
typedef unsigned long ttf_size_t;

/* libc: heap + mem + string */
extern void *malloc(ttf_size_t size);
extern void  free(void *ptr);
extern void *memcpy(void *dest, const void *src, ttf_size_t n);
extern void *memset(void *dest, int val, ttf_size_t n);
extern unsigned long strlen(const char *s);

/* libc: syscalls */
extern long write(int fd, const void *buf, unsigned long count);
extern int  map_file(const char *path, void **addr, unsigned long *size);

/* libm: math used by stb_truetype's STBTT_* hooks */
extern double floor(double x);
extern double ceil(double x);
extern double sqrt(double x);
extern double pow(double x, double y);
extern double fmod(double x, double y);
extern double cos(double x);
extern double acos(double x);
extern double fabs(double x);

/* ----------------------------------------------------------------------------
 * Serial diagnostics (SYS_WRITE=3, fd 1).
 * ------------------------------------------------------------------------- */
static void ttf_log(const char *m) { write(1, m, strlen(m)); }

/* ----------------------------------------------------------------------------
 * Wire stb_truetype to our freestanding deps. Define ALL STBTT_* hooks so the
 * header takes none of its default #include <math.h>/<stdlib.h>/<string.h>
 * branches. STBTT_assert -> no-op (we never want host assert()).
 * ------------------------------------------------------------------------- */
#define STBTT_ifloor(x)    ((int) floor(x))
#define STBTT_iceil(x)     ((int) ceil(x))
#define STBTT_sqrt(x)      sqrt(x)
#define STBTT_pow(x, y)    pow(x, y)
#define STBTT_fmod(x, y)   fmod(x, y)
#define STBTT_cos(x)       cos(x)
#define STBTT_acos(x)      acos(x)
#define STBTT_fabs(x)      fabs(x)

#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u)   ((void)(u), free(x))

#define STBTT_assert(x)    ((void)0)

#define STBTT_strlen(x)    strlen(x)
#define STBTT_memcpy       memcpy
#define STBTT_memset       memset

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* ----------------------------------------------------------------------------
 * Active font state.
 * ------------------------------------------------------------------------- */
static stbtt_fontinfo g_font;
static int            g_loaded = 0;
static unsigned char *g_ttf_data = 0;     /* mapped, read-only initrd buffer  */
static unsigned long  g_ttf_size = 0;

/* ----------------------------------------------------------------------------
 * Glyph coverage cache keyed by (codepoint, px_size).
 *
 * Each entry owns an 8-bit coverage bitmap (one byte per pixel, 0..255) as
 * produced by stbtt_GetCodepointBitmap, plus its dimensions, draw offsets and
 * the *unscaled* horizontal advance (we scale it per call). A small fixed-size
 * direct-mapped-ish cache with linear probing keeps the hot ASCII path fast
 * without dragging in a hashmap.
 * ------------------------------------------------------------------------- */
#define TTF_CACHE_SLOTS 256

typedef struct {
    int            used;
    int            codepoint;
    int            px_size;
    unsigned char *bitmap;     /* malloc'd coverage, w*h bytes, or 0 (empty glyph) */
    int            w, h;       /* bitmap dimensions in pixels                      */
    int            xoff, yoff; /* offset from pen (yoff is negative-up from base)  */
} ttf_glyph_entry;

static ttf_glyph_entry g_cache[TTF_CACHE_SLOTS];

static unsigned int ttf_cache_hash(int codepoint, int px_size) {
    unsigned int h = (unsigned int)codepoint * 2654435761u;
    h ^= (unsigned int)px_size * 40503u;
    return h % TTF_CACHE_SLOTS;
}

static void ttf_cache_flush(void) {
    for (int i = 0; i < TTF_CACHE_SLOTS; i++) {
        if (g_cache[i].used && g_cache[i].bitmap) {
            /* stbtt_GetCodepointBitmap uses STBTT_malloc -> our malloc, so free()*/
            free(g_cache[i].bitmap);
        }
        g_cache[i].used = 0;
        g_cache[i].codepoint = 0;
        g_cache[i].px_size = 0;
        g_cache[i].bitmap = 0;
        g_cache[i].w = g_cache[i].h = 0;
        g_cache[i].xoff = g_cache[i].yoff = 0;
    }
}

/*
 * Look up (or rasterize + insert) the coverage bitmap for a glyph. Returns a
 * pointer to the cache entry, or 0 on hard failure. An entry with bitmap==0 is
 * a legitimately blank glyph (e.g. space) and still carries advance via metrics.
 */
static ttf_glyph_entry *ttf_get_glyph(int codepoint, int px_size) {
    unsigned int start = ttf_cache_hash(codepoint, px_size);

    /* Linear probe for an existing match. */
    for (int i = 0; i < TTF_CACHE_SLOTS; i++) {
        unsigned int idx = (start + (unsigned int)i) % TTF_CACHE_SLOTS;
        ttf_glyph_entry *e = &g_cache[idx];
        if (e->used && e->codepoint == codepoint && e->px_size == px_size)
            return e;
        if (!e->used)
            break; /* miss: fall through to insertion at first free slot */
    }

    /* Find an insertion slot (first free; else evict the start slot). */
    ttf_glyph_entry *slot = 0;
    for (int i = 0; i < TTF_CACHE_SLOTS; i++) {
        unsigned int idx = (start + (unsigned int)i) % TTF_CACHE_SLOTS;
        if (!g_cache[idx].used) { slot = &g_cache[idx]; break; }
    }
    if (!slot) {
        slot = &g_cache[start];
        if (slot->bitmap) { free(slot->bitmap); slot->bitmap = 0; }
    }

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px_size);
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char *bm = stbtt_GetCodepointBitmap(&g_font, 0, scale, codepoint,
                                                 &w, &h, &xoff, &yoff);
    /* bm may be NULL for whitespace / undrawable glyphs — that's valid. */

    slot->used      = 1;
    slot->codepoint = codepoint;
    slot->px_size   = px_size;
    slot->bitmap    = bm;
    slot->w         = w;
    slot->h         = h;
    slot->xoff      = xoff;
    slot->yoff      = yoff;
    return slot;
}

/* ----------------------------------------------------------------------------
 * Alpha blend one coverage value (0..255) of `src` color over `dst` ARGB32.
 *   out = src*a + dst*(1-a), per channel, with a = coverage/255.
 * Done in integer math (round-to-nearest); preserves a sane output alpha.
 * ------------------------------------------------------------------------- */
static inline ttf_u32 ttf_blend(ttf_u32 dst, ttf_u32 src, unsigned int cov) {
    if (cov == 0) return dst;

    unsigned int sa = (src >> 24) & 0xFFu;
    /* Effective coverage scaled by the source color's own alpha. */
    unsigned int a = (cov * sa) / 255u;
    if (a == 0) return dst;
    if (a >= 255u) return src;

    unsigned int sr = (src >> 16) & 0xFFu;
    unsigned int sg = (src >> 8)  & 0xFFu;
    unsigned int sb =  src        & 0xFFu;

    unsigned int dr = (dst >> 16) & 0xFFu;
    unsigned int dg = (dst >> 8)  & 0xFFu;
    unsigned int db =  dst        & 0xFFu;
    unsigned int da = (dst >> 24) & 0xFFu;

    unsigned int ia = 255u - a;
    unsigned int rr = (sr * a + dr * ia + 127u) / 255u;
    unsigned int rg = (sg * a + dg * ia + 127u) / 255u;
    unsigned int rb = (sb * a + db * ia + 127u) / 255u;
    /* Composite alpha: a over da. */
    unsigned int ra = a + (da * ia + 127u) / 255u;
    if (ra > 255u) ra = 255u;

    return (ra << 24) | (rr << 16) | (rg << 8) | rb;
}

/* ----------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */
int ttf_is_loaded(void) { return g_loaded; }

int ttf_load(const char *initrd_path) {
    /* Replace any previous font: flush the cache first. */
    ttf_cache_flush();
    g_loaded = 0;

    void         *addr = 0;
    unsigned long size = 0;
    int rc = map_file(initrd_path, &addr, &size);
    if (rc != 0 || !addr || size == 0) {
        ttf_log("[TTF] map_file failed for: ");
        ttf_log(initrd_path ? initrd_path : "(null)");
        ttf_log("\n");
        return -1;
    }

    g_ttf_data = (unsigned char *)addr;
    g_ttf_size = size;

    /* Use the first font in the file (offset 0 for a single-face .ttf). */
    int off = stbtt_GetFontOffsetForIndex(g_ttf_data, 0);
    if (off < 0) off = 0;

    if (!stbtt_InitFont(&g_font, g_ttf_data, off)) {
        ttf_log("[TTF] stbtt_InitFont rejected font data\n");
        g_ttf_data = 0;
        g_ttf_size = 0;
        return -2;
    }

    g_loaded = 1;
    ttf_log("[TTF] font loaded ok\n");
    return 0;
}

void ttf_vmetrics(int px_size, int *ascent, int *descent, int *line_gap) {
    if (ascent)   *ascent = 0;
    if (descent)  *descent = 0;
    if (line_gap) *line_gap = 0;
    if (!g_loaded || px_size <= 0) return;

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px_size);
    int a = 0, d = 0, lg = 0;
    stbtt_GetFontVMetrics(&g_font, &a, &d, &lg);
    if (ascent)   *ascent   = (int)(a  * scale);
    if (descent)  *descent  = (int)(d  * scale);
    if (line_gap) *line_gap = (int)(lg * scale);
}

int ttf_text_width(const char *str, int px_size) {
    if (!g_loaded || !str || px_size <= 0) return 0;

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px_size);
    float penx = 0.0f;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        int cp = (int)*p;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        penx += adv * scale;
        if (p[1]) {
            int kern = stbtt_GetCodepointKernAdvance(&g_font, cp, (int)p[1]);
            penx += kern * scale;
        }
    }
    return (int)(penx + 0.5f);
}

void ttf_draw_text(ttf_u32 *buf, int stride_px, int bw, int bh,
                   int x, int y, const char *str,
                   int px_size, ttf_u32 color) {
    if (!g_loaded || !buf || !str || px_size <= 0 || stride_px <= 0) return;

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px_size);

    /* Sub-pixel pen carried as float; baseline is at integer row `y`. */
    float penx = (float)x;

    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        int cp = (int)*p;

        ttf_glyph_entry *g = ttf_get_glyph(cp, px_size);

        if (g && g->bitmap && g->w > 0 && g->h > 0) {
            /* Glyph top-left in buffer space:
             *   x: floor(pen) + xoff   y: baseline + yoff (yoff is negative-up) */
            int gx = (int)floor((double)penx) + g->xoff;
            int gy = y + g->yoff;

            for (int row = 0; row < g->h; row++) {
                int dy = gy + row;
                if (dy < 0 || dy >= bh) continue;
                const unsigned char *src_row = g->bitmap + (ttf_size_t)row * g->w;
                ttf_u32 *dst_row = buf + (ttf_size_t)dy * stride_px;
                for (int col = 0; col < g->w; col++) {
                    int dx = gx + col;
                    if (dx < 0 || dx >= bw) continue;
                    unsigned int cov = src_row[col];
                    if (cov == 0) continue;
                    dst_row[dx] = ttf_blend(dst_row[dx], color, cov);
                }
            }
        }

        /* Advance the pen by the scaled horizontal advance (+ kerning). */
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
        penx += adv * scale;
        if (p[1]) {
            int kern = stbtt_GetCodepointKernAdvance(&g_font, cp, (int)p[1]);
            penx += kern * scale;
        }
    }
}
