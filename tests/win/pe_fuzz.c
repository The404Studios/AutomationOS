/*
 * pe_fuzz.c -- deterministic mutation fuzzer for userspace/lib/pe (run under ASan + UBSan).
 *
 *   usage: pe_fuzz [seed] [fixture.exe ...]
 *     seed      decimal or 0x-hex (default 0x9E3779B97F4A7C15); the run is fully deterministic for a given seed
 *     fixtures  good PE files to mutate (default /tmp/winfix/nocrt_hello.exe and crt_hello.exe)
 *   env: PE_FUZZ_ITERS=N        iterations (default 300000)
 *        PE_FUZZ_INJECT=N       harness self-test: deliberately overrun the input buffer at iteration N (ASan path)
 *        PE_FUZZ_INJECT_UB=N    harness self-test: deliberate signed overflow at iteration N (UBSan path)
 *
 * Every iteration mutates a good fixture (bit flips, field overwrites with boundary values, insert/delete/truncate,
 * header-biased, plus mutations inside the import / relocation / TLS tables), hands pe_parse an EXACTLY-SIZED heap
 * copy, and -- when the file is accepted -- maps it into an exactly-sized zeroed image and runs pe_relocate,
 * pe_resolve_imports and pe_tls_info on it, then repeats the pipeline with a corrupted pe_info_t.  ASan turns any
 * out-of-bounds access into an abort; the failing input is saved to /tmp/pe_fuzz_crash.bin first.
 *
 * Success line:  PE-FUZZ: PASS iterations=N ok=K rejected=R
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pe.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define CRASH_PATH "/tmp/pe_fuzz_crash.bin"
#define MAX_FIXTURES 8
#define MAX_FIELDS 768
#define MAX_REGIONS 32
#define BIG_IMAGE (16u * 1024u * 1024u)       /* accepted images above this are parsed but not mapped (speed) */

/* ------------------------------------------------------------------------------------------------ */
/* crash capture: the current input is always reachable from these globals                           */
/* ------------------------------------------------------------------------------------------------ */
static const u8 *g_cur;
static size_t g_cur_len;
static u64 g_iter;

static void save_crash(void)
{
    static const char msg[] = "\nPE-FUZZ: FAIL -- failing input saved to " CRASH_PATH "\n";
    int fd;
    size_t off = 0;
    if (!g_cur) return;
    fd = open(CRASH_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        while (off < g_cur_len) {
            ssize_t w = write(fd, g_cur + off, g_cur_len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        close(fd);
    }
    if (write(2, msg, sizeof msg - 1) < 0) { /* nothing sensible to do */ }
    g_cur = NULL;                                             /* never save twice */
}

static void on_signal(int sig)
{
    save_crash();
    signal(sig, SIG_DFL);
    raise(sig);
}

void __sanitizer_set_death_callback(void (*cb)(void)) __attribute__((weak));

static void die(const char *what)
{
    fprintf(stderr, "PE-FUZZ: invariant violated at iteration %llu: %s\n", g_iter, what);
    abort();
}

/* ------------------------------------------------------------------------------------------------ */
/* xorshift64*                                                                                       */
/* ------------------------------------------------------------------------------------------------ */
static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u64 rnd64(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static u32 rnd(u32 n) { return n ? (u32)((rnd64() >> 32) % n) : 0; }

static const u32 k_interesting[] = {
    0, 1, 2, 3, 4, 7, 8, 0x0f, 0x10, 0x14, 0x28, 0x3f, 0x40, 0x7f, 0x80, 0xff, 0x100, 0x1ff, 0x200, 0x201, 0x3ff, 0x400,
    0x7ff, 0x800, 0xfff, 0x1000, 0x1001, 0x1fff, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7fff, 0x8000, 0xffff, 0x10000,
    0x20000, 0x1000000, 0x3ffffff, 0x4000000, 0x4000001, 0x4001000, 0x7fffffff, 0x80000000u, 0x80000001u, 0xc0000000u,
    0xfffff000u, 0xffff0000u, 0xfffffff0u, 0xfffffffeu, 0xffffffffu, 0x14000000u, 0x8664, 0x20b, 0x10b, 0x5a4d, 0x4550
};
#define N_INTERESTING (sizeof k_interesting / sizeof k_interesting[0])

/* ------------------------------------------------------------------------------------------------ */
/* fixtures                                                                                          */
/* ------------------------------------------------------------------------------------------------ */
typedef struct { u32 off; u32 len; } region_t;
typedef struct {
    const char *path;
    u8 *data;
    size_t len;
    u32 hdr_end;                         /* end of the section table (bias target) */
    u32 nfields;
    u32 field_off[MAX_FIELDS];
    u8 field_w[MAX_FIELDS];
    u32 nregions;
    region_t region[MAX_REGIONS];        /* import / reloc / tls table bytes, section raw ranges */
} fixture_t;

static u8 *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    u8 *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (u8 *)malloc(n > 0 ? (size_t)n : 1);
    if (!b || (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n)) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void add_field(fixture_t *fx, u32 off, u8 w)
{
    if (fx->nfields < MAX_FIELDS && off + w <= fx->len) { fx->field_off[fx->nfields] = off; fx->field_w[fx->nfields] = w; fx->nfields++; }
}
static void add_region(fixture_t *fx, u32 off, u32 len)
{
    if (fx->nregions < MAX_REGIONS && len && (size_t)off + len <= fx->len) { fx->region[fx->nregions].off = off; fx->region[fx->nregions].len = len; fx->nregions++; }
}
static int rva_to_off(const pe_info_t *in, u32 rva, u32 *off)
{
    u32 i;
    for (i = 0; i < in->n_sections; i++)
        if (rva >= in->sec[i].vrva && rva - in->sec[i].vrva < in->sec[i].raw_size) { *off = in->sec[i].raw_off + (rva - in->sec[i].vrva); return 1; }
    return 0;
}

static int prepare_fixture(fixture_t *fx, const char *path)
{
    pe_info_t info;
    u32 lf, opt, i, k;
    memset(fx, 0, sizeof *fx);
    fx->path = path;
    fx->data = load_file(path, &fx->len);
    if (!fx->data) { fprintf(stderr, "PE-FUZZ: cannot read fixture %s\n", path); return 0; }
    if (pe_parse(fx->data, fx->len, &info) != PE_OK) { fprintf(stderr, "PE-FUZZ: fixture %s does not parse\n", path); return 0; }
    lf = rd32(fx->data + 0x3c);
    opt = lf + 24;
    fx->hdr_end = opt + 240 + 40u * info.n_sections;
    add_field(fx, 0x3c, 4);
    add_field(fx, lf, 4);
    for (i = 0; i < 20; i += 2) add_field(fx, lf + 4 + i, i == 4 || i == 8 || i == 12 ? 4 : 2);    /* COFF header */
    add_field(fx, opt + 0, 2); add_field(fx, opt + 4, 4); add_field(fx, opt + 8, 4); add_field(fx, opt + 12, 4);
    add_field(fx, opt + 16, 4); add_field(fx, opt + 20, 4); add_field(fx, opt + 24, 8); add_field(fx, opt + 32, 4);
    add_field(fx, opt + 36, 4);
    for (i = 40; i < 52; i += 2) add_field(fx, opt + i, 2);
    add_field(fx, opt + 52, 4); add_field(fx, opt + 56, 4); add_field(fx, opt + 60, 4); add_field(fx, opt + 64, 4);
    add_field(fx, opt + 68, 2); add_field(fx, opt + 70, 2);
    add_field(fx, opt + 72, 8); add_field(fx, opt + 80, 8); add_field(fx, opt + 88, 8); add_field(fx, opt + 96, 8);
    add_field(fx, opt + 104, 4); add_field(fx, opt + 108, 4);
    for (i = 0; i < 16; i++) { add_field(fx, opt + 112 + 8 * i, 4); add_field(fx, opt + 116 + 8 * i, 4); }
    for (k = 0; k < info.n_sections; k++) {
        u32 s = opt + 240 + 40 * k;
        add_field(fx, s, 4); add_field(fx, s + 4, 4); add_field(fx, s + 8, 4); add_field(fx, s + 12, 4);
        add_field(fx, s + 16, 4); add_field(fx, s + 20, 4); add_field(fx, s + 36, 4); add_field(fx, s + 14, 2);
    }
    if (info.import_rva && rva_to_off(&info, info.import_rva, &i)) add_region(fx, i, info.import_size);
    if (info.reloc_rva && rva_to_off(&info, info.reloc_rva, &i)) add_region(fx, i, info.reloc_size);
    if (info.tls_rva && rva_to_off(&info, info.tls_rva, &i)) add_region(fx, i, info.tls_size);
    for (k = 0; k < info.n_sections; k++) add_region(fx, info.sec[k].raw_off, info.sec[k].raw_size);
    fprintf(stderr, "PE-FUZZ: fixture %s: %zu bytes, %u sections, %u header fields, %u table regions\n",
            path, fx->len, info.n_sections, fx->nfields, fx->nregions);
    return 1;
}

/* ------------------------------------------------------------------------------------------------ */
/* mutation                                                                                          */
/* ------------------------------------------------------------------------------------------------ */
static void put_le(u8 *b, size_t o, u32 w, u64 v) { u32 i; for (i = 0; i < w; i++) b[o + i] = (u8)(v >> (8 * i)); }
static u64 get_le(const u8 *b, size_t o, u32 w) { u64 v = 0; u32 i; for (i = 0; i < w; i++) v |= (u64)b[o + i] << (8 * i); return v; }

static u64 pick_value(u64 cur, u32 w)
{
    u64 v;
    switch (rnd(8)) {
    case 0: v = rnd64(); break;
    case 1: v = cur + (u64)(long long)(int)(rnd(9) - 4); break;
    case 2: v = cur + (rnd(2) ? 0x1000u : 0 - (u64)0x1000u); break;
    case 3: v = cur ^ (1ull << rnd(w * 8)); break;
    case 4: v = cur + (rnd(2) ? 0x200u : 0 - (u64)0x200u); break;
    default: v = k_interesting[rnd(N_INTERESTING)]; if (w == 8 && rnd(3) == 0) v |= (u64)k_interesting[rnd(N_INTERESTING)] << 32; break;
    }
    return w >= 8 ? v : (v & ((1ull << (w * 8)) - 1u));
}

static size_t header_pos(const fixture_t *fx, size_t len)
{
    size_t lim = fx->hdr_end < len ? fx->hdr_end : len;
    return lim ? rnd((u32)lim) : 0;
}

static size_t mutate(const fixture_t *fx, u8 *buf, size_t len, size_t cap)
{
    u32 nmut = 1 + rnd(4), m;
    if (rnd(10) == 0) nmut += rnd(6);
    for (m = 0; m < nmut && len > 0; m++) {
        u32 kind = rnd(100);
        if (kind < 22) {                                               /* header bit flip */
            size_t p = header_pos(fx, len);
            buf[p] ^= (u8)(1u << rnd(8));
        } else if (kind < 55) {                                        /* known field <- boundary value */
            u32 f = rnd(fx->nfields), w = fx->field_w[f], o = fx->field_off[f];
            if ((size_t)o + w <= len) put_le(buf, o, w, pick_value(get_le(buf, o, w), w));
        } else if (kind < 63) {                                        /* random header byte */
            buf[header_pos(fx, len)] = (u8)rnd(256);
        } else if (kind < 80) {                                        /* inside the import / reloc / tls tables */
            if (fx->nregions) {
                const region_t *rg = &fx->region[rnd(fx->nregions)];
                size_t p = (size_t)rg->off + rnd(rg->len);
                u32 w = 1u << rnd(4);
                if (p + w <= len) {
                    if (w == 1) buf[p] = (u8)(rnd(3) ? rnd(256) : (rnd(2) ? 0xff : 0));
                    else put_le(buf, p, w, pick_value(get_le(buf, p, w), w));
                }
            }
        } else if (kind < 85) {                                        /* insert */
            u32 n = 1 + rnd(16);
            size_t p = header_pos(fx, len);
            if (len + n <= cap) {
                memmove(buf + p + n, buf + p, len - p);
                { u32 i; for (i = 0; i < n; i++) buf[p + i] = (u8)rnd(256); }
                len += n;
            }
        } else if (kind < 90) {                                        /* delete */
            u32 n = 1 + rnd(16);
            size_t p = header_pos(fx, len);
            if (p + n <= len) { memmove(buf + p, buf + p + n, len - p - n); len -= n; }
        } else if (kind < 94) {                                        /* truncate */
            switch (rnd(4)) {
            case 0: len = len - 1; break;
            case 1: len = rnd((u32)len); break;
            case 2: len = fx->hdr_end < len ? fx->hdr_end + rnd(8) - 4 : len; if (len > cap) len = cap; break;
            default: len = len > 16 ? len - rnd(16) : len; break;
            }
        } else if (kind < 98) {                                        /* clone one 40-byte chunk over another */
            size_t a = header_pos(fx, len), b = header_pos(fx, len);
            if (a + 40 <= len && b + 40 <= len) memmove(buf + b, buf + a, 40);
        } else {                                                       /* 8 x 0xFF / 0x00 */
            size_t p = header_pos(fx, len);
            u8 v = rnd(2) ? 0xff : 0;
            u32 i;
            for (i = 0; i < 8 && p + i < len; i++) buf[p + i] = v;
        }
    }
    return len;
}

/* ------------------------------------------------------------------------------------------------ */
/* pipeline                                                                                          */
/* ------------------------------------------------------------------------------------------------ */
static u64 g_stats_err[16];             /* index = -code */
static u64 g_iters_ok, g_iters_rej, g_mapped, g_skipped_big, g_reloc_ok, g_imp_ok, g_tls_ok;
static u64 g_inject_at = ~0ull, g_inject_ub_at = ~0ull;

static int fuzz_resolver(const char *dll, const char *name, unsigned short ord, int by_ord, u64 *out, void *user)
{
    u32 *calls = (u32 *)user;
    (void)ord;
    if (!dll || !name) die("resolver got a NULL string");
    if (dll[0] == 0 || strlen(dll) >= PE_MAX_DLL_NAME) die("resolver got a bad dll name");
    if (strlen(name) >= PE_MAX_FUNC_NAME) die("resolver got an oversized function name");
    if (by_ord ? name[0] != 0 : name[0] == 0) die("by_ordinal / name mismatch");
    (*calls)++;
    switch (*calls & 3u) {
    case 0: *out = 0x00007FFB00000000ull + *calls; return 0;
    case 1: *out = 0x00007FFB00001000ull + *calls; return 1;
    case 2: return 1;
    default: *out = *calls; return 0;
    }
}

static int in_range(int r, int lo, int hi) { return r >= lo && r <= hi; }

static void check_info(const pe_info_t *in)
{
    u32 i;
    if (in->n_sections < 1 || in->n_sections > PE_MAX_SECTIONS) die("n_sections out of range");
    if (in->size_of_image == 0 || in->size_of_image > PE_MAX_IMAGE) die("size_of_image out of range");
    if (in->size_of_headers == 0 || in->size_of_headers > in->size_of_image) die("size_of_headers out of range");
    if (in->section_alignment < 0x1000 || (in->size_of_image % in->section_alignment)) die("alignment violated");
    for (i = 0; i < in->n_sections; i++) {
        u64 end = (u64)in->sec[i].vrva + ((u64)in->sec[i].vsize + in->section_alignment - 1) / in->section_alignment * in->section_alignment;
        if (in->sec[i].vrva % in->section_alignment) die("section rva misaligned");
        if (in->sec[i].name[8] != 0) die("section name not terminated");
        if (in->sec[i].vsize && end > in->size_of_image) die("section past image");
    }
    if ((u64)in->import_rva + in->import_size > in->size_of_image) die("import dir outside image");
    if ((u64)in->reloc_rva + in->reloc_size > in->size_of_image) die("reloc dir outside image");
    if ((u64)in->tls_rva + in->tls_size > in->size_of_image) die("tls dir outside image");
    if (in->entry_rva >= in->size_of_image) die("entry outside image");
}

static u64 pick_base(const pe_info_t *in)
{
    switch (rnd(7)) {
    case 0: return in->image_base;
    case 1: return 0x400000ull;
    case 2: return 0x00007FF700000000ull + ((u64)rnd(0x10000) << 16);
    case 3: return rnd64() & ~0xffffull;
    case 4: return 0xFFFFFFFFFFFF0000ull;
    case 5: return in->image_base + ((u64)rnd(256) << 16);
    default: return rnd64();
    }
}

static void pipeline(const pe_info_t *in, const u8 *file, size_t len, u64 base)
{
    u8 *img = (u8 *)calloc(in->size_of_image ? in->size_of_image : 1, 1);   /* exactly as large as the info claims */
    u32 calls = 0, n = 0, un = 0;
    int r;
    if (!img) die("calloc failed");
    r = pe_map(file, len, in, img);
    if (!in_range(r, -12, 0)) die("pe_map returned a non-error code");
    r = pe_relocate(in, img, base);
    if (!in_range(r, -12, 0)) die("pe_relocate returned an unexpected code");
    if (r == PE_OK) g_reloc_ok++;
    r = pe_resolve_imports(in, img, fuzz_resolver, &calls, &n, &un);
    if (!in_range(r, -12, 0)) die("pe_resolve_imports returned an unexpected code");
    if (un > n) die("n_unresolved > n_total");
    if (r == PE_OK) g_imp_ok++;
    {
        u64 s = 0, e = 0, x = 0, c = 0;
        r = pe_tls_info(in, img, base, &s, &e, &x, &c);
        if (!in_range(r, -12, 1)) die("pe_tls_info returned an unexpected code");
        if (r == PE_OK) {
            g_tls_ok++;
            if (s > e) die("tls start > end");
        }
    }
    free(img);
}

static void run_one(const u8 *file, size_t len)
{
    pe_info_t info;
    int r = pe_parse(file, len, &info);

    if (g_iter == g_inject_at) { volatile u8 x = file[len]; (void)x; }               /* harness self-test: heap overflow */
    if (g_iter == g_inject_ub_at) { volatile int big = 0x7fffffff; big = big + (int)(g_iter & 1) + 1; (void)big; }
    if (!in_range(r, -12, 0)) die("pe_parse returned an out-of-range code");
    g_stats_err[-r]++;
    if (r != PE_OK) {
        if (info.n_sections || info.size_of_image || info.import_rva) die("failed parse left a non-zero pe_info_t");
        g_iters_rej++;
        return;
    }
    g_iters_ok++;
    check_info(&info);
    if (info.size_of_image > BIG_IMAGE) { g_skipped_big++; return; }
    g_mapped++;
    pipeline(&info, file, len, pick_base(&info));

    if (rnd(5) == 0) {                                      /* round 2: the same pipeline over a corrupted pe_info_t */
        pe_info_t bad = info;
        u32 nm = 1 + rnd(3), i;
        for (i = 0; i < nm; i++) {
            u32 *tgt;
            switch (rnd(16)) {
            case 0: tgt = &bad.import_rva; break;
            case 1: tgt = &bad.import_size; break;
            case 2: tgt = &bad.reloc_rva; break;
            case 3: tgt = &bad.reloc_size; break;
            case 4: tgt = &bad.tls_rva; break;
            case 5: tgt = &bad.tls_size; break;
            case 6: tgt = &bad.sec[rnd(info.n_sections)].vrva; break;
            case 7: tgt = &bad.sec[rnd(info.n_sections)].vsize; break;
            case 8: tgt = &bad.sec[rnd(info.n_sections)].raw_size; break;
            case 9: tgt = &bad.sec[rnd(info.n_sections)].raw_off; break;
            case 10: tgt = &bad.size_of_headers; break;
            case 11: tgt = &bad.entry_rva; break;
            case 12: tgt = &bad.section_alignment; break;
            case 13: { unsigned short *ns = &bad.n_sections; *ns = (unsigned short)pick_value(*ns, 2); continue; }
            case 14: bad.image_base = pick_value(bad.image_base, 8); continue;
            default: tgt = &bad.size_of_image; break;
            }
            *tgt = (u32)pick_value(*tgt, 4);
            if (tgt == &bad.size_of_image && bad.size_of_image > info.size_of_image && bad.size_of_image <= PE_MAX_IMAGE)
                bad.size_of_image = info.size_of_image;     /* never claim more than the buffer we allocate below */
        }
        if (bad.size_of_image > info.size_of_image && bad.size_of_image <= PE_MAX_IMAGE) bad.size_of_image = info.size_of_image;
        {
            /* the buffer is as big as the ORIGINAL image; a corrupted info may claim <= that, or an invalid size */
            u8 *img = (u8 *)calloc(info.size_of_image, 1);
            u32 calls = 0, n = 0, un = 0;
            int rr;
            u64 base = pick_base(&bad);
            if (!img) die("calloc failed");
            rr = pe_map(file, len, &bad, img);
            if (!in_range(rr, -12, 0)) die("pe_map(bad info) unexpected code");
            rr = pe_relocate(&bad, img, base);
            if (!in_range(rr, -12, 0)) die("pe_relocate(bad info) unexpected code");
            rr = pe_resolve_imports(&bad, img, fuzz_resolver, &calls, &n, &un);
            if (!in_range(rr, -12, 0)) die("pe_resolve_imports(bad info) unexpected code");
            rr = pe_tls_info(&bad, img, base, NULL, NULL, NULL, NULL);
            if (!in_range(rr, -12, 1)) die("pe_tls_info(bad info) unexpected code");
            free(img);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    static fixture_t fx[MAX_FIXTURES];
    const char *defaults[2] = { "/tmp/winfix/nocrt_hello.exe", "/tmp/winfix/crt_hello.exe" };
    const char *paths[MAX_FIXTURES];
    u32 nfx = 0, i;
    u64 iters = 300000, seed = 0x9E3779B97F4A7C15ull;
    const char *e;
    struct timespec t0, t1;
    int a = 1;

    if (argc > 1 && (argv[1][0] >= '0' && argv[1][0] <= '9')) { seed = strtoull(argv[1], NULL, 0); a = 2; }
    for (; a < argc && nfx < MAX_FIXTURES; a++) paths[nfx++] = argv[a];
    if (nfx == 0) { paths[0] = defaults[0]; paths[1] = defaults[1]; nfx = 2; }
    if ((e = getenv("PE_FUZZ_ITERS")) != NULL) iters = strtoull(e, NULL, 0);
    if ((e = getenv("PE_FUZZ_INJECT")) != NULL) g_inject_at = strtoull(e, NULL, 0);
    if ((e = getenv("PE_FUZZ_INJECT_UB")) != NULL) g_inject_ub_at = strtoull(e, NULL, 0);
    g_rng = seed ? seed : 1;

    for (i = 0; i < nfx; i++)
        if (!prepare_fixture(&fx[i], paths[i])) return 2;

    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(save_crash);
    /* SIGABRT covers die() and sanitizer failures with abort_on_error=1 (UBSan has its own runtime copy that the
     * death callback above cannot reach).  Without a sanitizer runtime also catch the hard faults ourselves;
     * under ASan its own SEGV handler reports first and then runs the death callback. */
    signal(SIGABRT, on_signal);
    if (!__sanitizer_set_death_callback) {
        signal(SIGSEGV, on_signal); signal(SIGBUS, on_signal); signal(SIGFPE, on_signal);
        signal(SIGILL, on_signal); signal(SIGTRAP, on_signal);
    }

    fprintf(stderr, "PE-FUZZ: seed=0x%llx iterations=%llu fixtures=%u\n", seed, iters, nfx);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (g_iter = 0; g_iter < iters; g_iter++) {
        fixture_t *f = &fx[rnd(nfx)];
        size_t cap = f->len + 64, len = f->len;
        u8 *work = (u8 *)malloc(cap), *exact;
        memcpy(work, f->data, len);
        if (g_iter > 0 || rnd(4)) len = mutate(f, work, len, cap);          /* iteration 0 may run the pristine file */
        exact = (u8 *)malloc(len ? len : 1);                                /* EXACT size => ASan sees any overrun */
        memcpy(exact, work, len);
        free(work);
        g_cur = exact;
        g_cur_len = len;
        run_one(exact, len);
        g_cur = NULL;
        free(exact);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    fprintf(stderr, "PE-FUZZ: parse results:");
    for (i = 0; i < 14; i++) if (g_stats_err[i]) fprintf(stderr, " %s=%llu", pe_strerror(-(int)i), g_stats_err[i]);
    fprintf(stderr, "\nPE-FUZZ: mapped=%llu (skipped >16MiB: %llu) reloc_ok=%llu imports_ok=%llu tls_ok=%llu time=%.1fs\n",
            g_mapped, g_skipped_big, g_reloc_ok, g_imp_ok, g_tls_ok,
            (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
    printf("PE-FUZZ: PASS iterations=%llu ok=%llu rejected=%llu\n", iters, g_iters_ok, g_iters_rej);
    return 0;
}
