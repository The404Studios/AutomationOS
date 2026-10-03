/*
 * pe_host_test.c -- host-side tests for userspace/lib/pe (the hostile-input-safe PE32+ parser/loader library).
 *
 *   usage: pe_host_test [nocrt_hello.exe] [crt_hello.exe] [reloc_hello.exe] [g_msg_ptr_va_hex] [synthetic_out.exe]
 *
 * Built by tests/win/run_pe_tests.sh with:  gcc -std=gnu11 -O1 -g -fsanitize=address,undefined
 *                                               -fno-sanitize-recover=all
 * Every buffer handed to the library is a heap block of EXACTLY the size the contract promises (file == len,
 * image == size_of_image), so AddressSanitizer turns any out-of-bounds access into an immediate abort.
 *
 * Groups:
 *   A  real MinGW fixtures (nocrt_hello / crt_hello / reloc_hello): fields, imports, relocation, TLS
 *   B  hand-built synthetic PE: exact relocation arithmetic (DIR64/HIGHLOW/HIGH/LOW), imports, TLS
 *   C  hostile images: header / section / size / alignment / directory / reloc / import / ordinal / CLR / TLS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pe.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_U64(got, want) do { u64 g_ = (u64)(got), w_ = (u64)(want); g_checks++; \
    if (g_ != w_) { g_fail++; printf("  FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#define CHECK_ERR(label, got, want) do { int g_ = (got), w_ = (want); g_checks++; \
    if (g_ != w_) { g_fail++; printf("  FAIL %s:%d: [%s] got %d (%s), expected %d (%s)\n", __FILE__, __LINE__, label, \
        g_, pe_strerror(g_), w_, pe_strerror(w_)); } } while (0)

/* ------------------------------------------------------------------------------------------------ */
/* little-endian poke/peek                                                                           */
/* ------------------------------------------------------------------------------------------------ */
static void p16(u8 *b, size_t o, u32 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
static void p32(u8 *b, size_t o, u32 v) { p16(b, o, v & 0xffffu); p16(b, o + 2, v >> 16); }
static void p64(u8 *b, size_t o, u64 v) { p32(b, o, (u32)v); p32(b, o + 4, (u32)(v >> 32)); }
static u32 g16(const u8 *b, size_t o) { return (u32)b[o] | ((u32)b[o + 1] << 8); }
static u32 g32(const u8 *b, size_t o) { return g16(b, o) | (g16(b, o + 2) << 16); }
static u64 g64(const u8 *b, size_t o) { return (u64)g32(b, o) | ((u64)g32(b, o + 4) << 32); }

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

/* parse a COPY of f[0..n) held in an exactly-sized heap block */
static int do_parse(const u8 *f, size_t n, pe_info_t *info)
{
    u8 *c = (u8 *)malloc(n ? n : 1);
    int r;
    memcpy(c, f, n);
    r = pe_parse(c, n, info);
    free(c);
    return r;
}

/* parse + map; on success *img is an exactly-sized calloc'd image; *fcopy is the exact-size file copy */
typedef struct { u8 *file; size_t flen; u8 *img; pe_info_t info; } loaded_t;

static int load_image(const u8 *f, size_t n, loaded_t *L)
{
    int r;
    memset(L, 0, sizeof *L);
    L->file = (u8 *)malloc(n ? n : 1);
    memcpy(L->file, f, n);
    L->flen = n;
    r = pe_parse(L->file, n, &L->info);
    if (r != PE_OK) return r;
    L->img = (u8 *)calloc(L->info.size_of_image, 1);
    return pe_map(L->file, n, &L->info, L->img);
}
static void free_loaded(loaded_t *L) { free(L->file); free(L->img); memset(L, 0, sizeof *L); }

/* ------------------------------------------------------------------------------------------------ */
/* fake import resolver                                                                              */
/* ------------------------------------------------------------------------------------------------ */
#define RLOG_MAX 1024
typedef struct {
    int mode;           /* 0 all ok; 1 fail "Bar" with a trap addr; 2 fail everything, out untouched; 3 count only */
    int calls;
    char dll[RLOG_MAX][48];
    char name[RLOG_MAX][96];
    unsigned ord[RLOG_MAX];
    int byord[RLOG_MAX];
} rlog_t;

#define FAKE_ADDR(i) (0x00007FF800000000ull + (u64)(i) * 0x100ull)
#define TRAP_ADDR    0xDEAD000000001000ull

static int fake_resolver(const char *dll, const char *name, unsigned short ord, int byord, u64 *out, void *user)
{
    rlog_t *L = (rlog_t *)user;
    int i = L->calls++;
    if (!dll || !name) { printf("  FAIL: resolver got NULL string\n"); g_fail++; return 1; }
    if (i < RLOG_MAX) {
        snprintf(L->dll[i], sizeof L->dll[i], "%s", dll);
        snprintf(L->name[i], sizeof L->name[i], "%s", name);
        L->ord[i] = ord;
        L->byord[i] = byord;
    }
    switch (L->mode) {
    case 0: *out = FAKE_ADDR(i); return 0;
    case 1: if (strcmp(name, "Bar") == 0) { *out = TRAP_ADDR; return 1; }
            *out = FAKE_ADDR(i); return 0;
    case 2: return 1;
    default: *out = 1; return 0;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* synthetic PE builder                                                                              */
/* ------------------------------------------------------------------------------------------------ */
#define BASE     0x140000000ull
#define FILE_SZ  0xc00u
#define COFF     0x84u
#define OPT      0x98u
#define SECT     (OPT + 240u)
#define DIRO(i)  (OPT + 112u + 8u * (u32)(i))
enum { D_EXPORT = 0, D_IMPORT = 1, D_RELOC = 5, D_TLS = 9, D_DELAY = 13, D_CLR = 14 };
#define SECO(k)  (SECT + 40u * (u32)(k))      /* +0 name, +8 vsize, +12 vrva, +16 rawsz, +20 rawoff, +36 flags */

static u32 align_up(u32 v, u32 a) { return (v + a - 1u) & ~(a - 1u); }

static void set_dir(u8 *f, int idx, u32 rva, u32 size) { p32(f, DIRO(idx), rva); p32(f, DIRO(idx) + 4, size); }
static void set_sec(u8 *f, int k, const char *name, u32 vsize, u32 vrva, u32 rawsz, u32 rawoff, u32 flags)
{
    size_t i;
    for (i = 0; i < 8; i++) f[SECO(k) + i] = (u8)(i < strlen(name) ? name[i] : 0);
    p32(f, SECO(k) + 8, vsize);
    p32(f, SECO(k) + 12, vrva);
    p32(f, SECO(k) + 16, rawsz);
    p32(f, SECO(k) + 20, rawoff);
    p32(f, SECO(k) + 36, flags);
}

/* Layout (default): .text 0x1000 .data 0x2000 .idata 0x3000 .reloc 0x4000, SizeOfImage 0x5000, file 0xc00 bytes.
 *   .data: 0x2000 g_msg_ptr(DIR64) 0x2008 u32(HIGHLOW) 0x200c u16(HIGH) 0x200e u16(LOW) 0x2040 TLS dir
 *          0x2100 TLS template 0x2120 tls index 0x2130 callbacks[2]
 *   .idata: desc0 0x3000 (TEST.dll: Foo, #5, Bar)  desc1 0x3014 (OTHER.dll: Baz)  null 0x3028
 *           OFT0 0x3040  FT0 0x3060  OFT1 0x3080  FT1 0x3090  names 0x30c0 0x30e0  hint/name 0x3100 0x3110 0x3120
 *   .reloc: block A page 0x2000 (10 entries, 28 bytes) + block B page 0x1000 (2 entries, 12 bytes) = 40 bytes */
static void build_ex(u8 *f, u32 idata_vsize, u32 reloc_vsize)
{
    u32 reloc_rva = 0x3000u + align_up(idata_vsize, 0x1000u);
    u32 soi = reloc_rva + align_up(reloc_vsize, 0x1000u);
    u8 *t = f + 0x400, *d = f + 0x600, *id = f + 0x800, *rl = f + 0xa00;
    u32 i;
    static const u16 blkA[10] = { 0xA000, 0x3008, 0x100c, 0x200e, 0xA040, 0xA048, 0xA050, 0xA058, 0xA130, 0x0000 };
    static const u16 blkB[2] = { 0xA020, 0x0000 };

    memset(f, 0, FILE_SZ);
    f[0] = 'M'; f[1] = 'Z';
    p32(f, 0x3c, 0x80);
    f[0x80] = 'P'; f[0x81] = 'E';
    p16(f, COFF + 0, 0x8664);
    p16(f, COFF + 2, 4);
    p16(f, COFF + 16, 240);
    p16(f, COFF + 18, 0x0022);
    p16(f, OPT + 0, 0x20b);
    p32(f, OPT + 16, 0x1000);                       /* entry */
    p64(f, OPT + 24, BASE);
    p32(f, OPT + 32, 0x1000);
    p32(f, OPT + 36, 0x200);
    p32(f, OPT + 56, soi);
    p32(f, OPT + 60, 0x400);
    p16(f, OPT + 68, 3);
    p16(f, OPT + 70, 0x160);
    p64(f, OPT + 72, 0x200000);
    p64(f, OPT + 80, 0x1000);
    p32(f, OPT + 108, 16);
    set_sec(f, 0, ".text", 0x100, 0x1000, 0x200, 0x400, 0x60000020u);
    set_sec(f, 1, ".data", 0x200, 0x2000, 0x200, 0x600, 0xC0000040u);
    set_sec(f, 2, ".idata", idata_vsize, 0x3000, 0x200, 0x800, 0xC0000040u);
    set_sec(f, 3, ".reloc", reloc_vsize, reloc_rva, 0x200, 0xa00, 0x42000040u);
    set_dir(f, D_IMPORT, 0x3000, 0x3c);
    set_dir(f, D_RELOC, reloc_rva, 0x28);
    set_dir(f, D_TLS, 0x2040, 40);

    /* .text: ret sled + one pointer (DIR64 in block B) */
    for (i = 0; i < 0x100; i++) t[i] = 0xC3;
    p64(t, 0x20, BASE + 0x1000);
    /* .data */
    p64(d, 0x000, BASE + 0x1010);                   /* g_msg_ptr */
    p32(d, 0x008, 0x12345678u);
    p16(d, 0x00c, 0x1111);
    p16(d, 0x00e, 0x2222);
    p64(d, 0x040, BASE + 0x2100);                   /* TLS dir: start,end,index,callbacks,zerofill,chars */
    p64(d, 0x048, BASE + 0x2110);
    p64(d, 0x050, BASE + 0x2120);
    p64(d, 0x058, BASE + 0x2130);
    for (i = 0; i < 16; i++) d[0x100 + i] = (u8)('T' + i);
    p64(d, 0x130, BASE + 0x1020);                   /* callback[0]; callback[1] = 0 terminator */
    /* .idata */
    p32(id, 0x00, 0x3040); p32(id, 0x0c, 0x30c0); p32(id, 0x10, 0x3060);
    p32(id, 0x14, 0x3080); p32(id, 0x20, 0x30e0); p32(id, 0x24, 0x3090);
    p64(id, 0x40, 0x3100); p64(id, 0x48, 0x8000000000000005ull); p64(id, 0x50, 0x3110);
    p64(id, 0x60, 0x3100); p64(id, 0x68, 0x8000000000000005ull); p64(id, 0x70, 0x3110);
    p64(id, 0x80, 0x3120); p64(id, 0x90, 0x3120);
    memcpy(id + 0xc0, "TEST.dll", 9);
    memcpy(id + 0xe0, "OTHER.dll", 10);
    p16(id, 0x100, 1); memcpy(id + 0x102, "Foo", 4);
    p16(id, 0x110, 2); memcpy(id + 0x112, "Bar", 4);
    p16(id, 0x120, 3); memcpy(id + 0x122, "Baz", 4);
    /* .reloc */
    p32(rl, 0, 0x2000); p32(rl, 4, 28);
    for (i = 0; i < 10; i++) p16(rl, 8 + 2 * i, blkA[i]);
    p32(rl, 28, 0x1000); p32(rl, 32, 12);
    for (i = 0; i < 2; i++) p16(rl, 36 + 2 * i, blkB[i]);
}
static void build_good(u8 *f) { build_ex(f, 0x200, 0x28); }

static u8 g_m[FILE_SZ];
static u8 *fresh(void) { build_good(g_m); return g_m; }

/* many-section variant: n tiny bss-like sections (no raw data) */
static size_t build_many(u8 *f, size_t cap, u32 n)
{
    u32 soh = align_up(SECT + 40u * n, 0x200u), i, soi = align_up(soh, 0x1000u) + 0x1000u * n;
    memset(f, 0, cap);
    f[0] = 'M'; f[1] = 'Z'; p32(f, 0x3c, 0x80); f[0x80] = 'P'; f[0x81] = 'E';
    p16(f, COFF + 0, 0x8664); p16(f, COFF + 2, n); p16(f, COFF + 16, 240); p16(f, COFF + 18, 0x0022);
    p16(f, OPT + 0, 0x20b); p32(f, OPT + 16, align_up(soh, 0x1000u)); p64(f, OPT + 24, BASE);
    p32(f, OPT + 32, 0x1000); p32(f, OPT + 36, 0x200); p32(f, OPT + 56, soi); p32(f, OPT + 60, soh);
    p16(f, OPT + 68, 3); p32(f, OPT + 108, 16);
    for (i = 0; i < n; i++) {
        size_t o = SECT + 40u * i;
        memcpy(f + o, ".b", 3);
        p32(f, o + 8, 0x10); p32(f, o + 12, align_up(soh, 0x1000u) + 0x1000u * i); p32(f, o + 36, 0xC0000080u);
    }
    return soh;
}

/* ------------------------------------------------------------------------------------------------ */
/* parse-expectation helper                                                                          */
/* ------------------------------------------------------------------------------------------------ */
static void expect_parse(const char *label, const u8 *f, size_t n, int want)
{
    pe_info_t info;
    int r = do_parse(f, n, &info);
    CHECK_ERR(label, r, want);
    if (r != PE_OK) {                     /* failure contract: info is zeroed */
        const u8 *p = (const u8 *)&info;
        size_t i;
        int nz = 0;
        for (i = 0; i < sizeof info; i++) nz |= p[i];
        CHECK(nz == 0);
    }
}
#define MUT(label, want, ...) do { u8 *m = fresh(); __VA_ARGS__; expect_parse(label, m, FILE_SZ, want); } while (0)

/* ------------------------------------------------------------------------------------------------ */
/* Group B: synthetic PE, positive paths                                                             */
/* ------------------------------------------------------------------------------------------------ */
static void test_synth_good(u8 *synth_out_copy)
{
    loaded_t L;
    int r;
    u32 i;
    u64 base2, delta;
    u8 *before;
    static const u32 patched[][2] = { {0x2000, 8}, {0x2008, 4}, {0x200c, 2}, {0x200e, 2}, {0x2040, 8}, {0x2048, 8},
                                      {0x2050, 8}, {0x2058, 8}, {0x2130, 8}, {0x1020, 8} };
    u8 *mask;

    printf("[B] synthetic PE: parse/map/relocate/imports/tls\n");
    build_good(g_m);
    memcpy(synth_out_copy, g_m, FILE_SZ);
    r = load_image(g_m, FILE_SZ, &L);
    CHECK_ERR("synth good parse+map", r, PE_OK);
    CHECK_U64(L.info.image_base, BASE);
    CHECK_U64(L.info.size_of_image, 0x5000);
    CHECK_U64(L.info.size_of_headers, 0x400);
    CHECK_U64(L.info.entry_rva, 0x1000);
    CHECK_U64(L.info.section_alignment, 0x1000);
    CHECK_U64(L.info.file_alignment, 0x200);
    CHECK_U64(L.info.subsystem, 3);
    CHECK_U64(L.info.dll_characteristics, 0x160);
    CHECK_U64(L.info.n_sections, 4);
    CHECK_U64(L.info.is_dll, 0);
    CHECK_U64(L.info.stack_reserve, 0x200000);
    CHECK_U64(L.info.stack_commit, 0x1000);
    CHECK(strcmp(L.info.sec[0].name, ".text") == 0 && strcmp(L.info.sec[2].name, ".idata") == 0);
    CHECK_U64(L.info.sec[1].vrva, 0x2000);
    CHECK_U64(L.info.sec[3].raw_off, 0xa00);
    CHECK_U64(L.info.import_rva, 0x3000);
    CHECK_U64(L.info.reloc_rva, 0x4000);
    CHECK_U64(L.info.reloc_size, 0x28);
    CHECK_U64(L.info.tls_rva, 0x2040);
    CHECK_U64(L.info.delay_import_rva, 0);
    CHECK(strcmp(pe_strerror(PE_OK), "ok") == 0);
    /* map fidelity: headers and section bytes land at their RVAs, gaps stay zero */
    CHECK(L.img[0] == 'M' && L.img[1] == 'Z');
    CHECK(memcmp(L.img, g_m, 0x400) == 0);
    CHECK(memcmp(L.img + 0x1000, g_m + 0x400, 0x100) == 0);       /* .text copies min(raw, vsize) = 0x100 */
    CHECK(L.img[0x1100] == 0);                                    /* beyond vsize: not copied */
    CHECK(memcmp(L.img + 0x2000, g_m + 0x600, 0x200) == 0);
    CHECK(L.img[0x2300] == 0 && L.img[0x4fff] == 0);

    /* relocation at a different base: exact arithmetic for every type */
    before = (u8 *)malloc(L.info.size_of_image);
    memcpy(before, L.img, L.info.size_of_image);
    base2 = 0x400000ull;
    delta = base2 - BASE;
    r = pe_relocate(&L.info, L.img, base2);
    CHECK_ERR("synth relocate", r, PE_OK);
    CHECK_U64(g64(L.img, 0x2000), 0x401010ull);                   /* g_msg_ptr moved by exactly delta */
    CHECK_U64(g64(L.img, 0x2000), g64(before, 0x2000) + delta);
    CHECK_U64(g32(L.img, 0x2008), (u32)(0x12345678u + (u32)delta));
    CHECK_U64(g16(L.img, 0x200c), (u16)(0x1111u + (u16)(delta >> 16)));
    CHECK_U64(g16(L.img, 0x200e), (u16)(0x2222u + (u16)delta));
    CHECK_U64(g64(L.img, 0x2040), base2 + 0x2100);
    CHECK_U64(g64(L.img, 0x2058), base2 + 0x2130);
    CHECK_U64(g64(L.img, 0x2130), base2 + 0x1020);                /* callback[0] = BASE+0x1020 -> base2+0x1020 */
    CHECK_U64(g64(L.img, 0x1020), base2 + 0x1000);
    mask = (u8 *)calloc(L.info.size_of_image, 1);
    for (i = 0; i < sizeof patched / sizeof patched[0]; i++) memset(mask + patched[i][0], 1, patched[i][1]);
    for (i = 0; i < L.info.size_of_image; i++)
        if (!mask[i] && L.img[i] != before[i]) { CHECK(!"byte outside reloc targets changed"); break; }
    free(mask);

    /* tls info (after relocation) */
    {
        u64 s = 0, e = 0, idx = 0, cb = 0;
        r = pe_tls_info(&L.info, L.img, base2, &s, &e, &idx, &cb);
        CHECK_ERR("synth tls", r, PE_OK);
        CHECK_U64(s, base2 + 0x2100); CHECK_U64(e, base2 + 0x2110);
        CHECK_U64(idx, base2 + 0x2120); CHECK_U64(cb, base2 + 0x2130);
        r = pe_tls_info(&L.info, L.img, base2, NULL, NULL, NULL, NULL);       /* all outputs optional */
        CHECK_ERR("synth tls null outs", r, PE_OK);
        /* image still carries preferred-base VAs for a different base => rejected, never trusted */
        r = pe_tls_info(&L.info, before, base2, &s, &e, &idx, &cb);
        CHECK_ERR("tls unrelocated at other base", r, PE_E_BOUNDS);
        r = pe_tls_info(&L.info, before, BASE, &s, &e, &idx, &cb);
        CHECK_ERR("tls unrelocated at preferred base", r, PE_OK);
    }
    free(before);
    free_loaded(&L);

    /* a second base with a wrapping (negative) delta and high bits set */
    r = load_image(g_m, FILE_SZ, &L);
    CHECK_ERR("reload", r, PE_OK);
    base2 = 0x00007FF712340000ull;
    r = pe_relocate(&L.info, L.img, base2);
    CHECK_ERR("relocate to 7FF7...", r, PE_OK);
    CHECK_U64(g64(L.img, 0x2000), base2 + 0x1010);
    CHECK_U64(g64(L.img, 0x2048), base2 + 0x2110);
    free_loaded(&L);

    /* delta == 0: succeeds, changes nothing */
    r = load_image(g_m, FILE_SZ, &L);
    before = (u8 *)malloc(L.info.size_of_image);
    memcpy(before, L.img, L.info.size_of_image);
    r = pe_relocate(&L.info, L.img, BASE);
    CHECK_ERR("relocate delta 0", r, PE_OK);
    CHECK(memcmp(before, L.img, L.info.size_of_image) == 0);
    free(before);
    free_loaded(&L);

    /* top-of-address-space base: modular delta, no UB, exact result */
    r = load_image(g_m, FILE_SZ, &L);
    base2 = 0xFFFFFFFFFFFF0000ull;
    r = pe_relocate(&L.info, L.img, base2);
    CHECK_ERR("relocate to top", r, PE_OK);
    CHECK_U64(g64(L.img, 0x2000), base2 + 0x1010);
    r = pe_tls_info(&L.info, L.img, base2, NULL, NULL, NULL, NULL);
    CHECK_ERR("tls at last 64K base (fits)", r, PE_OK);
    CHECK_ERR("tls at wrapping base", pe_tls_info(&L.info, L.img, 0xFFFFFFFFFFFFF000ull, NULL, NULL, NULL, NULL), PE_E_BOUNDS);   /* base + size_of_image wraps */
    free_loaded(&L);

    /* no reloc dir */
    {
        u8 *m = fresh();
        set_dir(m, D_RELOC, 0, 0);
        r = load_image(m, FILE_SZ, &L);
        CHECK_ERR("no reloc dir load", r, PE_OK);
        CHECK_ERR("no reloc dir, delta != 0", pe_relocate(&L.info, L.img, 0x400000), PE_E_RELOC);
        CHECK_ERR("no reloc dir, delta == 0", pe_relocate(&L.info, L.img, BASE), PE_OK);
        free_loaded(&L);
    }

    /* no TLS dir => 1 */
    {
        u8 *m = fresh();
        u64 s = 77;
        set_dir(m, D_TLS, 0, 0);
        r = load_image(m, FILE_SZ, &L);
        CHECK_ERR("no tls load", r, PE_OK);
        CHECK_ERR("no tls => 1", pe_tls_info(&L.info, L.img, BASE, &s, NULL, NULL, NULL), 1);
        CHECK_U64(s, 77);
        free_loaded(&L);
    }

    /* DLL with entry 0, vsize==0 => effective vsize = raw size */
    {
        u8 *m = fresh();
        pe_info_t info;
        p16(m, COFF + 18, 0x2022);
        p32(m, OPT + 16, 0);
        p32(m, SECO(0) + 8, 0);
        r = do_parse(m, FILE_SZ, &info);
        CHECK_ERR("dll entry 0", r, PE_OK);
        CHECK_U64(info.is_dll, 1);
        CHECK_U64(info.sec[0].vsize, 0x200);
    }

    /* section-count boundaries */
    {
        u8 big[0x2000];
        pe_info_t info;
        size_t soh = build_many(big, sizeof big, 96);
        r = do_parse(big, soh, &info);
        CHECK_ERR("96 sections", r, PE_OK);
        CHECK_U64(info.n_sections, 96);
        soh = build_many(big, sizeof big, 1);
        CHECK_ERR("1 section", do_parse(big, soh, &info), PE_OK);
        soh = build_many(big, sizeof big, 96);
        p16(big, COFF + 2, 97);
        CHECK_ERR("97 sections", do_parse(big, soh, &info), PE_E_SECTIONS);
        p16(big, COFF + 2, 0);
        CHECK_ERR("0 sections", do_parse(big, soh, &info), PE_E_SECTIONS);
        p16(big, COFF + 2, 0xffff);
        CHECK_ERR("65535 sections", do_parse(big, soh, &info), PE_E_SECTIONS);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Group B: imports on the synthetic PE                                                              */
/* ------------------------------------------------------------------------------------------------ */
static void plant_hn(u8 *img, u32 rva, u32 hint, const char *name)
{
    p16(img, rva, hint);
    memcpy(img + rva + 2, name, strlen(name) + 1);
}

static void test_synth_imports(void)
{
    static rlog_t L;
    loaded_t Ld;
    u32 n = 99, u = 99;
    int r;

    printf("[B] synthetic PE: imports\n");
    build_good(g_m);
    r = load_image(g_m, FILE_SZ, &Ld);
    CHECK_ERR("imp load", r, PE_OK);
    memset(&L, 0, sizeof L);
    r = pe_resolve_imports(&Ld.info, Ld.img, fake_resolver, &L, &n, &u);
    CHECK_ERR("imp resolve", r, PE_OK);
    CHECK_U64(n, 4);
    CHECK_U64(u, 0);
    CHECK_U64(L.calls, 4);
    CHECK(strcmp(L.dll[0], "TEST.dll") == 0 && strcmp(L.name[0], "Foo") == 0 && L.ord[0] == 1 && L.byord[0] == 0);
    CHECK(strcmp(L.dll[1], "TEST.dll") == 0 && L.name[1][0] == 0 && L.ord[1] == 5 && L.byord[1] == 1);
    CHECK(strcmp(L.dll[2], "TEST.dll") == 0 && strcmp(L.name[2], "Bar") == 0 && L.ord[2] == 2);
    CHECK(strcmp(L.dll[3], "OTHER.dll") == 0 && strcmp(L.name[3], "Baz") == 0 && L.ord[3] == 3 && L.byord[3] == 0);
    CHECK_U64(g64(Ld.img, 0x3060), FAKE_ADDR(0));
    CHECK_U64(g64(Ld.img, 0x3068), FAKE_ADDR(1));
    CHECK_U64(g64(Ld.img, 0x3070), FAKE_ADDR(2));
    CHECK_U64(g64(Ld.img, 0x3078), 0);                            /* terminator untouched */
    CHECK_U64(g64(Ld.img, 0x3090), FAKE_ADDR(3));
    CHECK_U64(g64(Ld.img, 0x3040), 0x3100);                       /* OFT (lookup table) is never written */
    free_loaded(&Ld);

    /* partial failure: one slot gets the trap address and is counted unresolved; the walk continues */
    r = load_image(g_m, FILE_SZ, &Ld);
    memset(&L, 0, sizeof L);
    L.mode = 1;
    r = pe_resolve_imports(&Ld.info, Ld.img, fake_resolver, &L, &n, &u);
    CHECK_ERR("imp partial", r, PE_OK);
    CHECK_U64(n, 4); CHECK_U64(u, 1);
    CHECK_U64(g64(Ld.img, 0x3070), TRAP_ADDR);
    CHECK_U64(g64(Ld.img, 0x3090), FAKE_ADDR(3));
    free_loaded(&Ld);

    /* total failure with no out_addr: slots are zeroed (a call faults at VA 0, never at an attacker RVA) */
    r = load_image(g_m, FILE_SZ, &Ld);
    memset(&L, 0, sizeof L);
    L.mode = 2;
    r = pe_resolve_imports(&Ld.info, Ld.img, fake_resolver, &L, &n, &u);
    CHECK_ERR("imp all fail", r, PE_OK);
    CHECK_U64(n, 4); CHECK_U64(u, 4);
    CHECK_U64(g64(Ld.img, 0x3060), 0);
    CHECK_U64(g64(Ld.img, 0x3068), 0);
    CHECK_U64(g64(Ld.img, 0x3090), 0);
    free_loaded(&Ld);

    /* dry run: fn == NULL validates + counts, writes nothing; NULL counters are allowed */
    r = load_image(g_m, FILE_SZ, &Ld);
    r = pe_resolve_imports(&Ld.info, Ld.img, NULL, NULL, &n, &u);
    CHECK_ERR("imp dry run", r, PE_OK);
    CHECK_U64(n, 4); CHECK_U64(u, 0);
    CHECK_U64(g64(Ld.img, 0x3060), 0x3100);
    r = pe_resolve_imports(&Ld.info, Ld.img, NULL, NULL, NULL, NULL);
    CHECK_ERR("imp dry run null counters", r, PE_OK);
    free_loaded(&Ld);

    /* OriginalFirstThunk == 0 => names are read from FirstThunk itself (read-then-overwrite per slot) */
    {
        u8 *m = fresh();
        p32(m, 0x800 + 0x00, 0);
        r = load_image(m, FILE_SZ, &Ld);
        memset(&L, 0, sizeof L);
        r = pe_resolve_imports(&Ld.info, Ld.img, fake_resolver, &L, &n, &u);
        CHECK_ERR("imp OFT==0", r, PE_OK);
        CHECK_U64(n, 4);
        CHECK(strcmp(L.name[2], "Bar") == 0 && L.byord[1] == 1);
        CHECK_U64(g64(Ld.img, 0x3070), FAKE_ADDR(2));
        free_loaded(&Ld);
    }

    /* image with no import dir */
    {
        u8 *m = fresh();
        set_dir(m, D_IMPORT, 0, 0);
        r = load_image(m, FILE_SZ, &Ld);
        r = pe_resolve_imports(&Ld.info, Ld.img, fake_resolver, &L, &n, &u);
        CHECK_ERR("imp none", r, PE_OK);
        CHECK_U64(n, 0); CHECK_U64(u, 0);
        free_loaded(&Ld);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Group C: hostile headers                                                                          */
/* ------------------------------------------------------------------------------------------------ */
static void test_hostile_parse(void)
{
    size_t L;
    u32 big;
    printf("[C] hostile headers / sections / sizes / alignment / directories\n");

    CHECK_ERR("good", (expect_parse("good", fresh(), FILE_SZ, PE_OK), PE_OK), PE_OK);

    /* truncation: every prefix of the synthetic file is rejected as PE_E_TRUNC; the full file parses */
    build_good(g_m);
    for (L = 0; L < FILE_SZ; L++) {
        pe_info_t info;
        int r = do_parse(g_m, L, &info);
        if (r != PE_E_TRUNC) { CHECK_ERR("prefix", r, PE_E_TRUNC); printf("    (prefix length %zu)\n", L); break; }
        g_checks++;
    }
    expect_parse("full", g_m, FILE_SZ, PE_OK);
    { pe_info_t info; CHECK_ERR("NULL file", pe_parse(NULL, 100, &info), PE_E_TRUNC);
      CHECK_ERR("NULL out", pe_parse(g_m, FILE_SZ, NULL), PE_E_BOUNDS); }

    /* DOS header / e_lfanew */
    MUT("MZ wrong", PE_E_MAGIC, m[0] = 'X');
    MUT("MZ swapped", PE_E_MAGIC, m[0] = 'Z'; m[1] = 'M');
    MUT("e_lfanew == len", PE_E_TRUNC, p32(m, 0x3c, FILE_SZ));
    MUT("e_lfanew past EOF", PE_E_TRUNC, p32(m, 0x3c, 0x7fffffff));
    MUT("e_lfanew 0xFFFFFFFF", PE_E_TRUNC, p32(m, 0x3c, 0xffffffffu));
    MUT("e_lfanew 0xFFFFFFF0", PE_E_TRUNC, p32(m, 0x3c, 0xfffffff0u));
    MUT("e_lfanew len-23", PE_E_TRUNC, p32(m, 0x3c, FILE_SZ - 23));
    MUT("e_lfanew inside DOS hdr", PE_E_BOUNDS, p32(m, 0x3c, 0x10));
    MUT("e_lfanew 0", PE_E_BOUNDS, p32(m, 0x3c, 0));
    MUT("PE sig wrong", PE_E_MAGIC, m[0x80] = 'Q');
    MUT("PE sig tail", PE_E_MAGIC, m[0x82] = 1);
    MUT("e_lfanew -> garbage", PE_E_MAGIC, p32(m, 0x3c, 0x200));

    /* magic / machine */
    MUT("PE32 (0x10b)", PE_E_NOT_PE32PLUS, p16(m, OPT, 0x10b));
    MUT("PE32 + i386", PE_E_NOT_PE32PLUS, p16(m, OPT, 0x10b); p16(m, COFF, 0x14c));
    MUT("ROM magic 0x107", PE_E_MAGIC, p16(m, OPT, 0x107));
    MUT("opt magic 0", PE_E_MAGIC, p16(m, OPT, 0));
    MUT("ARM64 machine", PE_E_MACHINE, p16(m, COFF, 0xAA64));
    MUT("i386 machine", PE_E_MACHINE, p16(m, COFF, 0x14c));
    MUT("machine 0", PE_E_MACHINE, p16(m, COFF, 0));
    MUT("ARM machine", PE_E_MACHINE, p16(m, COFF, 0x1c0));

    /* optional header size / directory count */
    MUT("SizeOfOptionalHeader 0", PE_E_BOUNDS, p16(m, COFF + 16, 0));
    MUT("SizeOfOptionalHeader 1", PE_E_BOUNDS, p16(m, COFF + 16, 1));
    MUT("SizeOfOptionalHeader 100", PE_E_BOUNDS, p16(m, COFF + 16, 100));
    MUT("SizeOfOptionalHeader 0xE0 (<16 dirs)", PE_E_BOUNDS, p16(m, COFF + 16, 0xE0));
    MUT("SizeOfOptionalHeader past EOF", PE_E_TRUNC, p16(m, COFF + 16, 0xffff));
    MUT("NumberOfRvaAndSizes 17", PE_E_BOUNDS, p32(m, OPT + 108, 17));
    MUT("NumberOfRvaAndSizes 0xFFFFFFFF", PE_E_BOUNDS, p32(m, OPT + 108, 0xffffffffu));
    MUT("NumberOfRvaAndSizes 0 (no dirs) ok", PE_OK, p32(m, OPT + 108, 0));
    MUT("NumberOfRvaAndSizes 6 (no TLS dir) ok", PE_OK, p32(m, OPT + 108, 6));

    /* section count / table placement */
    MUT("0 sections", PE_E_SECTIONS, p16(m, COFF + 2, 0));
    MUT("97 sections", PE_E_SECTIONS, p16(m, COFF + 2, 97));
    MUT("96 sections but file too short", PE_E_TRUNC, p16(m, COFF + 2, 96));
    MUT("SizeOfHeaders < section table", PE_E_SECTIONS, p32(m, OPT + 60, 0x100));

    /* section geometry */
    MUT("section vsize 0x10000 > SizeOfImage", PE_E_SECTIONS, p32(m, SECO(0) + 8, 0x10000));
    MUT("section vsize 0xFFFFFFFF", PE_E_SECTIONS, p32(m, SECO(0) + 8, 0xffffffffu));
    MUT("section vrva 0xFFFFF000", PE_E_SECTIONS, p32(m, SECO(3) + 12, 0xfffff000u));
    MUT("section vrva 0x5000 (== SizeOfImage)", PE_E_SECTIONS, p32(m, SECO(3) + 12, 0x5000));
    MUT("section vrva misaligned", PE_E_ALIGN, p32(m, SECO(0) + 12, 0x1800));
    MUT("section vrva misaligned by 1", PE_E_ALIGN, p32(m, SECO(1) + 12, 0x2001));
    MUT("section inside header pages", PE_E_SECTIONS, p32(m, SECO(0) + 12, 0));
    MUT("sections overlap (same rva)", PE_E_SECTIONS, p32(m, SECO(1) + 12, 0x1000));
    MUT("sections overlap (vsize spill)", PE_E_SECTIONS, p32(m, SECO(0) + 8, 0x2001));
    MUT("sections touching (extent == one page) ok", PE_OK, p32(m, SECO(0) + 8, 0x1000));
    MUT("vsize spills onto a 2nd page, next section clear of it ok", PE_OK, p32(m, SECO(0) + 8, 0x1800);
        p32(m, SECO(1) + 12, 0x3000); p32(m, SECO(2) + 12, 0x4000); p32(m, SECO(3) + 12, 0x5000); p32(m, OPT + 56, 0x6000));
    MUT("sections out of order but disjoint ok", PE_OK,
        p32(m, SECO(1) + 12, 0x3000); p32(m, SECO(2) + 12, 0x2000));
    MUT("SizeOfImage shrunk under last section", PE_E_SECTIONS, p32(m, OPT + 56, 0x4000));
    MUT("raw past EOF", PE_E_TRUNC, p32(m, SECO(3) + 16, 0x10000));
    MUT("raw size 0xFFFFFFFF", PE_E_TRUNC, p32(m, SECO(0) + 16, 0xffffffffu));
    MUT("raw off+size wraps u32", PE_E_TRUNC, p32(m, SECO(0) + 20, 0xfffffe00u); p32(m, SECO(0) + 16, 0x400));
    MUT("raw off misaligned", PE_E_ALIGN, p32(m, SECO(0) + 20, 0x401));
    MUT("raw off only 0x100 aligned", PE_E_ALIGN, p32(m, SECO(0) + 20, 0x500));
    MUT("section name non-printable ok", PE_OK, m[SECO(0)] = 0x01; m[SECO(0) + 1] = 0xff);

    /* SizeOfImage / SizeOfHeaders */
    MUT("SizeOfImage 0", PE_E_SIZE, p32(m, OPT + 56, 0));
    MUT("SizeOfImage 0xFFFFFFFF", PE_E_SIZE, p32(m, OPT + 56, 0xffffffffu));
    MUT("SizeOfImage 0x80000000", PE_E_SIZE, p32(m, OPT + 56, 0x80000000u));
    MUT("SizeOfImage MAX+0x1000", PE_E_SIZE, p32(m, OPT + 56, PE_MAX_IMAGE + 0x1000u));
    MUT("SizeOfImage not multiple of SectionAlignment", PE_E_SIZE, p32(m, OPT + 56, 0x5001));
    MUT("SizeOfHeaders 0", PE_E_SIZE, p32(m, OPT + 60, 0));
    MUT("SizeOfHeaders > SizeOfImage", PE_E_SIZE, p32(m, OPT + 60, 0x6000));
    MUT("SizeOfHeaders > file", PE_E_TRUNC, p32(m, OPT + 60, 0x4000));

    /* alignment */
    MUT("FileAlignment 0x100", PE_E_ALIGN, p32(m, OPT + 36, 0x100));
    MUT("FileAlignment 0", PE_E_ALIGN, p32(m, OPT + 36, 0));
    MUT("FileAlignment 0x300", PE_E_ALIGN, p32(m, OPT + 36, 0x300));
    MUT("FileAlignment 0x20000", PE_E_ALIGN, p32(m, OPT + 36, 0x20000));
    MUT("SectionAlignment 0x800", PE_E_ALIGN, p32(m, OPT + 32, 0x800));
    MUT("SectionAlignment 0x1800", PE_E_ALIGN, p32(m, OPT + 32, 0x1800));
    MUT("SectionAlignment 0", PE_E_ALIGN, p32(m, OPT + 32, 0));
    MUT("SectionAlignment 0x20000", PE_E_ALIGN, p32(m, OPT + 32, 0x20000));
    MUT("SectionAlignment < FileAlignment", PE_E_ALIGN, p32(m, OPT + 36, 0x2000));
    MUT("ImageBase misaligned", PE_E_ALIGN, p64(m, OPT + 24, BASE + 0x800));
    MUT("ImageBase + SizeOfImage wraps address space", PE_E_BOUNDS, p64(m, OPT + 24, 0xffffffffffff0000ull); p32(m, OPT + 56, 0x20000));
    MUT("ImageBase 0x400000 ok", PE_OK, p64(m, OPT + 24, 0x400000));

    /* entry point */
    MUT("entry 0 on EXE", PE_E_BOUNDS, p32(m, OPT + 16, 0));
    MUT("entry past image", PE_E_BOUNDS, p32(m, OPT + 16, 0x5000));
    MUT("entry 0xFFFFFFFF", PE_E_BOUNDS, p32(m, OPT + 16, 0xffffffffu));
    MUT("entry inside headers", PE_E_BOUNDS, p32(m, OPT + 16, 0x10));
    MUT("entry in aligned slack ok", PE_OK, p32(m, OPT + 16, 0x1f00));
    MUT("entry in gap between sections", PE_E_BOUNDS, p32(m, SECO(1) + 12, 0x3000); p32(m, SECO(2) + 12, 0x4000);
        p32(m, SECO(3) + 12, 0x5000); p32(m, OPT + 56, 0x6000); p32(m, OPT + 16, 0x2800));

    /* directory ranges (parse-level => PE_E_BOUNDS) */
    MUT("import dir rva outside image", PE_E_BOUNDS, set_dir(m, D_IMPORT, 0x10000, 0x28));
    MUT("import dir rva == SizeOfImage", PE_E_BOUNDS, set_dir(m, D_IMPORT, 0x5000, 0x14));
    MUT("import dir size runs off image", PE_E_BOUNDS, set_dir(m, D_IMPORT, 0x3000, 0x3000));
    MUT("import dir size 0xFFFFFFFF", PE_E_BOUNDS, set_dir(m, D_IMPORT, 0x3000, 0xffffffffu));
    MUT("import dir rva+size wraps u32", PE_E_BOUNDS, set_dir(m, D_IMPORT, 0xfffffff0u, 0x20));
    MUT("import dir rva set, size 0", PE_E_BOUNDS, set_dir(m, D_IMPORT, 0x3000, 0));
    MUT("import dir rva 0, size set", PE_E_BOUNDS, set_dir(m, D_IMPORT, 0, 0x28));
    MUT("reloc dir outside image", PE_E_BOUNDS, set_dir(m, D_RELOC, 0x4fff, 0x28));
    MUT("reloc dir wraps u32", PE_E_BOUNDS, set_dir(m, D_RELOC, 0xffffff00u, 0x200));
    MUT("tls dir outside image", PE_E_BOUNDS, set_dir(m, D_TLS, 0x5000, 40));
    MUT("export dir outside image", PE_E_BOUNDS, set_dir(m, D_EXPORT, 0x9000, 0x40));
    MUT("delay dir outside image", PE_E_BOUNDS, set_dir(m, D_DELAY, 0x4fff, 0x40));
    MUT("delay dir recorded (not processed)", PE_OK, set_dir(m, D_DELAY, 0x3000, 0x40));
    MUT("export dir recorded", PE_OK, set_dir(m, D_EXPORT, 0x3000, 0x40));

    /* managed images */
    MUT("CLR header present", PE_E_UNSUPPORTED, set_dir(m, D_CLR, 0x1000, 0x48));
    MUT("CLR header garbage rva", PE_E_UNSUPPORTED, set_dir(m, D_CLR, 0xfffffff0u, 8));
    MUT("CLR rva only", PE_E_UNSUPPORTED, set_dir(m, D_CLR, 0x1000, 0));
    MUT("CLR size only", PE_E_UNSUPPORTED, set_dir(m, D_CLR, 0, 8));
    MUT("CLR beats other errors", PE_E_UNSUPPORTED, set_dir(m, D_CLR, 1, 1); p32(m, OPT + 56, 0));

    /* huge SizeOfImage right at the cap is accepted by parse; mapping it is the loader's allocation decision */
    big = PE_MAX_IMAGE;
    MUT("SizeOfImage == PE_MAX_IMAGE ok", PE_OK, p32(m, OPT + 56, big));
}

/* ------------------------------------------------------------------------------------------------ */
/* Group C: truncation sweep on the real fixtures                                                    */
/* ------------------------------------------------------------------------------------------------ */
static void truncation_sweep(const char *label, const u8 *f, size_t n)
{
    pe_info_t info, full;
    size_t raw_end = 0, L, i;
    u64 seed = 12345;
    int r = do_parse(f, n, &full);
    CHECK_ERR(label, r, PE_OK);
    if (r != PE_OK) return;
    for (i = 0; i < full.n_sections; i++)
        if (full.sec[i].raw_size && full.sec[i].raw_off + (size_t)full.sec[i].raw_size > raw_end)
            raw_end = full.sec[i].raw_off + full.sec[i].raw_size;
    for (L = 0; L < full.size_of_headers + 64 && L < raw_end; L++) {
        r = do_parse(f, L, &info);
        if (r != PE_E_TRUNC) { CHECK_ERR("fixture prefix", r, PE_E_TRUNC); printf("    (%s, prefix %zu)\n", label, L); return; }
        g_checks++;
    }
    for (i = 0; i < 200; i++) {                       /* sampled prefixes through the section data */
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        L = (size_t)((seed >> 33) % raw_end);
        r = do_parse(f, L, &info);
        CHECK_ERR("fixture sampled prefix", r, PE_E_TRUNC);
    }
    CHECK_ERR("fixture raw_end-1", do_parse(f, raw_end - 1, &info), PE_E_TRUNC);
    CHECK_ERR("fixture raw_end", do_parse(f, raw_end, &info), PE_OK);       /* overlay/symbols are optional */
    CHECK_ERR("fixture raw_end+1", do_parse(f, raw_end < n ? raw_end + 1 : n, &info), PE_OK);
}

/* ------------------------------------------------------------------------------------------------ */
/* Group C: hostile pe_map / pe_info                                                                 */
/* ------------------------------------------------------------------------------------------------ */
static int all_zero(const u8 *p, size_t n) { size_t i; for (i = 0; i < n; i++) if (p[i]) return 0; return 1; }

static void test_hostile_map(void)
{
    loaded_t L;
    pe_info_t bad;
    u8 *img, *file;
    int r;

    printf("[C] hostile pe_map / pe_info\n");
    build_good(g_m);
    r = load_image(g_m, FILE_SZ, &L);
    CHECK_ERR("map base", r, PE_OK);
    free(L.img);
    L.img = NULL;

    /* file shorter than what `info` promises: TRUNC and the image is untouched (all checks precede writes) */
    img = (u8 *)calloc(L.info.size_of_image, 1);
    file = (u8 *)malloc(0x900);
    memcpy(file, g_m, 0x900);
    CHECK_ERR("map short file", pe_map(file, 0x900, &L.info, img), PE_E_TRUNC);
    CHECK(all_zero(img, L.info.size_of_image));
    CHECK_ERR("map file < headers", pe_map(file, 0x100, &L.info, img), PE_E_TRUNC);
    CHECK(all_zero(img, L.info.size_of_image));
    CHECK_ERR("map len 0", pe_map(file, 0, &L.info, img), PE_E_TRUNC);
    free(file);

    /* tampered info structures */
    bad = L.info; bad.sec[0].vrva = bad.size_of_image - 4;
    CHECK_ERR("map vrva near end", pe_map(L.file, L.flen, &bad, img), PE_E_SECTIONS);
    CHECK(all_zero(img, L.info.size_of_image));
    bad = L.info; bad.sec[0].vrva = 0xfffffff0u;
    CHECK_ERR("map vrva wraps", pe_map(L.file, L.flen, &bad, img), PE_E_SECTIONS);
    bad = L.info; bad.sec[2].raw_off = 0xfffffff0u;
    CHECK_ERR("map raw_off wraps", pe_map(L.file, L.flen, &bad, img), PE_E_TRUNC);
    bad = L.info; bad.sec[1].raw_size = 0xffffffffu; bad.sec[1].vsize = 0xffffffffu;
    r = pe_map(L.file, L.flen, &bad, img);
    CHECK(r == PE_E_SECTIONS || r == PE_E_TRUNC);
    bad = L.info; bad.n_sections = 97;
    CHECK_ERR("map 97 sections", pe_map(L.file, L.flen, &bad, img), PE_E_SECTIONS);
    bad = L.info; bad.n_sections = 0;
    CHECK_ERR("map 0 sections", pe_map(L.file, L.flen, &bad, img), PE_E_SECTIONS);
    bad = L.info; bad.size_of_image = 0;
    CHECK_ERR("map SizeOfImage 0", pe_map(L.file, L.flen, &bad, img), PE_E_SIZE);
    bad = L.info; bad.size_of_image = PE_MAX_IMAGE + 0x1000;
    CHECK_ERR("map SizeOfImage > cap", pe_map(L.file, L.flen, &bad, img), PE_E_SIZE);
    bad = L.info; bad.size_of_headers = bad.size_of_image + 1;
    CHECK_ERR("map SizeOfHeaders > image", pe_map(L.file, L.flen, &bad, img), PE_E_SIZE);
    CHECK_ERR("map NULL info", pe_map(L.file, L.flen, NULL, img), PE_E_BOUNDS);
    CHECK_ERR("map NULL image", pe_map(L.file, L.flen, &L.info, NULL), PE_E_BOUNDS);
    CHECK_ERR("map NULL file", pe_map(NULL, L.flen, &L.info, img), PE_E_BOUNDS);
    CHECK(all_zero(img, L.info.size_of_image));
    CHECK_ERR("map ok after all that", pe_map(L.file, L.flen, &L.info, img), PE_OK);

    /* the other consumers re-validate `info` too */
    bad = L.info; bad.size_of_image = PE_MAX_IMAGE + 1;
    CHECK_ERR("reloc SizeOfImage > cap", pe_relocate(&bad, img, BASE + 0x10000), PE_E_SIZE);
    CHECK_ERR("imports SizeOfImage > cap", pe_resolve_imports(&bad, img, NULL, NULL, NULL, NULL), PE_E_SIZE);
    CHECK_ERR("tls SizeOfImage > cap", pe_tls_info(&bad, img, BASE, NULL, NULL, NULL, NULL), PE_E_SIZE);
    CHECK_ERR("reloc NULL info", pe_relocate(NULL, img, BASE), PE_E_BOUNDS);
    CHECK_ERR("reloc NULL image", pe_relocate(&L.info, NULL, BASE + 0x10000), PE_E_BOUNDS);
    CHECK_ERR("imports NULL info", pe_resolve_imports(NULL, img, NULL, NULL, NULL, NULL), PE_E_BOUNDS);
    CHECK_ERR("tls NULL info", pe_tls_info(NULL, img, BASE, NULL, NULL, NULL, NULL), PE_E_BOUNDS);
    bad = L.info; bad.reloc_rva = 0xfffff000u; bad.reloc_size = 0x28;
    CHECK_ERR("reloc dir tampered outside", pe_relocate(&bad, img, BASE + 0x10000), PE_E_RELOC);
    bad = L.info; bad.reloc_size = 0xffffffffu;
    CHECK_ERR("reloc dir tampered size", pe_relocate(&bad, img, BASE + 0x10000), PE_E_RELOC);
    bad = L.info; bad.import_rva = 0xfffff000u;
    CHECK_ERR("import dir tampered outside", pe_resolve_imports(&bad, img, NULL, NULL, NULL, NULL), PE_E_IMPORT);
    bad = L.info; bad.import_size = 0;
    CHECK_ERR("import rva set, size 0", pe_resolve_imports(&bad, img, NULL, NULL, NULL, NULL), PE_E_IMPORT);
    bad = L.info; bad.tls_rva = 0x4ff0;
    CHECK_ERR("tls dir tampered near end", pe_tls_info(&bad, img, BASE, NULL, NULL, NULL, NULL), PE_E_BOUNDS);
    bad = L.info; bad.tls_size = 8;
    CHECK_ERR("tls dir too small", pe_tls_info(&bad, img, BASE, NULL, NULL, NULL, NULL), PE_E_BOUNDS);
    free(img);
    free_loaded(&L);
}

/* ------------------------------------------------------------------------------------------------ */
/* Group C: hostile relocation tables                                                                */
/* ------------------------------------------------------------------------------------------------ */
static int reloc_with(const u8 *f, size_t n, u64 base)
{
    loaded_t L;
    int r = load_image(f, n, &L);
    if (r == PE_OK) r = pe_relocate(&L.info, L.img, base);
    free_loaded(&L);
    return r;
}
#define RMUT(label, want, ...) do { u8 *m = fresh(); __VA_ARGS__; CHECK_ERR(label, reloc_with(m, FILE_SZ, 0x400000), want); } while (0)
#define RA 0xa00   /* block A header in the file */
#define ZERO_ENTRIES(m) memset((m) + RA + 8, 0, 20)   /* turn block A's 10 entries into ABSOLUTE padding */

static void test_hostile_reloc(void)
{
    u8 *big;
    loaded_t L;
    u32 bsz, n;
    int r;

    printf("[C] hostile base-relocation tables\n");
    RMUT("reloc baseline", PE_OK, (void)0);
    RMUT("block size 0", PE_E_RELOC, p32(m, RA + 4, 0));
    RMUT("block size 4", PE_E_RELOC, p32(m, RA + 4, 4));
    RMUT("block size 7", PE_E_RELOC, p32(m, RA + 4, 7));
    RMUT("block size 9", PE_E_RELOC, p32(m, RA + 4, 9));
    RMUT("block size 10 (not multiple of 4)", PE_E_RELOC, p32(m, RA + 4, 10));
    RMUT("block size 30 (not multiple of 4)", PE_E_RELOC, p32(m, RA + 4, 30));
    RMUT("block size > dir (entry past block)", PE_E_RELOC, p32(m, RA + 4, 44));
    RMUT("block size 0x80000000", PE_E_RELOC, p32(m, RA + 4, 0x80000000u));
    RMUT("block size 0xFFFFFFFC", PE_E_RELOC, p32(m, RA + 4, 0xfffffffcu));
    RMUT("block B size 0", PE_E_RELOC, p32(m, RA + 32, 0));
    RMUT("block B overruns dir", PE_E_RELOC, p32(m, RA + 32, 16));
    RMUT("trailing bytes < header", PE_E_RELOC, set_dir(m, D_RELOC, 0x4000, 0x2c));
    RMUT("reloc dir of 4 bytes at the image end (header cut off)", PE_E_RELOC, set_dir(m, D_RELOC, 0x4ffc, 4));
    RMUT("reloc dir of 7 bytes at the image end", PE_E_RELOC, set_dir(m, D_RELOC, 0x4ff9, 7));
    RMUT("dir ends mid-block", PE_E_RELOC, set_dir(m, D_RELOC, 0x4000, 0x20));
    RMUT("trailing zero block (size 0)", PE_E_RELOC, set_dir(m, D_RELOC, 0x4000, 0x30));
    RMUT("type 4 HIGHADJ", PE_E_RELOC, p16(m, RA + 8, 0x4000));
    RMUT("type 5", PE_E_RELOC, p16(m, RA + 8, 0x5000));
    RMUT("type 7", PE_E_RELOC, p16(m, RA + 8, 0x7000));
    RMUT("type 9", PE_E_RELOC, p16(m, RA + 8, 0x9000));
    RMUT("type 11", PE_E_RELOC, p16(m, RA + 8, 0xB000));
    RMUT("type 15", PE_E_RELOC, p16(m, RA + 8, 0xF000));
    RMUT("page == SizeOfImage", PE_E_RELOC, p32(m, RA, 0x5000));
    RMUT("page near end, later entries outside", PE_E_RELOC, p32(m, RA, 0x4ff8));
    RMUT("page 0xFFFFF000", PE_E_RELOC, p32(m, RA, 0xfffff000u));
    RMUT("page+off wraps u32 (naive 32-bit math lands at 0)", PE_E_RELOC, p32(m, RA, 0xfffffff8u); p16(m, RA + 8, 0); p16(m, RA + 10, 0x3008));
    RMUT("HIGHLOW straddles image end", PE_E_RELOC, p32(m, RA, 0x4000); p16(m, RA + 8, 0x3000 | 0xffd));
    RMUT("DIR64 straddles image end", PE_E_RELOC, p32(m, RA, 0x4000); p16(m, RA + 8, 0xA000 | 0xff9));
    RMUT("DIR64 exactly fits image end", PE_OK, ZERO_ENTRIES(m); p32(m, RA, 0x4000); p16(m, RA + 8, 0xA000 | 0xff8));
    RMUT("target overlaps reloc table", PE_E_RELOC, ZERO_ENTRIES(m); p32(m, RA, 0x4000); p16(m, RA + 8, 0xA000 | 0x008));
    RMUT("target inside table's last byte", PE_E_RELOC, ZERO_ENTRIES(m); p32(m, RA, 0x4000); p16(m, RA + 8, 0x2000 | 0x027));
    RMUT("target just past table ok", PE_OK, ZERO_ENTRIES(m); p32(m, RA, 0x4000); p16(m, RA + 8, 0x2000 | 0x028));
    RMUT("ABSOLUTE with wild offset ignored", PE_OK, p16(m, RA + 8 + 18, 0x0fff));
    RMUT("empty block (header only)", PE_OK, p32(m, RA + 4, 8); set_dir(m, D_RELOC, 0x4000, 8));
    {   /* atomicity: a late malformed block must leave the image completely unpatched */
        u8 *m = fresh();
        u8 *snap;
        p32(m, RA + 32, 0);                           /* block B (second) is broken, block A is fine */
        r = load_image(m, FILE_SZ, &L);
        snap = (u8 *)malloc(L.info.size_of_image);
        memcpy(snap, L.img, L.info.size_of_image);
        CHECK_ERR("atomic reloc", pe_relocate(&L.info, L.img, 0x400000), PE_E_RELOC);
        CHECK(memcmp(snap, L.img, L.info.size_of_image) == 0);
        free(snap);
        free_loaded(&L);
    }

    /* delta 0 still validates the table (consistent behaviour regardless of load address) */
    {
        u8 *m = fresh();
        p32(m, RA + 4, 0);
        CHECK_ERR("reloc delta 0, malformed table", reloc_with(m, FILE_SZ, BASE), PE_E_RELOC);
    }

    /* entry-count cap: 1<<20 entries pass, 1<<20 + 2 fail.  Entries are zero ABSOLUTE padding living in the
     * zero-filled tail of a large .reloc section. */
    for (n = 0; n < 2; n++) {
        u32 ents = (1u << 20) + 2u * n;
        bsz = 8u + 2u * ents;
        big = (u8 *)malloc(FILE_SZ);
        build_ex(big, 0x200, bsz);
        memset(big + RA + 8, 0, 0x1f8);               /* all-ABSOLUTE entries */
        set_dir(big, D_RELOC, 0x4000, bsz);
        p32(big, RA, 0x2000);
        p32(big, RA + 4, bsz);
        r = reloc_with(big, FILE_SZ, 0x400000);
        CHECK_ERR(n == 0 ? "1<<20 entries" : "(1<<20)+2 entries", r, n == 0 ? PE_OK : PE_E_RELOC);
        free(big);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Group C: hostile import tables                                                                    */
/* ------------------------------------------------------------------------------------------------ */
static int imports_after(const u8 *f, size_t n, void (*tamper)(u8 *img, void *), void *ctx, u32 *total)
{
    loaded_t L;
    static rlog_t lg;
    int r = load_image(f, n, &L);
    unsigned int t = 0;
    if (r == PE_OK) {
        memset(&lg, 0, sizeof lg);
        lg.mode = 3;
        if (tamper) tamper(L.img, ctx);
        r = pe_resolve_imports(&L.info, L.img, fake_resolver, &lg, &t, NULL);
    }
    if (total) *total = t;
    free_loaded(&L);
    return r;
}
#define IMUT(label, want, ...) do { u8 *m = fresh(); __VA_ARGS__; CHECK_ERR(label, imports_after(m, FILE_SZ, NULL, NULL, NULL), want); } while (0)
#define IA 0x800   /* .idata raw offset in the file */

static void t_unterminated_dll(u8 *img, void *c) { (void)c; memset(img + 0x30c0, 'A', 0x5000 - 0x30c0); }
static void t_long_dll_256(u8 *img, void *c) { (void)c; memset(img + 0x30c0, 'A', 256); img[0x30c0 + 256] = 0; }
static void t_long_dll_255(u8 *img, void *c) { (void)c; memset(img + 0x30c0, 'A', 255); img[0x30c0 + 255] = 0; }
static void t_unterminated_fn(u8 *img, void *c) { (void)c; memset(img + 0x3100, 'B', 0x5000 - 0x3100); }
/* short names that run into the end of the image (the length cap alone would not stop the read) */
static void t_dll_tail(u8 *img, void *c) { (void)c; p32(img, 0x3000 + 12, 0x4f80); memset(img + 0x4f80, 'A', 0x80); }
static void t_fn_tail(u8 *img, void *c) { (void)c; p64(img, 0x3040, 0x4f80); p64(img, 0x3060, 0x4f80); memset(img + 0x4f80, 'A', 0x80); }
static void t_long_fn(u8 *img, void *c) { u32 len = *(u32 *)c; memset(img + 0x3102, 'B', len); img[0x3102 + len] = 0; }
/* valid hint/name + dll name parked in the (zero) header slack so tampering .idata cannot clobber them */
static void plant_names(u8 *img) { plant_hn(img, 0x300, 7, "Qux"); memcpy(img + 0x320, "D.dll", 6); }

static void t_no_terminator(u8 *img, void *c)
{
    u32 o;
    (void)c;
    plant_names(img);
    p32(img, 0x3000 + 0, 0x3040);                     /* desc0: OFT 0x3040, Name -> D.dll, FT 0x4800 */
    p32(img, 0x3000 + 12, 0x320);
    p32(img, 0x3000 + 16, 0x4800);
    memset(img + 0x3014, 0, 20);                      /* nothing after desc0 */
    for (o = 0x3040; o < 0x4800; o += 8) p64(img, o, 0x300);   /* lookup thunks forever, no null */
}
static void t_bad_thunk(u8 *img, void *c) { u64 v = *(u64 *)c; p64(img, 0x3040, v); }

static void test_hostile_imports(void)
{
    u8 *m;
    u32 total, len;
    u64 v;
    int r;
    static rlog_t lg;
    loaded_t L;

    printf("[C] hostile import tables\n");
    CHECK_ERR("imports baseline", imports_after(fresh(), FILE_SZ, NULL, NULL, &total), PE_OK);
    CHECK_U64(total, 4);
    IMUT("desc Name RVA == SizeOfImage", PE_E_IMPORT, p32(m, IA + 0x0c, 0x5000));
    IMUT("desc Name RVA 0xFFFFFFFF", PE_E_IMPORT, p32(m, IA + 0x0c, 0xffffffffu));
    IMUT("desc Name RVA 0 (others set)", PE_E_IMPORT, p32(m, IA + 0x0c, 0));
    IMUT("desc FirstThunk 0", PE_E_IMPORT, p32(m, IA + 0x10, 0));
    IMUT("desc FirstThunk == SizeOfImage", PE_E_IMPORT, p32(m, IA + 0x10, 0x5000));
    IMUT("desc FirstThunk slot straddles end", PE_E_IMPORT, p32(m, IA + 0x10, 0x4ffc));
    IMUT("desc OriginalFirstThunk straddles end", PE_E_IMPORT, p32(m, IA + 0x00, 0x4ffc));
    IMUT("desc OriginalFirstThunk 0xFFFFFFFF", PE_E_IMPORT, p32(m, IA + 0x00, 0xffffffffu));
    IMUT("empty dll name", PE_E_IMPORT, m[IA + 0xc0] = 0);
    IMUT("thunk by-name RVA outside image", PE_E_IMPORT, p64(m, IA + 0x40, 0x7fffffff); p64(m, IA + 0x60, 0x7fffffff));
    IMUT("thunk hint straddles end", PE_E_IMPORT, p64(m, IA + 0x40, 0x4fff); p64(m, IA + 0x60, 0x4fff));
    IMUT("thunk name starts at end", PE_E_IMPORT, p64(m, IA + 0x40, 0x4ffe); p64(m, IA + 0x60, 0x4ffe));
    IMUT("thunk by-name reserved bit 40", PE_E_IMPORT, p64(m, IA + 0x40, 0x100ull << 32 | 0x3100); p64(m, IA + 0x60, 0x100ull << 32 | 0x3100));
    IMUT("thunk by-name bit 31", PE_E_IMPORT, p64(m, IA + 0x40, 0x80003100ull); p64(m, IA + 0x60, 0x80003100ull));
    IMUT("empty function name", PE_E_IMPORT, m[IA + 0x102] = 0);

    /* ordinal imports: valid ones reach the resolver; reserved bits => PE_E_ORDINAL */
    IMUT("ordinal 0xFFFF ok", PE_OK, p64(m, IA + 0x48, 0x800000000000ffffull); p64(m, IA + 0x68, 0x800000000000ffffull));
    IMUT("ordinal reserved bit 16", PE_E_ORDINAL, p64(m, IA + 0x48, 0x8000000000010005ull); p64(m, IA + 0x68, 0x8000000000010005ull));
    IMUT("ordinal reserved bit 31", PE_E_ORDINAL, p64(m, IA + 0x48, 0x8000000080000005ull); p64(m, IA + 0x68, 0x8000000080000005ull));
    IMUT("ordinal reserved bit 40", PE_E_ORDINAL, p64(m, IA + 0x48, 0x8000010000000005ull); p64(m, IA + 0x68, 0x8000010000000005ull));
    IMUT("ordinal reserved bit 62", PE_E_ORDINAL, p64(m, IA + 0x48, 0xC000000000000005ull); p64(m, IA + 0x68, 0xC000000000000005ull));
    v = 0x8000000000000000ull | (1ull << 20);
    CHECK_ERR("ordinal via tamper (OFT)", imports_after(fresh(), FILE_SZ, t_bad_thunk, &v, NULL), PE_E_ORDINAL);

    /* unterminated / oversized strings */
    CHECK_ERR("dll name unterminated to image end", imports_after(fresh(), FILE_SZ, t_unterminated_dll, NULL, NULL), PE_E_IMPORT);
    CHECK_ERR("dll name 256 chars", imports_after(fresh(), FILE_SZ, t_long_dll_256, NULL, NULL), PE_E_IMPORT);
    CHECK_ERR("dll name 255 chars ok", imports_after(fresh(), FILE_SZ, t_long_dll_255, NULL, NULL), PE_OK);
    CHECK_ERR("fn name unterminated to image end", imports_after(fresh(), FILE_SZ, t_unterminated_fn, NULL, NULL), PE_E_IMPORT);
    CHECK_ERR("dll name unterminated within 128 bytes of image end", imports_after(fresh(), FILE_SZ, t_dll_tail, NULL, NULL), PE_E_IMPORT);
    CHECK_ERR("fn name unterminated within 128 bytes of image end", imports_after(fresh(), FILE_SZ, t_fn_tail, NULL, NULL), PE_E_IMPORT);
    len = PE_MAX_FUNC_NAME;
    CHECK_ERR("fn name 1024 chars", imports_after(fresh(), FILE_SZ, t_long_fn, &len, NULL), PE_E_IMPORT);
    len = PE_MAX_FUNC_NAME - 1;
    CHECK_ERR("fn name 1023 chars ok", imports_after(fresh(), FILE_SZ, t_long_fn, &len, NULL), PE_OK);

    /* chains with no terminator */
    CHECK_ERR("thunk chain without terminator", imports_after(fresh(), FILE_SZ, t_no_terminator, NULL, &total), PE_E_IMPORT);
    CHECK(total > 0);                                 /* progress is reported even on failure */
    {   /* descriptor array with no null terminator: 409 valid descriptors until the image ends mid-descriptor */
        u8 *img;
        u32 o;
        r = load_image(fresh(), FILE_SZ, &L);
        CHECK_ERR("load for desc-loop", r, PE_OK);
        img = L.img;
        plant_names(img);
        p64(img, 0x340, 0x300);                       /* OFT: one thunk, then the zero terminator */
        p64(img, 0x360, 0);                           /* FT slot */
        for (o = 0x3000; o + 20 <= 0x5000; o += 20) { p32(img, o, 0x340); p32(img, o + 12, 0x320); p32(img, o + 16, 0x360); }
        memset(&lg, 0, sizeof lg); lg.mode = 3;
        r = pe_resolve_imports(&L.info, img, fake_resolver, &lg, &total, NULL);
        CHECK_ERR("endless descriptors", r, PE_E_IMPORT);
        CHECK_U64(total, 409);                        /* every complete descriptor was walked, none beyond the image */
        free_loaded(&L);
    }
    {   /* import directory parked at the very end of the image; last descriptor is cut off by the image end */
        u8 *mm = fresh();
        u8 *img;
        u32 o;
        set_dir(mm, D_IMPORT, 0x4f00, 0x100);
        r = load_image(mm, FILE_SZ, &L);
        CHECK_ERR("load for tail dir", r, PE_OK);
        img = L.img;
        plant_names(img);
        p64(img, 0x340, 0x300);
        for (o = 0x4f00; o + 20 <= 0x5000; o += 20) { p32(img, o, 0x340); p32(img, o + 12, 0x320); p32(img, o + 16, 0x360); }
        memset(&lg, 0, sizeof lg); lg.mode = 3;
        r = pe_resolve_imports(&L.info, img, fake_resolver, &lg, &total, NULL);
        CHECK_ERR("descriptor array runs off image", r, PE_E_IMPORT);
        CHECK_U64(total, 12);
        free_loaded(&L);
    }

    /* caps on a large image: .idata is 3 MiB so it can hold the arrays */
    {
        u32 i, idata_v = 0x300000, ndesc, nthunks;
        m = (u8 *)malloc(FILE_SZ);

        for (nthunks = PE_MAX_THUNKS; nthunks <= PE_MAX_THUNKS + 1; nthunks++) {
            build_ex(m, idata_v, 0x28);
            r = load_image(m, FILE_SZ, &L);
            CHECK_ERR("big idata load", r, PE_OK);
            plant_hn(L.img, 0x300, 7, "Qux");
            memcpy(L.img + 0x320, "D.dll", 6);
            memset(L.img + 0x3000, 0, 60);
            p32(L.img, 0x3000 + 0, 0x4000); p32(L.img, 0x3000 + 12, 0x320); p32(L.img, 0x3000 + 16, 0x103000);
            for (i = 0; i < nthunks; i++) p64(L.img, 0x4000 + 8u * i, 0x300);
            memset(&lg, 0, sizeof lg); lg.mode = 3;
            r = pe_resolve_imports(&L.info, L.img, fake_resolver, &lg, &total, NULL);
            if (nthunks == PE_MAX_THUNKS) { CHECK_ERR("65536 thunks ok", r, PE_OK); CHECK_U64(total, PE_MAX_THUNKS); }
            else CHECK_ERR("65537 thunks", r, PE_E_IMPORT);
            free_loaded(&L);
        }

        for (ndesc = PE_MAX_IMPORT_DESCRIPTORS; ndesc <= PE_MAX_IMPORT_DESCRIPTORS + 1; ndesc++) {
            build_ex(m, idata_v, 0x28);
            r = load_image(m, FILE_SZ, &L);
            CHECK_ERR("big idata load 2", r, PE_OK);
            plant_hn(L.img, 0x300, 7, "Qux");
            memcpy(L.img + 0x320, "D.dll", 6);
            memset(L.img + 0x3000, 0, 20 * (PE_MAX_IMPORT_DESCRIPTORS + 8));
            for (i = 0; i < ndesc; i++) {
                p32(L.img, 0x3000 + 20u * i + 0, 0x200000); p32(L.img, 0x3000 + 20u * i + 12, 0x320);
                p32(L.img, 0x3000 + 20u * i + 16, 0x201000);
            }
            p64(L.img, 0x200000, 0x300);
            memset(&lg, 0, sizeof lg); lg.mode = 3;
            r = pe_resolve_imports(&L.info, L.img, fake_resolver, &lg, &total, NULL);
            if (ndesc == PE_MAX_IMPORT_DESCRIPTORS) { CHECK_ERR("4096 descriptors ok", r, PE_OK); CHECK_U64(total, PE_MAX_IMPORT_DESCRIPTORS); }
            else CHECK_ERR("4097 descriptors", r, PE_E_IMPORT);
            free_loaded(&L);
        }
        free(m);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Group C: hostile TLS directories                                                                  */
/* ------------------------------------------------------------------------------------------------ */
static void test_hostile_tls(void)
{
    loaded_t L;
    u64 b = 0x400000ull, s, e, idx, cb;
    u8 *snap;
    u32 o;
    int r;
    const u32 D = 0x2040;

    printf("[C] hostile TLS directories\n");
    build_good(g_m);
    r = load_image(g_m, FILE_SZ, &L);
    CHECK_ERR("tls load", r, PE_OK);
    CHECK_ERR("tls reloc", pe_relocate(&L.info, L.img, b), PE_OK);
    snap = (u8 *)malloc(L.info.size_of_image);
    memcpy(snap, L.img, L.info.size_of_image);

#define TLSCASE(label, want, ...) do { memcpy(L.img, snap, L.info.size_of_image); __VA_ARGS__; \
        CHECK_ERR(label, pe_tls_info(&L.info, L.img, b, &s, &e, &idx, &cb), want); } while (0)
    TLSCASE("tls baseline", PE_OK, (void)0);
    TLSCASE("tls start > end", PE_E_BOUNDS, p64(L.img, D + 0, b + 0x2111));
    TLSCASE("tls start below image", PE_E_BOUNDS, p64(L.img, D + 0, b - 8));
    TLSCASE("tls end past image", PE_E_BOUNDS, p64(L.img, D + 8, b + 0x5001));
    TLSCASE("tls end == image end ok", PE_OK, p64(L.img, D + 8, b + 0x5000));
    TLSCASE("tls start 0xFFFF...", PE_E_BOUNDS, p64(L.img, D + 0, 0xffffffffffffffffull); p64(L.img, D + 8, 0xffffffffffffffffull));
    TLSCASE("tls empty 0/0 ok", PE_OK, p64(L.img, D + 0, 0); p64(L.img, D + 8, 0));
    TLSCASE("tls index outside", PE_E_BOUNDS, p64(L.img, D + 16, b + 0x5000));
    TLSCASE("tls index straddles end", PE_E_BOUNDS, p64(L.img, D + 16, b + 0x4ffd));
    TLSCASE("tls index 0", PE_E_BOUNDS, p64(L.img, D + 16, 0));
    TLSCASE("tls callbacks outside", PE_E_BOUNDS, p64(L.img, D + 24, 0x7fff0000ull));
    TLSCASE("tls callbacks near end", PE_E_BOUNDS, p64(L.img, D + 24, b + 0x4ffc));
    TLSCASE("tls callbacks 0 (none) ok", PE_OK, p64(L.img, D + 24, 0));
    TLSCASE("tls callback target outside", PE_E_BOUNDS, p64(L.img, 0x2130, 0x1000));
    TLSCASE("tls callback target == end", PE_E_BOUNDS, p64(L.img, 0x2130, b + 0x5000));
    TLSCASE("tls callbacks unterminated", PE_E_BOUNDS, for (o = 0x2130; o + 8 <= 0x5000; o += 8) p64(L.img, o, b + 0x1000));
    TLSCASE("tls 64 callbacks then null ok", PE_OK, for (o = 0; o < 64; o++) p64(L.img, 0x2130 + 8 * o, b + 0x1000));
    TLSCASE("tls 65 callbacks (cap exceeded)", PE_E_BOUNDS, for (o = 0; o < 65; o++) p64(L.img, 0x2130 + 8 * o, b + 0x1000));
    CHECK_ERR("tls wrapping base", pe_tls_info(&L.info, L.img, ~0ull - 0x100, &s, &e, &idx, &cb), PE_E_BOUNDS);
    free(snap);
    free_loaded(&L);
}

/* ------------------------------------------------------------------------------------------------ */
/* Group A: real fixtures                                                                            */
/* ------------------------------------------------------------------------------------------------ */
typedef struct { u32 slot_rva; char dll[64]; char name[96]; u32 by_ord; u32 ord; } imp_t;

/* independent import walker (test-side re-implementation, trusting the fixture) */
static int walk_imports(const pe_info_t *in, const u8 *img, imp_t *out, int max)
{
    int n = 0;
    u32 d = in->import_rva;
    if (!d) return 0;
    for (;; d += 20) {
        u32 oft = g32(img, d), nm = g32(img, d + 12), ft = g32(img, d + 16), i;
        if (!oft && !nm && !ft) break;
        for (i = 0;; i++) {
            u64 e = g64(img, (oft ? oft : ft) + 8u * i);
            if (!e) break;
            if (n >= max) return -1;
            memset(&out[n], 0, sizeof out[n]);
            snprintf(out[n].dll, sizeof out[n].dll, "%s", (const char *)img + nm);
            out[n].slot_rva = ft + 8u * i;
            if (e >> 63) { out[n].by_ord = 1; out[n].ord = (u32)(e & 0xffff); }
            else snprintf(out[n].name, sizeof out[n].name, "%s", (const char *)img + (u32)e + 2);
            n++;
        }
    }
    return n;
}

typedef struct { u32 rva; u32 type; } rel_t;
static int walk_relocs(const pe_info_t *in, const u8 *img, rel_t *out, int max)
{
    u32 pos = in->reloc_rva, end = in->reloc_rva + in->reloc_size;
    int n = 0;
    while (pos + 8 <= end) {
        u32 page = g32(img, pos), bsz = g32(img, pos + 4), k;
        for (k = 0; k < (bsz - 8) / 2; k++) {
            u32 e = g16(img, pos + 8 + 2 * k);
            if ((e >> 12) == 0) continue;
            if (n >= max) return -1;
            out[n].rva = page + (e & 0xfff);
            out[n].type = e >> 12;
            n++;
        }
        pos += bsz;
    }
    return n;
}

static void check_fixture_basic(const char *label, const u8 *f, size_t n, pe_info_t *info)
{
    int r = do_parse(f, n, info);
    CHECK_ERR(label, r, PE_OK);
    if (r != PE_OK) return;
    CHECK_U64(g16(f, g32(f, 0x3c) + 4), 0x8664);                      /* machine == AMD64 */
    CHECK_U64(g16(f, g32(f, 0x3c) + 24), 0x20b);                      /* PE32+ */
    CHECK_U64(info->subsystem, 3);
    CHECK_U64(info->image_base, 0x140000000ull);
    CHECK_U64(info->section_alignment, 0x1000);
    CHECK_U64(info->file_alignment, 0x200);
    CHECK_U64(info->is_dll, 0);
    CHECK(info->n_sections >= 3 && info->n_sections <= 20);
    CHECK(info->size_of_image % 0x1000 == 0 && info->size_of_image >= 0x3000);
    CHECK(info->entry_rva >= 0x1000 && info->entry_rva < info->size_of_image);
    CHECK(strcmp(info->sec[0].name, ".text") == 0);
    CHECK(info->import_rva != 0 && info->import_size != 0);
    CHECK(info->stack_reserve >= 0x100000 && info->stack_commit >= 0x1000);
    CHECK_U64(info->delay_import_rva, 0);
}

static void test_fixtures(const char *nocrt_path, const char *crt_path)
{
    size_t n1 = 0, n2 = 0, k;
    u8 *f1 = load_file(nocrt_path, &n1), *f2 = load_file(crt_path, &n2);
    static rlog_t L;
    static imp_t imps[1024];
    static rel_t rels[8192];
    loaded_t Ld;
    pe_info_t info;
    int r, ni, nr, i;
    u32 total, unres, found;

    printf("[A] real fixtures: %s, %s\n", nocrt_path, crt_path);
    if (!f1 || !f2) { printf("  FAIL: cannot read fixtures\n"); g_fail++; free(f1); free(f2); return; }

    /* ---- nocrt_hello ---- */
    check_fixture_basic("nocrt parse", f1, n1, &info);
    r = load_image(f1, n1, &Ld);
    CHECK_ERR("nocrt map", r, PE_OK);
    ni = walk_imports(&Ld.info, Ld.img, imps, 1024);
    CHECK_U64(ni, 3);
    memset(&L, 0, sizeof L);
    r = pe_resolve_imports(&Ld.info, Ld.img, fake_resolver, &L, &total, &unres);
    CHECK_ERR("nocrt imports", r, PE_OK);
    CHECK_U64(total, 3); CHECK_U64(unres, 0); CHECK_U64(L.calls, 3);
    found = 0;
    for (i = 0; i < L.calls && i < RLOG_MAX; i++) {
        if (strcasecmp(L.dll[i], "KERNEL32.dll") == 0 && !L.byord[i]) {
            if (!strcmp(L.name[i], "GetStdHandle")) found |= 1;
            if (!strcmp(L.name[i], "WriteFile")) found |= 2;
            if (!strcmp(L.name[i], "ExitProcess")) found |= 4;
        }
    }
    CHECK_U64(found, 7);                                              /* KERNEL32: GetStdHandle/WriteFile/ExitProcess */
    for (i = 0; i < ni; i++) CHECK_U64(g64(Ld.img, imps[i].slot_rva), FAKE_ADDR(i));   /* IAT filled, in order */
    /* this fixture has no .reloc (the static pointer was constant-folded): rebasing must be refused, not guessed */
    if (Ld.info.reloc_rva == 0) {
        CHECK_ERR("nocrt relocate same base", pe_relocate(&Ld.info, Ld.img, 0x140000000ull), PE_OK);
        CHECK_ERR("nocrt relocate other base", pe_relocate(&Ld.info, Ld.img, 0x7FF700000000ull), PE_E_RELOC);
    } else {
        CHECK_ERR("nocrt relocate other base", pe_relocate(&Ld.info, Ld.img, 0x7FF700000000ull), PE_OK);
    }
    CHECK_ERR("nocrt tls", pe_tls_info(&Ld.info, Ld.img, 0x140000000ull, NULL, NULL, NULL, NULL), 1);
    free_loaded(&Ld);
    truncation_sweep("nocrt", f1, n1);

    /* ---- crt_hello ---- */
    check_fixture_basic("crt parse", f2, n2, &info);
    CHECK(info.reloc_rva != 0 && info.tls_rva != 0);
    r = load_image(f2, n2, &Ld);
    CHECK_ERR("crt map", r, PE_OK);
    ni = walk_imports(&Ld.info, Ld.img, imps, 1024);
    CHECK(ni >= 20);
    memset(&L, 0, sizeof L);
    r = pe_resolve_imports(&Ld.info, Ld.img, fake_resolver, &L, &total, &unres);
    CHECK_ERR("crt imports", r, PE_OK);
    CHECK_U64(total, ni); CHECK_U64(unres, 0); CHECK_U64(L.calls, ni);
    {
        int have_k32 = 0, have_other = 0;
        for (i = 0; i < ni && i < RLOG_MAX; i++) {
            if (strcasecmp(L.dll[i], "KERNEL32.dll") == 0) have_k32 = 1; else have_other = 1;
            CHECK(strcmp(L.dll[i], imps[i].dll) == 0 && strcmp(L.name[i], imps[i].name) == 0);
            CHECK_U64(g64(Ld.img, imps[i].slot_rva), FAKE_ADDR(i));
        }
        CHECK(have_k32 && have_other);                                /* msvcrt/ucrt + kernel32 */
    }
    free_loaded(&Ld);

    /* relocate at a DIFFERENT base: every DIR64 moves by exactly delta, every other byte is untouched */
    {
        u64 nb = 0x00007FF712340000ull, delta = nb - 0x140000000ull;
        u8 *before, *mask;
        int dir64 = 0, writable_dir64 = 0;
        r = load_image(f2, n2, &Ld);
        before = (u8 *)malloc(Ld.info.size_of_image);
        memcpy(before, Ld.img, Ld.info.size_of_image);
        nr = walk_relocs(&Ld.info, Ld.img, rels, 8192);
        CHECK(nr > 0);
        r = pe_relocate(&Ld.info, Ld.img, nb);
        CHECK_ERR("crt relocate", r, PE_OK);
        mask = (u8 *)calloc(Ld.info.size_of_image, 1);
        for (i = 0; i < nr; i++) {
            if (rels[i].type == 10) {
                u32 s, writable = 0;
                CHECK_U64(g64(Ld.img, rels[i].rva), g64(before, rels[i].rva) + delta);
                memset(mask + rels[i].rva, 1, 8);
                dir64++;
                for (s = 0; s < Ld.info.n_sections; s++)
                    if (rels[i].rva >= Ld.info.sec[s].vrva && rels[i].rva < Ld.info.sec[s].vrva + Ld.info.sec[s].vsize &&
                        (Ld.info.sec[s].flags & 0x80000000u)) writable = 1;
                writable_dir64 += writable;
            } else CHECK(!"unexpected reloc type in an AMD64 MinGW image");
        }
        CHECK(dir64 > 0);
        CHECK(writable_dir64 > 0);                                    /* pointers stored in writable data were fixed up */
        for (k = 0; k < Ld.info.size_of_image; k++)
            if (!mask[k] && Ld.img[k] != before[k]) { CHECK(!"crt: byte outside reloc targets changed"); break; }
        printf("    crt_hello: %d DIR64 relocs (%d in writable sections) moved by exactly 0x%llx\n", dir64, writable_dir64, delta);

        /* tls (after relocation) + the unrelocated-at-other-base negative */
        {
            u64 s = 0, e = 0, idx = 0, cb = 0;
            r = pe_tls_info(&Ld.info, Ld.img, nb, &s, &e, &idx, &cb);
            CHECK_ERR("crt tls", r, PE_OK);
            CHECK(s >= nb && e >= s && e <= nb + Ld.info.size_of_image);
            CHECK(idx >= nb && idx + 4 <= nb + Ld.info.size_of_image);
            printf("    crt_hello TLS: start=%llx end=%llx index=%llx callbacks=%llx\n", s, e, idx, cb);
            CHECK_ERR("crt tls unrelocated", pe_tls_info(&Ld.info, before, nb, &s, &e, &idx, &cb), PE_E_BOUNDS);
            CHECK_ERR("crt tls unrelocated @preferred", pe_tls_info(&Ld.info, before, 0x140000000ull, &s, &e, &idx, &cb), PE_OK);
        }
        free(before);
        free(mask);
        free_loaded(&Ld);
    }
    truncation_sweep("crt", f2, n2);
    free(f1);
    free(f2);
}

/* reloc_hello.exe: built by run_pe_tests.sh with a NON-static, non-const g_msg_ptr (so it lives in .data and
 * carries a DIR64 relocation); the script passes the VA of g_msg_ptr from `nm`. */
static void test_reloc_hello(const char *path, u64 gmsg_va)
{
    size_t n = 0;
    u8 *f = load_file(path, &n);
    loaded_t L;
    int r;
    u64 nb = 0x00007FF7ABCD0000ull, before, after, delta;

    printf("[A] reloc_hello fixture: %s (g_msg_ptr va=0x%llx)\n", path, gmsg_va);
    if (!f) { printf("  (not present: skipped)\n"); return; }
    r = load_image(f, n, &L);
    CHECK_ERR("reloc_hello load", r, PE_OK);
    if (r == PE_OK) {
        u32 rva = (u32)(gmsg_va - L.info.image_base), s;
        int in_writable = 0;
        delta = nb - L.info.image_base;
        CHECK(L.info.reloc_rva != 0);
        CHECK(gmsg_va >= L.info.image_base && rva + 8 <= L.info.size_of_image);
        for (s = 0; s < L.info.n_sections; s++)
            if (rva >= L.info.sec[s].vrva && rva < L.info.sec[s].vrva + L.info.sec[s].vsize && (L.info.sec[s].flags & 0x80000000u))
                in_writable = 1;
        CHECK(in_writable);                                           /* g_msg_ptr really is in a writable (.data) section */
        before = g64(L.img, rva);
        CHECK(before >= L.info.image_base && before < L.info.image_base + L.info.size_of_image);   /* points at g_msg */
        r = pe_relocate(&L.info, L.img, nb);
        CHECK_ERR("reloc_hello relocate", r, PE_OK);
        after = g64(L.img, rva);
        CHECK_U64(after, before + delta);                             /* adjusted by EXACTLY the delta */
        CHECK(after >= nb && after + 19 <= nb + L.info.size_of_image);
        if (after >= nb && after + 19 <= nb + L.info.size_of_image)
            CHECK(strncmp((const char *)L.img + (after - nb), "WINFIX: reloc hello", 19) == 0);   /* ... still points at the string */
    }
    free_loaded(&L);
    free(f);
}

/* ------------------------------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *nocrt = argc > 1 ? argv[1] : "/tmp/winfix/nocrt_hello.exe";
    const char *crt = argc > 2 ? argv[2] : "/tmp/winfix/crt_hello.exe";
    const char *rh = argc > 3 ? argv[3] : "";
    u64 gmsg = argc > 4 ? strtoull(argv[4], NULL, 16) : 0;
    u8 synth[FILE_SZ];

    test_fixtures(nocrt, crt);
    if (rh[0]) test_reloc_hello(rh, gmsg);
    test_synth_good(synth);
    test_synth_imports();
    test_hostile_parse();
    test_hostile_map();
    test_hostile_reloc();
    test_hostile_imports();
    test_hostile_tls();

    if (argc > 5 && argv[5][0] && strcmp(argv[5], "-") != 0) {        /* seed corpus for pe_fuzz */
        FILE *o = fopen(argv[5], "wb");
        if (o) { fwrite(synth, 1, FILE_SZ, o); fclose(o); }
    }

    if (g_fail) { printf("PE-HOST-TEST: FAIL (%d of %d checks failed)\n", g_fail, g_checks); return 1; }
    printf("PE-HOST-TEST: PASS (%d checks)\n", g_checks);
    return 0;
}
