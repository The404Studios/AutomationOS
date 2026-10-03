/*
 * pe_dll_test.c -- host-side tests for userspace/lib/pe/pe_exports.c and pe_modules.c (export lookup, forwarders,
 * delay imports, DLL module graph).  Run under ASan + UBSan by tests/win/run_dll_tests.sh:
 *
 *   usage: pe_dll_test <fixture-dir>        (fixture-dir = output of tests/win/dll_fix/build_dll_fixtures.sh)
 *
 * Groups
 *   E  export directory: synthetic DLLs from pe_dll_builder.h (sorted / unsorted / duplicate names, ordinal base,
 *      unused slots, forwarders, caps) and a table of hostile mutations, every buffer EXACTLY sized so ASan sees
 *      any out-of-bounds read; real MinGW fixtures cross-checked against the synthetic expectations
 *   D  delay-load import binding (pe_delay_imports)
 *   M  module graph on REAL MinGW DLLs executed through ms_abi on the host: import order, init order, detach order,
 *      forwarders, ordinal-only export + ordinal import, DIR64 relocation at the actual base, DllMain==FALSE rollback,
 *      import cycle, missing DLL, builtin precedence, LoadLibrary/FreeLibrary refcounts
 *   S  synthetic module graphs: depth cap, module cap, hostile DLL names (rejected, never truncated), forwarder
 *      chains / cycles / ordinal forwarders / builtin forwarders, delay imports through the loader, API misuse
 *
 * The DLL code is freestanding MinGW output, so the host can CALL it as ms_abi function pointers.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>

#include "pe.h"
#include "pe_exports.h"
#include "pe_modules.h"
#include "pe_dll_builder.h"

#define MS __attribute__((ms_abi))
typedef MS unsigned (*ms_u0_t)(void);
typedef MS int (*ms_i0_t)(void);
typedef MS int (*ms_i1_t)(int);
typedef MS int (*ms_i2_t)(int, int);

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_U64(got, want) do { u64 g_ = (u64)(got), w_ = (u64)(want); g_checks++; \
    if (g_ != w_) { g_fail++; printf("  FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#define CHECK_RC(got, want) do { int g_ = (got), w_ = (want); g_checks++; \
    if (g_ != w_) { g_fail++; printf("  FAIL %s:%d: %s == %d (%s), expected %d (%s)\n", __FILE__, __LINE__, #got, g_, \
        pe_mod_strerror(g_), w_, pe_mod_strerror(w_)); } } while (0)
#define CHECK_STR(got, want) do { const char *g_ = (got), *w_ = (want); g_checks++; \
    if (strcmp(g_, w_) != 0) { g_fail++; printf("  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)

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

/* ================================================================================================================ */
/* synthetic image helper: build -> (optionally poke the file) -> parse + map + relocate                             */
/* ================================================================================================================ */
typedef struct { bout_t b; u8 *img; pe_info_t info; } syn_t;

static void syn_make(syn_t *S, const bspec_t *spec)
{
    memset(S, 0, sizeof *S);
    if (bl_dll(spec, &S->b) != 0) { printf("FATAL: synthetic builder failed\n"); exit(2); }
}
static int syn_load(syn_t *S)
{
    int r = pe_parse(S->b.file, S->b.len, &S->info);
    if (r != PE_OK) return r;
    S->img = (u8 *)calloc(S->info.size_of_image, 1);                 /* EXACT size: ASan sees any overrun */
    r = pe_map(S->b.file, S->b.len, &S->info, S->img);
    if (r != PE_OK) return r;
    return pe_relocate(&S->info, S->img, S->info.image_base);
}
static void syn_free(syn_t *S) { bl_free(&S->b); free(S->img); S->img = NULL; }
static void poke32(syn_t *S, u32 rva, u32 v) { bl_p32(S->b.file, bl_off(&S->b, rva), v); }
static void poke16(syn_t *S, u32 rva, u32 v) { bl_p16(S->b.file, bl_off(&S->b, rva), v); }

static int find_n(const syn_t *S, const char *name, u32 *rva, char *fwd, unsigned cap)
{
    return pe_export_find(&S->info, S->img, name, 0, 0, rva, fwd, cap);
}
static int find_o(const syn_t *S, unsigned ord, u32 *rva, char *fwd, unsigned cap)
{
    return pe_export_find(&S->info, S->img, NULL, ord, 1, rva, fwd, cap);
}

/* ================================================================================================================ */
/* Group E: export directory                                                                                          */
/* ================================================================================================================ */
static const bfunc_t STD_F[5] = { {0x11, 0}, {0x22, 0}, {0x33, 0}, {0, 0}, {0, "other.func"} };
static const bname_t STD_N[5] = { {"alpha", 0}, {"beta", 1}, {"gamma", 2}, {"unused", 3}, {"fwdname", 4} };

static bspec_t std_spec(u32 base)
{
    bspec_t s;
    memset(&s, 0, sizeof s);
    s.base = base;
    s.nfuncs = 5; s.funcs = STD_F;
    s.nnames = 5; s.names = STD_N;
    s.dll_name = "std.dll";
    return s;
}

static void test_export_basic(void)
{
    syn_t S;
    bspec_t sp = std_spec(10);
    u32 rva, nf, nn, ord, v, idx;
    int isf, r;
    char fwd[64], nm[16];

    printf("[E1] synthetic export directory: name / ordinal / base / unused slot / forwarder\n");
    syn_make(&S, &sp);
    CHECK_RC(syn_load(&S), PE_OK);
    CHECK_RC(pe_export_validate(&S.info, S.img), PE_OK);

    CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_OK);       CHECK_U64(rva, S.b.stub_rva[0]);
    CHECK_RC(find_n(&S, "beta", &rva, NULL, 0), PE_OK);        CHECK_U64(rva, S.b.stub_rva[1]);
    CHECK_RC(find_n(&S, "gamma", &rva, NULL, 0), PE_OK);       CHECK_U64(rva, S.b.stub_rva[2]);
    CHECK(S.b.stub_rva[0] != 0 && S.b.stub_rva[0] != S.b.stub_rva[1]);
    CHECK_RC(find_o(&S, 10, &rva, NULL, 0), PE_OK);            CHECK_U64(rva, S.b.stub_rva[0]);
    CHECK_RC(find_o(&S, 11, &rva, NULL, 0), PE_OK);            CHECK_U64(rva, S.b.stub_rva[1]);
    CHECK_RC(find_o(&S, 12, &rva, NULL, 0), PE_OK);            CHECK_U64(rva, S.b.stub_rva[2]);
    CHECK_RC(find_o(&S, 13, &rva, NULL, 0), PE_E_NOTFOUND);    /* unused slot */
    CHECK_U64(rva, 0);
    CHECK_RC(find_n(&S, "unused", &rva, NULL, 0), PE_E_NOTFOUND);   /* a name that points at an unused slot */
    CHECK_RC(find_o(&S, 9, &rva, NULL, 0), PE_E_NOTFOUND);     /* below the ordinal base */
    CHECK_RC(find_o(&S, 15, &rva, NULL, 0), PE_E_NOTFOUND);    /* one past the last */
    CHECK_RC(find_o(&S, 0, &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(find_o(&S, 0xffffffffu, &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_NOTFOUND);

    /* names: exact, case-sensitive, never prefix / extension matched */
    CHECK_RC(find_n(&S, "ALPHA", &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(find_n(&S, "Alpha", &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(find_n(&S, "alph", &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(find_n(&S, "alphaa", &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(find_n(&S, "alpha ", &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(find_n(&S, "", &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(pe_export_find(&S.info, S.img, NULL, 0, 0, &rva, NULL, 0), PE_E_NOTFOUND);

    /* forwarder: reported, copied, bounded; never truncated */
    memset(fwd, 'Z', sizeof fwd);
    r = find_n(&S, "fwdname", &rva, fwd, sizeof fwd);
    CHECK_RC(r, PE_EXPORT_FORWARDER);
    CHECK_STR(fwd, "other.func");
    CHECK_U64(rva, S.b.fwd_rva[4]);
    CHECK_RC(find_o(&S, 14, &rva, fwd, sizeof fwd), PE_EXPORT_FORWARDER);
    CHECK_RC(find_n(&S, "fwdname", &rva, fwd, 11), PE_EXPORT_FORWARDER);     /* exactly strlen + 1 */
    CHECK_RC(find_n(&S, "fwdname", &rva, fwd, 10), PE_E_BUFSIZE);            /* one short: error, not truncation */
    CHECK_RC(find_n(&S, "fwdname", &rva, fwd, 1), PE_E_BUFSIZE);
    CHECK_RC(find_n(&S, "fwdname", &rva, fwd, 0), PE_E_BUFSIZE);             /* a buffer of 0 bytes cannot hold it */
    CHECK_RC(pe_export_find(&S.info, S.img, "fwdname", 0, 0, &rva, NULL, 0), PE_EXPORT_FORWARDER);
    CHECK_RC(pe_export_find(&S.info, S.img, "fwdname", 0, 0, NULL, NULL, 0), PE_EXPORT_FORWARDER);

    /* count / enumerate */
    CHECK_RC(pe_export_count(&S.info, S.img, &nf, &nn), PE_OK);
    CHECK_U64(nf, 5); CHECK_U64(nn, 5);
    CHECK_RC(pe_export_count(&S.info, S.img, NULL, NULL), PE_OK);
    for (idx = 0; idx < 5; idx++) {
        CHECK_RC(pe_export_at(&S.info, S.img, idx, &ord, &v, &isf), PE_OK);
        CHECK_U64(ord, 10 + idx);
        CHECK_U64(isf, idx == 4 ? 1 : 0);
        CHECK_U64(v == 0, idx == 3 ? 1 : 0);
        CHECK_RC(pe_export_name_at(&S.info, S.img, idx, nm, sizeof nm, &ord, &v), PE_OK);
        CHECK_STR(nm, STD_N[idx].name);
        CHECK_U64(ord, 10 + idx);
    }
    CHECK_RC(pe_export_at(&S.info, S.img, 5, &ord, &v, &isf), PE_E_NOTFOUND);
    CHECK_RC(pe_export_name_at(&S.info, S.img, 5, nm, sizeof nm, &ord, &v), PE_E_NOTFOUND);
    CHECK_RC(pe_export_name_at(&S.info, S.img, 4, nm, 7, &ord, &v), PE_E_BUFSIZE);   /* "fwdname" needs 8 */
    CHECK_RC(pe_export_name_at(&S.info, S.img, 4, nm, 8, &ord, &v), PE_OK);
    syn_free(&S);

    printf("[E2] unsorted names are found (no binary search); duplicates: first wins; aliases\n");
    {
        static const bfunc_t F[4] = { {0xA1, 0}, {0xA2, 0}, {0xA3, 0}, {0xA4, 0} };
        static const bname_t N[6] = { {"zeta", 0}, {"alpha", 1}, {"mid", 2}, {"beta", 3}, {"alpha", 2}, {"alias", 1} };
        bspec_t s2;
        memset(&s2, 0, sizeof s2);
        s2.nfuncs = 4; s2.funcs = F; s2.nnames = 6; s2.names = N;
        syn_make(&S, &s2);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_OK);
        CHECK_RC(find_n(&S, "zeta", &rva, NULL, 0), PE_OK);  CHECK_U64(rva, S.b.stub_rva[0]);
        CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_OK); CHECK_U64(rva, S.b.stub_rva[1]);   /* first "alpha" */
        CHECK_RC(find_n(&S, "mid", &rva, NULL, 0), PE_OK);   CHECK_U64(rva, S.b.stub_rva[2]);
        CHECK_RC(find_n(&S, "beta", &rva, NULL, 0), PE_OK);  CHECK_U64(rva, S.b.stub_rva[3]);
        CHECK_RC(find_n(&S, "alias", &rva, NULL, 0), PE_OK); CHECK_U64(rva, S.b.stub_rva[1]);   /* two names, one slot */
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_OK);       CHECK_U64(rva, S.b.stub_rva[0]);   /* default base 1 */
        CHECK_RC(find_o(&S, 4, &rva, NULL, 0), PE_OK);       CHECK_U64(rva, S.b.stub_rva[3]);
        CHECK_RC(find_o(&S, 5, &rva, NULL, 0), PE_E_NOTFOUND);
        syn_free(&S);
    }

    printf("[E3] no export directory / empty directory\n");
    {
        bspec_t s3;
        memset(&s3, 0, sizeof s3);
        s3.no_exports = 1;
        syn_make(&S, &s3);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "x", &rva, NULL, 0), PE_E_NOTFOUND);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_NOTFOUND);
        CHECK_RC(pe_export_validate(&S.info, S.img), 1);
        CHECK_RC(pe_export_count(&S.info, S.img, &nf, &nn), 1);
        CHECK_U64(nf, 0);
        CHECK_RC(pe_export_at(&S.info, S.img, 0, &ord, &v, &isf), PE_E_NOTFOUND);
        syn_free(&S);
        memset(&s3, 0, sizeof s3);                               /* directory present, zero exports */
        syn_make(&S, &s3);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_OK);
        CHECK_RC(find_n(&S, "x", &rva, NULL, 0), PE_E_NOTFOUND);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_NOTFOUND);
        syn_free(&S);
    }

    printf("[E4] name length boundaries (1023 chars matches, 1024 is malformed, a 1024-char request cannot match)\n");
    {
        static char n1023[1024], n1024[1025];
        bfunc_t F[2] = { {0x41, 0}, {0x42, 0} };
        bname_t N[2];
        bspec_t s4;
        memset(n1023, 'q', 1023); n1023[1023] = 0;
        memset(n1024, 'r', 1024); n1024[1024] = 0;
        N[0].name = n1023; N[0].index = 0; N[1].name = "tail"; N[1].index = 1;
        memset(&s4, 0, sizeof s4);
        s4.nfuncs = 2; s4.funcs = F; s4.nnames = 2; s4.names = N;
        syn_make(&S, &s4);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_OK);
        CHECK_RC(find_n(&S, n1023, &rva, NULL, 0), PE_OK);       CHECK_U64(rva, S.b.stub_rva[0]);
        CHECK_RC(find_n(&S, "tail", &rva, NULL, 0), PE_OK);
        CHECK_RC(find_n(&S, n1024, &rva, NULL, 0), PE_E_NOTFOUND);
        n1023[1022] = 0;                                         /* a 1022-char prefix must not match the 1023-char name */
        CHECK_RC(find_n(&S, n1023, &rva, NULL, 0), PE_E_NOTFOUND);
        syn_free(&S);
        N[0].name = n1024;                                       /* 1024 chars + NUL = 1025 bytes > cap: malformed */
        syn_make(&S, &s4);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_E_EXPORT);
        CHECK_RC(find_n(&S, "tail", &rva, NULL, 0), PE_E_EXPORT);      /* scan hits the bad entry first */
        CHECK_RC(find_o(&S, 2, &rva, NULL, 0), PE_OK);                 /* by ordinal does not look at the names */
        syn_free(&S);
    }

    printf("[E5] ordinal base extremes (no u32 wrap confusion)\n");
    {
        static const bfunc_t F[5] = { {0x51, 0}, {0x52, 0}, {0x53, 0}, {0x54, 0}, {0x55, 0} };
        static const bname_t N[2] = { {"hi", 4}, {"lo", 0} };
        bspec_t s5;
        memset(&s5, 0, sizeof s5);
        s5.base = 0xfffffffeu; s5.nfuncs = 5; s5.funcs = F; s5.nnames = 2; s5.names = N;
        syn_make(&S, &s5);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_o(&S, 0xfffffffeu, &rva, NULL, 0), PE_OK); CHECK_U64(rva, S.b.stub_rva[0]);
        CHECK_RC(find_o(&S, 0xffffffffu, &rva, NULL, 0), PE_OK); CHECK_U64(rva, S.b.stub_rva[1]);
        CHECK_RC(find_o(&S, 0, &rva, NULL, 0), PE_E_NOTFOUND);   /* would be index 2 if the base wrapped */
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_NOTFOUND);
        CHECK_RC(find_o(&S, 2, &rva, NULL, 0), PE_E_NOTFOUND);
        CHECK_RC(find_n(&S, "hi", &rva, NULL, 0), PE_OK);        CHECK_U64(rva, S.b.stub_rva[4]);
        syn_free(&S);
    }
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* hostile mutations                                                                                                  */
/* ---------------------------------------------------------------------------------------------------------------- */
static void test_export_hostile(void)
{
    syn_t S;
    bspec_t sp = std_spec(1);
    u32 rva, v;
    char fwd[64];
    (void)v;

    printf("[E6] hostile export tables: every defect is rejected with PE_E_EXPORT (or NOTFOUND), never a crash\n");

#define HOSTILE_BEGIN() do { syn_make(&S, &sp)
#define HOSTILE_END()   syn_free(&S); } while (0)

    /* --- name pointer outside the image / beyond the image end --- */
    HOSTILE_BEGIN(); poke32(&S, S.b.names_rva + 0, 0xfffffff0u);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "beta", &rva, NULL, 0), PE_E_EXPORT);
        CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_E_EXPORT);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_E_EXPORT);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_OK);           /* by ordinal: lazy validation, untouched entries fine */
        CHECK_RC(pe_export_name_at(&S.info, S.img, 0, fwd, sizeof fwd, NULL, NULL), PE_E_EXPORT);
    HOSTILE_END();
    HOSTILE_BEGIN(); poke32(&S, S.b.names_rva + 4, S.b.size_of_image);       /* == size_of_image: first byte past */
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "gamma", &rva, NULL, 0), PE_E_EXPORT);
        CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_OK);     /* entries before the bad one are still found */
    HOSTILE_END();
    HOSTILE_BEGIN(); poke32(&S, S.b.names_rva + 0, 0);                        /* name RVA 0 -> the DOS header: "MZ" */
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "MZ", &rva, NULL, 0), PE_OK);        /* a silly but legal name: it is inside the image */
        CHECK_U64(rva, S.b.stub_rva[0]);
        CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_E_NOTFOUND);
    HOSTILE_END();

    /* --- a name that runs to the end of the image without a NUL: exact-size copy so ASan sees an over-read --- */
    HOSTILE_BEGIN();
        CHECK_RC(syn_load(&S), PE_OK);
        {
            u32 nrva = S.b.name_rva[0];
            u32 n = nrva + 3;                                    /* image ends 3 bytes into the first name */
            u8 *tight = (u8 *)malloc(n);
            pe_info_t ti = S.info;
            memcpy(tight, S.img, n);
            ti.size_of_image = n;
            ti.export_size = n - ti.export_rva;                  /* keep the directory inside the shrunken image */
            CHECK_RC(pe_export_find(&ti, tight, "beta", 0, 0, &rva, NULL, 0), PE_E_EXPORT);
            CHECK_RC(pe_export_validate(&ti, tight), PE_E_EXPORT);
            CHECK_RC(pe_export_find(&ti, tight, NULL, 1, 1, &rva, NULL, 0), PE_OK);   /* ordinal 1: EAT is in range */
            free(tight);
        }
    HOSTILE_END();

    /* --- ordinal table entries --- */
    HOSTILE_BEGIN(); poke16(&S, S.b.ords_rva + 2, 5);                         /* == nfuncs */
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "beta", &rva, NULL, 0), PE_E_EXPORT);
        CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_OK);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_E_EXPORT);
        CHECK_RC(pe_export_name_at(&S.info, S.img, 1, fwd, sizeof fwd, NULL, NULL), PE_E_EXPORT);
    HOSTILE_END();
    HOSTILE_BEGIN(); poke16(&S, S.b.ords_rva + 0, 0xffff);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_E_EXPORT);
    HOSTILE_END();

    /* --- EAT entries --- */
    HOSTILE_BEGIN(); poke32(&S, S.b.eat_rva + 0, S.b.size_of_image);          /* first byte past the image */
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_E_EXPORT);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_EXPORT);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_E_EXPORT);
        CHECK_RC(pe_export_at(&S.info, S.img, 0, NULL, NULL, NULL), PE_E_EXPORT);
        CHECK_RC(find_n(&S, "beta", &rva, NULL, 0), PE_OK);
    HOSTILE_END();
    HOSTILE_BEGIN(); poke32(&S, S.b.eat_rva + 0, 0xffffffffu);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_EXPORT);
    HOSTILE_END();
    HOSTILE_BEGIN(); poke32(&S, S.b.eat_rva + 0, S.b.size_of_image - 1);      /* last byte of the image: in range */
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_OK);
        CHECK_U64(rva, S.b.size_of_image - 1);
    HOSTILE_END();

    /* --- forwarder strings --- */
    HOSTILE_BEGIN();                                                          /* forwarder -> empty string */
        CHECK_RC(syn_load(&S), PE_OK);
        S.img[S.b.fwd_rva[4]] = 0;
        CHECK_RC(find_o(&S, 5, &rva, fwd, sizeof fwd), PE_E_EXPORT);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_E_EXPORT);
    HOSTILE_END();
    {
        static char f255[256], f256[257];
        bfunc_t F[2] = { {0x31, 0}, {0, 0} };
        bname_t N[2] = { {"short", 0}, {"long", 1} };
        bspec_t s6;
        char big[300];
        memset(f255, 'd', 255); f255[255] = 0; f255[100] = '.';
        memset(f256, 'd', 256); f256[256] = 0; f256[100] = '.';
        memset(&s6, 0, sizeof s6);
        s6.nfuncs = 2; s6.funcs = F; s6.nnames = 2; s6.names = N;
        F[1].fwd = f255;
        syn_make(&S, &s6);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_OK);
        CHECK_RC(find_n(&S, "long", &rva, big, sizeof big), PE_EXPORT_FORWARDER);        /* 255 chars: the maximum */
        CHECK(strlen(big) == 255);
        CHECK_RC(find_n(&S, "long", &rva, big, 255), PE_E_BUFSIZE);
        CHECK_RC(find_n(&S, "long", &rva, big, 256), PE_EXPORT_FORWARDER);
        syn_free(&S);
        F[1].fwd = f256;
        syn_make(&S, &s6);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "long", &rva, big, sizeof big), PE_E_EXPORT);                /* 256 chars: over the cap */
        CHECK_RC(pe_export_validate(&S.info, S.img), PE_E_EXPORT);
        CHECK_RC(find_n(&S, "short", &rva, big, sizeof big), PE_OK);
        syn_free(&S);
    }

    /* --- directory header: counts and table extents --- */
    {
        struct { const char *what; u32 off; u32 val; } T[] = {
            { "nfuncs = 65537",           20, 65537u },
            { "nfuncs = 0xffffffff",      20, 0xffffffffu },
            { "nnames = 65537",           24, 65537u },
            { "nnames = 0x40000000",      24, 0x40000000u },
            { "eat_rva = 0",              28, 0u },
            { "names_rva = 0",            32, 0u },
            { "ords_rva = 0",             36, 0u },
            { "eat_rva past image",       28, 0xfffffff0u },
            { "names_rva past image",     32, 0xfffffff0u },
            { "ords_rva past image",      36, 0xfffffff0u },
        };
        size_t i;
        for (i = 0; i < sizeof T / sizeof T[0]; i++) {
            syn_make(&S, &sp);
            poke32(&S, S.b.export_rva + T[i].off, T[i].val);
            CHECK_RC(syn_load(&S), PE_OK);
            g_checks++;
            if (find_n(&S, "alpha", &rva, NULL, 0) != PE_E_EXPORT) { g_fail++; printf("  FAIL [%s]: name find\n", T[i].what); }
            g_checks++;
            if (find_o(&S, 1, &rva, NULL, 0) != PE_E_EXPORT) { g_fail++; printf("  FAIL [%s]: ordinal find\n", T[i].what); }
            g_checks++;
            if (pe_export_validate(&S.info, S.img) != PE_E_EXPORT) { g_fail++; printf("  FAIL [%s]: validate\n", T[i].what); }
            syn_free(&S);
        }
        /* a table ending exactly at the end of the image is fine, one byte further is not */
        syn_make(&S, &sp);
        poke32(&S, S.b.export_rva + 28, S.b.size_of_image - 4u * 5u);            /* EAT = last 20 bytes (all zero) */
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_NOTFOUND);                   /* zero entries = unused */
        syn_free(&S);
        syn_make(&S, &sp);
        poke32(&S, S.b.export_rva + 28, S.b.size_of_image - 4u * 5u + 1u);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_EXPORT);
        syn_free(&S);
        /* nnames > 0 with nfuncs == 0: every ordinal-table value is out of range */
        syn_make(&S, &sp);
        poke32(&S, S.b.export_rva + 20, 0);
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(find_n(&S, "alpha", &rva, NULL, 0), PE_E_EXPORT);
        CHECK_RC(find_o(&S, 1, &rva, NULL, 0), PE_E_NOTFOUND);
        syn_free(&S);
    }

    /* --- the pe_info_t is caller-owned: corrupt it --- */
    syn_make(&S, &sp);
    CHECK_RC(syn_load(&S), PE_OK);
    {
        pe_info_t bad = S.info;
        bad.export_size = 39;                                   /* smaller than IMAGE_EXPORT_DIRECTORY */
        CHECK_RC(pe_export_find(&bad, S.img, "alpha", 0, 0, &rva, NULL, 0), PE_E_EXPORT);
        bad = S.info; bad.export_rva = bad.size_of_image - 8;   /* directory runs off the image */
        CHECK_RC(pe_export_find(&bad, S.img, "alpha", 0, 0, &rva, NULL, 0), PE_E_EXPORT);
        bad = S.info; bad.export_rva = 0;                       /* size without RVA */
        CHECK_RC(pe_export_find(&bad, S.img, "alpha", 0, 0, &rva, NULL, 0), PE_E_EXPORT);
        bad = S.info; bad.size_of_image = 0;
        CHECK_RC(pe_export_find(&bad, S.img, "alpha", 0, 0, &rva, NULL, 0), PE_E_SIZE);
        bad = S.info; bad.size_of_image = PE_MAX_IMAGE + 1;
        CHECK_RC(pe_export_find(&bad, S.img, "alpha", 0, 0, &rva, NULL, 0), PE_E_SIZE);
        bad = S.info; bad.n_sections = 0;
        CHECK_RC(pe_export_find(&bad, S.img, "alpha", 0, 0, &rva, NULL, 0), PE_E_SECTIONS);
        CHECK_RC(pe_export_find(NULL, S.img, "alpha", 0, 0, &rva, NULL, 0), PE_E_BOUNDS);
        CHECK_RC(pe_export_find(&S.info, NULL, "alpha", 0, 0, &rva, NULL, 0), PE_E_BOUNDS);
        CHECK_RC(pe_export_validate(NULL, S.img), PE_E_BOUNDS);
        CHECK_RC(pe_export_count(&S.info, NULL, NULL, NULL), PE_E_BOUNDS);
    }
    syn_free(&S);
    (void)fwd;
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* forwarder-string parser                                                                                            */
/* ---------------------------------------------------------------------------------------------------------------- */
static void test_forwarder_parse(void)
{
    char dll[64], fn[64];
    unsigned ord;
    int by;
    static const struct { const char *in; int rc; const char *dll; const char *fn; unsigned ord; int by; } T[] = {
        { "NTDLL.RtlFoo",       PE_OK,    "NTDLL",     "RtlFoo", 0,     0 },
        { "a.b",                PE_OK,    "a",         "b",      0,     0 },
        { "my.lib.Func",        PE_OK,    "my.lib",    "Func",   0,     0 },       /* last dot separates */
        { "DLL.#12",            PE_OK,    "DLL",       "",       12,    1 },
        { "DLL.#65535",         PE_OK,    "DLL",       "",       65535, 1 },
        { "DLL.#1",             PE_OK,    "DLL",       "",       1,     1 },
        { "DLL.#0",             PE_E_FWD, "", "", 0, 0 },
        { "DLL.#65536",         PE_E_FWD, "", "", 0, 0 },
        { "DLL.#123456",        PE_E_FWD, "", "", 0, 0 },
        { "DLL.#",              PE_E_FWD, "", "", 0, 0 },
        { "DLL.#1x",            PE_E_FWD, "", "", 0, 0 },
        { "DLL.#-1",            PE_E_FWD, "", "", 0, 0 },
        { "nodot",              PE_E_FWD, "", "", 0, 0 },
        { ".func",              PE_E_FWD, "", "", 0, 0 },
        { "dll.",               PE_E_FWD, "", "", 0, 0 },
        { ".",                  PE_E_FWD, "", "", 0, 0 },
        { "",                   PE_E_FWD, "", "", 0, 0 },
        { "ab\x01.f",           PE_E_FWD, "", "", 0, 0 },
        { "ab.f\x7f",           PE_E_FWD, "", "", 0, 0 },
        { "ab.\xff",            PE_E_FWD, "", "", 0, 0 },
    };
    size_t i;
    char big[300];

    printf("[E7] forwarder string parser\n");
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        int r = pe_export_parse_forwarder(T[i].in, dll, sizeof dll, fn, sizeof fn, &ord, &by);
        g_checks++;
        if (r != T[i].rc) { g_fail++; printf("  FAIL [%s]: rc %d expected %d\n", T[i].in, r, T[i].rc); continue; }
        if (r == PE_OK) {
            g_checks += 4;
            if (strcmp(dll, T[i].dll) || strcmp(fn, T[i].fn) || ord != T[i].ord || by != T[i].by) { g_fail++; printf("  FAIL [%s]: fields\n", T[i].in); }
        }
    }
    CHECK_RC(pe_export_parse_forwarder(NULL, dll, sizeof dll, fn, sizeof fn, &ord, &by), PE_E_FWD);
    CHECK_RC(pe_export_parse_forwarder("abc.def", dll, 3, fn, sizeof fn, &ord, &by), PE_E_BUFSIZE);       /* "abc" needs 4 */
    CHECK_RC(pe_export_parse_forwarder("abc.def", dll, 4, fn, sizeof fn, &ord, &by), PE_OK);
    CHECK_RC(pe_export_parse_forwarder("abc.def", dll, 64, fn, 3, &ord, &by), PE_E_BUFSIZE);
    CHECK_RC(pe_export_parse_forwarder("abc.def", dll, 64, fn, 4, &ord, &by), PE_OK);
    CHECK_RC(pe_export_parse_forwarder("abc.def", NULL, 0, NULL, 0, NULL, NULL), PE_OK);
    memset(big, 'x', 299); big[299] = 0; big[10] = '.';
    CHECK_RC(pe_export_parse_forwarder(big, dll, sizeof dll, fn, sizeof fn, &ord, &by), PE_E_FWD);          /* >= 256 */
    memset(big, 'x', 255); big[255] = 0; big[10] = '.';
    CHECK_RC(pe_export_parse_forwarder(big, NULL, 0, NULL, 0, NULL, NULL), PE_OK);                          /* 255: ok */
}

/* ---------------------------------------------------------------------------------------------------------------- */
/* Group D: delay imports                                                                                             */
/* ---------------------------------------------------------------------------------------------------------------- */
typedef struct { int calls; char dll[8][32]; char name[8][32]; unsigned ord[8]; int byo[8]; } dlog_t;

static int delay_resolver(const char *dll, const char *name, unsigned short ord, int by_ord, unsigned long long *out, void *user)
{
    dlog_t *L = (dlog_t *)user;
    int i = L->calls++;
    if (i < 8) {
        snprintf(L->dll[i], sizeof L->dll[i], "%s", dll);
        snprintf(L->name[i], sizeof L->name[i], "%s", name);
        L->ord[i] = ord;
        L->byo[i] = by_ord;
    }
    if (strcmp(name, "fnA") == 0) { *out = 0x1111000000ull; return 0; }
    if (by_ord && ord == 9) { *out = 0x2222000000ull; return 0; }
    return 1;
}

static u64 rd64s(const u8 *p) { return (u64)bl_g32(p, 0) | ((u64)bl_g32(p, 4) << 32); }

static void test_delay_imports(void)
{
    static const char *const N1[3] = { "fnA", "#9", "fnB" };
    static const char *const N2[1] = { "x" };
    static const bimp_t D[2] = { { "dly.dll", 3, N1 }, { "gone.dll", 1, N2 } };
    syn_t S;
    bspec_t sp;
    dlog_t L;
    unsigned n, un;
    u64 orig0, orig1, orig2, orig3;
    u32 d0, d1;

    printf("[D1] delay-load imports: eager bind, unresolved slots keep the compiler's thunk\n");
    memset(&sp, 0, sizeof sp);
    sp.is_exe = 1; sp.no_exports = 1; sp.ndelay = 2; sp.delay = D;
    syn_make(&S, &sp);
    CHECK_RC(syn_load(&S), PE_OK);
    CHECK_U64(S.info.delay_import_rva, S.b.delay_rva);
    d0 = S.b.diat_rva[0]; d1 = S.b.diat_rva[1];
    orig0 = rd64s(S.img + d0); orig1 = rd64s(S.img + d0 + 8); orig2 = rd64s(S.img + d0 + 16); orig3 = rd64s(S.img + d1);
    CHECK_U64(orig0, S.info.image_base + S.b.text_rva);          /* relocated at the preferred base: unchanged */

    memset(&L, 0, sizeof L);
    CHECK_RC(pe_delay_imports(&S.info, S.img, NULL, NULL, &n, &un), PE_OK);       /* dry run */
    CHECK_U64(n, 4); CHECK_U64(un, 0);
    CHECK_U64(rd64s(S.img + d0), orig0);

    CHECK_RC(pe_delay_imports(&S.info, S.img, delay_resolver, &L, &n, &un), PE_OK);
    CHECK_U64(n, 4); CHECK_U64(un, 2); CHECK_U64(L.calls, 4);
    CHECK_STR(L.dll[0], "dly.dll"); CHECK_STR(L.name[0], "fnA");
    CHECK_STR(L.dll[1], "dly.dll"); CHECK_STR(L.name[1], ""); CHECK_U64(L.ord[1], 9); CHECK_U64(L.byo[1], 1);
    CHECK_STR(L.dll[2], "dly.dll"); CHECK_STR(L.name[2], "fnB");
    CHECK_STR(L.dll[3], "gone.dll"); CHECK_STR(L.name[3], "x");
    CHECK_U64(rd64s(S.img + d0), 0x1111000000ull);
    CHECK_U64(rd64s(S.img + d0 + 8), 0x2222000000ull);
    CHECK_U64(rd64s(S.img + d0 + 16), orig2);                    /* unresolved: untouched */
    CHECK_U64(rd64s(S.img + d1), orig3);
    (void)orig1;
    syn_free(&S);

    printf("[D2] delay-load: no directory / malformed descriptors\n");
    memset(&sp, 0, sizeof sp);
    sp.is_exe = 1; sp.no_exports = 1;
    syn_make(&S, &sp);
    CHECK_RC(syn_load(&S), PE_OK);
    CHECK_RC(pe_delay_imports(&S.info, S.img, delay_resolver, &L, &n, &un), PE_OK);
    CHECK_U64(n, 0);
    syn_free(&S);

    {
        struct { const char *what; u32 desc_off; u32 val; int want; } T[] = {
            { "grAttrs bit0 clear (VA form)", 0, 0, PE_E_IMPORT },
            { "name rva = 0",                 4, 0, PE_E_IMPORT },
            { "name rva past image",          4, 0xfffffff0u, PE_E_IMPORT },
            { "iat rva = 0",                 12, 0, PE_E_IMPORT },
            { "int rva = 0",                 16, 0, PE_E_IMPORT },
            { "iat rva past image",          12, 0xfffffff8u, PE_E_IMPORT },
            { "int rva past image",          16, 0xfffffff8u, PE_E_IMPORT },
        };
        size_t i;
        for (i = 0; i < sizeof T / sizeof T[0]; i++) {
            memset(&sp, 0, sizeof sp);
            sp.is_exe = 1; sp.no_exports = 1; sp.ndelay = 2; sp.delay = D;
            syn_make(&S, &sp);
            poke32(&S, S.b.delay_rva + T[i].desc_off, T[i].val);
            CHECK_RC(syn_load(&S), PE_OK);
            memset(&L, 0, sizeof L);
            g_checks++;
            if (pe_delay_imports(&S.info, S.img, delay_resolver, &L, &n, &un) != T[i].want) { g_fail++; printf("  FAIL [%s]\n", T[i].what); }
            syn_free(&S);
        }
        /* ordinal thunk with reserved bits */
        memset(&sp, 0, sizeof sp);
        sp.is_exe = 1; sp.no_exports = 1; sp.ndelay = 2; sp.delay = D;
        syn_make(&S, &sp);
        bl_p32(S.b.file, bl_off(&S.b, S.b.dint_rva[0]) + 8 + 4, 0x80010000u);       /* thunk #1: bit63 | bit48 */
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_RC(pe_delay_imports(&S.info, S.img, delay_resolver, &L, &n, &un), PE_E_ORDINAL);
        syn_free(&S);
        /* descriptor array with no terminator before the end of the image */
        memset(&sp, 0, sizeof sp);
        sp.is_exe = 1; sp.no_exports = 1; sp.ndelay = 1; sp.delay = D;
        syn_make(&S, &sp);
        CHECK_RC(syn_load(&S), PE_OK);
        {
            pe_info_t bad = S.info;
            bad.delay_import_rva = bad.size_of_image - 40;         /* only one 32-byte slot fits, non-zero tail */
            memset(S.img + bad.delay_import_rva, 0x41, 40);
            CHECK_RC(pe_delay_imports(&bad, S.img, delay_resolver, &L, &n, &un), PE_E_IMPORT);
        }
        syn_free(&S);
    }
}

/* ================================================================================================================ */
/* Host environment for the module loader tests                                                                       */
/* ================================================================================================================ */
#define VF_MAX 96
#define LOG_MAX 160

typedef struct { char name[96]; const u8 *data; size_t len; } vfile_t;

typedef struct {
    vfile_t vf[VF_MAX]; int nvf;
    char read_log[LOG_MAX][96]; int nread;
    int live_files;
    int alloc_calls, free_calls, live_images;
    unsigned ev[LOG_MAX]; int nev;               /* KERNEL32!SetLastError(value) log (DllMain markers) */
    char calls[2048];                            /* "name:reason " per call_entry */
    int exec;                                    /* execute the DllMain for real */
    char fail_name[96];                          /* DLL whose ATTACH returns FALSE */
    int builtin_calls;
    char bl_dll[LOG_MAX][64], bl_name[LOG_MAX][64]; int nbl;
    int stub_calls;
    int use_stub, try_preferred, pref_hits;
    char reenter_on[96], reenter_load[96];       /* DllMain of reenter_on does LoadLibrary(reenter_load) + run_inits */
    int reenter_idx, reenter_rc;
    pe_mod_ctx_t *ctx;
} host_t;

static host_t H;
static pe_mod_ctx_t C;
static pe_mod_ops_t OPS;

static MS unsigned h_SetLastError(unsigned v) { if (H.nev < LOG_MAX) H.ev[H.nev++] = v; return 0; }
static MS unsigned h_loud_stub(void) { H.stub_calls++; return 0xBAD; }
static MS int h_magic(int x) { return x + 1000; }
#define ADDR(f) ((u64)(uintptr_t)(f))

static int h_read_file(const char *name, const unsigned char **buf, unsigned long *len, void *u)
{
    int i;
    (void)u;
    if (H.nread < LOG_MAX) snprintf(H.read_log[H.nread], sizeof H.read_log[0], "%s", name);
    H.nread++;
    for (i = 0; i < H.nvf; i++) {
        if (strcasecmp(H.vf[i].name, name) == 0) {
            u8 *copy = (u8 *)malloc(H.vf[i].len ? H.vf[i].len : 1);       /* EXACT size: ASan sees over-reads and use-after-release */
            memcpy(copy, H.vf[i].data, H.vf[i].len);
            *buf = copy;
            *len = (unsigned long)H.vf[i].len;
            H.live_files++;
            return 0;
        }
    }
    return 1;
}
static void h_release_file(const unsigned char *buf, unsigned long len, void *u)
{
    (void)len; (void)u;
    free((void *)buf);
    H.live_files--;
}
static unsigned char *h_alloc(unsigned long size, unsigned long long pref, void *u)
{
    void *p = MAP_FAILED;
    (void)u;
    H.alloc_calls++;
    if (H.try_preferred && pref < (1ull << 46)) {
        p = mmap((void *)(uintptr_t)pref, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && (u64)(uintptr_t)p != pref) { munmap(p, size); p = MAP_FAILED; }
        if (p != MAP_FAILED) H.pref_hits++;
    }
    if (p == MAP_FAILED) p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    H.live_images++;
    return (unsigned char *)p;
}
static void h_free_image(unsigned char *img, unsigned long size, void *u)
{
    (void)u;
    munmap(img, size);
    H.free_calls++;
    H.live_images--;
}
static int h_builtin(const char *dll, const char *name, unsigned short ord, int by_ord, unsigned long long *out, void *u)
{
    (void)u;
    if (H.nbl < LOG_MAX) { snprintf(H.bl_dll[H.nbl], 64, "%s", dll); snprintf(H.bl_name[H.nbl], 64, "%s", name); }
    H.nbl++;
    H.builtin_calls++;
    if (strcasecmp(dll, "KERNEL32.dll") == 0) {
        if (!by_ord && strcmp(name, "SetLastError") == 0) { *out = ADDR(h_SetLastError); return 0; }
        *out = ADDR(h_loud_stub);                                /* a builtin DLL, function not implemented */
        return 1;
    }
    if (strcasecmp(dll, "testsys.dll") == 0) {
        if (!by_ord && strcmp(name, "Magic") == 0) { *out = ADDR(h_magic); return 0; }
        if (by_ord && ord == 5) { *out = ADDR(h_magic); return 0; }
        *out = ADDR(h_loud_stub);
        return 1;
    }
    return 1;                                                    /* not a builtin DLL: *out untouched (0) */
}
static int h_ustub(const char *dll, const char *name, unsigned short ord, int by_ord, unsigned long long *out, void *u)
{
    (void)dll; (void)name; (void)ord; (void)by_ord; (void)u;
    *out = ADDR(h_loud_stub);
    return 0;
}
static int h_call_entry(unsigned long long entry, unsigned long long base, unsigned reason, void *u)
{
    char one[128];
    const char *nm = "?";
    int k;
    (void)u;
    for (k = 0; k < (int)PE_MOD_MAX_MODULES; k++)               /* includes a module that is being unloaded */
        if (H.ctx->mod[k].state != PE_MOD_S_FREE && H.ctx->mod[k].base == base) nm = H.ctx->mod[k].name;
    snprintf(one, sizeof one, "%s:%u ", nm, reason);
    if (strlen(H.calls) + strlen(one) < sizeof H.calls) strcat(H.calls, one);
    if (reason == 1 && H.reenter_on[0] && strcasecmp(H.reenter_on, nm) == 0) {   /* LoadLibrary from inside a DllMain */
        H.reenter_on[0] = 0;
        H.reenter_idx = pe_mod_load(&C, &OPS, H.reenter_load);
        H.reenter_rc = pe_mod_run_inits(&C, &OPS);
    }
    if (reason == 1 && H.fail_name[0] && strcasecmp(H.fail_name, nm) == 0) return 0;
    if (H.exec) return ((MS int (*)(void *, unsigned, void *))(uintptr_t)entry)((void *)(uintptr_t)base, reason, NULL);
    return 1;
}

static void host_reset(void)
{
    memset(&H, 0, sizeof H);
    H.ctx = &C;
    memset(&OPS, 0, sizeof OPS);
    OPS.read_file = h_read_file;
    OPS.alloc_image = h_alloc;
    OPS.builtin_resolve = h_builtin;
    OPS.call_entry = h_call_entry;
    OPS.release_file = h_release_file;
    OPS.free_image = h_free_image;
    pe_mod_ctx_init(&C);
}
static void vf_add(const char *name, const u8 *data, size_t len)
{
    if (H.nvf >= VF_MAX) { printf("FATAL: vfs full\n"); exit(2); }
    snprintf(H.vf[H.nvf].name, sizeof H.vf[0].name, "%s", name);
    H.vf[H.nvf].data = data;
    H.vf[H.nvf].len = len;
    H.nvf++;
}
static int read_count(const char *name)
{
    int i, n = 0;
    for (i = 0; i < H.nread && i < LOG_MAX; i++) if (strcasecmp(H.read_log[i], name) == 0) n++;
    return n;
}
static int builtin_probe_count(const char *dll)
{
    int i, n = 0;
    for (i = 0; i < H.nbl && i < LOG_MAX; i++) if (strcasecmp(H.bl_dll[i], dll) == 0) n++;
    return n;
}
static int mod_count(void)
{
    int i, n = 0;
    for (i = 0; i < (int)PE_MOD_MAX_MODULES; i++) if (C.mod[i].state != PE_MOD_S_FREE) n++;
    return n;
}
static void end_host(const char *what)
{
    pe_mod_free_all(&C, &OPS);
    g_checks += 3;
    if (H.live_files != 0) { g_fail++; printf("  FAIL [%s]: %d file buffers not released\n", what, H.live_files); }
    if (H.live_images != 0) { g_fail++; printf("  FAIL [%s]: %d images not freed\n", what, H.live_images); }
    if (H.alloc_calls != H.free_calls) { g_fail++; printf("  FAIL [%s]: alloc %d != free %d\n", what, H.alloc_calls, H.free_calls); }
}

/* real MinGW fixtures */
typedef struct { const char *name; u8 *data; size_t len; } fix_t;
static fix_t FX[] = {
    { "dll_lib_a.dll", 0, 0 }, { "dll_lib_b.dll", 0, 0 }, { "dll_main.exe", 0, 0 }, { "dll_bad.dll", 0, 0 },
    { "dll_main_bad.exe", 0, 0 }, { "dll_cyc_x.dll", 0, 0 }, { "dll_cyc_y.dll", 0, 0 }, { "dll_main_cyc.exe", 0, 0 },
    { "dll_main_missing.exe", 0, 0 },
};
static fix_t *fx(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof FX / sizeof FX[0]; i++) if (strcmp(FX[i].name, name) == 0) return &FX[i];
    printf("FATAL: unknown fixture %s\n", name);
    exit(2);
}
static void fx_vf(const char *name) { fix_t *f = fx(name); vf_add(f->name, f->data, f->len); }

/* ================================================================================================================ */
/* export lookup on the REAL fixtures                                                                                 */
/* ================================================================================================================ */
static void test_fixture_exports(void)
{
    fix_t *a = fx("dll_lib_a.dll"), *b = fx("dll_lib_b.dll");
    syn_t S;
    u32 nf, nn, rva, i, ord;
    char fwd[64], nm[32];
    int r;

    printf("[E8] real MinGW DLL: dll_lib_a export directory (NONAME ordinal, forwarders)\n");
    memset(&S, 0, sizeof S);
    S.b.file = (u8 *)malloc(a->len);
    memcpy(S.b.file, a->data, a->len);
    S.b.len = a->len;
    CHECK_RC(syn_load(&S), PE_OK);
    CHECK_U64(S.info.is_dll, 1);
    CHECK_RC(pe_export_validate(&S.info, S.img), PE_OK);
    CHECK_RC(pe_export_count(&S.info, S.img, &nf, &nn), PE_OK);
    CHECK_U64(nf, 7); CHECK_U64(nn, 6);                          /* 7 functions, one of them (ordfn) has no name */
    CHECK_RC(find_n(&S, "add", &rva, NULL, 0), PE_OK);
    CHECK_RC(find_n(&S, "mul", &rva, NULL, 0), PE_OK);
    CHECK_RC(find_n(&S, "ordfn", &rva, NULL, 0), PE_E_NOTFOUND);  /* exported by ordinal only */
    CHECK_RC(find_o(&S, 7, &rva, NULL, 0), PE_OK);
    CHECK_RC(find_n(&S, "fwd_add", &rva, fwd, sizeof fwd), PE_EXPORT_FORWARDER);
    CHECK_STR(fwd, "dll_lib_b.sub");
    CHECK_RC(find_n(&S, "fwd_k32", &rva, fwd, sizeof fwd), PE_EXPORT_FORWARDER);
    CHECK_STR(fwd, "KERNEL32.SetLastError");
    CHECK_RC(find_n(&S, "Add", &rva, NULL, 0), PE_E_NOTFOUND);
    CHECK_RC(find_n(&S, "sub", &rva, NULL, 0), PE_E_NOTFOUND);   /* an IMPORT of a is not an export of a */
    for (i = 0; i < nn; i++) {                                   /* every name enumerates and round-trips */
        u32 rva2 = 0, rva3 = 0;
        int r2, r3;
        r = pe_export_name_at(&S.info, S.img, i, nm, sizeof nm, &ord, &rva);
        CHECK_RC(r, PE_OK);
        r2 = find_n(&S, nm, &rva2, fwd, sizeof fwd);
        CHECK(r2 == PE_OK || r2 == PE_EXPORT_FORWARDER);
        CHECK_U64(rva2, rva);
        r3 = find_o(&S, ord, &rva3, NULL, 0);
        CHECK_RC(r3, r2);
        CHECK_U64(rva3, rva);
    }
    syn_free(&S);

    memset(&S, 0, sizeof S);
    S.b.file = (u8 *)malloc(b->len);
    memcpy(S.b.file, b->data, b->len);
    S.b.len = b->len;
    CHECK_RC(syn_load(&S), PE_OK);
    CHECK_RC(pe_export_count(&S.info, S.img, &nf, &nn), PE_OK);
    CHECK_U64(nf, 2); CHECK_U64(nn, 2);
    CHECK_RC(find_n(&S, "sub", &rva, NULL, 0), PE_OK);
    CHECK_RC(find_n(&S, "b_id", &rva, NULL, 0), PE_OK);
    syn_free(&S);

    /* an EXE has no exports */
    {
        fix_t *m = fx("dll_main.exe");
        memset(&S, 0, sizeof S);
        S.b.file = (u8 *)malloc(m->len);
        memcpy(S.b.file, m->data, m->len);
        S.b.len = m->len;
        CHECK_RC(syn_load(&S), PE_OK);
        CHECK_U64(S.info.is_dll, 0);
        CHECK_RC(find_n(&S, "start", &rva, NULL, 0), PE_E_NOTFOUND);
        CHECK_RC(pe_export_validate(&S.info, S.img), 1);
        syn_free(&S);
    }
}

/* ================================================================================================================ */
/* Group M: the real module graph, executed                                                                           */
/* ================================================================================================================ */
static int call_proc(int mod, const char *name, unsigned ord, int by_ord, u64 *addr)
{
    return pe_mod_getproc(&C, &OPS, mod, name, ord, by_ord, addr);
}

static void test_real_graph(void)
{
    fix_t *mn = fx("dll_main.exe");
    int m, a, b, i, r;
    u64 ad, ad2, ad3;
    unsigned code;

    printf("[M1] real graph: dll_main.exe -> dll_lib_a.dll -> dll_lib_b.dll (executed through ms_abi)\n");
    host_reset();
    fx_vf("dll_lib_a.dll"); fx_vf("dll_lib_b.dll");
    vf_add("KERNEL32.dll", fx("dll_lib_b.dll")->data, fx("dll_lib_b.dll")->len);      /* decoy: must never be read */
    H.exec = 1;

    m = pe_mod_load_main(&C, &OPS, mn->data, mn->len, "/some/dir/dll_main.exe");
    CHECK_RC(m, 0);
    if (m < 0) { end_host("M1"); return; }
    CHECK_U64(C.has_main, 1);
    CHECK_U64(C.unresolved, 0);
    CHECK_U64(C.link_errors, 0);
    CHECK_STR(C.first_unresolved, "");
    CHECK_U64(mod_count(), 3);
    a = pe_mod_find(&C, "dll_lib_a.dll");
    b = pe_mod_find(&C, "dll_lib_b.dll");
    CHECK_RC(a, 1); CHECK_RC(b, 2);
    CHECK_RC(pe_mod_find(&C, "dll_main.exe"), 0);
    CHECK_RC(pe_mod_find(&C, "DLL_LIB_A.DLL"), 1);               /* case-insensitive */
    CHECK_RC(pe_mod_find(&C, "dll_lib_a"), 1);                   /* with or without .dll */
    CHECK_RC(pe_mod_find(&C, "Dll_Lib_B"), 2);
    CHECK_RC(pe_mod_find(&C, "kernel32.dll"), PE_E_NOTFOUND);    /* builtin: no module */
    CHECK_RC(pe_mod_find_base(&C, C.mod[1].base), 1);
    CHECK_RC(pe_mod_find_base(&C, C.mod[1].base + 1), PE_E_NOTFOUND);
    CHECK_STR(C.mod[0].name, "dll_main.exe");
    CHECK_U64(C.mod[0].flags & PE_MOD_F_MAIN, PE_MOD_F_MAIN);
    CHECK_U64(C.mod[1].deps, 1u << 2);                           /* a needs b */
    CHECK_U64(C.mod[0].deps, 1u << 1);                           /* main needs a (KERNEL32 is builtin: no edge) */
    CHECK_U64(C.mod[2].deps, 0);
    CHECK_U64(C.mod[0].refcount, 1); CHECK_U64(C.mod[1].refcount, 1); CHECK_U64(C.mod[2].refcount, 1);
    CHECK_U64(C.mod[1].state, PE_MOD_S_LINKED);

    /* builtin precedence: the decoy KERNEL32.dll in the file system was never even asked for */
    CHECK_U64(read_count("KERNEL32.dll"), 0);
    CHECK_U64(read_count("dll_lib_a.dll"), 1);
    CHECK_U64(read_count("dll_lib_b.dll"), 1);
    CHECK_U64(H.nread, 2);
    CHECK(builtin_probe_count("KERNEL32.dll") >= 2);             /* main and a each probed it; stopped after the verdict */
    CHECK_U64(H.nev, 0);                                         /* nothing executed yet */

    /* init order: dependencies first (b before a); the exe's entry point is NOT run by the loader */
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_lib_a.dll:1 ");
    CHECK_U64(H.nev, 2); CHECK_U64(H.ev[0], 0xB01); CHECK_U64(H.ev[1], 0xA01);
    CHECK_U64(C.mod[1].init, PE_MOD_I_ATTACHED);
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);                     /* idempotent */
    CHECK_U64(H.nev, 2);

    /* run the program: everything linked and callable (63 = all six probes) */
    code = ((ms_u0_t)(uintptr_t)pe_mod_entry_va(&C, m))();
    CHECK_U64(code, 63);
    CHECK_U64(H.nev, 3); CHECK_U64(H.ev[2], 0xCFF);              /* b attach, a attach, then main */

    /* GetProcAddress semantics, called through ms_abi */
    CHECK_RC(call_proc(a, "add", 0, 0, &ad), 0);
    CHECK(ad >= C.mod[1].base && ad < C.mod[1].base + C.mod[1].size);
    CHECK_U64(((ms_i2_t)(uintptr_t)ad)(20, 22), 42);
    CHECK_RC(call_proc(a, "mul", 0, 0, &ad), 0);
    CHECK_U64(((ms_i2_t)(uintptr_t)ad)(6, 7), 42);
    CHECK_RC(call_proc(a, "ordfn", 0, 0, &ad), PE_E_NOTFOUND);   /* NONAME: not reachable by name */
    CHECK_U64(ad, 0);
    CHECK_RC(call_proc(a, NULL, 7, 1, &ad), 0);
    CHECK_U64(((ms_i1_t)(uintptr_t)ad)(5), 16);                  /* ordinal-only export */
    CHECK_RC(call_proc(a, NULL, 0, 1, &ad), PE_E_NOTFOUND);
    CHECK_RC(call_proc(a, NULL, 8, 1, &ad), PE_E_NOTFOUND);
    CHECK_RC(call_proc(a, "ADD", 0, 0, &ad), PE_E_NOTFOUND);     /* export names are case-sensitive */
    CHECK_RC(call_proc(a, "add\x01", 0, 0, &ad), PE_E_NOTFOUND);
    CHECK_RC(call_proc(a, "", 0, 0, &ad), PE_E_NOTFOUND);
    CHECK_RC(call_proc(a, "sub", 0, 0, &ad), PE_E_NOTFOUND);     /* not an export of a */
    CHECK_RC(call_proc(a, "fwd_add", 0, 0, &ad), 0);             /* forwarder a.fwd_add -> b.sub */
    CHECK_RC(call_proc(b, "sub", 0, 0, &ad2), 0);
    CHECK_U64(ad, ad2);
    CHECK_U64(((ms_i2_t)(uintptr_t)ad)(10, 3), 7);
    CHECK(ad >= C.mod[2].base && ad < C.mod[2].base + C.mod[2].size);
    CHECK_RC(call_proc(a, "fwd_k32", 0, 0, &ad3), 0);            /* forwarder into the builtin KERNEL32 */
    CHECK_U64(ad3, ADDR(h_SetLastError));
    for (i = 1; i <= 7; i++) {                                   /* every public ordinal resolves, 8 does not */
        r = call_proc(a, NULL, (unsigned)i, 1, &ad);
        CHECK_RC(r, 0);
    }
    CHECK_RC(call_proc(a, NULL, 8, 1, &ad), PE_E_NOTFOUND);
    CHECK_RC(call_proc(b, "b_id", 0, 0, &ad), 0);
    CHECK_U64(((ms_i0_t)(uintptr_t)ad)(), 0xB);
    /* the DIR64 relocation was applied at the actual base: get_via_ptr dereferences a pointer stored in .data */
    CHECK_RC(call_proc(a, "get_via_ptr", 0, 0, &ad), 0);
    CHECK(C.mod[1].base != C.mod[1].info.image_base);            /* mmap(NULL) is never the 0x1xxxxxxxx preferred base */
    CHECK_U64(((ms_i0_t)(uintptr_t)ad)(), 77);
    /* argument checks */
    CHECK_RC(call_proc(-1, "add", 0, 0, &ad), PE_MOD_E_ARG);
    CHECK_RC(call_proc(31, "add", 0, 0, &ad), PE_MOD_E_ARG);
    CHECK_RC(call_proc(32, "add", 0, 0, &ad), PE_MOD_E_ARG);
    CHECK_RC(call_proc(a, NULL, 0, 0, &ad), PE_MOD_E_ARG);

    /* LoadLibrary / GetModuleHandle refcount semantics on already-loaded modules */
    CHECK_RC(pe_mod_load(&C, &OPS, "DLL_LIB_A.DLL"), 1);
    CHECK_U64(C.mod[1].refcount, 2);
    CHECK_RC(pe_mod_load(&C, &OPS, "dll_lib_a"), 1);
    CHECK_U64(C.mod[1].refcount, 3);
    CHECK_U64(H.nread, 2);                                       /* no re-read */
    CHECK_RC(pe_mod_free(&C, &OPS, 1), 0);
    CHECK_RC(pe_mod_free(&C, &OPS, 1), 0);
    CHECK_U64(C.mod[1].refcount, 1);
    CHECK_U64(mod_count(), 3);                                   /* still loaded: main holds it */
    CHECK_RC(pe_mod_free(&C, &OPS, 1), PE_MOD_E_ARG);            /* no explicit load left: an extra FreeLibrary is refused ... */
    CHECK_RC(pe_mod_free(&C, &OPS, 2), PE_MOD_E_ARG);            /* ... for dependencies too */
    CHECK_U64(mod_count(), 3);
    CHECK_U64(C.mod[1].refcount, 1); CHECK_U64(C.mod[2].refcount, 1);
    CHECK_RC(pe_mod_load(&C, &OPS, "nosuch.dll"), PE_MOD_E_NOFILE);
    CHECK_U64(mod_count(), 3);

    /* detach: reverse of attach order; idempotent */
    CHECK_RC(pe_mod_run_detach(&C, &OPS), 0);
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_lib_a.dll:1 dll_lib_a.dll:0 dll_lib_b.dll:0 ");
    CHECK_U64(H.nev, 5); CHECK_U64(H.ev[3], 0xA00); CHECK_U64(H.ev[4], 0xB00);
    CHECK_RC(pe_mod_run_detach(&C, &OPS), 0);
    CHECK_U64(H.nev, 5);
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);                     /* detached modules are not re-attached */
    CHECK_U64(H.nev, 5);
    end_host("M1");
    CHECK_U64(mod_count(), 0);
    CHECK_U64(C.has_main, 0);
}

static void test_loadlibrary_free(void)
{
    int a, b;

    printf("[M2] LoadLibrary/FreeLibrary without an exe: attach order, refcounted unload, slot reuse\n");
    host_reset();
    fx_vf("dll_lib_a.dll"); fx_vf("dll_lib_b.dll");
    H.exec = 1;
    a = pe_mod_load(&C, &OPS, "dll_lib_a");                      /* extension-less name works (".dll" is appended) */
    CHECK_RC(a, 0);
    CHECK_STR(C.mod[0].name, "dll_lib_a.dll");
    CHECK_RC(pe_mod_find(&C, "DLL_LIB_A.DLL"), 0);
    b = pe_mod_find(&C, "dll_lib_b.dll");
    CHECK_RC(b, 1);
    CHECK_U64(C.mod[0].refcount, 1);                             /* one public load */
    CHECK_U64(C.mod[1].refcount, 1);                             /* held by a */
    CHECK_U64(H.nev, 0);                                         /* LoadLibrary alone runs no DllMain */
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_lib_a.dll:1 ");
    CHECK_RC(pe_mod_load(&C, &OPS, "dll_lib_b.dll"), 1);         /* explicit load of the dependency: refcount 2 */
    CHECK_U64(C.mod[1].refcount, 2);
    CHECK_RC(pe_mod_free(&C, &OPS, 0), 0);                       /* a: refcount 0 -> detach a, release b (2 -> 1) */
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_lib_a.dll:1 dll_lib_a.dll:0 ");
    CHECK_U64(mod_count(), 1);
    CHECK_RC(pe_mod_find(&C, "dll_lib_a.dll"), PE_E_NOTFOUND);
    CHECK_U64(H.free_calls, 1);
    CHECK_RC(pe_mod_free(&C, &OPS, 1), 0);                       /* b: 1 -> 0 -> detach, unload */
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_lib_a.dll:1 dll_lib_a.dll:0 dll_lib_b.dll:0 ");
    CHECK_U64(mod_count(), 0);
    CHECK_U64(H.free_calls, 2);
    CHECK_RC(pe_mod_free(&C, &OPS, 1), PE_MOD_E_ARG);            /* already gone */
    CHECK_U64(H.nev, 4);
    CHECK_U64(H.ev[0], 0xB01); CHECK_U64(H.ev[1], 0xA01); CHECK_U64(H.ev[2], 0xA00); CHECK_U64(H.ev[3], 0xB00);
    /* load again: slots are reusable and everything works once more */
    H.nev = 0; H.calls[0] = 0;
    a = pe_mod_load(&C, &OPS, "dll_lib_a.dll");
    CHECK_RC(a, 0);
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_lib_a.dll:1 ");
    {
        u64 ad;
        CHECK_RC(call_proc(a, "a_sub_twice", 0, 0, &ad), 0);
        CHECK_U64(((ms_i2_t)(uintptr_t)ad)(10, 2), 6);           /* a's IAT into b works after a reload */
    }
    pe_mod_free_all(&C, &OPS);                                   /* still attached: free_all detaches, newest first */
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_lib_a.dll:1 dll_lib_a.dll:0 dll_lib_b.dll:0 ");
    end_host("M2");
}

static void test_bad_dllmain(void)
{
    int m, r;

    printf("[M3] DllMain returns FALSE: abort, roll back the attached ones in reverse, no DETACH for the failing DLL\n");
    host_reset();
    fx_vf("dll_bad.dll"); fx_vf("dll_lib_b.dll");
    H.exec = 1;
    m = pe_mod_load_main(&C, &OPS, fx("dll_main_bad.exe")->data, fx("dll_main_bad.exe")->len, "dll_main_bad.exe");
    CHECK_RC(m, 0);
    if (m < 0) { end_host("M3"); return; }
    CHECK_U64(C.unresolved, 0);
    CHECK_U64(mod_count(), 3);
    CHECK_RC(pe_mod_find(&C, "dll_bad.dll"), 1);
    CHECK_RC(pe_mod_find(&C, "dll_lib_b.dll"), 2);
    r = pe_mod_run_inits(&C, &OPS);
    CHECK_RC(r, PE_MOD_E_INIT);
    CHECK_U64(C.failed_module, 1);
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_bad.dll:1 dll_lib_b.dll:0 ");    /* b attached, bad refused, b rolled back */
    CHECK_U64(H.nev, 3); CHECK_U64(H.ev[0], 0xB01); CHECK_U64(H.ev[1], 0xBD01); CHECK_U64(H.ev[2], 0xB00);
    CHECK_U64(C.mod[1].init, PE_MOD_I_NONE);
    CHECK_U64(C.mod[2].init, PE_MOD_I_NONE);
    CHECK_U64(C.mod[0].init, PE_MOD_I_NONE);
    CHECK_RC(pe_mod_run_detach(&C, &OPS), 0);                    /* nothing attached: no stray DETACH */
    CHECK_U64(H.nev, 3);
    /* FreeLibrary of the failed DLL's top reference unloads it and its dependency without any DllMain calls */
    CHECK_RC(pe_mod_free(&C, &OPS, 0), 0);
    CHECK_U64(mod_count(), 0);
    CHECK_U64(H.nev, 3);
    end_host("M3");

    printf("[M3b] a failing DllMain behind a good DLL, reached through LoadLibrary\n");
    host_reset();
    fx_vf("dll_bad.dll"); fx_vf("dll_lib_b.dll");
    H.exec = 0;
    snprintf(H.fail_name, sizeof H.fail_name, "dll_bad.dll");
    CHECK_RC(pe_mod_load(&C, &OPS, "dll_lib_b.dll"), 0);
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    CHECK_STR(H.calls, "dll_lib_b.dll:1 ");
    CHECK_RC(pe_mod_load(&C, &OPS, "dll_bad.dll"), 1);
    r = pe_mod_run_inits(&C, &OPS);
    CHECK_RC(r, PE_MOD_E_INIT);
    CHECK_U64(C.failed_module, 1);
    CHECK_STR(H.calls, "dll_lib_b.dll:1 dll_bad.dll:1 ");        /* the previously attached b is NOT rolled back */
    CHECK_U64(C.mod[0].init, PE_MOD_I_ATTACHED);
    CHECK_RC(pe_mod_free(&C, &OPS, 1), 0);                       /* caller cleans up: bad gone, b keeps its public ref */
    CHECK_U64(mod_count(), 1);
    CHECK_U64(C.mod[0].refcount, 1);
    end_host("M3b");
}

static void test_cycle(void)
{
    int m;
    u64 ad;
    unsigned code = 0;
    int x, y;

    printf("[M4] import cycle x <-> y terminates; both link by address; y initialises before x\n");
    host_reset();
    fx_vf("dll_cyc_x.dll"); fx_vf("dll_cyc_y.dll");
    H.exec = 1;
    m = pe_mod_load_main(&C, &OPS, fx("dll_main_cyc.exe")->data, fx("dll_main_cyc.exe")->len, "dll_main_cyc.exe");
    CHECK_RC(m, 0);
    if (m < 0) { end_host("M4"); return; }
    CHECK_U64(C.unresolved, 0);
    CHECK_U64(mod_count(), 3);
    x = pe_mod_find(&C, "dll_cyc_x.dll");
    y = pe_mod_find(&C, "dll_cyc_y.dll");
    CHECK(x > 0 && y > 0 && x != y);
    CHECK_U64(read_count("dll_cyc_x.dll"), 1);
    CHECK_U64(read_count("dll_cyc_y.dll"), 1);                   /* each loaded exactly once */
    CHECK_U64(C.mod[x].deps, 1u << y);
    CHECK_U64(C.mod[y].deps, 1u << x);
    CHECK_U64(C.mod[x].refcount, 2); CHECK_U64(C.mod[y].refcount, 2);      /* main + the other cycle member */
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    {
        /* DFS starts from the exe and visits the lower index first, so the member reached SECOND is initialised first */
        int first = x < y ? y : x, second = x < y ? x : y;
        char want[160];
        snprintf(want, sizeof want, "%s:1 %s:1 ", C.mod[first].name, C.mod[second].name);
        CHECK_STR(H.calls, want);
        code = ((ms_u0_t)(uintptr_t)pe_mod_entry_va(&C, m))();
        CHECK_U64(code, 3);                                      /* x_calls_y()==102 and y_calls_x()==201 */
        CHECK_RC(call_proc(x, "x_val", 0, 0, &ad), 0);
        CHECK_U64(((ms_i0_t)(uintptr_t)ad)(), 1);
        CHECK_RC(pe_mod_run_detach(&C, &OPS), 0);
        snprintf(want, sizeof want, "%s:1 %s:1 %s:0 %s:0 ", C.mod[first].name, C.mod[second].name,
                 C.mod[second].name, C.mod[first].name);
        CHECK_STR(H.calls, want);
    }
    /* a cycle is released only by pe_mod_free_all (refcounts never reach zero): documented */
    end_host("M4");
}

static void test_missing_dll(void)
{
    int m;
    const pe_mod_t *M;
    unsigned code;
    u64 slot;

    printf("[M5] missing DLL: unresolved=1, first unresolved named, loud stub, no crash\n");
    host_reset();
    H.use_stub = 1;
    OPS.unresolved_stub = h_ustub;
    m = pe_mod_load_main(&C, &OPS, fx("dll_main_missing.exe")->data, fx("dll_main_missing.exe")->len, "dll_main_missing.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.unresolved, 1);
    CHECK_STR(C.first_unresolved, "nosuch.dll!foo");
    if (m < 0) { end_host("M5"); return; }
    CHECK_RC(C.first_unresolved_module, 0);
    CHECK_RC(C.first_unresolved_why, PE_MOD_E_NOFILE);              /* ... because the DLL file is missing */
    CHECK_U64(C.mod[0].unresolved, 1);
    CHECK_U64(mod_count(), 1);
    CHECK_U64(read_count("nosuch.dll"), 1);                      /* tried once, not once per thunk */
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    code = ((ms_u0_t)(uintptr_t)pe_mod_entry_va(&C, m))();
    CHECK_U64(code, 0xBAD);                                      /* foo() hit the loud stub */
    CHECK_U64(H.stub_calls, 1);
    end_host("M5");

    host_reset();                                                /* no stub callback: the slot stays 0 */
    m = pe_mod_load_main(&C, &OPS, fx("dll_main_missing.exe")->data, fx("dll_main_missing.exe")->len, "dll_main_missing.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.unresolved, 1);
    M = pe_mod_get(&C, m);
    CHECK(M != NULL);
    if (M) {
        u32 ft = bl_g32(M->image, M->info.import_rva + 16);
        slot = (u64)bl_g32(M->image, ft) | ((u64)bl_g32(M->image, ft + 4) << 32);
        CHECK_U64(slot, 0);
    }
    end_host("M5b");
}

/* ================================================================================================================ */
/* Group S: synthetic graphs                                                                                          */
/* ================================================================================================================ */
#define SB_MAX 96
static bout_t SB[SB_MAX];
static int nSB;
static void sb_reset(void) { int i; for (i = 0; i < nSB; i++) bl_free(&SB[i]); nSB = 0; }

/* builds a synthetic image and registers it in the host's virtual file system under `file_name` */
static bout_t *sb_add(const char *file_name, const bspec_t *spec)
{
    bout_t *o = &SB[nSB];
    if (nSB >= SB_MAX) { printf("FATAL: SB full\n"); exit(2); }
    if (bl_dll(spec, o) != 0) { printf("FATAL: builder failed for %s\n", file_name); exit(2); }
    nSB++;
    if (file_name) vf_add(file_name, o->file, o->len);
    return o;
}

/* leaf DLL: exports `f` (stub returning val) */
static bout_t *sb_leaf(const char *file, const char *fname, u32 val, u64 image_base)
{
    static bfunc_t F[BL_MAXF];
    static bname_t N[BL_MAXF];
    static int k;
    bspec_t s;
    F[k % BL_MAXF].val = val; F[k % BL_MAXF].fwd = NULL;
    N[k % BL_MAXF].name = fname; N[k % BL_MAXF].index = 0;
    memset(&s, 0, sizeof s);
    s.nfuncs = 1; s.funcs = &F[k % BL_MAXF]; s.nnames = 1; s.names = &N[k % BL_MAXF];
    s.has_entry = 1; s.image_base = image_base;
    k++;
    return sb_add(file, &s);
}

/* a DLL (or exe when is_exe) that imports `f` from `target` and, unless is_exe, also exports `f` */
static bout_t *sb_importer(const char *file, const char *target, int is_exe, u32 val)
{
    static bfunc_t F[BL_MAXF];
    static bname_t N[BL_MAXF];
    static bimp_t I[BL_MAXF];
    static const char *const names[1] = { "f" };
    static int k;
    bspec_t s;
    int kk = k++ % BL_MAXF;
    F[kk].val = val; F[kk].fwd = NULL;
    N[kk].name = "f"; N[kk].index = 0;
    I[kk].dll = target; I[kk].n = 1; I[kk].names = names;
    memset(&s, 0, sizeof s);
    s.has_entry = 1;
    s.nimps = 1; s.imps = &I[kk];
    if (is_exe) { s.is_exe = 1; s.no_exports = 1; }
    else { s.nfuncs = 1; s.funcs = &F[kk]; s.nnames = 1; s.names = &N[kk]; }
    return sb_add(file, &s);
}

static u64 iat_slot(int mod, int desc, int thunk)
{
    const pe_mod_t *M = pe_mod_get(&C, mod);
    u32 ft;
    if (!M) return ~0ull;
    ft = bl_g32(M->image, M->info.import_rva + 20u * (u32)desc + 16u);
    return (u64)bl_g32(M->image, ft + 8u * (u32)thunk) | ((u64)bl_g32(M->image, ft + 8u * (u32)thunk + 4) << 32);
}

/* chain00.dll .. chain(n-1).dll: chain(i) imports f from chain(i+1) and exports f; the last one is a leaf.
 * Returns the exe that imports f from chain00 (not registered in the vfs; the caller loads it as the main module). */
static bout_t *build_chain(int n)
{
    static char nm[32][24];
    int i;
    for (i = 0; i < n; i++) snprintf(nm[i], sizeof nm[i], "chain%02d.dll", i);
    for (i = n - 1; i >= 0; i--) {
        if (i == n - 1) sb_leaf(nm[i], "f", 0x1000u + (u32)i, 0);
        else sb_importer(nm[i], nm[i + 1], 0, 0x1000u + (u32)i);
    }
    return sb_importer(NULL, nm[0], 1, 0);
}

static void test_depth_and_cap(void)
{
    int i, m;
    bout_t *mb;
    u64 fa;

    printf("[S1] import depth cap: a 20-deep chain stops at depth %u, cleanly\n", PE_MOD_MAX_DEPTH);
    host_reset(); sb_reset();
    mb = build_chain(20);
    m = pe_mod_load_main(&C, &OPS, mb->file, mb->len, "chainmain.exe");
    CHECK_RC(m, 0);
    CHECK_U64(mod_count(), 1 + PE_MOD_MAX_DEPTH);                /* the exe (depth 0) + chain00..chain15 (depth 1..16) */
    CHECK_U64(C.unresolved, 1);
    CHECK_STR(C.first_unresolved, "chain16.dll!f");              /* chain15's import of chain16 is what failed */
    CHECK_RC(C.first_unresolved_why, PE_MOD_E_DEPTH);
    CHECK_U64(read_count("chain15.dll"), 1);
    CHECK_U64(read_count("chain16.dll"), 0);                     /* depth cap hit BEFORE any file access */
    CHECK_RC(pe_mod_find(&C, "chain15.dll"), 16);
    CHECK_U64(C.mod[16].unresolved, 1);
    CHECK_U64(iat_slot(16, 0, 0), 0);
    CHECK_U64(iat_slot(15, 0, 0) != 0, 1);                       /* every link below the cap is real */
    CHECK_RC(call_proc(1, "f", 0, 0, &fa), 0);
    CHECK(pe_mod_load(&C, &OPS, "chain17.dll") >= 0);          /* the nesting cap is relative to the requester: a fresh LoadLibrary starts at depth 1 */
    CHECK_U64(mod_count(), 1 + PE_MOD_MAX_DEPTH + 3);            /* chain17 + its dependencies chain18, chain19 */
    CHECK_U64(C.unresolved, 1);
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    end_host("S1a");
    sb_reset();

    host_reset();                                                /* a chain of exactly PE_MOD_MAX_DEPTH DLLs resolves fully */
    mb = build_chain(16);
    m = pe_mod_load_main(&C, &OPS, mb->file, mb->len, "chainmain.exe");
    CHECK_RC(m, 0);
    CHECK_U64(mod_count(), 17);
    CHECK_U64(C.unresolved, 0);
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    {
        /* init order down a chain is deepest first: chain15 ... chain00 */
        char want[512] = "";
        for (i = 15; i >= 0; i--) { char t[40]; snprintf(t, sizeof t, "chain%02d.dll:1 ", i); strcat(want, t); }
        CHECK_STR(H.calls, want);
    }
    CHECK_RC(pe_mod_run_detach(&C, &OPS), 0);
    {
        char want[1024] = "";
        for (i = 15; i >= 0; i--) { char t[40]; snprintf(t, sizeof t, "chain%02d.dll:1 ", i); strcat(want, t); }
        for (i = 0; i < 16; i++) { char t[40]; snprintf(t, sizeof t, "chain%02d.dll:0 ", i); strcat(want, t); }
        CHECK_STR(H.calls, want);                                /* detach is the exact reverse */
    }
    end_host("S1b");
    sb_reset();

    printf("[S2] module table cap: %u modules max, the rest unresolved, nothing breaks\n", PE_MOD_MAX_MODULES);
    host_reset();
    {
        static const char *const fn1[1] = { "f" };
        static bimp_t I[40];
        static char names[40][24];
        bspec_t s;
        int total_unres;
        for (i = 0; i < 40; i++) {
            snprintf(names[i], sizeof names[i], "leaf%02d.dll", i);
            sb_leaf(names[i], "f", 0x4000u + (u32)i, 0);
            I[i].dll = names[i]; I[i].n = 1; I[i].names = fn1;
        }
        memset(&s, 0, sizeof s);
        s.is_exe = 1; s.no_exports = 1; s.nimps = 40; s.imps = I;
        mb = sb_add(NULL, &s);
        m = pe_mod_load_main(&C, &OPS, mb->file, mb->len, "bigmain.exe");
        CHECK_RC(m, 0);
        CHECK_U64(mod_count(), PE_MOD_MAX_MODULES);
        total_unres = 40 - ((int)PE_MOD_MAX_MODULES - 1);
        CHECK_U64(C.unresolved, total_unres);
        CHECK_STR(C.first_unresolved, "leaf31.dll!f");
        CHECK_RC(C.first_unresolved_why, PE_MOD_E_FULL);
        CHECK_U64(iat_slot(0, 31, 0), 0);
        CHECK_U64(iat_slot(0, 30, 0) != 0, 1);
        CHECK_RC(pe_mod_load(&C, &OPS, "leaf39.dll"), PE_MOD_E_FULL);
        CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
        end_host("S2");
    }
    sb_reset();
}

static void test_hostile_names(void)
{
    static const char *const fn1[1] = { "f" };
    static bimp_t I[1];
    char n63[80], n64[80], n200[260];
    size_t i;
    struct { const char *what; const char *dll; } T[] = {
        { "path separator \\",       "a\b.dll" },
        { "path separator /",        "../x.dll" },
        { "absolute path",           "/etc/passwd" },
        { "drive colon",             "C:x.dll" },
        { "control char",            "evil\x01.dll" },
        { "escape sequence",         "ev\x1b[2Jil.dll" },
        { "DEL",                     "del\x7f.dll" },
        { "high bit",                "\xff\xfe.dll" },
        { "wildcard",                "*.dll" },
        { "question mark",           "a?.dll" },
        { "quote",                   "a\".dll" },
        { "pipe",                    "a|b.dll" },
        { "only dots stem",          "..dll" },
        { "just .dll",               ".dll" },
        { "single dot",              "." },
        { "double dot",              ".." },
    };
    bspec_t s;
    int m;
    const char *const names[1] = { "f" };

    (void)fn1;
    printf("[S3] hostile DLL names: rejected (never read, never truncated into another match)\n");
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        host_reset(); sb_reset();
        sb_leaf("x.dll", "f", 1, 0);
        I[0].dll = T[i].dll; I[0].n = 1; I[0].names = names;
        memset(&s, 0, sizeof s);
        s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
        sb_add(NULL, &s);
        m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
        g_checks += 4;
        if (m != 0) { g_fail++; printf("  FAIL [%s]: load rc %d\n", T[i].what, m); }
        if (C.unresolved != 1) { g_fail++; printf("  FAIL [%s]: unresolved %u\n", T[i].what, C.unresolved); }
        if (C.first_unresolved_why != PE_MOD_E_NAME) { g_fail++; printf("  FAIL [%s]: why %d\n", T[i].what, C.first_unresolved_why); }
        if (H.nread != 0) { g_fail++; printf("  FAIL [%s]: read_file called %d times\n", T[i].what, H.nread); }
        if (mod_count() != 1) { g_fail++; printf("  FAIL [%s]: %d modules\n", T[i].what, mod_count()); }
        {
            int k, bad = 0;
            for (k = 0; C.first_unresolved[k]; k++) if ((unsigned char)C.first_unresolved[k] < 0x20 || (unsigned char)C.first_unresolved[k] >= 0x7f) bad = 1;
            g_checks++;
            if (bad) { g_fail++; printf("  FAIL [%s]: first_unresolved contains non-printable bytes\n", T[i].what); }
        }
        end_host(T[i].what);
        sb_reset();
    }

    /* 63 characters loads, 64 does not, and the 64-char name must not be cut down to the 63-char DLL */
    memset(n63, 'n', 59); strcpy(n63 + 59, ".dll");              /* 63 chars */
    memset(n64, 'n', 60); strcpy(n64 + 60, ".dll");              /* 64 chars: n63 plus one more 'n' */
    memset(n200, 'n', 196); strcpy(n200 + 196, ".dll");          /* 200 chars (pe.c accepts up to 255) */
    host_reset(); sb_reset();
    sb_leaf(n63, "f", 0x63, 0);
    I[0].dll = n63; I[0].n = 1; I[0].names = names;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.unresolved, 0);                                  /* exactly 63 characters: accepted */
    CHECK_U64(mod_count(), 2);
    end_host("n63");
    sb_reset();

    host_reset();
    sb_leaf(n63, "f", 0x63, 0);                                  /* the 63-char DLL exists ... */
    vf_add(n64, SB[nSB - 1].file, SB[nSB - 1].len);              /* ... and even a file for the 64-char name does */
    I[0].dll = n64; I[0].n = 1; I[0].names = names;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.unresolved, 1);                                  /* 64 chars: rejected, not truncated to the 63-char match */
    CHECK_U64(H.nread, 0);
    CHECK_U64(mod_count(), 1);
    CHECK_RC(pe_mod_load(&C, &OPS, n64), PE_MOD_E_NAME);
    CHECK_RC(pe_mod_find(&C, n64), PE_MOD_E_NAME);
    CHECK_RC(pe_mod_load(&C, &OPS, ""), PE_MOD_E_NAME);
    CHECK_RC(pe_mod_load(&C, &OPS, NULL), PE_MOD_E_NAME);
    CHECK_RC(pe_mod_load(&C, &OPS, "a/b.dll"), PE_MOD_E_NAME);
    CHECK_U64(H.nread, 0);
    end_host("n64");
    sb_reset();

    host_reset();
    I[0].dll = n200; I[0].n = 1; I[0].names = names;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.unresolved, 1);
    CHECK_U64(H.nread, 0);
    CHECK(strlen(C.first_unresolved) < PE_MOD_MSG_MAX);
    CHECK(strstr(C.first_unresolved, "...") != NULL);            /* display text cut, visibly */
    end_host("n200");
    sb_reset();

    /* a name without extension gets ".dll"; one with another extension is left alone */
    host_reset();
    sb_leaf("bare.dll", "f", 1, 0);
    vf_add("other.ocx", SB[nSB - 1].file, SB[nSB - 1].len);
    CHECK_RC(pe_mod_load(&C, &OPS, "bare"), 0);
    CHECK_STR(C.mod[0].name, "bare.dll");
    CHECK_RC(pe_mod_load(&C, &OPS, "other.ocx"), 1);
    CHECK_STR(C.mod[1].name, "other.ocx");
    CHECK_RC(pe_mod_load(&C, &OPS, "BARE.DLL"), 0);
    end_host("ext");
    sb_reset();

    /* empty import DLL name => malformed import table => the image is refused outright */
    host_reset();
    I[0].dll = ""; I[0].n = 1; I[0].names = names;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
    CHECK_RC(m, PE_E_IMPORT);
    CHECK_U64(mod_count(), 0);
    CHECK_U64(H.live_images, 0);                                 /* the image allocated for it was released */
    end_host("emptyname");
    sb_reset();
}

static void test_builtin_paths(void)
{
    static const char *const names[4] = { "SetLastError", "Sleep", "NoSuchFunc", "#77" };
    static const char *const fn1[1] = { "f" };
    static bimp_t I[2];
    bspec_t s;
    int m;

    printf("[S4] builtin DLLs: precedence over a same-named file, unimplemented names get the loud stub\n");
    host_reset(); sb_reset();
    sb_leaf("KERNEL32.dll", "SetLastError", 0xDEAD, 0);          /* a real FILE that would win without precedence */
    vf_add("kernel32.dll", SB[nSB - 1].file, SB[nSB - 1].len);
    I[0].dll = "KERNEL32.dll"; I[0].n = 4; I[0].names = names;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(mod_count(), 1);
    CHECK_U64(H.nread, 0);                                       /* neither spelling was ever read */
    CHECK_U64(C.unresolved, 3);                                  /* Sleep, NoSuchFunc, #77 */
    CHECK_STR(C.first_unresolved, "KERNEL32.dll!Sleep");
    CHECK_RC(C.first_unresolved_why, PE_E_NOTFOUND);
    CHECK_U64(iat_slot(0, 0, 0), ADDR(h_SetLastError));
    CHECK_U64(iat_slot(0, 0, 1), ADDR(h_loud_stub));
    CHECK_U64(iat_slot(0, 0, 2), ADDR(h_loud_stub));
    CHECK_U64(iat_slot(0, 0, 3), ADDR(h_loud_stub));
    CHECK_U64(builtin_probe_count("KERNEL32.dll"), 4);           /* once per thunk; a builtin stays builtin */
    end_host("S4a");
    sb_reset();

    /* a name with no extension in the import table is canonicalised to .dll before the builtin sees it */
    host_reset();
    I[0].dll = "testsys"; I[0].n = 1; I[0].names = (const char *const[]){ "Magic" };
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.unresolved, 0);
    CHECK_STR(H.bl_dll[0], "testsys.dll");
    CHECK_U64(iat_slot(0, 0, 0), ADDR(h_magic));
    end_host("S4b");
    sb_reset();

    /* ordinal import from a builtin DLL */
    host_reset();
    I[0].dll = "testsys.dll"; I[0].n = 2; I[0].names = (const char *const[]){ "#5", "#6" };
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.unresolved, 1);
    CHECK_STR(C.first_unresolved, "testsys.dll!#6");
    CHECK_U64(iat_slot(0, 0, 0), ADDR(h_magic));
    end_host("S4c");
    sb_reset();

    /* a non-builtin DLL is probed once, then every thunk goes straight to its export table */
    host_reset();
    {
        static bfunc_t F[3] = { {0x11, 0}, {0x12, 0}, {0x13, 0} };
        static bname_t N[3] = { {"p", 0}, {"q", 1}, {"r", 2} };
        static const char *const pq[3] = { "p", "q", "r" };
        bspec_t d;
        memset(&d, 0, sizeof d); d.nfuncs = 3; d.funcs = F; d.nnames = 3; d.names = N;
        sb_add("lib3.dll", &d);
        I[0].dll = "lib3.dll"; I[0].n = 3; I[0].names = pq;
        (void)fn1;
        memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
        sb_add(NULL, &s);
        m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "m.exe");
        CHECK_RC(m, 0);
        CHECK_U64(C.unresolved, 0);
        CHECK_U64(builtin_probe_count("lib3.dll"), 1);
        CHECK_U64(iat_slot(0, 0, 0), C.mod[1].base + SB[0].stub_rva[0]);
        CHECK_U64(iat_slot(0, 0, 1), C.mod[1].base + SB[0].stub_rva[1]);
        CHECK_U64(iat_slot(0, 0, 2), C.mod[1].base + SB[0].stub_rva[2]);
        end_host("S4d");
    }
    sb_reset();
}

static void test_forwarders(void)
{
    u64 ad, ad2;
    int p, q, h, i;
    char nm[24][24], fw[24][40];

    printf("[S5] forwarder chains: cycle, depth cap, ordinal forwarders, missing / malformed targets, builtin targets\n");
    host_reset(); sb_reset();
    {   /* p.ping -> q.pong ; q.pong -> p.ping : a cycle */
        static bfunc_t FP[1] = { {0, "fq.pong"} }, FQ[1] = { {0, "fp.ping"} };
        static bname_t NP[1] = { {"ping", 0} }, NQ[1] = { {"pong", 0} };
        bspec_t sp;
        memset(&sp, 0, sizeof sp); sp.nfuncs = 1; sp.funcs = FP; sp.nnames = 1; sp.names = NP;
        sb_add("fp.dll", &sp);
        memset(&sp, 0, sizeof sp); sp.nfuncs = 1; sp.funcs = FQ; sp.nnames = 1; sp.names = NQ;
        sb_add("fq.dll", &sp);
        p = pe_mod_load(&C, &OPS, "fp.dll");
        CHECK_RC(p, 0);
        CHECK_RC(call_proc(p, "ping", 0, 0, &ad), PE_MOD_E_FWD_CYCLE);
        CHECK_U64(ad, 0);
        q = pe_mod_find(&C, "fq.dll");
        CHECK_RC(q, 1);                                          /* the target was loaded on demand */
        CHECK_RC(call_proc(q, "pong", 0, 0, &ad), PE_MOD_E_FWD_CYCLE);
        CHECK_U64(C.mod[p].deps & (1u << q), 1u << q);           /* forwarder targets become init-before dependencies */
        end_host("S5a");
    }
    sb_reset();

    host_reset();
    {   /* hop00 -> hop01 -> ... -> hop19 (real) : 19 forwards; the cap is PE_MOD_MAX_FWD_HOPS = 16 */
        static bfunc_t F[20][1];
        static bname_t N[20][1];
        for (i = 0; i < 20; i++) {
            bspec_t s;
            snprintf(nm[i], sizeof nm[i], "hop%02d.dll", i);
            snprintf(fw[i], sizeof fw[i], "hop%02d.f", i + 1);
            memset(&s, 0, sizeof s);
            F[i][0].val = (i == 19) ? 0x7777 : 0;
            F[i][0].fwd = (i == 19) ? NULL : fw[i];
            N[i][0].name = "f"; N[i][0].index = 0;
            s.nfuncs = 1; s.funcs = F[i]; s.nnames = 1; s.names = N[i];
            sb_add(nm[i], &s);
        }
        h = pe_mod_load(&C, &OPS, "hop00.dll");
        CHECK_RC(h, 0);
        CHECK_RC(call_proc(h, "f", 0, 0, &ad), PE_MOD_E_FWD_DEPTH);              /* 19 hops > 16 */
        CHECK_U64(ad, 0);
        p = pe_mod_find(&C, "hop03.dll");
        CHECK(p >= 0);
        CHECK_RC(call_proc(p, "f", 0, 0, &ad), 0);                               /* 16 hops: exactly at the cap */
        CHECK_RC(call_proc(pe_mod_find(&C, "hop04.dll"), "f", 0, 0, &ad2), 0);   /* 15 hops */
        CHECK_U64(ad, ad2);
        CHECK(ad >= C.mod[pe_mod_find(&C, "hop19.dll")].base);
        CHECK_U64(mod_count(), 20);
        end_host("S5b");
    }
    sb_reset();

    host_reset();
    {   /* ordinal forwarders, missing and malformed targets, builtin targets, junk forwarder DLL names */
        static bfunc_t FO[8] = { {0, "fo2.#2"}, {0, "nodll.x"}, {0, "nodots"}, {0, "testsys.Magic"}, {0, "testsys.#5"},
                                 {0, "KERNEL32.SetLastError"}, {0, "KERNEL32.Sleep"}, {0, "a/b.f"} };
        static bname_t NO[8] = { {"viaord", 0}, {"missing", 1}, {"malformed", 2}, {"sysname", 3}, {"sysord", 4},
                                 {"k32", 5}, {"k32stub", 6}, {"junk", 7} };
        static bfunc_t F2[3] = { {0x21, 0}, {0x22, 0}, {0, 0} };
        static bname_t N2[1] = { {"two", 1} };
        bspec_t s;
        u64 base2;
        memset(&s, 0, sizeof s); s.nfuncs = 8; s.funcs = FO; s.nnames = 8; s.names = NO;
        sb_add("fo.dll", &s);
        memset(&s, 0, sizeof s); s.nfuncs = 3; s.funcs = F2; s.nnames = 1; s.names = N2;
        sb_add("fo2.dll", &s);
        p = pe_mod_load(&C, &OPS, "fo.dll");
        CHECK_RC(p, 0);
        CHECK_RC(call_proc(p, "viaord", 0, 0, &ad), 0);          /* fo2 ordinal 2 = index 1 = stub 0x22 */
        q = pe_mod_find(&C, "fo2.dll");
        CHECK_RC(q, 1);
        base2 = C.mod[q].base;
        CHECK_U64(ad, base2 + SB[1].stub_rva[1]);
        CHECK_RC(call_proc(p, "missing", 0, 0, &ad), PE_MOD_E_NOFILE);
        CHECK_RC(call_proc(p, "malformed", 0, 0, &ad), PE_E_FWD);
        CHECK_RC(call_proc(p, "sysname", 0, 0, &ad), 0);
        CHECK_U64(ad, ADDR(h_magic));
        CHECK_RC(call_proc(p, "sysord", 0, 0, &ad), 0);
        CHECK_U64(ad, ADDR(h_magic));
        CHECK_RC(call_proc(p, "k32", 0, 0, &ad), 0);
        CHECK_U64(ad, ADDR(h_SetLastError));
        CHECK_RC(call_proc(p, "k32stub", 0, 0, &ad), 1);         /* builtin DLL, unimplemented: stub address, rc 1 */
        CHECK_U64(ad, ADDR(h_loud_stub));
        CHECK_RC(call_proc(p, "junk", 0, 0, &ad), PE_E_FWD);     /* junk forwarder DLL name is rejected, never read */
        CHECK_U64(read_count("a/b.dll"), 0);
        CHECK_U64(read_count("nodll.dll"), 1);
        CHECK_U64(mod_count(), 2);
        end_host("S5c");
    }
    sb_reset();
}

static void test_delay_modules(void)
{
    static const char *const N1[2] = { "dfn", "nodfn" };
    static const char *const N2[1] = { "zzz" };
    static const bimp_t D[2] = { { "dlib.dll", 2, N1 }, { "nolib.dll", 1, N2 } };
    bspec_t s;
    int m;
    const pe_mod_t *M;
    u64 s0, s1, s2;
    u32 d0, d1;

    printf("[S6] delay-load imports through the loader: eager bind; failures are not 'unresolved'; PE_MOD_CF_NO_DELAY\n");
    host_reset(); sb_reset();
    sb_leaf("dlib.dll", "dfn", 0xD0, 0);
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.ndelay = 2; s.delay = D;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "dm.exe");
    CHECK_RC(m, 0);
    M = pe_mod_get(&C, m);
    CHECK(M != NULL);
    d0 = SB[nSB - 1].diat_rva[0]; d1 = SB[nSB - 1].diat_rva[1];
    s0 = (u64)bl_g32(M->image, d0) | ((u64)bl_g32(M->image, d0 + 4) << 32);
    s1 = (u64)bl_g32(M->image, d0 + 8) | ((u64)bl_g32(M->image, d0 + 12) << 32);
    s2 = (u64)bl_g32(M->image, d1) | ((u64)bl_g32(M->image, d1 + 4) << 32);
    CHECK_U64(mod_count(), 2);
    CHECK_U64(C.unresolved, 0);                                  /* delay imports never count as unresolved */
    CHECK_U64(C.delay_unresolved, 2);                            /* nodfn, zzz */
    CHECK_U64(s0, C.mod[1].base + SB[0].stub_rva[0]);
    CHECK_U64(s1, M->base + SB[nSB - 1].text_rva);               /* untouched thunk, relocated with the image */
    CHECK_U64(s2, M->base + SB[nSB - 1].text_rva);
    CHECK_U64(M->deps, 1u << 1);
    CHECK_U64(read_count("nolib.dll"), 1);
    end_host("S6a");
    sb_reset();

    host_reset();
    C.flags = PE_MOD_CF_NO_DELAY;
    sb_leaf("dlib.dll", "dfn", 0xD0, 0);
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.ndelay = 2; s.delay = D;
    sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, SB[nSB - 1].file, SB[nSB - 1].len, "dm.exe");
    CHECK_RC(m, 0);
    CHECK_U64(mod_count(), 1);
    CHECK_U64(H.nread, 0);
    CHECK_U64(C.delay_unresolved, 0);
    end_host("S6b");
    sb_reset();
}

#define HI_BASE1 0x200000000000ull       /* inside ASan's HighMem: a fixed mmap there can succeed even under the sanitizer */
#define HI_BASE2 0x200100000000ull
#define HI_BASE3 0x200200000000ull

static void test_preferred_base(void)
{
    static bimp_t II[3];
    static const char *const nn3[1] = { "g" };
    bspec_t s;
    bout_t *mb, *nr;
    int m;

    printf("[S7] preferred base is a hint: used when free, relocation applied for the actual base otherwise\n");
    host_reset(); sb_reset();
    H.try_preferred = 1;
    sb_leaf("pref1.dll", "g", 0x61, HI_BASE1);
    sb_leaf("pref2.dll", "g", 0x62, HI_BASE1);                   /* same preferred base: the second must move */
    sb_leaf("pref3.dll", "g", 0x63, HI_BASE2);
    II[0].dll = "pref1.dll"; II[0].n = 1; II[0].names = nn3;
    II[1].dll = "pref2.dll"; II[1].n = 1; II[1].names = nn3;
    II[2].dll = "pref3.dll"; II[2].n = 1; II[2].names = nn3;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 3; s.imps = II;
    mb = sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, mb->file, mb->len, "pm.exe");
    CHECK_RC(m, 0);
    if (m >= 0) {
        CHECK_U64(C.unresolved, 0);
        printf("      (preferred-base mmap honoured %d of 4 time(s))\n", H.pref_hits);
        CHECK(H.pref_hits >= 2);                                 /* pref1 and pref3 at least (pref2 collides with pref1) */
        CHECK_U64(C.mod[1].base, HI_BASE1);                      /* delta == 0: nothing to relocate */
        CHECK(C.mod[2].base != HI_BASE1);                        /* never two images at one address */
        CHECK_U64(C.mod[3].base, HI_BASE2);
        CHECK_U64(iat_slot(0, 0, 0), C.mod[1].base + SB[0].stub_rva[0]);   /* correct for WHATEVER base each got */
        CHECK_U64(iat_slot(0, 1, 0), C.mod[2].base + SB[1].stub_rva[0]);
        CHECK_U64(iat_slot(0, 2, 0), C.mod[3].base + SB[2].stub_rva[0]);
    }
    end_host("S7");
    sb_reset();

    /* an image WITHOUT a relocation directory can only be loaded at exactly its preferred base */
    host_reset();
    H.try_preferred = 1;
    memset(&s, 0, sizeof s);
    s.image_base = HI_BASE3; s.has_entry = 1;
    nr = sb_add("norel.dll", &s);
    bl_p32(nr->file, 0x98 + 112 + 8 * 5, 0);                     /* DataDirectory[5] (base relocs) := empty */
    bl_p32(nr->file, 0x98 + 112 + 8 * 5 + 4, 0);
    CHECK_RC(pe_mod_load(&C, &OPS, "norel.dll"), 0);             /* preferred base free: delta 0, no table needed */
    CHECK_U64(C.mod[0].base, HI_BASE3);
    end_host("S7b");
    host_reset();
    H.try_preferred = 0;                                         /* alloc ignores the hint: cannot rebase => refuse */
    vf_add("norel.dll", nr->file, nr->len);
    CHECK_RC(pe_mod_load(&C, &OPS, "norel.dll"), PE_E_RELOC);
    CHECK_U64(mod_count(), 0);
    CHECK_U64(H.live_images, 0);                                 /* the image that could not be relocated was released */
    end_host("S7c");
    sb_reset();
}

static void test_reentrancy(void)
{
    printf("[S9] LoadLibrary + pe_mod_run_inits from inside a DllMain: no double attach, order preserved\n");
    host_reset(); sb_reset();
    sb_leaf("re_a.dll", "f", 1, 0);
    sb_leaf("re_b.dll", "f", 2, 0);
    CHECK_RC(pe_mod_load(&C, &OPS, "re_a.dll"), 0);
    snprintf(H.reenter_on, sizeof H.reenter_on, "re_a.dll");
    snprintf(H.reenter_load, sizeof H.reenter_load, "re_b.dll");
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    CHECK_RC(H.reenter_idx, 1);
    CHECK_RC(H.reenter_rc, 0);
    CHECK_STR(H.calls, "re_a.dll:1 re_b.dll:1 ");                /* re_a once (not re-entered), then the nested attach */
    CHECK_U64(C.mod[0].init, PE_MOD_I_ATTACHED);
    CHECK_U64(C.mod[1].init, PE_MOD_I_ATTACHED);
    CHECK_U64(C.mod[1].attach_seq < C.mod[0].attach_seq, 1);     /* the nested one finished first */
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);                     /* idempotent */
    CHECK_STR(H.calls, "re_a.dll:1 re_b.dll:1 ");
    CHECK_RC(pe_mod_run_detach(&C, &OPS), 0);
    CHECK_STR(H.calls, "re_a.dll:1 re_b.dll:1 re_a.dll:0 re_b.dll:0 ");
    end_host("S9");
    sb_reset();
}

static void test_dependency_failures(void)
{
    static const char *const nf[1] = { "f" };
    static const char *const nnf[1] = { "nofunc" };
    static bimp_t I[1];
    static u8 garbage[100];
    struct { const char *what; int why; const char *func; } T[] = {
        { "not a PE at all",                  PE_E_MAGIC,         "f" },
        { "truncated PE",                     PE_E_TRUNC,         "f" },
        { "an exe where a DLL is expected",   PE_MOD_E_KIND,      "f" },
        { "malformed export table",           PE_E_EXPORT,        "f" },
        { "missing export in a good DLL",     PE_E_NOTFOUND,      "nofunc" },
        { "missing DLL file",                 PE_MOD_E_NOFILE,    "f" },
    };
    size_t i;

    printf("[S10] dependency failures are reported with their reason, cleanly, with every buffer released\n");
    memset(garbage, 'A', sizeof garbage);
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        bspec_t s;
        bout_t *mb, *d;
        int m;
        host_reset(); sb_reset();
        switch (i) {
        case 0: vf_add("dep.dll", garbage, sizeof garbage); break;
        case 1: d = sb_leaf(NULL, "f", 1, 0); vf_add("dep.dll", d->file, 90); break;
        case 2: memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; d = sb_add("dep.dll", &s); (void)d; break;
        case 3: {
            static bfunc_t F[1] = { {0x71, 0} };
            static bname_t N[1] = { {"f", 0} };
            memset(&s, 0, sizeof s); s.nfuncs = 1; s.funcs = F; s.nnames = 1; s.names = N;
            d = sb_add("dep.dll", &s);
            bl_p16(d->file, bl_off(d, d->ords_rva), 9);
            break;
        }
        case 4: sb_leaf("dep.dll", "f", 1, 0); break;
        default: break;
        }
        I[0].dll = "dep.dll"; I[0].n = 1; I[0].names = (T[i].func[0] == 'n') ? nnf : nf;
        memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
        mb = sb_add(NULL, &s);
        m = pe_mod_load_main(&C, &OPS, mb->file, mb->len, "m.exe");
        g_checks += 5;
        if (m != 0) { g_fail++; printf("  FAIL [%s]: load rc %d\n", T[i].what, m); }
        if (C.unresolved != 1) { g_fail++; printf("  FAIL [%s]: unresolved %u\n", T[i].what, C.unresolved); }
        if (C.first_unresolved_why != T[i].why) { g_fail++; printf("  FAIL [%s]: why %d (%s), expected %d (%s)\n", T[i].what, C.first_unresolved_why, pe_mod_strerror(C.first_unresolved_why), T[i].why, pe_mod_strerror(T[i].why)); }
        if (mod_count() != (i == 4 ? 2 : 1)) { g_fail++; printf("  FAIL [%s]: %d modules\n", T[i].what, mod_count()); }
        if (H.live_images != (i == 4 ? 2 : 1) || H.live_files != 0) { g_fail++; printf("  FAIL [%s]: live images %d files %d\n", T[i].what, H.live_images, H.live_files); }
        end_host(T[i].what);
        sb_reset();
    }
}

/* an import whose export FORWARDS into a builtin DLL's unimplemented function: stub in the IAT, counted unresolved */
static void test_forward_import_to_builtin(void)
{
    static bfunc_t FO[2] = { {0, "KERNEL32.Sleep"}, {0, "KERNEL32.SetLastError"} };
    static bname_t NO[2] = { {"k32stub", 0}, {"k32ok", 1} };
    static const char *const names[2] = { "k32stub", "k32ok" };
    static bimp_t I[1];
    bspec_t s;
    bout_t *mb;
    int m;

    printf("[S11] import forwarded into a builtin: the stub address lands in the IAT, the import counts as unresolved\n");
    host_reset(); sb_reset();
    memset(&s, 0, sizeof s); s.nfuncs = 2; s.funcs = FO; s.nnames = 2; s.names = NO;
    sb_add("fo.dll", &s);
    I[0].dll = "fo.dll"; I[0].n = 2; I[0].names = names;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
    mb = sb_add(NULL, &s);
    m = pe_mod_load_main(&C, &OPS, mb->file, mb->len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.unresolved, 1);
    CHECK_STR(C.first_unresolved, "fo.dll!k32stub");
    CHECK_RC(C.first_unresolved_why, PE_E_NOTFOUND);
    CHECK_U64(iat_slot(0, 0, 0), ADDR(h_loud_stub));
    CHECK_U64(iat_slot(0, 0, 1), ADDR(h_SetLastError));
    end_host("S11");
    sb_reset();
}

/* an import / delay-import table whose IAT overlaps another descriptor's lookup table: the dry run passes, the link
 * itself turns the table malformed -- flagged, counted, never a crash */
static void test_link_errors(void)
{
    static const char *const nk[1] = { "SetLastError" };
    static const char *const nl[1] = { "p" };
    static const char *const dn0[1] = { "fnA" };
    static const char *const dn1[1] = { "x" };
    static bimp_t I[2];
    static bimp_t D[2];
    bspec_t s;
    bout_t *mb, *lib;
    int m;

    printf("[S12] overlapping IAT / lookup tables: link error is flagged and counted, nothing crashes\n");
    host_reset(); sb_reset();
    {
        static bfunc_t F[1] = { {0x91, 0} };
        static bname_t N[1] = { {"p", 0} };
        memset(&s, 0, sizeof s); s.nfuncs = 1; s.funcs = F; s.nnames = 1; s.names = N;
        lib = sb_add("lib3.dll", &s); (void)lib;
    }
    I[0].dll = "KERNEL32.dll"; I[0].n = 1; I[0].names = nk;
    I[1].dll = "lib3.dll"; I[1].n = 1; I[1].names = nl;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 2; s.imps = I;
    mb = sb_add(NULL, &s);
    bl_p32(mb->file, bl_off(mb, mb->import_rva) + 16, mb->ilt_rva[1]);       /* desc0.FirstThunk := desc1's lookup table */
    m = pe_mod_load_main(&C, &OPS, mb->file, mb->len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.link_errors, 1);
    CHECK_RC(C.last_error, PE_E_IMPORT);
    CHECK_U64(C.mod[0].flags & PE_MOD_F_LINK_ERR, PE_MOD_F_LINK_ERR);
    CHECK_U64(mod_count(), 1);                                    /* desc1 was never reached */
    CHECK_RC(pe_mod_run_inits(&C, &OPS), 0);
    end_host("S12a");
    sb_reset();

    host_reset();
    sb_leaf("dlib.dll", "fnA", 0xD0, 0);
    D[0].dll = "dlib.dll"; D[0].n = 1; D[0].names = dn0;
    D[1].dll = "gone.dll"; D[1].n = 1; D[1].names = dn1;
    memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.ndelay = 2; s.delay = D;
    mb = sb_add(NULL, &s);
    bl_p32(mb->file, bl_off(mb, mb->delay_rva) + 12, mb->dint_rva[1]);       /* desc0.rvaIAT := desc1's INT */
    m = pe_mod_load_main(&C, &OPS, mb->file, mb->len, "m.exe");
    CHECK_RC(m, 0);
    CHECK_U64(C.link_errors, 1);
    CHECK_RC(C.last_error, PE_E_IMPORT);
    CHECK_U64(C.mod[0].flags & PE_MOD_F_LINK_ERR, PE_MOD_F_LINK_ERR);
    CHECK_U64(C.unresolved, 0);
    end_host("S12b");
    sb_reset();
}

static void test_api_misuse(void)
{
    u64 ad;
    int m;
    pe_mod_ops_t o2;

    printf("[S8] API misuse and image-kind checks\n");
    host_reset(); sb_reset();
    sb_leaf("lib.dll", "f", 1, 0);
    CHECK_RC(pe_mod_load_main(NULL, &OPS, SB[0].file, SB[0].len, "x"), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_load_main(&C, NULL, SB[0].file, SB[0].len, "x"), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_load_main(&C, &OPS, NULL, 0, "x"), PE_MOD_E_ARG);
    o2 = OPS; o2.alloc_image = NULL;
    CHECK_RC(pe_mod_load_main(&C, &o2, SB[0].file, SB[0].len, "x"), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_load_main(&C, &OPS, SB[0].file, SB[0].len, "x"), PE_MOD_E_KIND);        /* a DLL is not a main module */
    CHECK_U64(mod_count(), 0);
    CHECK_RC(pe_mod_load_main(&C, &OPS, SB[0].file, 10, "x"), PE_E_TRUNC);                  /* parse errors pass through */
    CHECK_RC(pe_mod_load_main(&C, &OPS, SB[0].file, 0, "x"), PE_E_TRUNC);
    {
        static const char *const nn[1] = { "f" };
        static bimp_t I[1];
        bspec_t s;
        bout_t *e;
        I[0].dll = "lib.dll"; I[0].n = 1; I[0].names = nn;
        memset(&s, 0, sizeof s); s.is_exe = 1; s.no_exports = 1; s.nimps = 1; s.imps = I;
        e = sb_add("prog.exe", &s);
        CHECK_RC(pe_mod_load(&C, &OPS, "prog.exe"), PE_MOD_E_KIND);                         /* an exe is not a DLL dependency */
        CHECK_U64(mod_count(), 0);
        CHECK_U64(H.live_images, 0);
        m = pe_mod_load_main(&C, &OPS, e->file, e->len, "C:\\very\\long\\path\\prog.exe");
        CHECK_RC(m, 0);
        CHECK_STR(C.mod[0].name, "prog.exe");                                                /* path stripped */
        CHECK_RC(pe_mod_find(&C, "prog.exe"), 0);
        CHECK_RC(pe_mod_load_main(&C, &OPS, e->file, e->len, "again.exe"), PE_MOD_E_STATE);  /* only one main */
        CHECK_U64(mod_count(), 2);
        CHECK_RC(pe_mod_load(&C, &OPS, "prog.exe"), 0);                                      /* LoadLibrary of the exe's name: handle */
        CHECK_U64(C.mod[0].refcount, 2);
    }
    CHECK_RC(pe_mod_run_inits(&C, NULL), PE_MOD_E_ARG);
    o2 = OPS; o2.call_entry = NULL;
    CHECK_RC(pe_mod_run_inits(&C, &o2), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_run_detach(&C, &o2), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_free(&C, &OPS, 40), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_free(&C, &OPS, -1), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_free(&C, &OPS, 20), PE_MOD_E_ARG);                                       /* free slot */
    CHECK_RC(pe_mod_getproc(&C, &OPS, 1, "f", 0, 0, NULL), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_getproc(&C, NULL, 1, "f", 0, 0, &ad), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_getproc(NULL, &OPS, 1, "f", 0, 0, &ad), PE_MOD_E_ARG);
    CHECK(pe_mod_get(&C, 99) == NULL && pe_mod_get(&C, -3) == NULL && pe_mod_get(&C, 25) == NULL);
    CHECK_U64(pe_mod_entry_va(&C, 25), 0);
    CHECK_RC(pe_mod_find(NULL, "x.dll"), PE_MOD_E_ARG);
    CHECK_RC(pe_mod_find_base(NULL, 1), PE_MOD_E_ARG);
    {
        int e;
        for (e = 1; e >= -42; e--) {                                 /* every code has a distinct, non-empty description */
            int e2;
            const char *d = pe_mod_strerror(e);
            CHECK(d != NULL && strlen(d) > 1);
            for (e2 = e - 1; e2 >= -42; e2--)
                if (!(strcmp(d, "unknown PE error") == 0 && strcmp(pe_mod_strerror(e2), "unknown PE error") == 0))
                    CHECK(strcmp(d, pe_mod_strerror(e2)) != 0);
        }
    }
    CHECK(strlen(pe_mod_strerror(PE_MOD_E_FULL)) > 3 && strlen(pe_mod_strerror(PE_E_FWD)) > 3 && strlen(pe_mod_strerror(-999)) > 3);
    CHECK(strlen(pe_export_strerror(PE_E_BUFSIZE)) > 3 && strcmp(pe_export_strerror(0), "ok") == 0);
    end_host("S8");
    sb_reset();

    /* a malformed export table makes the DLL itself unloadable (validated once, at load) */
    host_reset();
    {
        bspec_t s;
        static bfunc_t F[1] = { {0x71, 0} };
        static bname_t N[1] = { {"bad", 0} };
        bout_t *b;
        memset(&s, 0, sizeof s); s.nfuncs = 1; s.funcs = F; s.nnames = 1; s.names = N;
        b = sb_add("badexp.dll", &s);
        bl_p16(b->file, bl_off(b, b->ords_rva), 9);                                           /* ordinal 9 >= nfuncs */
        CHECK_RC(pe_mod_load(&C, &OPS, "badexp.dll"), PE_E_EXPORT);
        CHECK_U64(mod_count(), 0);
        CHECK_U64(H.live_images, 0);
        CHECK_U64(H.live_files, 0);
    }
    end_host("S8b");
    sb_reset();
}

/* ================================================================================================================ */
int main(int argc, char **argv)
{
    size_t i;
    const char *dir = argc > 1 ? argv[1] : "/tmp/pe_dll_dev/fix";
    char path[512];

    for (i = 0; i < sizeof FX / sizeof FX[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, FX[i].name);
        FX[i].data = load_file(path, &FX[i].len);
        if (!FX[i].data) { fprintf(stderr, "PE-DLL-HOST-TEST: cannot read fixture %s\n", path); return 2; }
    }

    test_export_basic();
    test_export_hostile();
    test_forwarder_parse();
    test_delay_imports();
    test_fixture_exports();
    test_real_graph();
    test_loadlibrary_free();
    test_bad_dllmain();
    test_cycle();
    test_missing_dll();
    test_depth_and_cap();
    test_hostile_names();
    test_builtin_paths();
    test_forwarders();
    test_delay_modules();
    test_preferred_base();
    test_reentrancy();
    test_dependency_failures();
    test_forward_import_to_builtin();
    test_link_errors();
    test_api_misuse();

    for (i = 0; i < sizeof FX / sizeof FX[0]; i++) free(FX[i].data);
    if (g_fail) { printf("PE-DLL-HOST-TEST: FAIL (%d of %d checks failed)\n", g_fail, g_checks); return 1; }
    printf("PE-DLL-HOST-TEST: PASS checks=%d\n", g_checks);
    return 0;
}
