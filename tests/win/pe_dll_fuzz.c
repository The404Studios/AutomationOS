/*
 * pe_dll_fuzz.c -- deterministic mutation fuzzer for userspace/lib/pe/pe_exports.c + pe_modules.c (ASan + UBSan).
 *
 *   usage: pe_dll_fuzz [seed] [fixture ...]
 *     seed      decimal or 0x-hex; the run is fully deterministic for a given seed
 *     fixtures  good PE files to mutate: real MinGW DLLs / exes (basename = the name other modules import them by).
 *               A set of synthetic DLLs (pe_dll_builder.h: forwarders, ordinal base, unsorted names, delay imports ...)
 *               is always added.
 *   env: PE_DLL_FUZZ_ITERS=N      iterations (default 200000)
 *        PE_DLL_FUZZ_CRASH=path   where the failing input is saved (default /tmp/pe_dll_dev/pe_dll_fuzz_crash.bin)
 *        PE_DLL_FUZZ_INJECT=N     harness self-test: deliberately overrun the input buffer at iteration N (ASan path)
 *        PE_DLL_FUZZ_INJECT_UB=N  harness self-test: deliberate signed overflow at iteration N (UBSan path)
 *
 * Every iteration mutates a good image (bit flips, field overwrites with boundary values and fixture-specific
 * "interesting" RVAs, insert / delete / truncate, header-biased, and -- for ~half the iterations -- targeted at the
 * export directory, its three tables and the strings behind them), hands pe_parse an EXACTLY-SIZED heap copy, and
 * when accepted maps it into an exactly-sized zeroed image, relocates it at a random base and then
 *   - exercises pe_export_validate / count / find (by name, by ordinal, with assorted forwarder buffer sizes) /
 *     at / name_at / parse_forwarder and pe_delay_imports, checking return-code sets and output invariants;
 *   - DIFFERENTIAL ORACLE: whenever pe_export_validate() says OK, an independent libc implementation re-derives every
 *     name's result (first duplicate wins, forwarder range rule, ordinal base) and it must match pe_export_find();
 *     validate==OK additionally forbids pe_export_find from ever answering PE_E_EXPORT;
 *   - every 3rd iteration additionally drives the whole MODULE LOADER (pe_mod_load_main / pe_mod_load / run_inits /
 *     getproc incl. forwarders / free / run_detach / free_all) with the mutated image served through read_file next
 *     to the pristine fixtures; DllMain is never executed (call_entry returns pseudo-random TRUE/FALSE), and the
 *     file buffers / images are exact-size heap blocks so ASan catches overruns, use-after-release and leaks.
 *   Per-iteration wall time is bounded (a stuck / super-linear path is a failure).
 * ASan turns any out-of-bounds access into an abort; the failing input is saved first.
 *
 * Success line:  PE-DLL-FUZZ: PASS iterations=N ok=K rejected=R ...
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "pe.h"
#include "pe_exports.h"
#include "pe_modules.h"
#include "pe_dll_builder.h"

#define DEFAULT_CRASH "/tmp/pe_dll_dev/pe_dll_fuzz_crash.bin"
#define MAX_FIXTURES 24
#define MAX_FIELDS 768
#define MAX_REGIONS 48
#define MAX_RVAS 24
#define BIG_IMAGE (8u * 1024u * 1024u)          /* accepted images above this are parsed but not mapped (speed) */
#define ITER_SECONDS 3.0                         /* wall-clock bound per iteration */

/* ------------------------------------------------------------------------------------------------ */
/* crash capture                                                                                     */
/* ------------------------------------------------------------------------------------------------ */
static const u8 *g_cur;
static size_t g_cur_len;
static u64 g_iter;
static char g_crash_path[256] = DEFAULT_CRASH;

static void save_crash(void)
{
    static const char msg[] = "\nPE-DLL-FUZZ: FAIL -- failing input saved\n";
    int fd;
    size_t off = 0;
    if (!g_cur) return;
    fd = open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        while (off < g_cur_len) {
            ssize_t w = write(fd, g_cur + off, g_cur_len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        close(fd);
    }
    if (write(2, msg, sizeof msg - 1) < 0) { /* nothing sensible to do */ }
    g_cur = NULL;
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
    fprintf(stderr, "PE-DLL-FUZZ: invariant violated at iteration %llu: %s\n", g_iter, what);
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
    0, 1, 2, 3, 4, 5, 6, 7, 8, 0x0f, 0x10, 0x14, 0x20, 0x28, 0x3f, 0x40, 0x7f, 0x80, 0xff, 0x100, 0x1ff, 0x200, 0x201, 0x3ff, 0x400,
    0x7ff, 0x800, 0xfff, 0x1000, 0x1001, 0x1fff, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7fff, 0x8000, 0xffff, 0x10000,
    0x10001, 0x20000, 0x1000000, 0x3ffffff, 0x4000000, 0x4000001, 0x7fffffff, 0x80000000u, 0x80000001u, 0xc0000000u,
    0xfffff000u, 0xffff0000u, 0xfffffff0u, 0xfffffffeu, 0xffffffffu, 0x8664, 0x20b, 0x10b, 0x5a4d, 0x4550
};
#define N_INTERESTING (sizeof k_interesting / sizeof k_interesting[0])

/* ------------------------------------------------------------------------------------------------ */
/* fixtures                                                                                          */
/* ------------------------------------------------------------------------------------------------ */
typedef struct { u32 off; u32 len; int export_related; } region_t;
typedef struct {
    char name[64];                       /* basename: what other modules import it as */
    u8 *data;
    size_t len;
    int is_dll, synthetic;
    u32 hdr_end;
    u32 nfields;
    u32 field_off[MAX_FIELDS];
    u8 field_w[MAX_FIELDS];
    u32 nregions, n_exp_regions;
    region_t region[MAX_REGIONS];
    u32 nrvas;
    u32 rvas[MAX_RVAS];                  /* fixture-specific interesting RVAs / sizes */
    int nnames;
    char names[16][64];                  /* export names present in the pristine fixture */
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
static u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }

static void add_field(fixture_t *fx, u32 off, u8 w)
{
    if (fx->nfields < MAX_FIELDS && off + w <= fx->len) { fx->field_off[fx->nfields] = off; fx->field_w[fx->nfields] = w; fx->nfields++; }
}
static void add_region(fixture_t *fx, u32 off, u32 len, int exp)
{
    if (fx->nregions < MAX_REGIONS && len && (size_t)off + len <= fx->len) {
        fx->region[fx->nregions].off = off; fx->region[fx->nregions].len = len; fx->region[fx->nregions].export_related = exp;
        fx->nregions++;
        if (exp) fx->n_exp_regions++;
    }
}
static void add_rva(fixture_t *fx, u32 v) { if (fx->nrvas < MAX_RVAS) fx->rvas[fx->nrvas++] = v; }
static int rva_to_off(const pe_info_t *in, u32 rva, u32 *off)
{
    u32 i;
    for (i = 0; i < in->n_sections; i++)
        if (rva >= in->sec[i].vrva && rva - in->sec[i].vrva < in->sec[i].raw_size) { *off = in->sec[i].raw_off + (rva - in->sec[i].vrva); return 1; }
    return 0;
}

static int prepare_fixture(fixture_t *fx, const char *name, u8 *data, size_t len, int synthetic)
{
    pe_info_t info;
    u8 *img;
    u32 lf, opt, i, k, o;
    memset(fx, 0, sizeof *fx);
    snprintf(fx->name, sizeof fx->name, "%.63s", name);
    fx->data = data; fx->len = len; fx->synthetic = synthetic;
    if (pe_parse(fx->data, fx->len, &info) != PE_OK) { fprintf(stderr, "PE-DLL-FUZZ: fixture %s does not parse\n", name); return 0; }
    fx->is_dll = (int)info.is_dll;
    lf = rd32(fx->data + 0x3c);
    opt = lf + 24;
    fx->hdr_end = opt + 240 + 40u * info.n_sections;
    add_field(fx, 0x3c, 4);
    add_field(fx, lf, 4);
    for (i = 0; i < 20; i += 2) add_field(fx, lf + 4 + i, i == 4 || i == 8 || i == 12 ? 4 : 2);
    add_field(fx, opt + 0, 2); add_field(fx, opt + 16, 4); add_field(fx, opt + 24, 8); add_field(fx, opt + 32, 4);
    add_field(fx, opt + 36, 4); add_field(fx, opt + 56, 4); add_field(fx, opt + 60, 4); add_field(fx, opt + 68, 2);
    add_field(fx, opt + 70, 2); add_field(fx, opt + 108, 4);
    for (i = 0; i < 16; i++) { add_field(fx, opt + 112 + 8 * i, 4); add_field(fx, opt + 116 + 8 * i, 4); }
    for (k = 0; k < info.n_sections; k++) {
        u32 s = opt + 240 + 40 * k;
        add_field(fx, s + 8, 4); add_field(fx, s + 12, 4); add_field(fx, s + 16, 4); add_field(fx, s + 20, 4); add_field(fx, s + 36, 4);
    }
    /* regions: the export directory, its tables and strings, the import / delay / reloc tables, section raws */
    img = (u8 *)calloc(info.size_of_image, 1);
    if (img && pe_map(fx->data, fx->len, &info, img) == PE_OK) {
        if (info.export_rva && rva_to_off(&info, info.export_rva, &o)) {
            u32 nf = rd32(img + info.export_rva + 20), nn = rd32(img + info.export_rva + 24);
            u32 eat = rd32(img + info.export_rva + 28), nm = rd32(img + info.export_rva + 32), od = rd32(img + info.export_rva + 36);
            u32 nameRva;
            add_region(fx, o, 40, 1);
            if (rva_to_off(&info, eat, &o)) add_region(fx, o, 4 * nf, 1);
            if (rva_to_off(&info, nm, &o)) add_region(fx, o, 4 * nn, 1);
            if (rva_to_off(&info, od, &o)) add_region(fx, o, 2 * nn, 1);
            if (rva_to_off(&info, info.export_rva + 40, &o)) add_region(fx, o, info.export_size > 40 ? info.export_size - 40 : 1, 1);
            add_rva(fx, info.export_rva); add_rva(fx, info.export_rva + 40); add_rva(fx, info.export_rva + info.export_size);
            add_rva(fx, info.export_rva + info.export_size - 1); add_rva(fx, eat); add_rva(fx, nm); add_rva(fx, od);
            add_rva(fx, info.export_size); add_rva(fx, nf); add_rva(fx, nn);
            for (i = 0; i < nn && (int)i < 16; i++) {
                nameRva = rd32(img + nm + 4 * i);
                if (nameRva < info.size_of_image && strlen((const char *)img + nameRva) < 63) {
                    snprintf(fx->names[fx->nnames], 64, "%s", (const char *)img + nameRva);
                    fx->nnames++;
                    add_rva(fx, nameRva);
                }
            }
        }
        add_rva(fx, info.size_of_image); add_rva(fx, info.size_of_image - 1); add_rva(fx, info.size_of_image - 8);
        add_rva(fx, info.size_of_image - 40); add_rva(fx, info.size_of_image - 4);
        add_rva(fx, info.import_rva); add_rva(fx, info.delay_import_rva);
    }
    free(img);
    if (info.import_rva && rva_to_off(&info, info.import_rva, &o)) add_region(fx, o, info.import_size, 0);
    if (info.delay_import_rva && rva_to_off(&info, info.delay_import_rva, &o)) add_region(fx, o, 64, 0);
    if (info.reloc_rva && rva_to_off(&info, info.reloc_rva, &o)) add_region(fx, o, info.reloc_size, 0);
    for (k = 0; k < info.n_sections; k++) add_region(fx, info.sec[k].raw_off, info.sec[k].raw_size, 0);
    fprintf(stderr, "PE-DLL-FUZZ: fixture %-22s %6zu bytes, %u sections, %u fields, %u regions (%u export), %d names%s\n",
            name, fx->len, info.n_sections, fx->nfields, fx->nregions, fx->n_exp_regions, fx->nnames, synthetic ? " [synthetic]" : "");
    return 1;
}

/* ------------------------------------------------------------------------------------------------ */
/* synthetic fixtures                                                                                */
/* ------------------------------------------------------------------------------------------------ */
static int build_synthetics(fixture_t *fx, u32 *nfx, u32 max)
{
    static bfunc_t F1[6] = { {0x11, 0}, {0x22, 0}, {0, 0}, {0, "dll_lib_b.sub"}, {0, "KERNEL32.SetLastError"}, {0, "fwd_ord.#2"} };
    static bname_t N1[6] = { {"zeta", 0}, {"alpha", 1}, {"unusedslot", 2}, {"fwd_b", 3}, {"fwd_k", 4}, {"fwd_o", 5} };
    static bfunc_t F2[4] = { {0x31, 0}, {0x32, 0}, {0x33, 0}, {0x34, 0} };
    static bname_t N2[6] = { {"mid", 2}, {"alpha", 0}, {"beta", 1}, {"alpha", 3}, {"alias", 1}, {"omega", 3} };
    static const char *const dn1[3] = { "fnA", "#9", "fnB" };
    static const char *const dn2[1] = { "x" };
    static const bimp_t D[2] = { { "dly.dll", 3, dn1 }, { "gone.dll", 1, dn2 } };
    static const char *const in1[3] = { "sub", "#7", "add" };
    static const bimp_t I1[1] = { { "dll_lib_a.dll", 3, in1 } };
    static bname_t N3[1] = { {"f", 1} };
    bspec_t s;
    bout_t b;
    struct { const char *name; bspec_t spec; } T[6];
    u32 i;

    memset(T, 0, sizeof T);
    memset(&s, 0, sizeof s);
    s.base = 3; s.nfuncs = 6; s.funcs = F1; s.nnames = 6; s.names = N1; s.has_entry = 1; s.nimps = 1; s.imps = I1;
    T[0].name = "syn_fwd.dll"; T[0].spec = s;
    memset(&s, 0, sizeof s);
    s.nfuncs = 4; s.funcs = F2; s.nnames = 6; s.names = N2; s.has_entry = 1; s.image_base = 0x1c0000000ull;
    T[1].name = "syn_unsorted.dll"; T[1].spec = s;
    memset(&s, 0, sizeof s);
    s.is_exe = 1; s.no_exports = 1; s.ndelay = 2; s.delay = D; s.nimps = 1; s.imps = I1;
    T[2].name = "syn_delay.exe"; T[2].spec = s;
    memset(&s, 0, sizeof s);
    s.base = 0xfffffff0u; s.nfuncs = 4; s.funcs = F2; s.nnames = 2; s.names = N2; s.has_entry = 1;
    T[3].name = "syn_highbase.dll"; T[3].spec = s;
    memset(&s, 0, sizeof s);
    s.nfuncs = 2; s.funcs = F2; s.nnames = 1; s.names = N3; s.has_entry = 1;
    T[4].name = "fwd_ord.dll"; T[4].spec = s;
    for (i = 0; i < 5 && *nfx < max; i++) {
        u8 *copy;
        if (bl_dll(&T[i].spec, &b) != 0) { fprintf(stderr, "PE-DLL-FUZZ: synthetic builder failed\n"); return 0; }
        copy = (u8 *)malloc(b.len);
        memcpy(copy, b.file, b.len);
        if (!prepare_fixture(&fx[*nfx], T[i].name, copy, b.len, 1)) return 0;
        (*nfx)++;
        bl_free(&b);
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------ */
/* mutation                                                                                          */
/* ------------------------------------------------------------------------------------------------ */
static void put_le(u8 *b, size_t o, u32 w, u64 v) { u32 i; for (i = 0; i < w; i++) b[o + i] = (u8)(v >> (8 * i)); }
static u64 get_le(const u8 *b, size_t o, u32 w) { u64 v = 0; u32 i; for (i = 0; i < w; i++) v |= (u64)b[o + i] << (8 * i); return v; }

static u64 pick_value(const fixture_t *fx, u64 cur, u32 w)
{
    u64 v;
    switch (rnd(10)) {
    case 0: v = rnd64(); break;
    case 1: v = cur + (u64)(long long)(int)(rnd(9) - 4); break;
    case 2: v = cur + (rnd(2) ? 0x1000u : 0 - (u64)0x1000u); break;
    case 3: v = cur ^ (1ull << rnd(w * 8)); break;
    case 4: v = cur + (rnd(2) ? 0x200u : 0 - (u64)0x200u); break;
    case 5: case 6:
        if (fx->nrvas) { v = fx->rvas[rnd(fx->nrvas)] + (u64)(long long)(int)(rnd(5) - 2); break; }
        /* fall through */
    default: v = k_interesting[rnd(N_INTERESTING)]; if (w == 8 && rnd(3) == 0) v |= (u64)k_interesting[rnd(N_INTERESTING)] << 32; break;
    }
    return w >= 8 ? v : (v & ((1ull << (w * 8)) - 1u));
}

static size_t header_pos(const fixture_t *fx, size_t len)
{
    size_t lim = fx->hdr_end < len ? fx->hdr_end : len;
    return lim ? rnd((u32)lim) : 0;
}

static size_t mutate(const fixture_t *fx, u8 *buf, size_t len, size_t cap, int export_bias)
{
    u32 nmut = 1 + rnd(4), m;
    if (rnd(10) == 0) nmut += rnd(6);
    for (m = 0; m < nmut && len > 0; m++) {
        u32 kind = rnd(100);
        if (export_bias && fx->n_exp_regions && kind < 60) kind = 55 + rnd(25);     /* steer to the table-region branch */
        if (kind < 15) {                                               /* header bit flip */
            size_t p = header_pos(fx, len);
            buf[p] ^= (u8)(1u << rnd(8));
        } else if (kind < 35) {                                        /* known header field <- boundary value */
            u32 f = rnd(fx->nfields), w = fx->field_w[f], o = fx->field_off[f];
            if ((size_t)o + w <= len) put_le(buf, o, w, pick_value(fx, get_le(buf, o, w), w));
        } else if (kind < 40) {                                        /* random header byte */
            buf[header_pos(fx, len)] = (u8)rnd(256);
        } else if (kind < 82) {                                        /* inside a table region (export-biased) */
            if (fx->nregions) {
                const region_t *rg;
                size_t p;
                u32 w, tries;
                for (tries = 0, rg = &fx->region[rnd(fx->nregions)]; tries < 6 && export_bias && fx->n_exp_regions && !rg->export_related; tries++)
                    rg = &fx->region[rnd(fx->nregions)];
                w = 1u << rnd(4);
                p = (size_t)rg->off + rnd(rg->len);
                if (w >= 4 && rnd(2)) p &= ~(size_t)3;                 /* table entries are 4-aligned */
                else if (w == 2 && rnd(2)) p &= ~(size_t)1;
                if (p + w <= len) {
                    if (w == 1) buf[p] = (u8)(rnd(3) ? rnd(256) : (rnd(2) ? 0xff : 0));
                    else put_le(buf, p, w, pick_value(fx, get_le(buf, p, w), w));
                }
            }
        } else if (kind < 87) {                                        /* insert */
            u32 n = 1 + rnd(16);
            size_t p = header_pos(fx, len);
            if (len + n <= cap) {
                memmove(buf + p + n, buf + p, len - p);
                { u32 i; for (i = 0; i < n; i++) buf[p + i] = (u8)rnd(256); }
                len += n;
            }
        } else if (kind < 91) {                                        /* delete */
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
/* statistics                                                                                        */
/* ------------------------------------------------------------------------------------------------ */
static u64 g_stats_err[20];
static u64 g_iters_ok, g_iters_rej, g_mapped, g_skipped_big, g_validated, g_no_expdir, g_export_bad, g_finds, g_forwarders,
           g_oracle_names, g_modruns, g_modloads, g_modinits_failed, g_getprocs, g_delay_ok;
static u64 g_inject_at = ~0ull, g_inject_ub_at = ~0ull;

static int in_set(int r, const int *set, int n) { int i; for (i = 0; i < n; i++) if (r == set[i]) return 1; return 0; }
#define IN_SET(r, ...) in_set((r), (const int[]){ __VA_ARGS__ }, (int)(sizeof((const int[]){ __VA_ARGS__ }) / sizeof(int)))

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

/* ------------------------------------------------------------------------------------------------ */
/* export exercise + differential oracle                                                             */
/* ------------------------------------------------------------------------------------------------ */
static const u32 k_fwd_caps[] = { 0, 1, 2, 5, 11, 16, 64, 255, 256, 300 };

static void check_find_result(const pe_info_t *in, int r, u32 rva, const char *fwd, u32 cap, int validated)
{
    if (!IN_SET(r, PE_OK, PE_EXPORT_FORWARDER, PE_E_NOTFOUND, PE_E_EXPORT, PE_E_BUFSIZE, PE_E_BOUNDS, PE_E_SIZE, PE_E_SECTIONS))
        die("pe_export_find returned an out-of-set code");
    if (validated && r == PE_E_EXPORT) die("pe_export_find said PE_E_EXPORT on a table pe_export_validate accepted");
    if (r == PE_OK || r == PE_EXPORT_FORWARDER) {
        if (rva == 0 || rva >= in->size_of_image) die("pe_export_find returned an RVA outside the image");
        if (r == PE_EXPORT_FORWARDER && !((u64)rva >= in->export_rva && (u64)rva < (u64)in->export_rva + in->export_size))
            die("forwarder RVA outside the export directory");
        if (r == PE_OK && ((u64)rva >= in->export_rva && (u64)rva < (u64)in->export_rva + in->export_size))
            die("plain export RVA inside the export directory");
    } else if (rva != 0) {
        die("pe_export_find left a nonzero RVA on failure");
    }
    if (r == PE_EXPORT_FORWARDER && fwd) {
        u32 n;
        if (cap == 0) die("forwarder reported although the buffer has 0 bytes");
        n = (u32)strnlen(fwd, cap);
        if (n == cap || n == 0 || n >= PE_MAX_FWD) die("forwarder string not terminated / empty / too long");
    }
    if (r == PE_E_BUFSIZE && fwd && cap && fwd[0] != 0) die("fwd not cleared after PE_E_BUFSIZE");
}

/* independent re-derivation (libc) valid ONLY when pe_export_validate() == PE_OK */
static void oracle_check(const pe_info_t *in, const u8 *img)
{
    const u8 *d = img + in->export_rva;
    u32 base = rd32(d + 16), nf = rd32(d + 20), nn = rd32(d + 24), eat = rd32(d + 28), names = rd32(d + 32), ords = rd32(d + 36);
    u32 i, lim = nn < 96 ? nn : 96;
    u64 dlo = in->export_rva, dhi = (u64)in->export_rva + in->export_size;

    for (i = 0; i < lim; i++) {
        const char *nm = (const char *)img + rd32(img + names + 4ull * i);
        u32 j, idx, v, grva = 0, grva2 = 0;
        int want, got, got2;
        char fwd[300], fwd2[300];
        for (j = 0; j < nn; j++) if (strcmp((const char *)img + rd32(img + names + 4ull * j), nm) == 0) break;   /* first duplicate wins */
        idx = rd16(img + ords + 2ull * j);
        v = rd32(img + eat + 4ull * idx);
        want = (v == 0) ? PE_E_NOTFOUND : ((u64)v >= dlo && (u64)v < dhi) ? PE_EXPORT_FORWARDER : PE_OK;

        memset(fwd, 'Q', sizeof fwd);
        got = pe_export_find(in, img, nm, 0, 0, &grva, fwd, sizeof fwd);
        g_oracle_names++;
        if (got != want) die("oracle: name lookup result differs");
        if (want != PE_E_NOTFOUND && grva != v) die("oracle: name lookup RVA differs");
        if (want == PE_EXPORT_FORWARDER && strcmp(fwd, (const char *)img + v) != 0) die("oracle: forwarder string differs");

        if ((u64)base + idx <= 0xffffffffull) {
            memset(fwd2, 'Q', sizeof fwd2);
            got2 = pe_export_find(in, img, NULL, base + idx, 1, &grva2, fwd2, sizeof fwd2);
            if (got2 != want) die("oracle: ordinal lookup result differs");
            if (want != PE_E_NOTFOUND && grva2 != v) die("oracle: ordinal lookup RVA differs");
            if (want == PE_EXPORT_FORWARDER && strcmp(fwd2, fwd) != 0) die("oracle: ordinal forwarder string differs");
        }
        (void)nf;
    }
}

static void random_name(char *buf, u32 cap)          /* junk probe: random length / bytes, sometimes huge */
{
    u32 n, i;
    switch (rnd(6)) {
    case 0: n = 0; break;
    case 1: n = 1 + rnd(8); break;
    case 2: n = 1000 + rnd(30); break;                /* around PE_MAX_EXPORT_NAME */
    default: n = 1 + rnd(24); break;
    }
    if (n >= cap) n = cap - 1;
    for (i = 0; i < n; i++) buf[i] = (char)(rnd(5) == 0 ? 1 + rnd(255) : 'a' + rnd(26));
    buf[n] = 0;
}

static void exercise_exports(const pe_info_t *in, u8 *img, const fixture_t *fx)
{
    static char probe[1200];
    u32 nf = 0, nn = 0, i;
    int v, r, validated;

    v = pe_export_validate(in, img);
    if (!IN_SET(v, PE_OK, 1, PE_E_EXPORT, PE_E_BOUNDS, PE_E_SIZE, PE_E_SECTIONS)) die("pe_export_validate returned an out-of-set code");
    validated = (v == PE_OK);
    if (validated) g_validated++; else if (v == 1) g_no_expdir++; else g_export_bad++;
    r = pe_export_count(in, img, &nf, &nn);
    if (!IN_SET(r, PE_OK, 1, PE_E_EXPORT, PE_E_BOUNDS, PE_E_SIZE, PE_E_SECTIONS)) die("pe_export_count returned an out-of-set code");
    if (r == PE_OK && (nf > PE_MAX_EXPORTS || nn > PE_MAX_EXPORTS)) die("pe_export_count exceeded the cap");
    if (v == PE_OK && r != PE_OK) die("validate OK but count failed");
    if (validated) oracle_check(in, img);

    /* by name: known names, mutated names, junk, huge */
    for (i = 0; i < 8; i++) {
        const char *q;
        u32 rva = 0, cap = k_fwd_caps[rnd(sizeof k_fwd_caps / sizeof k_fwd_caps[0])];
        char fwd[320];
        if (i < 4 && fx->nnames) { q = fx->names[rnd((u32)fx->nnames)]; }
        else if (i < 6 && fx->nnames) {
            snprintf(probe, sizeof probe, "%s", fx->names[rnd((u32)fx->nnames)]);
            if (probe[0]) probe[rnd((u32)strlen(probe))] ^= (char)(1u << rnd(7));
            q = probe;
        } else { random_name(probe, sizeof probe); q = probe; }
        char *fp = rnd(8) == 0 ? NULL : fwd;
        memset(fwd, 'Q', sizeof fwd);
        r = pe_export_find(in, img, q, 0, 0, &rva, fp, cap);
        g_finds++;
        if (r == PE_EXPORT_FORWARDER) g_forwarders++;
        check_find_result(in, r, rva, fp, cap, validated);
    }
    /* by ordinal: boundaries around the base and the function count, plus random */
    {
        u8 *d = in->export_rva && (u64)in->export_rva + 40 <= in->size_of_image ? img + in->export_rva : NULL;
        u32 base = d ? rd32(d + 16) : 1, nfd = d ? rd32(d + 20) : 0;
        u32 ords[10];
        ords[0] = 0; ords[1] = 1; ords[2] = base; ords[3] = base + 1; ords[4] = base + (nfd ? nfd - 1 : 0); ords[5] = base + nfd;
        ords[6] = 0xffff; ords[7] = 0xffffffffu; ords[8] = (u32)rnd64(); ords[9] = base + rnd(nfd + 2);
        for (i = 0; i < 10; i++) {
            u32 rva = 0, cap = k_fwd_caps[rnd(sizeof k_fwd_caps / sizeof k_fwd_caps[0])];
            char fwd[320];
            char *fp = rnd(8) == 0 ? NULL : fwd;
            memset(fwd, 'Q', sizeof fwd);
            r = pe_export_find(in, img, NULL, ords[i], 1, &rva, fp, cap);
            g_finds++;
            if (r == PE_EXPORT_FORWARDER) g_forwarders++;
            check_find_result(in, r, rva, fp, cap, validated);
        }
    }
    /* enumeration */
    for (i = 0; i < nf + 1 && i < 24; i++) {
        u32 ord = 7, rva = 7;
        int isf = 7;
        r = pe_export_at(in, img, i, &ord, &rva, &isf);
        if (!IN_SET(r, PE_OK, PE_E_NOTFOUND, PE_E_EXPORT, PE_E_BOUNDS, PE_E_SIZE, PE_E_SECTIONS)) die("pe_export_at out-of-set code");
        if (validated && r == PE_E_EXPORT) die("pe_export_at: PE_E_EXPORT on a validated table");
        if (r == PE_OK && (isf != 0 && isf != 1)) die("pe_export_at: is_forwarder not 0/1");
        if (r != PE_OK && (rva != 0 || isf != 0 || ord != 0)) die("pe_export_at left outputs set on failure");
    }
    for (i = 0; i < nn + 1 && i < 24; i++) {
        char nm[80];
        u32 cap = rnd(4) == 0 ? 3 + rnd(20) : 80, ord = 7, rva = 7;
        r = pe_export_name_at(in, img, i, nm, cap, &ord, &rva);
        if (!IN_SET(r, PE_OK, PE_E_NOTFOUND, PE_E_EXPORT, PE_E_BUFSIZE, PE_E_BOUNDS, PE_E_SIZE, PE_E_SECTIONS)) die("pe_export_name_at out-of-set code");
        if (validated && r == PE_E_EXPORT) die("pe_export_name_at: PE_E_EXPORT on a validated table");
        if (r == PE_OK && strnlen(nm, cap) >= cap) die("pe_export_name_at: name not terminated");
        if (r != PE_OK && nm[0] != 0) die("pe_export_name_at: name not cleared on failure");
    }
    /* forwarder-string parser on random / real strings */
    for (i = 0; i < 4; i++) {
        char dll[64], fn[64], s[130];
        u32 ord = 7, n, k;
        int by = 7;
        n = rnd(100);
        for (k = 0; k < n; k++) s[k] = (char)(rnd(12) == 0 ? '.' : rnd(30) == 0 ? '#' : rnd(30) == 0 ? (int)rnd(256) : '0' + (int)rnd(40));
        s[n] = 0;
        r = pe_export_parse_forwarder(s, dll, sizeof dll, fn, sizeof fn, &ord, &by);
        if (!IN_SET(r, PE_OK, PE_E_FWD, PE_E_BUFSIZE)) die("parse_forwarder out-of-set code");
        if (r == PE_OK) {
            if (dll[0] == 0 || strlen(dll) + 1 + strlen(fn) + (by ? 2 : 0) > strlen(s) + 1) die("parse_forwarder: parts longer than the input");
            if (by && (ord == 0 || ord > 0xffff || fn[0] != 0)) die("parse_forwarder: bad ordinal result");
            if (!by && fn[0] == 0) die("parse_forwarder: empty function");
        }
    }
    /* delay-load imports on the same image (bound by the image, not the directory) */
    {
        u32 calls = 0, n = 0, un = 0;
        r = pe_delay_imports(in, img, fuzz_resolver, &calls, &n, &un);
        if (!IN_SET(r, PE_OK, PE_E_IMPORT, PE_E_ORDINAL, PE_E_BOUNDS, PE_E_SIZE, PE_E_SECTIONS)) die("pe_delay_imports out-of-set code");
        if (un > n) die("delay: n_unresolved > n_total");
        if (r == PE_OK && in->delay_import_rva) g_delay_ok++;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* module-loader harness (never executes DllMain)                                                     */
/* ------------------------------------------------------------------------------------------------ */
#define VF_MAX 32
static struct {
    struct { char name[64]; const u8 *data; size_t len; } vf[VF_MAX];
    int nvf, live_files, live_images, allocs, frees;
    int nread, nbuiltin, ncalls;
    u32 rngstate;
    pe_mod_ctx_t *ctx;
} FH;
static pe_mod_ctx_t FC;

static int f_read(const char *name, const unsigned char **buf, unsigned long *len, void *u)
{
    int i;
    (void)u;
    FH.nread++;
    if (!name || !name[0] || strlen(name) >= PE_MOD_NAME_MAX) die("read_file got a bad name");
    {
        size_t k;
        for (k = 0; name[k]; k++) if ((unsigned char)name[k] < 0x20 || (unsigned char)name[k] >= 0x7f || strchr("/\\:*?\"<>|", name[k])) die("read_file got a junk name");
    }
    for (i = 0; i < FH.nvf; i++) {
        if (strcasecmp(FH.vf[i].name, name) == 0) {
            u8 *copy = (u8 *)malloc(FH.vf[i].len ? FH.vf[i].len : 1);     /* EXACT size */
            memcpy(copy, FH.vf[i].data, FH.vf[i].len);
            *buf = copy;
            *len = (unsigned long)FH.vf[i].len;
            FH.live_files++;
            return 0;
        }
    }
    return 1;
}
static void f_release(const unsigned char *buf, unsigned long len, void *u) { (void)len; (void)u; free((void *)buf); FH.live_files--; }
static unsigned char *f_alloc(unsigned long size, unsigned long long pref, void *u)
{
    void *p;
    (void)u; (void)pref;
    FH.allocs++;
    if (size == 0 || size > PE_MAX_IMAGE || (size & 0xfff)) die("alloc_image got an invalid size");
    if (rnd(64) == 0) return NULL;                                            /* allocation failure path */
    p = aligned_alloc(4096, size);                                            /* exact: ASan guards the image end */
    if (!p) return NULL;
    memset(p, 0, size);
    FH.live_images++;
    return (unsigned char *)p;
}
static void f_free(unsigned char *img, unsigned long size, void *u) { (void)size; (void)u; free(img); FH.frees++; FH.live_images--; }
static int f_builtin(const char *dll, const char *name, unsigned short ord, int by_ord, unsigned long long *out, void *u)
{
    u32 h = 5381;
    const char *p;
    (void)u; (void)ord;
    FH.nbuiltin++;
    if (!dll || !name || !dll[0]) die("builtin_resolve got a NULL/empty string");
    if (*out != 0) die("builtin_resolve: *out_addr not pre-zeroed");
    if (by_ord ? name[0] != 0 : name[0] == 0) die("builtin_resolve: by_ordinal / name mismatch");
    if (strcasecmp(dll, "KERNEL32.dll") != 0 && strcasecmp(dll, "NTDLL.dll") != 0) return 1;   /* not a builtin DLL */
    for (p = name; *p; p++) h = h * 33u + (u8)*p;
    switch (h & 3u) {
    case 0: *out = 0x00007FFB00010000ull + (h & 0xffff0u); return 0;
    case 1: *out = 0x00007FFB00020000ull; return 1;            /* unimplemented: loud stub */
    case 2: return 1;                                          /* builtin DLL declined, no stub */
    default: *out = 0x00007FFB00030000ull + by_ord; return 0;
    }
}
static int f_ustub(const char *dll, const char *name, unsigned short ord, int by_ord, unsigned long long *out, void *u)
{
    (void)dll; (void)name; (void)ord; (void)by_ord; (void)u;
    if ((FH.ncalls++ & 1) == 0) { *out = 0x00007FFB00040000ull; return 0; }
    return 1;
}
static int f_call_entry(unsigned long long entry, unsigned long long base, unsigned reason, void *u)
{
    (void)u;
    if (entry == 0 || base == 0 || entry < base) die("call_entry got a bad entry / base");
    if (reason != PE_MOD_DLL_PROCESS_ATTACH && reason != PE_MOD_DLL_PROCESS_DETACH) die("call_entry got a bad reason");
    FH.ncalls++;
    if (reason == PE_MOD_DLL_PROCESS_DETACH) return 1;
    return rnd(8) != 0;                                        /* ~1 in 8 attaches "fails" */
}

static void check_ctx(const pe_mod_ctx_t *c)
{
    u32 i, n = 0;
    size_t k;
    for (i = 0; i < PE_MOD_MAX_MODULES; i++) {
        const pe_mod_t *m = &c->mod[i];
        if (m->state == PE_MOD_S_FREE) continue;
        n++;
        if (m->state != PE_MOD_S_LINKED) die("module not LINKED between calls");
        if (m->image == NULL || (m->base & 0xfff) || m->base != (u64)(uintptr_t)m->image) die("module base / image inconsistent");
        if (m->refcount < 0) die("negative refcount");
        if (m->deps & ~((PE_MOD_MAX_MODULES == 32) ? 0xffffffffu : ((1u << PE_MOD_MAX_MODULES) - 1))) die("deps out of range");
        {
            u32 j;
            for (j = 0; j < PE_MOD_MAX_MODULES; j++)
                if ((m->deps & (1u << j)) && c->mod[j].state != PE_MOD_S_LINKED) die("dependency on a module that is not loaded");
        }
        if (m->init > PE_MOD_I_DETACHED) die("bad init state");
    }
    if (n > PE_MOD_MAX_MODULES) die("too many modules");
    if (strlen(c->first_unresolved) >= PE_MOD_MSG_MAX) die("first_unresolved not terminated");
    for (k = 0; c->first_unresolved[k]; k++)
        if ((unsigned char)c->first_unresolved[k] < 0x20 || (unsigned char)c->first_unresolved[k] >= 0x7f) die("first_unresolved has non-printable bytes");
    if (c->unresolved == 0 && c->first_unresolved[0]) die("first_unresolved set but unresolved == 0");
}

static void module_run(fixture_t *fx, u32 nfx, u32 mi, const u8 *mfile, size_t mlen)
{
    pe_mod_ops_t ops;
    u32 i;
    int m, r;
    const fixture_t *mainfx = NULL;

    memset(&FH, 0, sizeof FH);
    FH.ctx = &FC;
    memset(&ops, 0, sizeof ops);
    ops.read_file = f_read; ops.alloc_image = f_alloc; ops.builtin_resolve = rnd(8) ? f_builtin : NULL;
    ops.call_entry = f_call_entry; ops.release_file = f_release; ops.free_image = f_free;
    ops.unresolved_stub = rnd(2) ? f_ustub : NULL;
    pe_mod_ctx_init(&FC);
    if (rnd(4) == 0) FC.flags = PE_MOD_CF_NO_DELAY;

    for (i = 0; i < nfx && FH.nvf < VF_MAX; i++) {
        snprintf(FH.vf[FH.nvf].name, sizeof FH.vf[0].name, "%.63s", fx[i].name);
        FH.vf[FH.nvf].data = (i == mi) ? mfile : fx[i].data;
        FH.vf[FH.nvf].len = (i == mi) ? mlen : fx[i].len;
        FH.nvf++;
    }
    /* main module: the mutated exe if there is one, else a pristine exe */
    if (!fx[mi].is_dll) mainfx = &fx[mi];
    else for (i = 0; i < nfx; i++) if (!fx[i].is_dll && !fx[i].synthetic) { mainfx = &fx[i]; break; }
    if (mainfx) {
        const u8 *md = (mainfx == &fx[mi]) ? mfile : mainfx->data;
        size_t ml = (mainfx == &fx[mi]) ? mlen : mainfx->len;
        m = pe_mod_load_main(&FC, &ops, md, ml, mainfx->name);
        g_modloads++;
        if (m < 0 && !IN_SET(m, PE_MOD_E_FULL, PE_MOD_E_ALLOC, PE_MOD_E_KIND, PE_E_TRUNC, PE_E_MAGIC, PE_E_NOT_PE32PLUS, PE_E_MACHINE,
                             PE_E_BOUNDS, PE_E_SECTIONS, PE_E_SIZE, PE_E_ALIGN, PE_E_RELOC, PE_E_IMPORT, PE_E_ORDINAL, PE_E_UNSUPPORTED,
                             PE_E_EXPORT))
            die("pe_mod_load_main returned an unexpected code");
        check_ctx(&FC);
    }
    /* LoadLibrary of every DLL fixture name (mutated one included), then of the mutated one again */
    for (i = 0; i < nfx; i++) {
        if (!fx[i].is_dll) continue;
        if (i != mi && rnd(3)) continue;
        r = pe_mod_load(&FC, &ops, fx[i].name);
        g_modloads++;
        if (r < 0 && !IN_SET(r, PE_MOD_E_FULL, PE_MOD_E_ALLOC, PE_MOD_E_KIND, PE_MOD_E_NOFILE, PE_E_TRUNC, PE_E_MAGIC, PE_E_NOT_PE32PLUS,
                             PE_E_MACHINE, PE_E_BOUNDS, PE_E_SECTIONS, PE_E_SIZE, PE_E_ALIGN, PE_E_RELOC, PE_E_IMPORT, PE_E_ORDINAL,
                             PE_E_UNSUPPORTED, PE_E_EXPORT))
            die("pe_mod_load returned an unexpected code");
        check_ctx(&FC);
    }

    r = pe_mod_run_inits(&FC, &ops);
    if (r != 0 && r != PE_MOD_E_INIT) die("pe_mod_run_inits returned an unexpected code");
    if (r == PE_MOD_E_INIT) { g_modinits_failed++; if (FC.failed_module < 0 || FC.failed_module >= (int)PE_MOD_MAX_MODULES) die("failed_module out of range"); }
    check_ctx(&FC);

    /* GetProcAddress on every module: known names, junk, ordinals; forwarders may load more DLLs on demand */
    for (i = 0; i < PE_MOD_MAX_MODULES; i++) {
        u32 q;
        if (FC.mod[i].state == PE_MOD_S_FREE) continue;
        for (q = 0; q < 6; q++) {
            u64 addr = 7;
            char probe[80];
            int by = (q >= 4), gr;
            const fixture_t *src = &fx[rnd(nfx)];
            if (!by) {
                if (src->nnames) snprintf(probe, sizeof probe, "%s", src->names[rnd((u32)src->nnames)]);
                else random_name(probe, sizeof probe);
            } else probe[0] = 0;
            gr = pe_mod_getproc(&FC, &ops, (int)i, probe, (unsigned)rnd(12), by, &addr);
            g_getprocs++;
            if (!IN_SET(gr, 0, 1, PE_E_NOTFOUND, PE_E_EXPORT, PE_E_FWD, PE_E_BUFSIZE, PE_MOD_E_FWD_CYCLE, PE_MOD_E_FWD_DEPTH, PE_MOD_E_FULL,
                        PE_MOD_E_DEPTH, PE_MOD_E_NOFILE, PE_MOD_E_ALLOC, PE_MOD_E_KIND, PE_MOD_E_NAME, PE_E_TRUNC, PE_E_MAGIC,
                        PE_E_NOT_PE32PLUS, PE_E_MACHINE, PE_E_BOUNDS, PE_E_SECTIONS, PE_E_SIZE, PE_E_ALIGN, PE_E_RELOC, PE_E_IMPORT,
                        PE_E_ORDINAL, PE_E_UNSUPPORTED))
                die("pe_mod_getproc returned an unexpected code");
            if (gr == 0 && addr == 0) die("getproc: success with a zero address");
            if (gr < 0 && addr != 0) die("getproc: failure with a nonzero address");
            check_ctx(&FC);
        }
    }
    r = pe_mod_run_inits(&FC, &ops);                               /* forwarder targets loaded on demand */
    if (r != 0 && r != PE_MOD_E_INIT) die("second pe_mod_run_inits returned an unexpected code");
    check_ctx(&FC);

    /* free a few modules by refcount, then detach and free everything */
    for (i = 0; i < 4; i++) {
        int k = (int)rnd(PE_MOD_MAX_MODULES);
        if (FC.mod[k].state != PE_MOD_S_FREE) { (void)pe_mod_free(&FC, &ops, k); check_ctx(&FC); }
    }
    if (rnd(2)) { r = pe_mod_run_detach(&FC, &ops); if (r != 0) die("pe_mod_run_detach failed"); }
    pe_mod_free_all(&FC, &ops);
    for (i = 0; i < PE_MOD_MAX_MODULES; i++) if (FC.mod[i].state != PE_MOD_S_FREE) die("free_all left a module");
    if (FH.live_files != 0) die("file buffers leaked / double-released");
    if (FH.live_images != 0) die("images leaked");
    if (FH.frees > FH.allocs) die("free count exceeds alloc count");
    if (FH.nread > 4096 * 40) die("unbounded number of read_file calls");
    g_modruns++;
}

/* ------------------------------------------------------------------------------------------------ */
/* pipeline                                                                                          */
/* ------------------------------------------------------------------------------------------------ */
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

static void check_info(const pe_info_t *in)
{
    if (in->n_sections < 1 || in->n_sections > PE_MAX_SECTIONS) die("n_sections out of range");
    if (in->size_of_image == 0 || in->size_of_image > PE_MAX_IMAGE) die("size_of_image out of range");
    if ((u64)in->export_rva + in->export_size > in->size_of_image) die("export dir outside image");
    if ((u64)in->import_rva + in->import_size > in->size_of_image) die("import dir outside image");
}

static void run_one(fixture_t *fx, u32 nfx, u32 fi, const u8 *file, size_t len)
{
    pe_info_t info;
    int r = pe_parse(file, len, &info);

    if (g_iter == g_inject_at) { volatile u8 x = file[len]; (void)x; }               /* harness self-test: heap overflow */
    if (g_iter == g_inject_ub_at) { volatile int big = 0x7fffffff; big = big + (int)(g_iter & 1) + 1; (void)big; }
    if (r > 0 || r < -12) die("pe_parse returned an out-of-range code");
    g_stats_err[-r]++;
    if (r != PE_OK) {
        if (info.n_sections || info.size_of_image || info.export_rva) die("failed parse left a non-zero pe_info_t");
        g_iters_rej++;
    } else {
        g_iters_ok++;
        check_info(&info);
        if (info.size_of_image > BIG_IMAGE) g_skipped_big++;
        else {
            u8 *img = (u8 *)calloc(info.size_of_image, 1);                           /* EXACT size */
            g_mapped++;
            if (!img) die("calloc failed");
            r = pe_map(file, len, &info, img);
            if (r != PE_OK && !IN_SET(r, PE_E_TRUNC, PE_E_SECTIONS, PE_E_BOUNDS, PE_E_SIZE)) die("pe_map returned an unexpected code");
            r = pe_relocate(&info, img, pick_base(&info));
            if (r > 0 || r < -12) die("pe_relocate returned an unexpected code");
            exercise_exports(&info, img, &fx[fi]);
            if (rnd(5) == 0) {                                                       /* round 2: caller-owned pe_info_t corrupted */
                pe_info_t bad = info;
                switch (rnd(6)) {
                case 0: bad.export_rva = (u32)pick_value(&fx[fi], bad.export_rva, 4); break;
                case 1: bad.export_size = (u32)pick_value(&fx[fi], bad.export_size, 4); break;
                case 2:
                    bad.size_of_image = (u32)pick_value(&fx[fi], bad.size_of_image, 4);
                    if (bad.size_of_image > info.size_of_image) bad.size_of_image = info.size_of_image;
                    break;
                case 3: bad.delay_import_rva = (u32)pick_value(&fx[fi], bad.delay_import_rva, 4); break;
                case 4: bad.n_sections = (unsigned short)pick_value(&fx[fi], bad.n_sections, 2); break;
                default: bad.export_rva = bad.size_of_image - rnd(64); bad.export_size = rnd(200); break;
                }
                exercise_exports(&bad, img, &fx[fi]);
            }
            free(img);
        }
    }
    if (g_iter % 3 == 0) module_run(fx, nfx, fi, file, len);
}

/* ------------------------------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    static fixture_t fx[MAX_FIXTURES];
    u32 nfx = 0, i;
    u64 iters = 200000, seed = 0xD11F00D5EED5EED1ull;
    const char *e;
    struct timespec t0, t1;
    int a = 1;

    if (argc > 1 && (argv[1][0] >= '0' && argv[1][0] <= '9')) { seed = strtoull(argv[1], NULL, 0); a = 2; }
    if ((e = getenv("PE_DLL_FUZZ_ITERS")) != NULL) iters = strtoull(e, NULL, 0);
    if ((e = getenv("PE_DLL_FUZZ_INJECT")) != NULL) g_inject_at = strtoull(e, NULL, 0);
    if ((e = getenv("PE_DLL_FUZZ_INJECT_UB")) != NULL) g_inject_ub_at = strtoull(e, NULL, 0);
    if ((e = getenv("PE_DLL_FUZZ_CRASH")) != NULL) snprintf(g_crash_path, sizeof g_crash_path, "%s", e);
    g_rng = seed ? seed : 1;

    for (; a < argc && nfx < MAX_FIXTURES - 8; a++) {
        size_t len;
        u8 *d = load_file(argv[a], &len);
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        if (!d) { fprintf(stderr, "PE-DLL-FUZZ: cannot read fixture %s\n", argv[a]); return 2; }
        if (!prepare_fixture(&fx[nfx], base, d, len, 0)) return 2;
        nfx++;
    }
    if (!build_synthetics(fx, &nfx, MAX_FIXTURES)) return 2;

    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(save_crash);
    signal(SIGABRT, on_signal);
    if (!__sanitizer_set_death_callback) {
        signal(SIGSEGV, on_signal); signal(SIGBUS, on_signal); signal(SIGFPE, on_signal);
        signal(SIGILL, on_signal); signal(SIGTRAP, on_signal);
    }

    fprintf(stderr, "PE-DLL-FUZZ: seed=0x%llx iterations=%llu fixtures=%u\n", seed, iters, nfx);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (g_iter = 0; g_iter < iters; g_iter++) {
        struct timespec i0, i1;
        u32 fi = rnd(nfx);
        fixture_t *f = &fx[fi];
        size_t cap = f->len + 64, len = f->len;
        u8 *work = (u8 *)malloc(cap), *exact;
        double dt;
        clock_gettime(CLOCK_MONOTONIC, &i0);
        memcpy(work, f->data, len);
        if (g_iter > 0 || rnd(4)) len = mutate(f, work, len, cap, rnd(2));        /* iteration 0 may run the pristine file */
        exact = (u8 *)malloc(len ? len : 1);                                      /* EXACT size => ASan sees any overrun */
        memcpy(exact, work, len);
        free(work);
        g_cur = exact;
        g_cur_len = len;
        run_one(fx, nfx, fi, exact, len);
        g_cur = NULL;
        free(exact);
        clock_gettime(CLOCK_MONOTONIC, &i1);
        dt = (double)(i1.tv_sec - i0.tv_sec) + (double)(i1.tv_nsec - i0.tv_nsec) / 1e9;
        if (dt > ITER_SECONDS) { g_cur = NULL; die("iteration exceeded its wall-clock bound (unbounded work?)"); }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    fprintf(stderr, "PE-DLL-FUZZ: parse results:");
    for (i = 0; i < 14; i++) if (g_stats_err[i]) fprintf(stderr, " %s=%llu", pe_strerror(-(int)i), g_stats_err[i]);
    fprintf(stderr, "\nPE-DLL-FUZZ: mapped=%llu (skipped >8MiB: %llu) export: validated=%llu none=%llu malformed=%llu finds=%llu forwarders=%llu oracle_names=%llu delay_ok=%llu\n",
            g_mapped, g_skipped_big, g_validated, g_no_expdir, g_export_bad, g_finds, g_forwarders, g_oracle_names, g_delay_ok);
    fprintf(stderr, "PE-DLL-FUZZ: module loader: runs=%llu loads=%llu getprocs=%llu failed_inits=%llu  time=%.1fs\n",
            g_modruns, g_modloads, g_getprocs, g_modinits_failed,
            (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
    printf("PE-DLL-FUZZ: PASS iterations=%llu ok=%llu rejected=%llu validated=%llu modruns=%llu\n", iters, g_iters_ok, g_iters_rej, g_validated, g_modruns);
    return 0;
}
