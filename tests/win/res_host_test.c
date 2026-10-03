/*
 * res_host_test.c -- host-side tests for userspace/lib/pe/pe_resources.[ch] and pe_manifest.[ch].
 *
 *   usage: res_host_test <res_hello.exe> <res_good.manifest>
 *   env:   RES_DUMP_DIR=<dir>   also write every hand-built hostile PE / manifest to <dir>/ (inspection / corpus tools)
 *
 * Built by tests/win/run_res_tests.sh with  gcc -std=gnu11 -O1 -g -fsanitize=address,undefined
 * -fno-sanitize-recover=all.  Every buffer handed to the library is a heap block of EXACTLY the size the contract
 * promises (file == len, image == size_of_image, manifest text == len), so AddressSanitizer turns any
 * out-of-bounds access into an immediate abort.
 *
 * Groups:
 *   A  real MinGW/windres fixture: manifest, version, multi-language, named and string-typed resources, enumeration
 *   B  manifest scanner: exact values, encodings, error codes and positions, caps, entity bombs, role confusion
 *   C  hostile resource trees: loops, overlaps, depth, bounds, 100k entries, giant names, node budget, bad headers
 *   D  version resource: exact values, table selection, truncation at every length, hostile blobs
 *   E  dependency hints (no file I/O): known / private / unsafe names
 *   F  error strings
 */
#include "pe.h"
#include "pe_resources.h"
#include "pe_manifest.h"
#include "res_common.h"

static int g_checks, g_fail;

#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_U(got, want) do { u64 g_ = (u64)(got), w_ = (u64)(want); g_checks++; \
    if (g_ != w_) { g_fail++; printf("  FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { g_fail++; printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)
#define CHECK_S(got, want) do { const char *g_ = (got), *w_ = (want); g_checks++; \
    if (strcmp(g_, w_) != 0) { g_fail++; printf("  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)

static const char *g_dump_dir;

static void dump_file(const char *name, const u8 *p, size_t n)
{
    char path[512];
    FILE *f;
    if (!g_dump_dir) return;
    snprintf(path, sizeof path, "%s/%s", g_dump_dir, name);
    f = fopen(path, "wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); }
}

/* ------------------------------------------------------------------------------------------------ */
/* loading                                                                                           */
/* ------------------------------------------------------------------------------------------------ */
typedef struct { u8 *file; size_t flen; u8 *img; pe_info_t info; } img_t;

static int load_img(const u8 *f, size_t n, img_t *L)
{
    int r;
    memset(L, 0, sizeof *L);
    L->file = dup_exact(f, n);
    L->flen = n;
    r = pe_parse(L->file, n, &L->info);
    if (r != PE_OK) return r;
    L->img = (u8 *)calloc(L->info.size_of_image, 1);
    return pe_map(L->file, n, &L->info, L->img);
}
static void free_img(img_t *L) { free(L->file); free(L->img); memset(L, 0, sizeof *L); }

/* build a synthetic PE around `rsrc` and load it */
static int load_tree(const char *name, const rt_t *r, u32 len, u32 dir_size, img_t *L)
{
    size_t fl;
    u8 *f = make_pe(r->b, len, dir_size, &fl);
    int rc;
    dump_file(name, f, fl);
    rc = load_img(f, fl, L);
    free(f);
    if (rc != PE_OK) printf("  FAIL: synthetic PE %s did not load: %d\n", name, rc), g_fail++;
    return rc;
}

static int in_image(const img_t *L, const u8 *p, u32 n)
{
    return p >= L->img && (u64)(p - L->img) <= L->info.size_of_image && (u64)n <= L->info.size_of_image - (u64)(p - L->img);
}

/* ------------------------------------------------------------------------------------------------ */
/* manifest helpers                                                                                  */
/* ------------------------------------------------------------------------------------------------ */
static int pm(const char *s, pe_manifest_t *m)
{
    size_t n = strlen(s);
    u8 *c = dup_exact((const u8 *)s, n);
    int r = pe_manifest_parse(c, n, m);
    free(c);
    return r;
}
static int pm_bytes(const u8 *s, size_t n, pe_manifest_t *m)
{
    u8 *c = dup_exact(s, n);
    int r = pe_manifest_parse(c, n, m);
    free(c);
    return r;
}

/* compare two parses ignoring encoding / error_pos (UTF-8 vs UTF-16 equivalence) */
static int man_equiv(const pe_manifest_t *a, const pe_manifest_t *b)
{
    pe_manifest_t x = *a, y = *b;
    x.encoding = y.encoding = 0;
    x.error_pos = y.error_pos = 0;
    return memcmp(&x, &y, sizeof x) == 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* Group A: the real windres fixture                                                                  */
/* ------------------------------------------------------------------------------------------------ */
typedef struct { unsigned n; char first_type[64]; char first_name[64]; unsigned stop_after; } enum_acc_t;

static int enum_cb(const pe_res_entry_t *e, void *user)
{
    enum_acc_t *a = (enum_acc_t *)user;
    if (a->n == 0) {
        snprintf(a->first_type, sizeof a->first_type, "%s", e->type.is_named ? e->type.str : "#");
        snprintf(a->first_name, sizeof a->first_name, "%s", e->name.is_named ? e->name.str : "#");
    }
    a->n++;
    return a->stop_after && a->n >= a->stop_after;
}

static void expect_text(const img_t *L, unsigned type, unsigned id, unsigned lang, const char *want_with_nul, const char *label)
{
    const u8 *d = NULL;
    unsigned sz = 0;
    int r = pe_res_find(&L->info, L->img, type, id, lang, &d, &sz);
    if (r != PE_OK) { g_checks++; g_fail++; printf("  FAIL [%s]: find rc=%d (%s)\n", label, r, pe_res_strerror(r)); return; }
    g_checks++;
    if (sz != strlen(want_with_nul) + 1 || memcmp(d, want_with_nul, sz) != 0 || !in_image(L, d, sz)) {
        g_fail++;
        printf("  FAIL [%s]: got %u bytes \"%.*s\", expected \"%s\"\n", label, sz, (int)(sz ? sz - 1 : 0), (const char *)d, want_with_nul);
    }
}

static void test_fixture(const char *exe, const char *manifest_path)
{
    size_t fl, ml, u16len;
    u8 *file = load_file(exe, &fl), *mtxt = load_file(manifest_path, &ml), *m16;
    img_t L;
    pe_manifest_t m, m2;
    pe_version_t v;
    pe_res_entry_t e;
    const u8 *d = NULL;
    unsigned sz = 0, rva = 0, size = 0;
    int r;

    printf("[A] real fixture %s\n", exe);
    if (!file || !mtxt) { printf("  FAIL: cannot read fixtures\n"); g_fail++; free(file); free(mtxt); return; }
    r = load_img(file, fl, &L);
    CHECK_I(r, PE_OK);
    if (r != PE_OK) { free(file); free(mtxt); return; }

    /* the resource directory */
    r = pe_res_dir(&L.info, L.img, &rva, &size);
    CHECK_I(r, 0);
    CHECK_U(rva, g32(file, g32(file, 0x3c) + 24 + 112 + 16));
    CHECK_U(size, g32(file, g32(file, 0x3c) + 24 + 112 + 20));
    CHECK(rva != 0 && size != 0 && (u64)rva + size <= L.info.size_of_image);

    /* the manifest resource is byte-identical to the file it was built from */
    r = pe_res_find(&L.info, L.img, PE_RT_MANIFEST, 1, 0, &d, &sz);
    CHECK_I(r, 0);
    CHECK_U(sz, ml);
    CHECK(d && sz == ml && memcmp(d, mtxt, ml) == 0 && in_image(&L, d, sz));
    CHECK_I(pe_res_find(&L.info, L.img, PE_RT_MANIFEST, 2, 0, &d, &sz), PE_RES_NOTFOUND);
    CHECK(d == NULL && sz == 0);
    CHECK_I(pe_res_find(&L.info, L.img, 99, PE_RES_ID_ANY, 0, &d, &sz), PE_RES_NOTFOUND);
    CHECK_I(pe_res_find(&L.info, L.img, PE_RT_MANIFEST, PE_RES_ID_ANY, PE_RES_LANG_ANY, &d, &sz), 0);

    /* parsed manifest: exact values */
    r = pe_manifest_from_image(&L.info, L.img, &m);
    CHECK_I(r, 0);
    CHECK_I(m.well_formed, 1);
    CHECK_I(m.error, 0);
    CHECK_I(m.encoding, PE_MAN_ENC_UTF8);
    CHECK_I(m.is_assembly, 1);
    CHECK_U(m.n_elements, 21);
    CHECK_U(m.n_attributes, 25);
    CHECK_U(m.max_depth, 5);
    CHECK_I(m.has_identity, 1);
    CHECK_S(m.identity.name, "Example.ResHello");
    CHECK_S(m.identity.version, "1.2.3.4");
    CHECK_S(m.identity.type, "win32");
    CHECK_S(m.identity.processor_architecture, "amd64");
    CHECK_S(m.identity.public_key_token, "0123456789abcdef");
    CHECK_S(m.identity.language, "");
    CHECK(m.identity.ver_valid && m.identity.ver[0] == 1 && m.identity.ver[1] == 2 && m.identity.ver[2] == 3 && m.identity.ver[3] == 4);
    CHECK_I(m.identity.token_valid, 1);
    CHECK_U(m.n_deps, 1);
    CHECK_U(m.n_deps_total, 1);
    CHECK_S(m.deps[0].name, "Microsoft.Windows.Common-Controls");
    CHECK_S(m.deps[0].version, "6.0.0.0");
    CHECK_S(m.deps[0].type, "win32");
    CHECK_S(m.deps[0].processor_architecture, "*");
    CHECK_S(m.deps[0].public_key_token, "6595b64144ccf1df");
    CHECK_S(m.deps[0].language, "*");
    CHECK(m.deps[0].ver_valid && m.deps[0].ver[0] == 6 && m.deps[0].ver[1] == 0 && m.deps[0].ver[2] == 0 && m.deps[0].ver[3] == 0);
    CHECK_I(m.deps[0].token_valid, 1);
    CHECK_I(m.has_exec_level, 1);
    CHECK_I(m.exec_level, PE_MAN_EXEC_AS_INVOKER);
    CHECK_U(m.exec_levels_seen, 1u << PE_MAN_EXEC_AS_INVOKER);
    CHECK_U(m.n_exec_level, 1);
    CHECK_I(m.has_ui_access, 1);
    CHECK_I(m.ui_access, 0);
    CHECK_U(m.n_os, 3);
    CHECK_U(m.n_os_total, 3);
    CHECK_S(m.os_guid[0], "{35138b9a-5d96-4fbd-8e2d-a2440225f93a}");
    CHECK_S(m.os_guid[1], "{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}");
    CHECK_S(m.os_guid[2], "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}");
    CHECK_U(m.os_mask, PE_MAN_OS_WIN7 | PE_MAN_OS_WIN8 | PE_MAN_OS_WIN10);
    CHECK_S(m.dpi_aware, "true/pm");
    CHECK_S(m.dpi_awareness, "PerMonitorV2, PerMonitor");
    CHECK_S(m.long_path_aware, "true");
    CHECK_S(m.active_code_page, "UTF-8");
    CHECK(m.has_dpi_aware && m.has_dpi_awareness && m.has_long_path_aware && m.has_active_code_page);
    CHECK_U(m.truncated, 0);
    CHECK_I(pe_manifest_requires_elevation(&m), PE_MAN_ELEV_NONE);
    CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_PER_MONITOR_V2);

    /* the same document as UTF-8 with BOM, and as UTF-16LE with BOM, parses identically */
    {
        u8 *bom = (u8 *)malloc(ml + 3);
        bom[0] = 0xEF; bom[1] = 0xBB; bom[2] = 0xBF;
        memcpy(bom + 3, mtxt, ml);
        r = pm_bytes(bom, ml + 3, &m2);
        CHECK_I(r, 0);
        CHECK(man_equiv(&m, &m2));
        free(bom);
        m16 = to_utf16le_bom(mtxt, ml, &u16len);
        dump_file("res_good_utf16.manifest", m16, u16len);
        r = pm_bytes(m16, u16len, &m2);
        CHECK_I(r, 0);
        CHECK_I(m2.encoding, PE_MAN_ENC_UTF16LE);
        CHECK(man_equiv(&m, &m2));
        /* no BOM, '<' 0x00 start */
        r = pm_bytes(m16 + 2, u16len - 2, &m2);
        CHECK_I(r, 0);
        CHECK(man_equiv(&m, &m2));
        free(m16);
    }

    /* image of an EXE: id 1 first; the same image presented as a DLL still finds id 1 (fallback order) */
    {
        pe_info_t dll = L.info;
        dll.is_dll = 1;
        r = pe_manifest_from_image(&dll, L.img, &m2);
        CHECK_I(r, 0);
        CHECK(man_equiv(&m, &m2));
    }

    /* version info */
    r = pe_version_info(&L.info, L.img, 0x0409, &v);
    CHECK_I(r, 0);
    CHECK_I(v.has_fixed, 1);
    CHECK_U(v.struct_version, 0x00010000);
    CHECK_U(v.file_version_ms, 0x00010002); CHECK_U(v.file_version_ls, 0x00030004);
    CHECK_U(v.product_version_ms, 0x00050006); CHECK_U(v.product_version_ls, 0x00070008);
    CHECK(v.file_version[0] == 1 && v.file_version[1] == 2 && v.file_version[2] == 3 && v.file_version[3] == 4);
    CHECK(v.product_version[0] == 5 && v.product_version[1] == 6 && v.product_version[2] == 7 && v.product_version[3] == 8);
    CHECK_U(v.file_flags_mask, 0x3f); CHECK_U(v.file_flags, 0); CHECK_U(v.file_os, 0x40004); CHECK_U(v.file_type, 1); CHECK_U(v.file_subtype, 0);
    CHECK_I(v.has_strings, 1);
    CHECK_U(v.lang, 0x0409); CHECK_U(v.codepage, 1200);
    CHECK_S(v.file_description, "Resource Test App");
    CHECK_S(v.product_name, "Res Test Product");
    CHECK_S(v.company_name, "Example Corp");
    CHECK_S(v.original_filename, "res_hello.exe");
    CHECK_S(v.file_version_str, "1.2.3.4");
    CHECK_S(v.product_version_str, "5.6.7.8");
    CHECK_U(v.have_str, PE_VER_S_DESCRIPTION | PE_VER_S_PRODUCT | PE_VER_S_COMPANY | PE_VER_S_ORIGINAL | PE_VER_S_FILEVER | PE_VER_S_PRODVER);
    CHECK_U(v.truncated, 0);
    r = pe_version_info(&L.info, L.img, 0x0407, &v);                         /* German table */
    CHECK_I(r, 0);
    CHECK_U(v.lang, 0x0407);
    CHECK_S(v.file_description, "Ressourcen-Testprogramm");
    CHECK_S(v.product_name, "Res Testprodukt");
    CHECK_S(v.company_name, "");
    CHECK_U(v.have_str, PE_VER_S_DESCRIPTION | PE_VER_S_PRODUCT);
    r = pe_version_info(&L.info, L.img, 0x0C07, &v);                         /* de-AT: primary-language match */
    CHECK_I(r, 0); CHECK_U(v.lang, 0x0407);
    r = pe_version_info(&L.info, L.img, 0x0411, &v);                         /* ja: nothing -> Translation[0] = en-US */
    CHECK_I(r, 0); CHECK_U(v.lang, 0x0409);
    r = pe_version_info(&L.info, L.img, PE_RES_LANG_ANY, &v);
    CHECK_I(r, 0); CHECK_U(v.lang, 0x0409); CHECK_S(v.product_name, "Res Test Product");

    /* multi-language RCDATA 200: neutral, de-DE, en-US, fr-FR (tree order) */
    expect_text(&L, PE_RT_RCDATA, 200, 0x040C, "lang-fr-FR", "fr-FR exact");
    expect_text(&L, PE_RT_RCDATA, 200, 0x0407, "lang-de-DE", "de-DE exact");
    expect_text(&L, PE_RT_RCDATA, 200, 0x0409, "lang-en-US", "en-US exact");
    expect_text(&L, PE_RT_RCDATA, 200, 0x0000, "lang-neutral", "neutral exact");
    expect_text(&L, PE_RT_RCDATA, 200, 0x0809, "lang-en-US", "en-GB -> primary en");
    expect_text(&L, PE_RT_RCDATA, 200, 0x0C0C, "lang-fr-FR", "fr-CA -> primary fr");
    expect_text(&L, PE_RT_RCDATA, 200, 0x0411, "lang-neutral", "ja -> neutral");
    expect_text(&L, PE_RT_RCDATA, 200, PE_RES_LANG_ANY, "lang-neutral", "ANY -> first entry");
    expect_text(&L, PE_RT_RCDATA, 300, 0x0409, "only-de-DE", "only language available");
    expect_text(&L, PE_RT_RCDATA, 300, PE_RES_LANG_ANY, "only-de-DE", "only language, ANY");
    CHECK_I(pe_res_find_ex(&L.info, L.img, &(pe_res_key_t){0, PE_RT_RCDATA}, &(pe_res_key_t){0, 200}, 0x040C, &e), 0);
    CHECK_U(e.lang, 0x040C); CHECK_U(e.codepage, 0); CHECK(e.type.is_named == 0 && e.type.id == PE_RT_RCDATA && e.name.id == 200);
    CHECK(e.data == L.img + e.rva && in_image(&L, e.data, e.size));

    /* string-named resource and string-named type, case-insensitive */
    {
        pe_res_key_t t = pe_res_key_id(PE_RT_RCDATA), n = pe_res_key_name("GREETING");
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, 0, &e), 0);
        CHECK(e.name.is_named && strcmp(e.name.str, "GREETING") == 0 && e.name.name_len == 8);
        CHECK_U(e.size, 12);
        CHECK(memcmp(e.data, "hello-named\0", 12) == 0);
        CHECK_U(e.lang, 0x0409);
        n = pe_res_key_name("greeting");
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, 0, &e), 0);
        n = pe_res_key_name("GREETIN");
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, 0, &e), PE_RES_NOTFOUND);
        n = pe_res_key_name("GREETINGS");
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, 0, &e), PE_RES_NOTFOUND);
        n = pe_res_key_id(200);                                            /* numeric key never matches a named entry */
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, 0, &e), 0);
        t = pe_res_key_name("MYTYPE");
        n = pe_res_key_id(1);
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, 0, &e), 0);
        CHECK(e.type.is_named && strcmp(e.type.str, "MYTYPE") == 0);
        CHECK_U(e.size, 14); CHECK(memcmp(e.data, "custom-type-1\0", 14) == 0);
        n = pe_res_key_name("foo");
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, 0, &e), 0);
        CHECK_U(e.size, 16); CHECK(memcmp(e.data, "custom-type-foo\0", 16) == 0);
        t = pe_res_key_name("MYTYP");
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, 0, &e), PE_RES_NOTFOUND);
        t = pe_res_key_any(); n = pe_res_key_any();                        /* first of everything: MYTYPE (named) / FOO */
        CHECK_I(pe_res_find_ex(&L.info, L.img, &t, &n, PE_RES_LANG_ANY, &e), 0);
        CHECK(e.type.is_named && strcmp(e.type.str, "MYTYPE") == 0 && e.name.is_named && strcmp(e.name.str, "FOO") == 0);
        {
            char longkey[400];
            memset(longkey, 'A', sizeof longkey - 1);
            longkey[sizeof longkey - 1] = 0;
            t = pe_res_key_name(longkey);
            CHECK_I(pe_res_find_ex(&L.info, L.img, &t, NULL, 0, &e), PE_E_RES_LIMIT);
        }
    }

    /* string table blocks */
    CHECK_I(pe_res_find(&L.info, L.img, PE_RT_STRING, 1, 0, &d, &sz), 0);
    CHECK_U(sz, 0x48);
    CHECK_I(pe_res_find(&L.info, L.img, PE_RT_STRING, 2, 0, &d, &sz), 0);
    CHECK_U(sz, 0x40);

    /* enumeration */
    {
        enum_acc_t a;
        unsigned nv = 0;
        memset(&a, 0, sizeof a);
        r = pe_res_enum(&L.info, L.img, NULL, enum_cb, &a, &nv);
        CHECK_I(r, 0); CHECK_U(nv, 12); CHECK_U(a.n, 12);
        CHECK_S(a.first_type, "MYTYPE"); CHECK_S(a.first_name, "FOO");
        memset(&a, 0, sizeof a);
        { pe_res_key_t t = pe_res_key_id(PE_RT_RCDATA); r = pe_res_enum(&L.info, L.img, &t, enum_cb, &a, &nv); }
        CHECK_I(r, 0); CHECK_U(nv, 6); CHECK_U(a.n, 6);
        CHECK_S(a.first_type, "#"); CHECK_S(a.first_name, "GREETING");
        memset(&a, 0, sizeof a);
        { pe_res_key_t t = pe_res_key_name("mytype"); r = pe_res_enum(&L.info, L.img, &t, enum_cb, &a, &nv); }
        CHECK_I(r, 0); CHECK_U(nv, 2);
        memset(&a, 0, sizeof a);
        { pe_res_key_t t = pe_res_key_id(99); r = pe_res_enum(&L.info, L.img, &t, enum_cb, &a, &nv); }
        CHECK_I(r, 0); CHECK_U(nv, 0);
        memset(&a, 0, sizeof a);
        a.stop_after = 3;
        r = pe_res_enum(&L.info, L.img, NULL, enum_cb, &a, &nv);
        CHECK_I(r, 0); CHECK_U(nv, 3); CHECK_U(a.n, 3);
        CHECK_I(pe_res_enum(&L.info, L.img, NULL, NULL, NULL, &nv), PE_E_BOUNDS);
        CHECK_U(nv, 0);
    }

    /* the version resource found by type, equals what pe_version_info parsed */
    CHECK_I(pe_res_find(&L.info, L.img, PE_RT_VERSION, PE_RES_ID_ANY, 0, &d, &sz), 0);
    CHECK_U(sz, 0x338);
    free_img(&L);
    free(file);
    free(mtxt);
}

/* ------------------------------------------------------------------------------------------------ */
/* Group B: manifest scanner                                                                          */
/* ------------------------------------------------------------------------------------------------ */
static void expect_err(const char *label, const char *doc, int want, unsigned want_pos)
{
    pe_manifest_t m;
    int r = pm(doc, &m);
    g_checks++;
    if (r != want || m.error != want || (want_pos != 0xFFFFFFFFu && m.error_pos != want_pos) || (want != 0 && m.error_pos > strlen(doc))) {
        g_fail++;
        printf("  FAIL [%s]: rc=%d (%s) pos=%u, expected rc=%d (%s) pos=%d\n", label, r, pe_manifest_strerror(r), m.error_pos,
               want, pe_manifest_strerror(want), (int)want_pos);
    }
    g_checks++;
    if ((want == 0) != (m.well_formed != 0) && want != PE_E_MAN_ROOT) {
        g_fail++;
        printf("  FAIL [%s]: well_formed=%d inconsistent with rc=%d\n", label, m.well_formed, want);
    }
}

static char *nested_doc(unsigned levels_below_root, size_t *len)
{
    /* <assembly><a><a>...</a></a></assembly> */
    size_t cap = 16 + levels_below_root * 8 + 16, n = 0;
    char *s = (char *)malloc(cap);
    unsigned i;
    n += (size_t)snprintf(s + n, cap - n, "<assembly>");
    for (i = 0; i < levels_below_root; i++) n += (size_t)snprintf(s + n, cap - n, "<a>");
    for (i = 0; i < levels_below_root; i++) n += (size_t)snprintf(s + n, cap - n, "</a>");
    n += (size_t)snprintf(s + n, cap - n, "</assembly>");
    *len = n;
    return s;
}

static void test_manifest(const char *manifest_path)
{
    pe_manifest_t m;
    size_t ml, n, i;
    u8 *good = load_file(manifest_path, &ml);
    int r;

    printf("[B] manifest scanner\n");

    /* --- minimal / structure --- */
    r = pm("<assembly/>", &m);
    CHECK_I(r, 0); CHECK_I(m.well_formed, 1); CHECK_I(m.is_assembly, 1); CHECK_U(m.n_elements, 1); CHECK_I(m.has_identity, 0);
    CHECK_I(m.error_pos, 0); CHECK_I(m.has_exec_level, 0); CHECK_I(m.exec_level, PE_MAN_EXEC_NONE);
    CHECK_I(pe_manifest_requires_elevation(&m), PE_MAN_ELEV_NONE); CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_UNSPECIFIED);
    r = pm("<?xml version=\"1.0\"?>\n<!-- c -->\n<assembly manifestVersion=\"1.0\">\n</assembly>\n\n", &m);
    CHECK_I(r, 0); CHECK_U(m.n_attributes, 1);
    r = pm("<assembly/>   \n\0\0\0", &m);                              /* (the C string stops at the first NUL: padded copy below) */
    CHECK_I(r, 0);
    {
        static const u8 padded[] = "<assembly/>   \n\0\0\0";
        r = pm_bytes(padded, sizeof padded - 1, &m);
        CHECK_I(r, 0);
        r = pm_bytes(padded, sizeof padded, &m);                       /* includes the C terminator too */
        CHECK_I(r, 0);
    }

    /* --- namespaces and prefixes --- */
    r = pm("<asm:assembly xmlns:asm=\"urn:schemas-microsoft-com:asm.v1\"><asm:assemblyIdentity name=\"X\" version=\"1.0.0.0\" type=\"win32\"/></asm:assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "X"); CHECK_S(m.identity.version, "1.0.0.0"); CHECK_S(m.identity.type, "win32");
    r = pm("<assembly><assemblyIdentity xmlns:q=\"u\" q:name=\"evil\" name=\"real\"/></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "real");
    r = pm("<assembly><assemblyIdentity q:name=\"evil\" xmlns:q=\"u\"/></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "");
    r = pm("<assembly><assemblyIdentity name='single' version = '2.0.0.1'\n\ttype\n=\n\"win32\" /></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "single"); CHECK_S(m.identity.version, "2.0.0.1"); CHECK_S(m.identity.type, "win32");

    /* --- entities --- */
    r = pm("<assembly><assemblyIdentity name=\"a&amp;b&lt;c&gt;&quot;d&apos;&#65;&#x42;&#x20ac;\"/></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "a&b<c>\"d'AB?");
    r = pm("<assembly><assemblyIdentity name=\"&amp;amp;\"/></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "&amp;");                 /* one level only */
    r = pm("<assembly><assemblyIdentity name=\"  padded  \"/></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "padded");
    r = pm("<assembly><assemblyIdentity name=\"a\tb\nc\"/></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "a b c");

    /* --- comments / CDATA / PIs are skipped, never interpreted --- */
    r = pm("<assembly><!-- <assemblyIdentity name=\"evil\"/> --><![CDATA[<assemblyIdentity name=\"evil2\"/>]]><?pi x?>"
           "<assemblyIdentity name=\"good\"/></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "good"); CHECK_U(m.n_elements, 2);
    r = pm("<assembly><!-- a - b -- c --><assemblyIdentity name=\"x\"/></assembly>", &m);
    CHECK_I(r, 0);

    /* --- identity rules --- */
    r = pm("<assembly><assemblyIdentity name=\"first\"/><assemblyIdentity name=\"second\"/></assembly>", &m);
    CHECK_I(r, 0); CHECK_S(m.identity.name, "first");
    r = pm("<assembly><dependency><assemblyIdentity name=\"misplaced\"/></dependency><foo><assemblyIdentity name=\"misplaced2\"/></foo></assembly>", &m);
    CHECK_I(r, 0); CHECK_I(m.has_identity, 0); CHECK_U(m.n_deps_total, 0);
    r = pm("<assembly><dependency><dependentAssembly/></dependency></assembly>", &m);
    CHECK_I(r, 0); CHECK_U(m.n_deps_total, 0);
    {
        char big[8192];
        size_t o = 0;
        unsigned k;
        o += (size_t)snprintf(big + o, sizeof big - o, "<assembly><dependency>");
        for (k = 0; k < 20; k++)
            o += (size_t)snprintf(big + o, sizeof big - o, "<dependentAssembly><assemblyIdentity name=\"dep%u\" version=\"1.0.0.%u\"/></dependentAssembly>", k, k);
        o += (size_t)snprintf(big + o, sizeof big - o, "</dependency></assembly>");
        r = pm(big, &m);
        CHECK_I(r, 0); CHECK_U(m.n_deps, 16); CHECK_U(m.n_deps_total, 20);
        CHECK_S(m.deps[0].name, "dep0"); CHECK_S(m.deps[15].name, "dep15"); CHECK_U(m.deps[15].ver[3], 15);
    }
    {
        char big[1024], nm[300];
        memset(nm, 'n', 200); nm[200] = 0;
        snprintf(big, sizeof big, "<assembly><assemblyIdentity name=\"%s\"/></assembly>", nm);
        r = pm(big, &m);
        CHECK_I(r, 0); CHECK_U(strlen(m.identity.name), 127); CHECK_I(m.identity.truncated, 1); CHECK_U(m.truncated, 1);
    }

    /* --- version / token validation --- */
    {
        static const struct { const char *v; int ok; } vt[] = {
            { "1.2.3.4", 1 }, { "0.0.0.0", 1 }, { "65535.65535.65535.65535", 1 }, { "1.2.3", 0 }, { "65536.0.0.0", 0 },
            { "1.2.3.4.5", 0 }, { "1.2.3.-4", 0 }, { "", 0 }, { "1..3.4", 0 }, { "a.b.c.d", 0 }, { "1.2.3.4 ", 1 }, { "000001.2.3.4", 0 },
        };
        for (i = 0; i < sizeof vt / sizeof vt[0]; i++) {
            char d[200];
            snprintf(d, sizeof d, "<assembly><assemblyIdentity version=\"%s\"/></assembly>", vt[i].v);
            r = pm(d, &m);
            CHECK_I(r, 0);
            CHECK_I(m.identity.ver_valid, vt[i].ok);
            if (!vt[i].ok) CHECK(m.identity.ver[0] == 0 && m.identity.ver[1] == 0 && m.identity.ver[2] == 0 && m.identity.ver[3] == 0);
        }
        r = pm("<assembly><assemblyIdentity publicKeyToken=\"6595B64144CCF1DF\"/></assembly>", &m);
        CHECK_I(r, 0); CHECK_I(m.identity.token_valid, 1);
        r = pm("<assembly><assemblyIdentity publicKeyToken=\"6595b64144ccf1d\"/></assembly>", &m);
        CHECK_I(m.identity.token_valid, 0);
        r = pm("<assembly><assemblyIdentity publicKeyToken=\"6595b64144ccf1dfa\"/></assembly>", &m);
        CHECK_I(m.identity.token_valid, 0);
        r = pm("<assembly><assemblyIdentity publicKeyToken=\"6595b64144ccf1dg\"/></assembly>", &m);
        CHECK_I(m.identity.token_valid, 0);
    }

    /* --- requestedExecutionLevel --- */
    {
        static const struct { const char *lvl; int want; int elev; } lv[] = {
            { "asInvoker", PE_MAN_EXEC_AS_INVOKER, PE_MAN_ELEV_NONE },
            { "requireAdministrator", PE_MAN_EXEC_REQUIRE_ADMIN, PE_MAN_ELEV_REQUIRED },
            { "highestAvailable", PE_MAN_EXEC_HIGHEST, PE_MAN_ELEV_IF_AVAILABLE },
            { "REQUIREADMINISTRATOR", PE_MAN_EXEC_REQUIRE_ADMIN, PE_MAN_ELEV_REQUIRED },
            { " requireAdministrator ", PE_MAN_EXEC_REQUIRE_ADMIN, PE_MAN_ELEV_REQUIRED },
            { "bogus", PE_MAN_EXEC_UNKNOWN, PE_MAN_ELEV_NONE },
            { "", PE_MAN_EXEC_UNKNOWN, PE_MAN_ELEV_NONE },
        };
        char d[400];
        for (i = 0; i < sizeof lv / sizeof lv[0]; i++) {
            snprintf(d, sizeof d, "<assembly><trustInfo><security><requestedPrivileges><requestedExecutionLevel level=\"%s\" uiAccess=\"true\"/>"
                     "</requestedPrivileges></security></trustInfo></assembly>", lv[i].lvl);
            r = pm(d, &m);
            CHECK_I(r, 0); CHECK_I(m.exec_level, lv[i].want); CHECK_I(pe_manifest_requires_elevation(&m), lv[i].elev);
            CHECK_I(m.ui_access, 1); CHECK_I(m.has_ui_access, 1); CHECK_U(m.n_exec_level, 1);
        }
        r = pm("<assembly><trustInfo><security><requestedPrivileges><requestedExecutionLevel/></requestedPrivileges></security></trustInfo></assembly>", &m);
        CHECK_I(r, 0); CHECK_I(m.has_exec_level, 1); CHECK_I(m.exec_level, PE_MAN_EXEC_NONE); CHECK_I(m.has_ui_access, 0);
        /* duplicates: first wins for exec_level, but elevation is conservative */
        r = pm("<assembly><trustInfo><security><requestedPrivileges>"
               "<requestedExecutionLevel level=\"asInvoker\"/><requestedExecutionLevel level=\"requireAdministrator\" uiAccess=\"TRUE\"/>"
               "</requestedPrivileges></security></trustInfo></assembly>", &m);
        CHECK_I(r, 0); CHECK_I(m.exec_level, PE_MAN_EXEC_AS_INVOKER); CHECK_U(m.n_exec_level, 2);
        CHECK_U(m.exec_levels_seen, (1u << PE_MAN_EXEC_AS_INVOKER) | (1u << PE_MAN_EXEC_REQUIRE_ADMIN));
        CHECK_I(pe_manifest_requires_elevation(&m), PE_MAN_ELEV_REQUIRED); CHECK_I(m.ui_access, 1);
        /* a foreign-namespace look-alike: documented behaviour = matched by local name, elevation conservative */
        r = pm("<assembly><trustInfo><security><requestedPrivileges><requestedExecutionLevel level=\"asInvoker\"/></requestedPrivileges></security></trustInfo>"
               "<x:trustInfo xmlns:x=\"evil\"><x:security><x:requestedPrivileges><x:requestedExecutionLevel level=\"requireAdministrator\"/>"
               "</x:requestedPrivileges></x:security></x:trustInfo></assembly>", &m);
        CHECK_I(r, 0); CHECK_I(pe_manifest_requires_elevation(&m), PE_MAN_ELEV_REQUIRED);
        /* wrong place: ignored */
        r = pm("<assembly><requestedExecutionLevel level=\"requireAdministrator\"/><trustInfo><requestedExecutionLevel level=\"requireAdministrator\"/></trustInfo></assembly>", &m);
        CHECK_I(r, 0); CHECK_I(m.has_exec_level, 0); CHECK_I(pe_manifest_requires_elevation(&m), PE_MAN_ELEV_NONE);
        /* duplicate tracked attribute is a syntax error */
        expect_err("dup level attr", "<assembly><trustInfo><security><requestedPrivileges><requestedExecutionLevel level=\"a\" level=\"b\"/>"
                   "</requestedPrivileges></security></trustInfo></assembly>", PE_E_MAN_SYNTAX, 0xFFFFFFFFu);
    }

    /* --- supportedOS --- */
    {
        char d[2048];
        size_t o = 0;
        unsigned k;
        o += (size_t)snprintf(d + o, sizeof d - o, "<assembly><compatibility><application>");
        for (k = 0; k < 10; k++) o += (size_t)snprintf(d + o, sizeof d - o, "<supportedOS Id=\"{00000000-0000-0000-0000-00000000000%u}\"/>", k);
        o += (size_t)snprintf(d + o, sizeof d - o, "<supportedOS Id=\"{E2011457-1546-43C5-A5FE-008DEEE3D3F0}\"/>");
        o += (size_t)snprintf(d + o, sizeof d - o, "<supportedOS Id=\"{1f676c76-80e1-4239-95bb-83d0f6d0da78}\"/>");
        o += (size_t)snprintf(d + o, sizeof d - o, "<supportedOS/></application></compatibility></assembly>");
        r = pm(d, &m);
        CHECK_I(r, 0); CHECK_U(m.n_os, 8); CHECK_U(m.n_os_total, 12);
        CHECK_S(m.os_guid[0], "{00000000-0000-0000-0000-000000000000}");
        CHECK_U(m.os_mask, PE_MAN_OS_VISTA | PE_MAN_OS_WIN81);                    /* mask covers ALL seen, even past the 8 stored */
        r = pm("<assembly><compatibility><application><supportedOS Id=\"{E2011457-1546-43C5-A5FE-008DEEE3D3F0}\"/></application></compatibility></assembly>", &m);
        CHECK_S(m.os_guid[0], "{e2011457-1546-43c5-a5fe-008deee3d3f0}"); CHECK_U(m.os_mask, PE_MAN_OS_VISTA);
        r = pm("<assembly><application><supportedOS Id=\"{35138b9a-5d96-4fbd-8e2d-a2440225f93a}\"/></application></assembly>", &m);   /* wrong parent */
        CHECK_U(m.n_os_total, 0);
    }

    /* --- windowsSettings text --- */
    {
        const char *pre = "<assembly><application><windowsSettings>", *post = "</windowsSettings></application></assembly>";
        char d[4096];
        snprintf(d, sizeof d, "%s<dpiAware>  true/pm \n</dpiAware>%s", pre, post);
        r = pm(d, &m); CHECK_I(r, 0); CHECK_S(m.dpi_aware, "true/pm"); CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_PER_MONITOR);
        snprintf(d, sizeof d, "%s<dpiAware>t&amp;r<!-- c -->u<![CDATA[ignored]]>e</dpiAware>%s", pre, post);
        r = pm(d, &m); CHECK_I(r, 0); CHECK_S(m.dpi_aware, "t&rue");
        snprintf(d, sizeof d, "%s<dpiAware/><dpiAware>true</dpiAware>%s", pre, post);
        r = pm(d, &m); CHECK_I(r, 0); CHECK_I(m.has_dpi_aware, 1); CHECK_S(m.dpi_aware, "");                    /* first wins */
        snprintf(d, sizeof d, "%s<dpiAware>false</dpiAware><dpiAware>true</dpiAware>%s", pre, post);
        r = pm(d, &m); CHECK_S(m.dpi_aware, "false"); CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_UNAWARE);
        snprintf(d, sizeof d, "%s<dpiAware>true<x>child text</x>tail</dpiAware>%s", pre, post);
        r = pm(d, &m); CHECK_I(r, 0); CHECK_S(m.dpi_aware, "truetail");
        snprintf(d, sizeof d, "%s<dpiAware>true</dpiAware><dpiAwareness>unknown, system,permonitor</dpiAwareness>%s", pre, post);
        r = pm(d, &m); CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_SYSTEM);                                    /* awareness wins, first known token */
        snprintf(d, sizeof d, "%s<dpiAwareness>junk</dpiAwareness><dpiAware>True/PM</dpiAware>%s", pre, post);
        r = pm(d, &m); CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_PER_MONITOR);                               /* no known token: fall back */
        snprintf(d, sizeof d, "%s<dpiAware>per monitor</dpiAware>%s", pre, post);
        r = pm(d, &m); CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_PER_MONITOR);
        snprintf(d, sizeof d, "%s<dpiAwareness>PerMonitorV2</dpiAwareness>%s", pre, post);
        r = pm(d, &m); CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_PER_MONITOR_V2);
        snprintf(d, sizeof d, "%s<dpiAwareness>unaware</dpiAwareness>%s", pre, post);
        r = pm(d, &m); CHECK_I(pe_manifest_dpi_mode(&m), PE_MAN_DPI_UNAWARE);
        snprintf(d, sizeof d, "%s<activeCodePage>UTF-8</activeCodePage><longPathAware>TRUE</longPathAware>%s", pre, post);
        r = pm(d, &m); CHECK_S(m.active_code_page, "UTF-8"); CHECK_S(m.long_path_aware, "TRUE");
        /* wrong place */
        r = pm("<assembly><windowsSettings><dpiAware>true</dpiAware></windowsSettings><dpiAware>true</dpiAware></assembly>", &m);
        CHECK_I(r, 0); CHECK_I(m.has_dpi_aware, 0);
        /* text longer than the field: cut + flagged, scan continues */
        {
            size_t o = (size_t)snprintf(d, sizeof d, "%s<dpiAware>", pre);
            unsigned k;
            for (k = 0; k < 100; k++) d[o++] = 'z';
            snprintf(d + o, sizeof d - o, "</dpiAware>%s", post);
            r = pm(d, &m);
            CHECK_I(r, 0); CHECK_U(strlen(m.dpi_aware), 31); CHECK_U(m.truncated, 1);
        }
    }

    /* --- encodings --- */
    {
        static const u8 u8bom[] = { 0xEF, 0xBB, 0xBF, '<', 'a', 's', 's', 'e', 'm', 'b', 'l', 'y', '>', '<', 'a', 's', 's', 'e', 'm', 'b', 'l', 'y', 'I', 'd', 'e', 'n', 't', 'i', 't', 'y', ' ', 'n', 'a', 'm', 'e', '=', '"', 0xC3, 0xA9, 'x', 0xF0, 0x9F, 0x98, 0x80, '"', '/', '>', '<', '/', 'a', 's', 's', 'e', 'm', 'b', 'l', 'y', '>' };
        r = pm_bytes(u8bom, sizeof u8bom, &m);
        CHECK_I(r, 0); CHECK_I(m.encoding, PE_MAN_ENC_UTF8); CHECK_S(m.identity.name, "?x?");
        /* UTF-16LE with a BMP non-ASCII char, an ASCII char and a surrogate pair inside an attribute value */
        {
            static const char pre[] = "<assembly><assemblyIdentity name=\"", post[] = "\"/></assembly>";
            static const u16 mid[] = { 0x00E9, 'x', 0xD83D, 0xDE00 };
            size_t cap = 2 + 2 * (sizeof pre + sizeof post + 8), ul = 0, k;
            u8 *w = (u8 *)malloc(cap);
            w[ul++] = 0xFF; w[ul++] = 0xFE;
            for (k = 0; k + 1 < sizeof pre; k++) { w[ul++] = (u8)pre[k]; w[ul++] = 0; }
            for (k = 0; k < 4; k++) { w[ul++] = (u8)(mid[k] & 0xff); w[ul++] = (u8)(mid[k] >> 8); }
            for (k = 0; k + 1 < sizeof post; k++) { w[ul++] = (u8)post[k]; w[ul++] = 0; }
            r = pm_bytes(w, ul, &m);
            CHECK_I(r, 0); CHECK_I(m.encoding, PE_MAN_ENC_UTF16LE); CHECK_S(m.identity.name, "?x?");
            dump_file("res_utf16_surrogate.manifest", w, ul);
            free(w);
        }
        /* UTF-16BE refused; odd-length UTF-16 reported but everything before it extracted */
        {
            static const u8 be[] = { 0xFE, 0xFF, 0x00, '<', 0x00, 'a', 0x00, '/', 0x00, '>' };
            static const u8 be2[] = { 0x00, '<', 0x00, 'a', 0x00, '/', 0x00, '>' };
            r = pm_bytes(be, sizeof be, &m); CHECK_I(r, PE_E_MAN_ENCODING); CHECK_U(m.error_pos, 0); CHECK_I(m.well_formed, 0);
            r = pm_bytes(be2, sizeof be2, &m); CHECK_I(r, PE_E_MAN_ENCODING);
        }
        {
            const char *ascii = "<assembly><assemblyIdentity name=\"odd\"/></assembly>";
            size_t ul;
            u8 *w = to_utf16le_bom((const u8 *)ascii, strlen(ascii), &ul);
            dump_file("res_utf16_odd.manifest", w, ul + 1);
            r = pm_bytes(w, ul + 1, &m);                           /* one stray trailing byte */
            CHECK_I(r, PE_E_MAN_ENCODING); CHECK_I(m.well_formed, 0); CHECK_U(m.error_pos, ul);
            CHECK_S(m.identity.name, "odd");
            r = pm_bytes(w, ul - 1, &m);                           /* odd length, cut inside the last unit: truncated doc */
            CHECK(r == PE_E_MAN_TRUNC || r == PE_E_MAN_ENCODING || r == PE_E_MAN_SYNTAX);
            CHECK_I(m.well_formed, 0);
            r = pm_bytes(w, 3, &m);  CHECK(r != 0); CHECK_I(m.well_formed, 0);
            r = pm_bytes(w, 1, &m);  CHECK(r != 0);
            r = pm_bytes(w, 2, &m);  CHECK_I(r, PE_E_MAN_TRUNC);
            free(w);
        }
    }

    /* --- errors: exact codes and positions --- */
    expect_err("unclosed root", "<assembly>", PE_E_MAN_TRUNC, 10);
    expect_err("unclosed child", "<assembly><a>", PE_E_MAN_TRUNC, 13);
    expect_err("mismatched end tag", "<assembly></assemblx>", PE_E_MAN_SYNTAX, 10);
    expect_err("mismatched prefix", "<assembly><x:a></y:a></assembly>", PE_E_MAN_SYNTAX, 15);
    expect_err("second root", "<assembly/><assembly/>", PE_E_MAN_SYNTAX, 11);
    expect_err("junk after root", "<assembly/>junk", PE_E_MAN_SYNTAX, 11);
    expect_err("text before root", "text<assembly/>", PE_E_MAN_SYNTAX, 0);
    expect_err("end tag alone", "</assembly>", PE_E_MAN_SYNTAX, 0);
    expect_err("unquoted attr", "<assembly a=b/>", PE_E_MAN_SYNTAX, 12);
    expect_err("attr without =", "<assembly attr/>", PE_E_MAN_SYNTAX, 14);
    expect_err("lt in attr", "<assembly a=\"<\"/>", PE_E_MAN_SYNTAX, 13);
    expect_err("no space between attrs", "<assembly a=\"1\"b=\"2\"/>", PE_E_MAN_SYNTAX, 15);
    expect_err("bad name start", "<1abc/>", PE_E_MAN_SYNTAX, 1);
    expect_err("lone slash", "<assembly /x>", PE_E_MAN_SYNTAX, 10);
    expect_err("unterminated attr", "<assembly a=\"x", PE_E_MAN_TRUNC, 14);
    expect_err("unterminated tag", "<assembly a=\"x\"", PE_E_MAN_TRUNC, 15);
    expect_err("tag cut at <", "<assembly><", PE_E_MAN_TRUNC, 10);
    expect_err("unknown entity", "<assembly a=\"&bogus;\"/>", PE_E_MAN_ENTITY, 13);
    expect_err("entity no semicolon", "<assembly>&amp</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("entity cut", "<assembly>&amp", PE_E_MAN_TRUNC, 10);
    expect_err("empty entity", "<assembly>&;</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("entity with space", "<assembly>& amp;</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("numeric zero", "<assembly>&#0;</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("numeric empty hex", "<assembly>&#x;</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("numeric too big", "<assembly>&#x110000;</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("numeric surrogate", "<assembly>&#xD800;</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("numeric huge", "<assembly>&#99999999999999999999;</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("hex digit in decimal", "<assembly>&#12a;</assembly>", PE_E_MAN_ENTITY, 10);
    expect_err("doctype", "<!DOCTYPE assembly [<!ENTITY a \"b\">]><assembly/>", PE_E_MAN_DOCTYPE, 0);
    expect_err("doctype after decl", "<?xml version=\"1.0\"?><!DOCTYPE x><assembly/>", PE_E_MAN_DOCTYPE, 21);
    expect_err("other <!", "<assembly><!ENTITY x></assembly>", PE_E_MAN_SYNTAX, 10);
    expect_err("unterminated comment", "<assembly><!-- never closed", PE_E_MAN_TRUNC, 10);
    expect_err("comment close fake", "<assembly><!-->", PE_E_MAN_TRUNC, 10);
    expect_err("unterminated PI", "<?xml version=\"1.0\"", PE_E_MAN_TRUNC, 0);
    expect_err("unterminated CDATA", "<assembly><![CDATA[ x", PE_E_MAN_TRUNC, 10);
    expect_err("CDATA outside root", "<![CDATA[x]]><assembly/>", PE_E_MAN_SYNTAX, 0);
    expect_err("empty", "", PE_E_MAN_TRUNC, 0);
    expect_err("blank only", "  \n\t ", PE_E_MAN_TRUNC, 5);
    expect_err("comment only", "<!-- x -->", PE_E_MAN_TRUNC, 10);
    expect_err("root not assembly", "<a/>", PE_E_MAN_ROOT, 0);
    expect_err("root not assembly prefixed", "<x:a><b/></x:a>", PE_E_MAN_ROOT, 0);
    expect_err("unterminated end tag", "<assembly></assembly", PE_E_MAN_TRUNC, 20);
    expect_err("end tag junk", "<assembly></assembly x>", PE_E_MAN_SYNTAX, 21);
    {
        static const u8 nulmid[] = "<assembly>\0</assembly>";
        r = pm_bytes(nulmid, sizeof nulmid - 1, &m);
        CHECK_I(r, PE_E_MAN_SYNTAX); CHECK_U(m.error_pos, 10);
        r = pe_manifest_parse(NULL, 5, &m); CHECK_I(r, PE_E_MAN_TRUNC);
        r = pe_manifest_parse((const u8 *)"x", 1, NULL); CHECK_I(r, PE_E_BOUNDS);
    }
    CHECK_I(pe_manifest_parse((const u8 *)"<assembly/>", 0, &m), PE_E_MAN_TRUNC);

    /* --- caps --- */
    {
        char *s;
        size_t len;
        /* depth: root is level 1; 32 levels in total are accepted, 33 are not */
        s = nested_doc(31, &len); r = pm_bytes((u8 *)s, len, &m); CHECK_I(r, 0); CHECK_U(m.max_depth, 32); free(s);
        s = nested_doc(32, &len); r = pm_bytes((u8 *)s, len, &m); CHECK_I(r, PE_E_MAN_DEPTH); CHECK_U(m.error_pos, 10 + 3 * 31); free(s);
        s = nested_doc(100, &len); dump_file("res_deep100.manifest", (u8 *)s, len);
        r = pm_bytes((u8 *)s, len, &m); CHECK_I(r, PE_E_MAN_DEPTH); CHECK_I(m.well_formed, 0); free(s);
        s = nested_doc(5000, &len); r = pm_bytes((u8 *)s, len, &m); CHECK_I(r, PE_E_MAN_DEPTH); free(s);
        /* elements */
        {
            size_t cap = 16 + 4 * 9000, o = 0;
            unsigned k;
            s = (char *)malloc(cap);
            o += (size_t)snprintf(s + o, cap - o, "<assembly>");
            for (k = 0; k < 8191; k++) { memcpy(s + o, "<a/>", 4); o += 4; }
            o += (size_t)snprintf(s + o, cap - o, "</assembly>");
            r = pm_bytes((u8 *)s, o, &m); CHECK_I(r, 0); CHECK_U(m.n_elements, 8192);
            o -= 11; memcpy(s + o, "<a/></assembly>", 15); o += 15;
            r = pm_bytes((u8 *)s, o, &m); CHECK_I(r, PE_E_MAN_LIMIT); CHECK_I(m.well_formed, 0);
            free(s);
        }
        /* attributes per element: 32 ok, 33 not */
        {
            char d[2048];
            size_t o = 0;
            unsigned k;
            o += (size_t)snprintf(d + o, sizeof d - o, "<assembly");
            for (k = 0; k < 32; k++) o += (size_t)snprintf(d + o, sizeof d - o, " a%u=\"x\"", k);
            snprintf(d + o, sizeof d - o, "/>");
            r = pm(d, &m); CHECK_I(r, 0); CHECK_U(m.n_attributes, 32);
            o = 0;
            o += (size_t)snprintf(d + o, sizeof d - o, "<assembly");
            for (k = 0; k < 33; k++) o += (size_t)snprintf(d + o, sizeof d - o, " a%u=\"x\"", k);
            snprintf(d + o, sizeof d - o, "/>");
            r = pm(d, &m); CHECK_I(r, PE_E_MAN_LIMIT);
        }
        /* names: 128 chars ok, 129 not (element and attribute) */
        {
            char d[1024], nm[200];
            memset(nm, 'e', 129); nm[129] = 0;
            snprintf(d, sizeof d, "<assembly><%s/></assembly>", nm + 1);   /* 128 */
            r = pm(d, &m); CHECK_I(r, 0);
            snprintf(d, sizeof d, "<assembly><%s/></assembly>", nm);       /* 129 */
            r = pm(d, &m); CHECK_I(r, PE_E_MAN_LIMIT);
            snprintf(d, sizeof d, "<assembly %s=\"1\"/>", nm);
            r = pm(d, &m); CHECK_I(r, PE_E_MAN_LIMIT);
            memset(nm, 'p', 300 > sizeof nm ? sizeof nm - 1 : 199); nm[199] = 0;
            snprintf(d, sizeof d, "<%s:assembly/>", nm);
            r = pm(d, &m); CHECK_I(r, PE_E_MAN_LIMIT);
        }
        /* document size: exactly 64 KiB ok (UTF-8 and UTF-16), one more byte is refused without scanning */
        {
            size_t total = PE_MAN_MAX_DOC;
            u8 *b = (u8 *)malloc(total + 1);
            memset(b, ' ', total + 1);
            memcpy(b, "<assembly/>", 11);
            r = pm_bytes(b, total, &m); CHECK_I(r, 0);
            r = pm_bytes(b, total + 1, &m); CHECK_I(r, PE_E_MAN_TOOBIG); CHECK_U(m.error_pos, PE_MAN_MAX_DOC); CHECK_I(m.well_formed, 0);
            {
                size_t k;
                u8 *w = (u8 *)calloc(total, 1);
                w[0] = 0xFF; w[1] = 0xFE;
                for (k = 0; k < 11; k++) w[2 + 2 * k] = (u8)"<assembly/>"[k];
                for (k = 11; k + 1 < total / 2; k++) w[2 + 2 * k] = ' ';
                r = pm_bytes(w, total, &m); CHECK_I(r, 0);
                free(w);
            }
            free(b);
        }
    }

    /* --- entity bombs --- */
    {
        const char *lol = "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">"
                          "<!ENTITY lol3 \"&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;\">]><assembly><assemblyIdentity name=\"&lol3;\"/></assembly>";
        char *big;
        size_t k, o;
        dump_file("res_entity_bomb_dtd.manifest", (const u8 *)lol, strlen(lol));
        r = pm(lol, &m); CHECK_I(r, PE_E_MAN_DOCTYPE); CHECK_I(m.well_formed, 0);
        /* the same without a DTD: &lol3; is simply an unknown entity */
        r = pm("<assembly><assemblyIdentity name=\"&lol3;\"/></assembly>", &m); CHECK_I(r, PE_E_MAN_ENTITY);
        /* a flood of valid references: decoded into a bounded field, truncation reported, time linear */
        big = (char *)malloc(70000);
        o = (size_t)snprintf(big, 70000, "<assembly><assemblyIdentity name=\"");
        for (k = 0; k < 12000; k++) { memcpy(big + o, "&amp;", 5); o += 5; }
        o += (size_t)snprintf(big + o, 70000 - o, "\"/></assembly>");
        dump_file("res_entity_flood.manifest", (const u8 *)big, o);
        r = pm_bytes((u8 *)big, o, &m);
        CHECK_I(r, 0); CHECK_U(strlen(m.identity.name), 127); CHECK_I(m.identity.truncated, 1);
        CHECK(m.identity.name[0] == '&' && m.identity.name[126] == '&');
        /* the same flood as element text (capture) */
        o = (size_t)snprintf(big, 70000, "<assembly><application><windowsSettings><dpiAware>");
        for (k = 0; k < 12000; k++) { memcpy(big + o, "&lt;", 4); o += 4; }
        o += (size_t)snprintf(big + o, 70000 - o, "</dpiAware></windowsSettings></application></assembly>");
        r = pm_bytes((u8 *)big, o, &m);
        CHECK_I(r, 0); CHECK_U(strlen(m.dpi_aware), 31); CHECK_U(m.truncated, 1); CHECK(m.dpi_aware[0] == '<');
        free(big);
    }

    /* --- prefix truncation: every proper prefix of the realistic manifest is rejected, none crashes --- */
    if (good) {
        size_t root_end = 0;
        const char *needle = "</assembly>";
        u8 *w;
        size_t wl;
        for (i = 0; i + strlen(needle) <= ml; i++) if (memcmp(good + i, needle, strlen(needle)) == 0) root_end = i + strlen(needle);
        CHECK(root_end != 0);
        for (n = 0; n <= ml; n++) {
            r = pm_bytes(good, n, &m);
            g_checks++;
            if ((n < root_end && r == 0) || (n >= root_end && r != 0)) { g_fail++; printf("  FAIL: UTF-8 prefix %zu/%zu rc=%d\n", n, ml, r); }
            if (m.error_pos > n) { g_fail++; printf("  FAIL: error_pos %u > prefix %zu\n", m.error_pos, n); }
        }
        w = to_utf16le_bom(good, ml, &wl);
        for (n = 0; n <= wl; n++) {
            r = pm_bytes(w, n, &m);
            g_checks++;
            if (n < 2 + 2 * root_end && r == 0) { g_fail++; printf("  FAIL: UTF-16 prefix %zu/%zu rc=%d\n", n, wl, r); }
            if (n >= 2 + 2 * root_end && n % 2 == 0 && r != 0) { g_fail++; printf("  FAIL: UTF-16 prefix %zu/%zu rc=%d (complete doc)\n", n, wl, r); }
            if (n >= 2 + 2 * root_end && n % 2 == 1 && r != PE_E_MAN_ENCODING) { g_fail++; printf("  FAIL: UTF-16 odd prefix %zu rc=%d\n", n, r); }
            if (m.error_pos > n) { g_fail++; printf("  FAIL: UTF-16 error_pos %u > prefix %zu\n", m.error_pos, n); }
        }
        free(w);
    }

    /* --- the two helpers on hand-made structs --- */
    {
        pe_manifest_t *hm = (pe_manifest_t *)calloc(1, sizeof *hm);
        CHECK_I(pe_manifest_requires_elevation(NULL), PE_MAN_ELEV_NONE);
        CHECK_I(pe_manifest_dpi_mode(NULL), PE_MAN_DPI_UNSPECIFIED);
        hm->exec_levels_seen = 1u << PE_MAN_EXEC_HIGHEST;
        CHECK_I(pe_manifest_requires_elevation(hm), PE_MAN_ELEV_IF_AVAILABLE);
        hm->has_dpi_awareness = 1;
        memset(hm->dpi_awareness, 'x', sizeof hm->dpi_awareness - 1);                    /* one 63-char token, NUL-terminated */
        CHECK_I(pe_manifest_dpi_mode(hm), PE_MAN_DPI_UNSPECIFIED);
        free(hm);
    }
    free(good);
}

/* ------------------------------------------------------------------------------------------------ */
/* Group C: hostile resource trees                                                                    */
/* ------------------------------------------------------------------------------------------------ */
static int find_base(const img_t *L, unsigned type, unsigned id, const u8 **d, unsigned *sz)
{
    return pe_res_find(&L->info, L->img, type, id, 0, d, sz);
}

typedef struct { unsigned n; } cnt_t;
static int cnt_cb(const pe_res_entry_t *e, void *u)
{
    cnt_t *c = (cnt_t *)u;
    /* invariants of every reported leaf */
    if (e->type.str[PE_RES_NAME_MAX - 1] != 0 || e->name.str[PE_RES_NAME_MAX - 1] != 0) { g_fail++; printf("  FAIL: name not terminated\n"); }
    c->n++;
    return 0;
}

/* patch the resource data-directory entry of PE file `f`, load it, and judge pe_res_dir / pe_res_find */
static void dir_case(const u8 *f, size_t fl, const char *label, u32 rva, u32 size, int want_dir, int want_find)
{
    u8 *c = dup_exact(f, fl);
    img_t LL;
    unsigned a = 7, b = 7, sz = 7;
    const u8 *d = NULL;
    p32(c, PE_DIRO(2), rva);
    p32(c, PE_DIRO(2) + 4, size);
    if (load_img(c, fl, &LL) != PE_OK) {
        g_fail++;
        printf("  FAIL: [%s] did not load\n", label);
    } else {
        int rd = pe_res_dir(&LL.info, LL.img, &a, &b), rf = pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz);
        g_checks += 2;
        if (rd != want_dir) { g_fail++; printf("  FAIL: [%s] pe_res_dir=%d expected %d\n", label, rd, want_dir); }
        if (rf != want_find) { g_fail++; printf("  FAIL: [%s] pe_res_find=%d expected %d\n", label, rf, want_find); }
        if (want_dir != 0) CHECK(a == 0 && b == 0);
        if (want_find != 0) CHECK(d == NULL && sz == 0);
    }
    free_img(&LL);
    free(c);
}

static void test_hostile_resources(void)
{
    rt_t r;
    img_t L;
    const u8 *d = NULL;
    unsigned sz = 0, nv = 0;
    int rc;
    pe_res_entry_t e;

    printf("[C] hostile resource trees\n");

    /* sanity: the minimal tree resolves */
    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    CHECK_I(load_tree("res_base.exe", &r, RT_BASE_LEN, 0, &L), PE_OK);
    rc = find_base(&L, 24, 1, &d, &sz);
    CHECK_I(rc, 0); CHECK_U(sz, 8); CHECK(d && memcmp(d, "MANIFEST", 8) == 0);
    CHECK_I(find_base(&L, 24, 2, &d, &sz), PE_RES_NOTFOUND);
    CHECK_I(find_base(&L, 16, 1, &d, &sz), PE_RES_NOTFOUND);
    free_img(&L); rt_free(&r);

    /* --- loops / overlap / depth --- */
    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x20, 0, 1, RT_HIGH | 0x00);                                  /* type dir -> root */
    load_tree("res_loop_root.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_LOOP);
    CHECK_I(pe_res_enum(&L.info, L.img, NULL, cnt_cb, &(cnt_t){0}, &nv), PE_E_RES_LOOP);
    CHECK_I(find_base(&L, 24, 1, &d, &sz) < 0 && d == NULL && sz == 0, 1);
    free_img(&L); rt_free(&r);

    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x38, 0, 0x409, RT_HIGH | 0x20);                              /* lang dir -> type dir (a 3-cycle): stopped by the depth cap */
    load_tree("res_loop_cycle3.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_DEPTH);
    CHECK_I(pe_res_enum(&L.info, L.img, NULL, cnt_cb, &(cnt_t){0}, &nv), PE_E_RES_DEPTH);
    free_img(&L); rt_free(&r);

    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x00, 0, 24, RT_HIGH | 0x10);                                 /* child table starts inside the parent's entries */
    load_tree("res_overlap_parent.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_LOOP);
    free_img(&L); rt_free(&r);

    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x20, 0, 1, RT_HIGH | 0x30);                                  /* lang table starting inside its parent's entry area */
    load_tree("res_overlap_child.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_LOOP);
    free_img(&L); rt_free(&r);

    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x20, 0, 1, RT_HIGH | 0x28);                                  /* the same idea, but the bytes there read as a huge count */
    load_tree("res_overlap_child_count.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_LIMIT);
    free_img(&L); rt_free(&r);

    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x38, 0, 0x409, RT_HIGH | 0x50);                              /* directory where the data entry belongs */
    load_tree("res_depth4.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_DEPTH);
    CHECK_I(pe_res_enum(&L.info, L.img, NULL, cnt_cb, &(cnt_t){0}, &nv), PE_E_RES_DEPTH);
    free_img(&L); rt_free(&r);

    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x00, 0, 24, 0x50);                                           /* type entry is a data entry */
    load_tree("res_leaf_at_level1.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_FORMAT);
    CHECK_I(pe_res_enum(&L.info, L.img, NULL, cnt_cb, &(cnt_t){0}, &nv), PE_E_RES_FORMAT);
    free_img(&L); rt_free(&r);

    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x20, 0, 1, 0x50);                                            /* name entry is a data entry */
    load_tree("res_leaf_at_level2.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_FORMAT);
    free_img(&L); rt_free(&r);

    /* --- offsets past the end --- */
    {
        static const u32 bad_dir[] = { 0x68, 0x70, 0x1000, 0x7fffff00u, 0x7fffffffu - 15, 0x7fffffffu };
        unsigned k;
        for (k = 0; k < sizeof bad_dir / sizeof bad_dir[0]; k++) {
            rt_new(&r, RT_BASE_LEN); rt_base(&r);
            rt_ent(&r, 0x00, 0, 24, RT_HIGH | bad_dir[k]);
            load_tree("res_dir_offset_oob.exe", &r, RT_BASE_LEN, 0, &L);
            CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_BOUNDS);
            free_img(&L); rt_free(&r);
        }
    }
    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x38, 0, 0x409, 0x60);                                        /* data entry (16 bytes) runs past the tree end */
    load_tree("res_dataentry_oob.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_BOUNDS);
    free_img(&L); rt_free(&r);
    rt_new(&r, RT_BASE_LEN); rt_base(&r);
    rt_ent(&r, 0x38, 0, 0x409, 0x7fffffffu);
    load_tree("res_dataentry_oob2.exe", &r, RT_BASE_LEN, 0, &L);
    CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_E_RES_BOUNDS);
    free_img(&L); rt_free(&r);
    {
        struct { u32 rva, size; int want; } dv[] = {
            { RSRC_RVA + 0x60, 8, 0 },                    /* ok */
            { 0, 0, 0 },                                  /* empty at 0: allowed (inside the image) */
            { RSRC_RVA + 0x60, 0x10000, PE_E_RES_BOUNDS },
            { 0xFFFFF000u, 8, PE_E_RES_BOUNDS },          /* RVA past the image */
            { 0xFFFFFFF8u, 16, PE_E_RES_BOUNDS },         /* rva + size wraps 32 bits */
            { 8, 0xFFFFFFFFu, PE_E_RES_BOUNDS },
            { 0x3000, 0, 0 },                             /* rva == size_of_image, size 0: empty, still inside */
            { 0x3000, 1, PE_E_RES_BOUNDS },
            { 0x3001, 0, PE_E_RES_BOUNDS },
        };
        unsigned k;
        for (k = 0; k < sizeof dv / sizeof dv[0]; k++) {
            rt_new(&r, RT_BASE_LEN); rt_base(&r);
            rt_dat(&r, 0x50, dv[k].rva, dv[k].size, 0);
            load_tree("res_data_rva.exe", &r, RT_BASE_LEN, 0, &L);
            rc = find_base(&L, 24, 1, &d, &sz);
            CHECK_I(rc, dv[k].want);
            if (rc == 0) CHECK(in_image(&L, d, sz) && sz == dv[k].size);
            else CHECK(d == NULL && sz == 0);
            free_img(&L); rt_free(&r);
        }
    }

    /* --- counts: 100k entries, over the cap, beyond the tree --- */
    {
        static const struct { u32 named, ids; int want; } cn[] = {
            { 50000, 50000, PE_E_RES_LIMIT }, { 0, 65535, PE_E_RES_LIMIT }, { 65535, 65535, PE_E_RES_LIMIT },
            { 32768, 1, PE_E_RES_LIMIT }, { 0, 32768, PE_E_RES_BOUNDS },       /* at the cap: counted, then does not fit the tree */
            { 0, 1000, PE_E_RES_BOUNDS },
        };
        unsigned k;
        for (k = 0; k < sizeof cn / sizeof cn[0]; k++) {
            rt_new(&r, RT_BASE_LEN); rt_base(&r);
            rt_dir(&r, 0x00, cn[k].named, cn[k].ids);
            load_tree("res_count_100k.exe", &r, RT_BASE_LEN, 0, &L);
            CHECK_I(find_base(&L, 24, 1, &d, &sz), cn[k].want);
            CHECK_I(pe_res_enum(&L.info, L.img, NULL, cnt_cb, &(cnt_t){0}, &nv), cn[k].want);
            free_img(&L); rt_free(&r);
        }
        rt_new(&r, RT_BASE_LEN); rt_base(&r);
        rt_dir(&r, 0x38, 0, 0);                                              /* language directory with no entries */
        load_tree("res_empty_lang.exe", &r, RT_BASE_LEN, 0, &L);
        CHECK_I(find_base(&L, 24, 1, &d, &sz), PE_RES_NOTFOUND);
        free_img(&L); rt_free(&r);
    }

    /* --- a LARGE but valid directory: 20000 ids at the name level (below every cap) --- */
    {
        const u32 N = 20000;
        /* layout: root (1 entry: type 10) at 0x00; the NAME table (N ids) at 0x18; ONE shared language table behind it */
        u32 name_dir = 0x18, ldir = name_dir + 16 + 8 * N, dat = ldir + 24, blob = dat + 16, len = blob + 4, k;
        rt_new(&r, len);
        rt_dir(&r, 0x00, 0, 1); rt_ent(&r, 0x00, 0, PE_RT_RCDATA, RT_HIGH | name_dir);
        rt_dir(&r, name_dir, 0, N);
        for (k = 0; k < N; k++) rt_ent(&r, name_dir, k, 1000 + k, RT_HIGH | ldir);   /* all names share one language dir (legal DAG) */
        rt_dir(&r, ldir, 0, 1); rt_ent(&r, ldir, 0, 0x409, dat);
        rt_dat(&r, dat, RSRC_RVA + blob, 4, 0);
        memcpy(r.b + blob, "BIG!", 4);
        load_tree("res_large_valid.exe", &r, len, 0, &L);
        CHECK_I(pe_res_find(&L.info, L.img, 10, 1000, 0, &d, &sz), 0);
        CHECK(sz == 4 && memcmp(d, "BIG!", 4) == 0);
        CHECK_I(pe_res_find(&L.info, L.img, 10, 1000 + N - 1, 0, &d, &sz), 0);     /* the very last entry */
        CHECK_I(pe_res_find(&L.info, L.img, 10, 1000 + N, 0, &d, &sz), PE_RES_NOTFOUND);
        {
            cnt_t c = {0};
            rc = pe_res_enum(&L.info, L.img, NULL, cnt_cb, &c, &nv);
            CHECK_I(rc, 0); CHECK_U(nv, N); CHECK_U(c.n, N);
        }
        free_img(&L); rt_free(&r);
    }

    /* --- node budget: shared sub-directories make an enumeration explode; it must stop at the cap --- */
    {
        const u32 N = 20000;
        u32 name_dir = 16 + 8 * N, ldir = name_dir + 16 + 8 * N, dat = ldir + 24, blob = dat + 16, len = blob + 4, k;
        rt_new(&r, len);
        rt_dir(&r, 0x00, 0, N);
        for (k = 0; k < N; k++) rt_ent(&r, 0x00, k, 1 + k, RT_HIGH | name_dir);      /* N types sharing ONE name table */
        rt_dir(&r, name_dir, 0, N);
        for (k = 0; k < N; k++) rt_ent(&r, name_dir, k, 1 + k, RT_HIGH | ldir);      /* of N names sharing ONE language table */
        rt_dir(&r, ldir, 0, 1); rt_ent(&r, ldir, 0, 0x409, dat);
        rt_dat(&r, dat, RSRC_RVA + blob, 4, 0);
        load_tree("res_node_budget.exe", &r, len, 0, &L);
        CHECK_I(pe_res_find(&L.info, L.img, 7, 5, 0, &d, &sz), 0);                   /* a single lookup stays well inside the budget */
        {
            cnt_t c = {0};
            rc = pe_res_enum(&L.info, L.img, NULL, cnt_cb, &c, &nv);
            CHECK_I(rc, PE_E_RES_LIMIT);
            CHECK(nv == c.n && nv < PE_RES_MAX_NODES);
        }
        free_img(&L); rt_free(&r);
    }

    /* --- names: giant, out of range, bad length words --- */
    {
        /* root: 1 named entry + 1 id entry (24 -> normal tree below).  Layout (all offsets inside a 0x400 block):
         *   0x00 root(1 named,1 id)  0x20 type-24 dir  0x38 lang dir  0x50 data entry  0x60 blob  0x70.. name strings */
        u32 k;
        static const struct { u32 len_word; u32 str_off; int enum_rc; int named_find_rc; } nm[] = {
            { 3,      0x70,        0,                PE_RES_NOTFOUND },    /* sane 3-char name "ABC"-> matches? (see below) */
            { 0xFFFF, 0x70,        PE_E_RES_BOUNDS,  PE_RES_NOTFOUND },    /* giant: claims 128 KiB of characters */
            { 0x7FFF, 0x3F0,       PE_E_RES_BOUNDS,  PE_RES_NOTFOUND },    /* near the end of the block */
            { 3,      0x3FE,       PE_E_RES_BOUNDS,  PE_RES_NOTFOUND },    /* length word itself straddles the end */
            { 3,      0x7FFFFFFF,  PE_E_RES_BOUNDS,  PE_RES_NOTFOUND },    /* offset far outside */
        };
        for (k = 0; k < sizeof nm / sizeof nm[0]; k++) {
            rt_new(&r, 0x400); rt_base(&r);
            rt_dir(&r, 0x00, 1, 1);
            /* make room: move the id entry to slot 1, named entry in slot 0 */
            rt_ent(&r, 0x00, 1, 24, RT_HIGH | 0x20);
            rt_ent(&r, 0x00, 0, RT_HIGH | nm[k].str_off, RT_HIGH | 0x20);
            memset(r.b + 0x70, 0, 0x30);
            if (nm[k].str_off == 0x70) { rt_str(&r, 0x70, "ABC"); p16(r.b, 0x70, nm[k].len_word); }
            else if (nm[k].str_off + 2 <= 0x400) p16(r.b, nm[k].str_off, nm[k].len_word);
            load_tree("res_name_hostile.exe", &r, 0x400, 0, &L);
            CHECK_I(find_base(&L, 24, 1, &d, &sz), 0);                       /* numeric lookups never read the name */
            {
                pe_res_key_t t = pe_res_key_name("ABC");
                rc = pe_res_find_ex(&L.info, L.img, &t, NULL, 0, &e);
                if (k == 0) CHECK_I(rc, 0);                                    /* the sane name really matches */
                else CHECK(rc == PE_RES_NOTFOUND || rc == PE_E_RES_BOUNDS);
            }
            rc = pe_res_enum(&L.info, L.img, NULL, cnt_cb, &(cnt_t){0}, &nv);
            CHECK_I(rc, nm[k].enum_rc);
            free_img(&L); rt_free(&r);
        }
        /* a long but legal name renders truncated, with the true length reported */
        {
            u32 big = 200, j;
            rt_new(&r, 0x400); rt_base(&r);
            rt_dir(&r, 0x00, 1, 1);
            rt_ent(&r, 0x00, 1, 24, RT_HIGH | 0x20);
            rt_ent(&r, 0x00, 0, RT_HIGH | 0x70, RT_HIGH | 0x20);
            p16(r.b, 0x70, big);
            for (j = 0; j < big; j++) p16(r.b, 0x72 + 2 * j, j % 7 == 0 ? 0x20AC : 'a' + (j % 26));
            load_tree("res_name_long.exe", &r, 0x400, 0, &L);
            {
                pe_res_key_t t = pe_res_key_any();
                rc = pe_res_find_ex(&L.info, L.img, &t, NULL, 0, &e);          /* ANY: first entry = the named one */
                CHECK_I(rc, 0);
                CHECK(e.type.is_named && e.type.name_len == 200 && strlen(e.type.str) == PE_RES_NAME_MAX - 1);
                CHECK(e.type.str[0] == '?' && e.type.str[1] == 'b');
            }
            free_img(&L); rt_free(&r);
        }
    }

    /* --- language level details --- */
    {
        /* type 10 / id 7 with three language entries: one STRING-named, 0x409, 0x407; exercises scoring and "named lang -> 0" */
        u32 names = 0x18, langs = 0x30, str_at = langs + 16 + 24, dat0 = str_at + 8, blob0 = dat0 + 48, len = blob0 + 16, k;
        rt_new(&r, len);
        rt_dir(&r, 0x00, 0, 1); rt_ent(&r, 0x00, 0, 10, RT_HIGH | names);
        rt_dir(&r, names, 0, 1); rt_ent(&r, names, 0, 7, RT_HIGH | langs);
        rt_dir(&r, langs, 1, 2);
        rt_str(&r, str_at, "XX");
        rt_ent(&r, langs, 0, RT_HIGH | str_at, dat0);                          /* named language entry -> reports lang 0 */
        rt_ent(&r, langs, 1, 0x0409, dat0 + 16);
        rt_ent(&r, langs, 2, 0x0407, dat0 + 32);
        for (k = 0; k < 3; k++) { rt_dat(&r, dat0 + 16 * k, RSRC_RVA + blob0 + 4 * k, 4, 0); memcpy(r.b + blob0 + 4 * k, k == 0 ? "NMD!" : k == 1 ? "ENU!" : "DEU!", 4); }
        load_tree("res_lang_named.exe", &r, len, 0, &L);
        CHECK_I(pe_res_find_ex(&L.info, L.img, &(pe_res_key_t){0, 10}, &(pe_res_key_t){0, 7}, 0x0407, &e), 0);
        CHECK(e.lang == 0x0407 && memcmp(e.data, "DEU!", 4) == 0);
        CHECK_I(pe_res_find_ex(&L.info, L.img, &(pe_res_key_t){0, 10}, &(pe_res_key_t){0, 7}, 0x0000, &e), 0);   /* exact neutral == the named-language entry */
        CHECK(e.lang == 0 && memcmp(e.data, "NMD!", 4) == 0);
        CHECK_I(pe_res_find_ex(&L.info, L.img, &(pe_res_key_t){0, 10}, &(pe_res_key_t){0, 7}, 0x0411, &e), 0);   /* ja -> neutral */
        CHECK(e.lang == 0 && memcmp(e.data, "NMD!", 4) == 0);
        CHECK_I(pe_res_find_ex(&L.info, L.img, &(pe_res_key_t){0, 10}, &(pe_res_key_t){0, 7}, PE_RES_LANG_ANY, &e), 0);
        CHECK(memcmp(e.data, "NMD!", 4) == 0);                                                                  /* first entry */
        free_img(&L); rt_free(&r);
    }

    /* --- the data directory itself --- */
    {
        size_t fl;
        u8 *f;
        rt_new(&r, RT_BASE_LEN); rt_base(&r);
        f = make_pe(r.b, RT_BASE_LEN, 0, &fl);
        /* pe_parse does not validate the resource directory entry; pe_res_dir / pe_res_find must judge it */
        dir_case(f, fl, "dir ok",               RSRC_RVA,      RT_BASE_LEN, 0,                0);
        dir_case(f, fl, "dir rva past image",   0x7000000u,    0x100,       PE_E_BOUNDS,      PE_E_BOUNDS);
        dir_case(f, fl, "dir size past image",  RSRC_RVA,      0x10000000u, PE_E_BOUNDS,      PE_E_BOUNDS);
        dir_case(f, fl, "dir rva+size wraps",   0xFFFFFFF0u,   0x20,        PE_E_BOUNDS,      PE_E_BOUNDS);
        dir_case(f, fl, "dir size 0, rva set",  RSRC_RVA,      0,           PE_E_BOUNDS,      PE_E_BOUNDS);
        dir_case(f, fl, "dir rva 0, size set",  0,             0x100,       PE_E_BOUNDS,      PE_E_BOUNDS);
        dir_case(f, fl, "dir absent",           0,             0,           PE_RES_NOTFOUND,  PE_RES_NOTFOUND);
        dir_case(f, fl, "dir size 8 (< header)", RSRC_RVA,     8,           0,                PE_E_RES_BOUNDS);
        dir_case(f, fl, "dir size cuts the tree", RSRC_RVA,    0x30,        0,                PE_E_RES_BOUNDS);
        dir_case(f, fl, "dir over zero memory", RSRC_RVA + 0x800, 0x100,    0,                PE_RES_NOTFOUND);   /* all zero = empty root */
        free(f);
        rt_free(&r);
    }
    {
        size_t fl;
        u8 *f;
        img_t LL;
        rt_new(&r, RT_BASE_LEN); rt_base(&r);
        f = make_pe(r.b, RT_BASE_LEN, 0, &fl);
        p32(f, PE_OPT + 108, 2);                                             /* NumberOfRvaAndSizes = 2: no resource slot at all */
        CHECK_I(load_img(f, fl, &LL), PE_OK);
        CHECK_I(pe_res_dir(&LL.info, LL.img, NULL, NULL), PE_RES_NOTFOUND);
        CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_RES_NOTFOUND);
        CHECK_I(pe_res_enum(&LL.info, LL.img, NULL, cnt_cb, &(cnt_t){0}, &nv), PE_RES_NOTFOUND);
        CHECK_I(pe_manifest_from_image(&LL.info, LL.img, &(pe_manifest_t){0}), PE_RES_NOTFOUND);
        CHECK_I(pe_version_info(&LL.info, LL.img, 0, &(pe_version_t){0}), PE_RES_NOTFOUND);
        free_img(&LL);
        free(f); rt_free(&r);
    }

    /* --- corrupted pe_info_t / corrupted mapped headers --- */
    {
        img_t LL;
        pe_info_t bad;
        rt_new(&r, RT_BASE_LEN); rt_base(&r);
        load_tree("res_base2.exe", &r, RT_BASE_LEN, 0, &LL);
        CHECK_I(pe_res_find(NULL, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        CHECK_I(pe_res_find(&LL.info, NULL, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, NULL, NULL), 0);            /* both outputs optional */
        bad = LL.info; bad.size_of_image = 0;            CHECK_I(pe_res_find(&bad, LL.img, 24, 1, 0, &d, &sz), PE_E_SIZE);
        bad = LL.info; bad.size_of_image = PE_MAX_IMAGE + 0x1000u; CHECK_I(pe_res_find(&bad, LL.img, 24, 1, 0, &d, &sz), PE_E_SIZE);
        bad = LL.info; bad.size_of_headers = bad.size_of_image + 1; CHECK_I(pe_res_find(&bad, LL.img, 24, 1, 0, &d, &sz), PE_E_SIZE);
        bad = LL.info; bad.size_of_headers = 0x20;       CHECK_I(pe_res_find(&bad, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        bad = LL.info; bad.size_of_image = 0x2000;       /* smaller image: the directory (RVA 0x2000) no longer fits */
        CHECK_I(pe_res_find(&bad, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        bad = LL.info; bad.size_of_headers = 0x90;       /* headers end before the optional header */
        CHECK_I(pe_res_find(&bad, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        CHECK_I(pe_manifest_from_image(NULL, LL.img, &(pe_manifest_t){0}), PE_E_BOUNDS);
        CHECK_I(pe_manifest_from_image(&LL.info, LL.img, NULL), PE_E_BOUNDS);
        CHECK_I(pe_version_info(&LL.info, LL.img, 0, NULL), PE_E_BOUNDS);
        /* header corruption in the mapped image itself */
        LL.img[0] = 'X';                                  CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_MAGIC);
        LL.img[0] = 'M';
        p32(LL.img, 0x3c, 0xFFFFFFF0u);                   CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        p32(LL.img, 0x3c, 0x30);                          CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        p32(LL.img, 0x3c, 0x3F8);                         CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        p32(LL.img, 0x3c, 0x80);
        LL.img[0x80] = 'Q';                               CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_MAGIC);
        LL.img[0x80] = 'P';
        p16(LL.img, PE_OPT, 0x10b);                       CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_MAGIC);
        p16(LL.img, PE_OPT, 0x20b);
        p16(LL.img, PE_COFF + 16, 0xFFFF);                CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        p16(LL.img, PE_COFF + 16, 100);                   CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        p16(LL.img, PE_COFF + 16, 240);
        p32(LL.img, PE_OPT + 108, 17);                    CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), PE_E_BOUNDS);
        p32(LL.img, PE_OPT + 108, 16);
        CHECK_I(pe_res_find(&LL.info, LL.img, 24, 1, 0, &d, &sz), 0);                 /* restored: works again */
        free_img(&LL); rt_free(&r);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Group D: version resource                                                                          */
/* ------------------------------------------------------------------------------------------------ */
static int pv(const vbuf_t *v, unsigned lang, pe_version_t *out)
{
    u8 *c = dup_exact(v->b, v->len);
    int r = pe_version_parse(c, v->len, lang, out);
    free(c);
    return r;
}
static int pv_raw(const u8 *b, size_t n, unsigned lang, pe_version_t *out)
{
    u8 *c = dup_exact(b, n);
    int r = pe_version_parse(c, (unsigned)n, lang, out);
    free(c);
    return r;
}

static void test_version(void)
{
    vbuf_t v;
    pe_version_t o;
    int r;
    u32 i;

    printf("[D] version resource\n");
    vb_new(&v, 8192); vb_good(&v, "040904B0", "040704B0", 0x04B00409u);
    dump_file("res_version_good.bin", v.b, v.len);
    r = pv(&v, 0x0409, &o);
    CHECK_I(r, 0); CHECK_I(o.has_fixed, 1); CHECK_U(o.file_version_ms, 0x00010002); CHECK_U(o.product_version_ls, 0x00070008);
    CHECK(o.file_version[0] == 1 && o.file_version[3] == 4 && o.product_version[0] == 5 && o.product_version[3] == 8);
    CHECK_U(o.lang, 0x0409); CHECK_U(o.codepage, 0x04B0); CHECK_S(o.file_description, "Resource Test App"); CHECK_S(o.company_name, "Example Corp");
    CHECK_S(o.product_version_str, "5.6.7.8"); CHECK_S(o.original_filename, "");
    r = pv(&v, 0x0407, &o); CHECK_I(r, 0); CHECK_U(o.lang, 0x0407); CHECK_S(o.file_description, "Zweite Beschreibung"); CHECK_U(o.have_str, PE_VER_S_DESCRIPTION | PE_VER_S_PRODUCT);
    r = pv(&v, 0x0807, &o); CHECK_U(o.lang, 0x0407);                         /* de-CH: primary match */
    r = pv(&v, 0x0C0A, &o); CHECK_U(o.lang, 0x0409);                         /* es: Translation[0] */
    r = pv(&v, PE_RES_LANG_ANY, &o); CHECK_U(o.lang, 0x0409);
    vb_free(&v);

    /* Translation names the second table first */
    vb_new(&v, 8192); vb_good(&v, "040904B0", "040704B0", 0x04B00407u);
    r = pv(&v, 0x0411, &o); CHECK_I(r, 0); CHECK_U(o.lang, 0x0407);
    r = pv(&v, PE_RES_LANG_ANY, &o); CHECK_U(o.lang, 0x0407);
    vb_free(&v);

    /* table key not hex: still a table, lang unknown */
    vb_new(&v, 8192); vb_good(&v, "NOTAHEXKEY!", NULL, 0x04B00409u);
    r = pv(&v, 0x0409, &o); CHECK_I(r, 0); CHECK_I(o.has_strings, 1); CHECK_U(o.lang, 0xFFFFFFFFu); CHECK_S(o.product_name, "Res Test Product");
    vb_free(&v);

    /* no VarFileInfo / no StringFileInfo / no fixed info */
    {
        u32 root, sfi, st;
        vb_new(&v, 4096);
        root = vb_begin(&v, 0, 0, "VS_VERSION_INFO");                 /* no fixed info at all */
        vb_pad4(&v);
        sfi = vb_begin(&v, 0, 1, "StringFileInfo"); st = vb_begin(&v, 0, 1, "0409FDE9");
        vb_string(&v, "ProductName", "Only Strings"); vb_end(&v, st); vb_end(&v, sfi);
        vb_end(&v, root);
        r = pv(&v, 0, &o);
        CHECK_I(r, 0); CHECK_I(o.has_fixed, 0); CHECK_I(o.has_strings, 1); CHECK_S(o.product_name, "Only Strings"); CHECK_U(o.codepage, 0xFDE9);
        vb_free(&v);
        vb_new(&v, 4096);
        root = vb_begin(&v, 52, 0, "VS_VERSION_INFO");
        vb_w32(&v, 0xFEEF04BDu); vb_w32(&v, 0x10000); vb_w32(&v, 0x00020001u); vb_w32(&v, 0); vb_w32(&v, 0); vb_w32(&v, 0);
        vb_w32(&v, 0); vb_w32(&v, 0); vb_w32(&v, 0); vb_w32(&v, 0); vb_w32(&v, 0); vb_w32(&v, 0); vb_w32(&v, 0);
        vb_end(&v, root);
        r = pv(&v, 0, &o);
        CHECK_I(r, 0); CHECK_I(o.has_fixed, 1); CHECK_I(o.has_strings, 0); CHECK_U(o.file_version[0], 2); CHECK_U(o.file_version[1], 1);
        vb_free(&v);
    }

    /* hostile: bad signature / key / sizes */
    vb_new(&v, 8192); vb_good(&v, "040904B0", NULL, 0x04B00409u);
    {
        u8 *b = dup_exact(v.b, v.len);
        u32 sig_off = 6 + 2 * 16, strt;
        (void)strt;
        sig_off = (sig_off + 3) & ~3u;                                       /* value starts after key + padding */
        CHECK_U(g32(b, sig_off), 0xFEEF04BDu);
        p32(b, sig_off, 0xFEEF04BEu);
        r = pe_version_parse(b, (unsigned)v.len, 0, &o); CHECK_I(r, PE_E_VERSION); CHECK_I(o.has_fixed, 0);
        p32(b, sig_off, 0xFEEF04BDu);
        b[6] = 'X';
        r = pe_version_parse(b, (unsigned)v.len, 0, &o); CHECK_I(r, PE_E_VERSION);
        b[6] = 'V';
        p16(b, 2, 51);                                                        /* root value length 51 (< 52) */
        r = pe_version_parse(b, (unsigned)v.len, 0, &o); CHECK_I(r, PE_E_VERSION);
        p16(b, 2, 0xFFFF);                                                    /* value claims 64 KiB */
        r = pe_version_parse(b, (unsigned)v.len, 0, &o); CHECK(r == 0 || r == PE_E_VERSION);
        p16(b, 2, 52);
        p16(b, 0, 0xFFFF);                                                    /* root wLength too big: clamped (tolerated) */
        r = pe_version_parse(b, (unsigned)v.len, 0, &o); CHECK_I(r, 0); CHECK_S(o.product_name, "Res Test Product");
        p16(b, 0, 5);                                                         /* root wLength < 6 */
        r = pe_version_parse(b, (unsigned)v.len, 0, &o); CHECK_I(r, PE_E_VERSION);
        p16(b, 0, 40);                                                        /* root wLength cuts inside the value */
        r = pe_version_parse(b, (unsigned)v.len, 0, &o); CHECK_I(r, PE_E_VERSION);
        free(b);
    }
    /* every truncation length: no crash; complete only at full length (clamp makes some shorter cuts parse) */
    for (i = 0; i <= v.len; i++) {
        r = pv_raw(v.b, i, 0x0409, &o);
        g_checks++;
        if (r != 0 && r != PE_E_VERSION) { g_fail++; printf("  FAIL: truncation %u rc=%d\n", i, r); }
        if (i == v.len && r != 0) { g_fail++; printf("  FAIL: full blob rc=%d\n", r); }
        if (i < 6 && r == 0) { g_fail++; printf("  FAIL: %u-byte blob accepted\n", i); }
    }
    CHECK_I(pe_version_parse(NULL, 10, 0, &o), PE_E_VERSION);
    CHECK_I(pe_version_parse(v.b, 0, 0, &o), PE_E_VERSION);
    CHECK_I(pe_version_parse(v.b, (unsigned)v.len, 0, NULL), PE_E_BOUNDS);
    vb_free(&v);

    /* hostile children */
    {
        u32 root, sfi, st, s;
        /* child wLength beyond its parent */
        vb_new(&v, 4096);
        root = vb_begin(&v, 0, 0, "VS_VERSION_INFO");
        sfi = vb_begin(&v, 0, 1, "StringFileInfo"); st = vb_begin(&v, 0, 1, "040904B0");
        s = vb_begin(&v, 5, 1, "ProductName"); vb_wstr(&v, "abcd"); vb_end(&v, s);
        p16(v.b, s, 0x7000);
        vb_end(&v, st); vb_end(&v, sfi); vb_end(&v, root);
        r = pv(&v, 0, &o); CHECK_I(r, PE_E_VERSION);
        vb_free(&v);
        /* child wLength < 6 (would loop forever without the check) */
        vb_new(&v, 4096);
        root = vb_begin(&v, 0, 0, "VS_VERSION_INFO");
        sfi = vb_begin(&v, 0, 1, "StringFileInfo"); st = vb_begin(&v, 0, 1, "040904B0");
        s = vb_begin(&v, 5, 1, "ProductName"); vb_wstr(&v, "abcd"); vb_end(&v, s);
        p16(v.b, s, 0);
        vb_end(&v, st); vb_end(&v, sfi); vb_end(&v, root);
        r = pv(&v, 0, &o); CHECK_I(r, PE_E_VERSION);
        vb_free(&v);
        /* unterminated key: no NUL inside the node */
        vb_new(&v, 4096);
        root = vb_begin(&v, 0, 0, "VS_VERSION_INFO");
        sfi = vb_begin(&v, 0, 1, "StringFileInfo");
        v.len = sfi + 6;
        for (i = 0; i < 30; i++) vb_w16(&v, 'K');                            /* key never terminated */
        vb_end(&v, sfi); vb_end(&v, root);
        r = pv(&v, 0, &o); CHECK_I(r, PE_E_VERSION);
        vb_free(&v);
        /* key longer than the 64-unit cap (terminated) */
        vb_new(&v, 4096);
        root = vb_begin(&v, 0, 0, "VS_VERSION_INFO");
        sfi = vb_begin(&v, 0, 1, "StringFileInfo");
        s = vb_begin(&v, 0, 1, "040904B0");
        {
            char longkey[200];
            u32 lk;
            memset(longkey, 'L', 150); longkey[150] = 0;
            lk = vb_begin(&v, 0, 1, longkey);
            vb_end(&v, lk);
        }
        vb_end(&v, s); vb_end(&v, sfi); vb_end(&v, root);
        r = pv(&v, 0, &o); CHECK_I(r, PE_E_VERSION);
        vb_free(&v);
        /* strings: non-ASCII, control chars, missing NUL, vlen too big, vlen 0, duplicates, very long */
        vb_new(&v, 8192);
        root = vb_begin(&v, 0, 0, "VS_VERSION_INFO");
        sfi = vb_begin(&v, 0, 1, "StringFileInfo"); st = vb_begin(&v, 0, 1, "040904B0");
        s = vb_begin(&v, 6, 1, "ProductName"); vb_w16(&v, 'a'); vb_w16(&v, 0x20AC); vb_w16(&v, 0xD83D); vb_w16(&v, 0xDE00); vb_w16(&v, '\n'); vb_w16(&v, 0); vb_end(&v, s);
        s = vb_begin(&v, 400, 1, "FileDescription"); vb_w16(&v, 'x'); vb_w16(&v, 'y'); vb_end(&v, s);        /* vlen lies: bounded by node end, no NUL */
        s = vb_begin(&v, 0, 1, "CompanyName"); vb_wstr(&v, "ignored because vlen 0 means empty... "); vb_end(&v, s);
        s = vb_begin(&v, 3, 1, "FileVersion"); vb_wstr(&v, "1.0"); vb_end(&v, s);
        s = vb_begin(&v, 3, 1, "FileVersion"); vb_wstr(&v, "2.0"); vb_end(&v, s);                          /* duplicate: first wins */
        s = vb_begin(&v, 0, 1, "ProductVersion");
        for (i = 0; i < 300; i++) vb_w16(&v, 'v');
        vb_w16(&v, 0);
        v.b[s + 2] = 0xFF; v.b[s + 3] = 0x7F;                                                            /* vlen 32767 */
        vb_end(&v, s);
        vb_end(&v, st); vb_end(&v, sfi); vb_end(&v, root);
        r = pv(&v, 0x0409, &o);
        CHECK_I(r, 0);
        CHECK_S(o.product_name, "a??" " ");                                                              /* '\n' -> ' ' ; euro and the pair -> '?' each unit group */
        CHECK_S(o.file_description, "xy");
        CHECK_S(o.company_name, "");
        CHECK_S(o.file_version_str, "1.0");
        CHECK_U(strlen(o.product_version_str), 127); CHECK_U(o.truncated, PE_VER_S_PRODVER);
        vb_free(&v);
        /* too many nodes: > 2048 strings in one table */
        vb_new(&v, 1u << 20);
        root = vb_begin(&v, 0, 0, "VS_VERSION_INFO");
        sfi = vb_begin(&v, 0, 1, "StringFileInfo"); st = vb_begin(&v, 0, 1, "040904B0");
        for (i = 0; i < 2100; i++) vb_string(&v, "K", "v");
        vb_end(&v, st); vb_end(&v, sfi); vb_end(&v, root);
        r = pv(&v, 0, &o); CHECK_I(r, PE_E_VERSION);
        vb_free(&v);
        /* 20 tables: only the first 16 are remembered (and the later ones are skipped, not an error) */
        vb_new(&v, 1u << 16);
        root = vb_begin(&v, 0, 0, "VS_VERSION_INFO");
        sfi = vb_begin(&v, 0, 1, "StringFileInfo");
        for (i = 0; i < 20; i++) {
            char key[16];
            snprintf(key, sizeof key, "%04X04B0", 0x0400 + i);
            st = vb_begin(&v, 0, 1, key);
            vb_string(&v, "ProductName", key);
            vb_end(&v, st);
        }
        vb_end(&v, sfi); vb_end(&v, root);
        r = pv(&v, 0x0400 + 17, &o); CHECK_I(r, 0); CHECK_U(o.lang, 0x0400);                                 /* 17th not stored: falls back to the first */
        r = pv(&v, 0x0400 + 5, &o); CHECK_I(r, 0); CHECK_U(o.lang, 0x0405); CHECK_S(o.product_name, "040504B0");
        vb_free(&v);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Group E: dependency hints                                                                          */
/* ------------------------------------------------------------------------------------------------ */
static void test_dep_hints(void)
{
    pe_man_assembly_t a;
    pe_dep_hint_t h;
    int r;
    pe_manifest_t m;

    printf("[E] dependency hints\n");
    r = pm("<assembly><dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" "
           "processorArchitecture=\"*\" publicKeyToken=\"6595B64144CCF1DF\" language=\"*\"/></dependentAssembly>"
           "<dependentAssembly><assemblyIdentity name=\"My.Private.Lib\" version=\"1.0.0.0\" processorArchitecture=\"x86\"/></dependentAssembly>"
           "<dependentAssembly><assemblyIdentity name=\"Vendor.Thing\" version=\"2.1.0.0\" processorArchitecture=\"AMD64\" publicKeyToken=\"abcdef0123456789\"/></dependentAssembly>"
           "</dependency></assembly>", &m);
    CHECK_I(r, 0); CHECK_U(m.n_deps, 3);
    r = pe_manifest_dep_hint(&m.deps[0], &h);
    CHECK_I(r, 0); CHECK_I(h.kind, PE_DEP_KNOWN_SYSTEM); CHECK_I(h.arch_compatible, 1);
    CHECK_S(h.system_dll, "comctl32.dll");
    CHECK_S(h.sxs_prefix, "amd64_microsoft.windows.common-controls_6595b64144ccf1df_");
    CHECK_S(h.sxs_version, "6.0.0.0");
    CHECK_S(h.dll_candidate[0], "Microsoft.Windows.Common-Controls.dll");
    CHECK_S(h.dll_candidate[1], "Microsoft.Windows.Common-Controls\\Microsoft.Windows.Common-Controls.dll");
    CHECK_S(h.manifest_candidate[0], "Microsoft.Windows.Common-Controls.manifest");
    CHECK_S(h.manifest_candidate[1], "Microsoft.Windows.Common-Controls\\Microsoft.Windows.Common-Controls.manifest");
    r = pe_manifest_dep_hint(&m.deps[1], &h);
    CHECK_I(r, 0); CHECK_I(h.kind, PE_DEP_PRIVATE); CHECK_I(h.arch_compatible, 0); CHECK_S(h.sxs_prefix, ""); CHECK_S(h.system_dll, "");
    CHECK_S(h.dll_candidate[0], "My.Private.Lib.dll");
    r = pe_manifest_dep_hint(&m.deps[2], &h);
    CHECK_I(r, 0); CHECK_I(h.kind, PE_DEP_WINSXS); CHECK_I(h.arch_compatible, 1);
    CHECK_S(h.sxs_prefix, "amd64_vendor.thing_abcdef0123456789_");

    /* unsafe names: refused, `out` left empty */
    {
        static const char *bad[] = { "", "..", "..\\evil", "../evil", "a/b", "a\\b", "a:b", "a b", "a..b", ".hidden", "trailing.", "C:\\x", "name\x01", "n\xC3\xA9" };
        unsigned i;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            memset(&a, 0, sizeof a);
            snprintf(a.name, sizeof a.name, "%s", bad[i]);
            memset(&h, 0xAA, sizeof h);
            r = pe_manifest_dep_hint(&a, &h);
            CHECK_I(r, PE_E_MAN_UNSAFE);
            CHECK(h.kind == 0 && h.sxs_prefix[0] == 0 && h.dll_candidate[0][0] == 0 && h.dll_candidate[1][0] == 0 && h.manifest_candidate[0][0] == 0);
        }
        memset(&a, 0, sizeof a);
        memset(a.name, 'A', 127);                                            /* longest legal name */
        r = pe_manifest_dep_hint(&a, &h);
        CHECK_I(r, 0); CHECK_U(strlen(h.dll_candidate[1]), 127 + 1 + 127 + 4); CHECK_U(strlen(h.manifest_candidate[1]), 127 + 1 + 127 + 9);
        a.truncated = 1;                                                     /* a cut name may be a different file */
        CHECK_I(pe_manifest_dep_hint(&a, &h), PE_E_MAN_UNSAFE);
    }
    /* hand-built struct whose fields are NOT NUL-terminated: bounded reads only (exact-size heap block => ASan checks) */
    {
        pe_man_assembly_t *ha = (pe_man_assembly_t *)malloc(sizeof *ha);
        memset(ha, 'x', sizeof *ha);
        ha->truncated = 0;
        r = pe_manifest_dep_hint(ha, &h);
        CHECK(r == 0 || r == PE_E_MAN_UNSAFE);
        memset(ha->name, 'N', sizeof ha->name);
        ha->truncated = 0;
        r = pe_manifest_dep_hint(ha, &h);
        CHECK_I(r, 0); CHECK_U(strlen(h.dll_candidate[0]), 127 + 4);
        free(ha);
    }
    CHECK_I(pe_manifest_dep_hint(NULL, &h), PE_E_BOUNDS);
    CHECK_I(pe_manifest_dep_hint(&a, NULL), PE_E_BOUNDS);
}

/* ------------------------------------------------------------------------------------------------ */
/* Group F: error strings                                                                             */
/* ------------------------------------------------------------------------------------------------ */
static void test_strerror(void)
{
    static const int res_codes[] = { PE_RES_NOTFOUND, PE_E_RES_BOUNDS, PE_E_RES_FORMAT, PE_E_RES_LOOP, PE_E_RES_DEPTH, PE_E_RES_LIMIT, PE_E_VERSION };
    static const int man_codes[] = { PE_E_MAN_SYNTAX, PE_E_MAN_TRUNC, PE_E_MAN_ENCODING, PE_E_MAN_TOOBIG, PE_E_MAN_DEPTH, PE_E_MAN_LIMIT,
                                     PE_E_MAN_ENTITY, PE_E_MAN_DOCTYPE, PE_E_MAN_ROOT, PE_E_MAN_UNSAFE };
    int c;
    unsigned i, j;

    printf("[F] error strings\n");
    for (c = -80; c <= 3; c++) {                                  /* never NULL, never empty, for ANY code */
        const char *a = pe_res_strerror(c), *b = pe_manifest_strerror(c);
        CHECK(a && a[0] && b && b[0]);
    }
    for (i = 0; i < sizeof res_codes / sizeof res_codes[0]; i++)  /* every documented code has its own message */
        for (j = i + 1; j < sizeof res_codes / sizeof res_codes[0]; j++)
            CHECK(strcmp(pe_res_strerror(res_codes[i]), pe_res_strerror(res_codes[j])) != 0);
    for (i = 0; i < sizeof man_codes / sizeof man_codes[0]; i++) {
        for (j = i + 1; j < sizeof man_codes / sizeof man_codes[0]; j++)
            CHECK(strcmp(pe_manifest_strerror(man_codes[i]), pe_manifest_strerror(man_codes[j])) != 0);
        CHECK(strcmp(pe_manifest_strerror(man_codes[i]), pe_manifest_strerror(-1000)) != 0);   /* not the "unknown" fallback */
    }
    for (i = 0; i < sizeof res_codes / sizeof res_codes[0]; i++)
        CHECK(strcmp(pe_res_strerror(res_codes[i]), pe_res_strerror(-1000)) != 0);
    CHECK_S(pe_manifest_strerror(PE_E_RES_LOOP), pe_res_strerror(PE_E_RES_LOOP));              /* manifest layer defers to the resource layer */
    CHECK_S(pe_res_strerror(PE_E_ALIGN), pe_strerror(PE_E_ALIGN));                              /* ... which defers to pe.h */
    CHECK_S(pe_manifest_strerror(PE_OK), "ok");
}

/* ------------------------------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s res_hello.exe res_good.manifest\n", argv[0]); return 2; }
    g_dump_dir = getenv("RES_DUMP_DIR");
    test_fixture(argv[1], argv[2]);
    test_manifest(argv[2]);
    test_hostile_resources();
    test_version();
    test_dep_hints();
    test_strerror();
    if (g_fail) {
        printf("RES-HOST-TEST: FAIL (%d of %d checks failed)\n", g_fail, g_checks);
        return 1;
    }
    printf("RES-HOST-TEST: PASS checks=%d\n", g_checks);
    return 0;
}
