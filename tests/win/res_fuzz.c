/*
 * res_fuzz.c -- deterministic mutation fuzzer for userspace/lib/pe/pe_resources.[ch] and pe_manifest.[ch]
 * (run under ASan + UBSan; modelled on tests/win/pe_fuzz.c).
 *
 *   usage: res_fuzz [seed] res_hello.exe res_good.manifest
 *     seed   decimal or 0x-hex (default 0x9E3779B97F4A7C15); a run is fully deterministic for a given seed
 *   env:   RES_FUZZ_ITERS=N         iterations (default 300000)
 *          RES_FUZZ_CRASH=path      where the failing input is saved (default /tmp/res_fuzz_crash.bin)
 *          RES_FUZZ_INJECT=N        harness self-test: overrun the input buffer at iteration N (ASan path)
 *          RES_FUZZ_INJECT_UB=N     harness self-test: signed overflow at iteration N (UBSan path)
 *        and the run_res_tests.sh self-test builds this file against pe_manifest.c / pe_resources.c compiled with
 *        -DPE_MAN_INJECT_BUG=1 / -DPE_RES_INJECT_BUG=1 (a deliberate library bug) to prove the fuzzer finds it.
 *
 * Four targets, picked per iteration:
 *   PE    mutate a PE file (bit flips, boundary values in resource-tree words, inserts/deletes/truncation, chunk
 *         clones that create loops and shared nodes); when pe_parse accepts it, map it into an EXACT-size image
 *         and run pe_res_dir / pe_res_find_ex (id, name, any keys, random languages) / pe_res_enum (+ early
 *         stop) / pe_version_info / pe_manifest_from_image, then repeat with a corrupted pe_info_t.
 *   MAN   mutate manifest text (UTF-8, UTF-16LE, hostile seeds) with XML-aware operators and hand the scanner an
 *         exactly-sized heap copy; ASCII-only inputs are also parsed as UTF-16LE and must give the same answer.
 *   VER   mutate a VS_VERSIONINFO blob (field-aware 16-bit boundary values) -> pe_version_parse.
 *   HINT  random pe_man_assembly_t contents (unterminated fields, separators, "..") -> pe_manifest_dep_hint.
 * Every result is checked against the API's invariants (return code ranges, NUL-terminated printable ASCII
 * strings, counts within caps, pointers inside the image, consistent flags); any violation aborts with the
 * failing input saved to RES_FUZZ_CRASH.
 *
 * Success line:  RES-FUZZ: PASS iterations=N pe=.. man=.. ver=.. hint=.. ...
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "pe.h"
#include "pe_resources.h"
#include "pe_manifest.h"
#include "res_common.h"

#define DEFAULT_CRASH "/tmp/res_fuzz_crash.bin"
#define MAX_SEEDS 16
#define BIG_IMAGE (16u * 1024u * 1024u)
#define MAN_CAP 70000u                       /* a little over PE_MAN_MAX_DOC: the TOOBIG path stays reachable */

/* ------------------------------------------------------------------------------------------------ */
/* crash capture                                                                                      */
/* ------------------------------------------------------------------------------------------------ */
static const u8 *g_cur;
static size_t g_cur_len;
static u64 g_iter;
static const char *g_kind = "?";
static const char *g_crash_path = DEFAULT_CRASH;

static void save_crash(void)
{
    char msg[256];
    int fd, n;
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
    n = snprintf(msg, sizeof msg, "\nRES-FUZZ: FAIL -- iteration %llu, target %s, failing input saved to %s\n", g_iter, g_kind, g_crash_path);
    if (n > 0 && write(2, msg, (size_t)n) < 0) { /* nothing sensible to do */ }
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
    fprintf(stderr, "RES-FUZZ: invariant violated at iteration %llu (%s): %s\n", g_iter, g_kind, what);
    abort();
}

/* ------------------------------------------------------------------------------------------------ */
/* rng                                                                                                */
/* ------------------------------------------------------------------------------------------------ */
static u64 g_rng = 0x9E3779B97F4A7C15ull;
static u64 rnd64(void)
{
    g_rng ^= g_rng >> 12; g_rng ^= g_rng << 25; g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static u32 rnd(u32 n) { return n ? (u32)((rnd64() >> 32) % n) : 0; }

static const u32 k_interesting[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 0x0f, 0x10, 0x11, 0x14, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x50, 0x60, 0x68, 0x7f, 0x80, 0xff, 0x100,
    0x1ff, 0x200, 0x201, 0x3ff, 0x400, 0x7ff, 0x800, 0xfff, 0x1000, 0x2000, 0x2060, 0x3000, 0x7fff, 0x8000, 0xffff, 0x10000,
    0x7fffffffu, 0x80000000u, 0x80000001u, 0x80000010u, 0x80000018u, 0x80000020u, 0x80000038u, 0x80000050u, 0x8000ffffu,
    0x80007fffu, 0x8000ffffu, 0xc0000000u, 0xfffff000u, 0xffff0000u, 0xfffffff0u, 0xfffffff8u, 0xfffffffeu, 0xffffffffu,
    0x409, 0x407, 0x40c, 0x800, 0x8000, 0xffff8000u, 0x20ac, 0xd800, 0xdc00, 0xfeef04bdu, 0x04b00409u
};
#define N_INTERESTING (sizeof k_interesting / sizeof k_interesting[0])

static void put_le(u8 *b, size_t o, u32 w, u64 v) { u32 i; for (i = 0; i < w; i++) b[o + i] = (u8)(v >> (8 * i)); }
static u64 get_le(const u8 *b, size_t o, u32 w) { u64 v = 0; u32 i; for (i = 0; i < w; i++) v |= (u64)b[o + i] << (8 * i); return v; }

static u64 pick_value(u64 cur, u32 w)
{
    u64 v;
    switch (rnd(8)) {
    case 0: v = rnd64(); break;
    case 1: v = cur + (u64)(long long)(int)(rnd(9) - 4); break;
    case 2: v = cur ^ 0x80000000ull; break;                       /* flip the dir/named bit */
    case 3: v = cur ^ (1ull << rnd(w * 8)); break;
    case 4: v = cur + (rnd(2) ? 8u : 0 - (u64)8u); break;
    default: v = k_interesting[rnd(N_INTERESTING)]; break;
    }
    return w >= 8 ? v : (v & ((1ull << (w * 8)) - 1u));
}

/* ------------------------------------------------------------------------------------------------ */
/* seeds                                                                                              */
/* ------------------------------------------------------------------------------------------------ */
typedef struct { u8 *data; size_t len; u32 lo, hi, hdr_end; } seed_t;     /* lo..hi: the interesting region (resource section) */
static seed_t g_pe[MAX_SEEDS], g_man[MAX_SEEDS], g_ver[MAX_SEEDS];
static u32 n_pe, n_man, n_ver;

static void add_seed(seed_t *tab, u32 *n, const u8 *p, size_t len, u32 lo, u32 hi, u32 hdr_end)
{
    if (*n >= MAX_SEEDS) return;
    tab[*n].data = dup_exact(p, len);
    tab[*n].len = len;
    tab[*n].lo = lo; tab[*n].hi = hi; tab[*n].hdr_end = hdr_end;
    (*n)++;
}

static void add_pe_seed(const u8 *f, size_t len)
{
    pe_info_t info;
    u32 i, lo = 0, hi = 0;
    if (pe_parse(f, len, &info) != PE_OK) { fprintf(stderr, "RES-FUZZ: a PE seed does not parse\n"); exit(2); }
    for (i = 0; i < info.n_sections; i++)
        if (strcmp(info.sec[i].name, ".rsrc") == 0) { lo = info.sec[i].raw_off; hi = info.sec[i].raw_off + info.sec[i].raw_size; }
    if (hi == 0) { fprintf(stderr, "RES-FUZZ: a PE seed has no .rsrc\n"); exit(2); }
    add_seed(g_pe, &n_pe, f, len, lo, hi, g32(f, 0x3c) + 24 + 240 + 40u * info.n_sections);   /* end of the section table */
}

/* ------------------------------------------------------------------------------------------------ */
/* invariant checks                                                                                   */
/* ------------------------------------------------------------------------------------------------ */
static int printable_str(const char *s, size_t cap)
{
    size_t i;
    for (i = 0; i < cap; i++) {
        u8 c = (u8)s[i];
        if (c == 0) return 1;
        if (c < 0x20 || c > 0x7e) return 0;
    }
    return 0;                                                       /* not terminated inside the field */
}
#define STRCHK(f) do { if (!printable_str((f), sizeof(f))) die("string field not NUL-terminated printable ASCII: " #f); } while (0)

static void check_assembly(const pe_man_assembly_t *a)
{
    unsigned short v[4];
    unsigned k;
    STRCHK(a->name); STRCHK(a->version); STRCHK(a->type); STRCHK(a->processor_architecture);
    STRCHK(a->public_key_token); STRCHK(a->language);
    if (a->ver_valid) {
        if (sscanf(a->version, "%hu.%hu.%hu.%hu", &v[0], &v[1], &v[2], &v[3]) != 4) die("ver_valid but version does not parse");
        if (memcmp(v, a->ver, sizeof v) != 0) die("ver[] does not match version string");
    } else if (a->ver[0] | a->ver[1] | a->ver[2] | a->ver[3]) die("ver[] nonzero while !ver_valid");
    if (a->token_valid) {
        if (strlen(a->public_key_token) != 16) die("token_valid with wrong length");
        for (k = 0; k < 16; k++) if (!strchr("0123456789abcdefABCDEF", a->public_key_token[k])) die("token_valid with non-hex");
    }
}

static void check_hint(const pe_man_assembly_t *a)
{
    pe_dep_hint_t h;
    int r = pe_manifest_dep_hint(a, &h);
    unsigned i, j;
    const char *paths[4];
    if (r != 0 && r != PE_E_MAN_UNSAFE) die("dep_hint returned an unexpected code");
    if (r != 0) {
        if (h.kind != 0 || h.sxs_prefix[0] || h.dll_candidate[0][0] || h.dll_candidate[1][0] || h.manifest_candidate[0][0]) die("dep_hint left data in `out` on error");
        return;
    }
    paths[0] = h.dll_candidate[0]; paths[1] = h.dll_candidate[1]; paths[2] = h.manifest_candidate[0]; paths[3] = h.manifest_candidate[1];
    for (i = 0; i < 4; i++) {
        size_t n = strnlen(paths[i], PE_DEP_PATH_SZ);
        if (n == 0 || n >= PE_DEP_PATH_SZ) die("candidate path empty / unterminated");
        for (j = 0; j < n; j++) {
            char c = paths[i][j];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '\\'))
                die("candidate path contains an unsafe character");
            if (c == '.' && paths[i][j + 1] == '.') die("candidate path contains ..");
        }
        if (paths[i][0] == '\\' || paths[i][0] == '.') die("candidate path is absolute or hidden");
    }
    if (strnlen(h.sxs_prefix, PE_DEP_SXS_SZ) >= PE_DEP_SXS_SZ || strnlen(h.system_dll, sizeof h.system_dll) >= sizeof h.system_dll ||
        strnlen(h.sxs_version, sizeof h.sxs_version) >= sizeof h.sxs_version) die("hint string unterminated");
    if (h.kind < PE_DEP_PRIVATE || h.kind > PE_DEP_KNOWN_SYSTEM) die("hint kind out of range");
}

static u64 g_man_ok, g_man_rej, g_man_class[12];
static u64 g_hint_ok, g_hint_rej;

static void check_manifest(const pe_manifest_t *m, size_t len, int rc)
{
    unsigned i;
    if (rc != m->error) die("return code != out->error");
    if (rc != 0 && (rc > PE_E_MAN_SYNTAX || rc < PE_E_MAN_UNSAFE)) die("manifest error code out of range");
    if (rc == 0 && (!m->well_formed || !m->is_assembly)) die("rc==0 but !well_formed / !is_assembly");
    if (rc == PE_E_MAN_ROOT && (!m->well_formed || m->is_assembly)) die("ROOT inconsistent");
    if (rc != 0 && rc != PE_E_MAN_ROOT && m->well_formed) die("error with well_formed set");
    if (m->error_pos > len) die("error_pos beyond the input");
    if (rc == 0 && m->error_pos != 0) die("error_pos set on success");
    if (m->encoding < 0 || m->encoding > 2) die("encoding out of range");
    if (m->n_elements > PE_MAN_MAX_ELEMENTS || m->max_depth > PE_MAN_MAX_DEPTH || m->n_attributes > PE_MAN_MAX_ELEMENTS * PE_MAN_MAX_ATTRS) die("count above cap");
    if (m->n_deps > PE_MAN_MAX_DEPS || m->n_deps > m->n_deps_total || m->n_os > PE_MAN_MAX_OS || m->n_os > m->n_os_total) die("dep/os count inconsistent");
    if (m->exec_level < 0 || m->exec_level > PE_MAN_EXEC_UNKNOWN || (m->exec_levels_seen >> 5) != 0 || m->os_mask > 0x1f) die("enum/mask out of range");
    if (m->n_exec_level == 0 && (m->has_exec_level || m->exec_levels_seen)) die("exec level state inconsistent");
    if (m->has_exec_level && !(m->exec_levels_seen & (1u << m->exec_level))) die("first exec level not in seen mask");
    if (!m->has_identity && (m->identity.name[0] || m->identity.version[0] || m->identity.truncated)) die("identity data without has_identity");
    check_assembly(&m->identity);
    for (i = 0; i < m->n_deps; i++) { check_assembly(&m->deps[i]); check_hint(&m->deps[i]); }
    for (i = m->n_deps; i < PE_MAN_MAX_DEPS; i++) if (m->deps[i].name[0] || m->deps[i].version[0]) die("unused dep slot written");
    for (i = 0; i < m->n_os; i++) STRCHK(m->os_guid[i]);
    STRCHK(m->dpi_aware); STRCHK(m->dpi_awareness); STRCHK(m->long_path_aware); STRCHK(m->active_code_page);
    if ((!m->has_dpi_aware && m->dpi_aware[0]) || (!m->has_dpi_awareness && m->dpi_awareness[0]) ||
        (!m->has_long_path_aware && m->long_path_aware[0]) || (!m->has_active_code_page && m->active_code_page[0])) die("text without has_* flag");
    i = (unsigned)pe_manifest_requires_elevation(m);
    if (i > PE_MAN_ELEV_IF_AVAILABLE) die("elevation out of range");
    if ((m->exec_levels_seen & (1u << PE_MAN_EXEC_REQUIRE_ADMIN)) && i != PE_MAN_ELEV_REQUIRED) die("elevation not conservative");
    i = (unsigned)pe_manifest_dpi_mode(m);
    if (i > PE_MAN_DPI_PER_MONITOR_V2) die("dpi mode out of range");
    if (rc == 0) g_man_ok++; else { g_man_rej++; g_man_class[(unsigned)(PE_E_MAN_SYNTAX - rc) < 12u ? (unsigned)(PE_E_MAN_SYNTAX - rc) : 11u]++; }
}

static void check_version(const pe_version_t *v, int rc)
{
    if (rc != 0 && rc != PE_E_VERSION && rc != PE_E_BOUNDS) die("version rc out of range");
    STRCHK(v->file_description); STRCHK(v->product_name); STRCHK(v->company_name);
    STRCHK(v->original_filename); STRCHK(v->file_version_str); STRCHK(v->product_version_str);
    if (v->truncated & ~v->have_str) die("truncated bit without have_str");
    if (v->have_str && !v->has_strings) die("have_str without has_strings");
    if (v->has_fixed) {
        if (v->file_version[0] != (v->file_version_ms >> 16) || v->file_version[1] != (v->file_version_ms & 0xffff) ||
            v->file_version[2] != (v->file_version_ls >> 16) || v->file_version[3] != (v->file_version_ls & 0xffff)) die("file_version mismatch");
        if (v->product_version[0] != (v->product_version_ms >> 16) || v->product_version[3] != (v->product_version_ls & 0xffff)) die("product_version mismatch");
    } else if (v->file_version_ms | v->file_version_ls | v->file_version[0]) die("fixed data without has_fixed");
    if (!v->has_strings && (v->lang != 0xFFFFFFFFu || v->codepage != 0xFFFFFFFFu)) die("lang set without strings");
}

static int in_image(const u8 *img, u32 isz, const u8 *p, u32 n)
{
    return p >= img && (u64)(p - img) <= isz && (u64)n <= (u64)isz - (u64)(p - img);
}

static void check_entry(const pe_res_entry_t *e, const u8 *img, u32 isz)
{
    if (!in_image(img, isz, e->data, e->size)) die("resource data outside the image");
    if (e->data != img + e->rva) die("data != image + rva");
    if ((u64)e->rva + e->size > isz) die("rva+size past the image");
    if (e->lang > 0xffff) die("lang above 16 bits");
    if (!printable_str(e->type.str, sizeof e->type.str) || !printable_str(e->name.str, sizeof e->name.str)) die("entry name string bad");
    if ((e->type.is_named && strlen(e->type.str) > PE_RES_NAME_MAX - 1) || (!e->type.is_named && e->type.str[0])) die("type id/name flags inconsistent");
    if ((e->name.is_named && strlen(e->name.str) > PE_RES_NAME_MAX - 1) || (!e->name.is_named && e->name.str[0])) die("name id/name flags inconsistent");
    if (e->type.is_named && strlen(e->type.str) > e->type.name_len) die("name_len shorter than the rendered string");
}

/* ------------------------------------------------------------------------------------------------ */
/* the PE pipeline                                                                                    */
/* ------------------------------------------------------------------------------------------------ */
static u64 g_pe_mapped, g_pe_find_ok, g_pe_enum_ok, g_pe_enum_err, g_pe_leaves, g_pe_man, g_pe_ver, g_pe_rej, g_pe_big;
static u64 g_rc_stat[32];

typedef struct { u32 n, stop_at; const u8 *img; u32 isz; } fuzz_enum_t;
static int fuzz_enum_cb(const pe_res_entry_t *e, void *u)
{
    fuzz_enum_t *f = (fuzz_enum_t *)u;
    check_entry(e, f->img, f->isz);
    f->n++;
    return f->stop_at && f->n >= f->stop_at;
}

static int rc_in_range(int r) { return r == 0 || r == 1 || (r >= -12 && r < 0) || (r >= -25 && r <= -20); }

static void exercise(const pe_info_t *in, const u8 *img)
{
    static const char *const names[] = { "GREETING", "greeting", "MYTYPE", "FOO", "ABC", "A", "", "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ" };
    static const u32 langs[] = { 0, 0x409, 0x407, 0x40c, 0x809, 0x411, PE_RES_LANG_ANY, 0xffff, 0x10409 };
    static const u32 types[] = { 24, 16, 10, 6, 1, 3, 14, 0, 0x7fffffff, PE_RES_ID_ANY };
    static const u32 ids[] = { 1, 2, 200, 300, 0, 17, 0xffff, PE_RES_ID_ANY };
    const u32 isz = in->size_of_image;
    pe_res_entry_t e;
    pe_res_key_t kt, kn;
    pe_manifest_t *m = (pe_manifest_t *)malloc(sizeof *m);
    pe_version_t *v = (pe_version_t *)malloc(sizeof *v);
    unsigned rva = 0, size = 0, nv = 0, k;
    const u8 *d;
    unsigned sz;
    int r;

    r = pe_res_dir(in, img, &rva, &size);
    if (!rc_in_range(r)) die("pe_res_dir rc out of range");
    g_rc_stat[r < 0 ? -r : 0]++;
    if (r == 0 && (!rva || !size || (u64)rva + size > isz)) die("pe_res_dir returned a bad range");
    if (r != 0 && (rva | size)) die("pe_res_dir wrote outputs on failure");

    for (k = 0; k < 6; k++) {
        u32 pref = langs[rnd(sizeof langs / sizeof langs[0])];
        switch (rnd(4)) {
        case 0: kt = pe_res_key_id(types[rnd(sizeof types / sizeof types[0])]); kn = pe_res_key_id(ids[rnd(sizeof ids / sizeof ids[0])]); break;
        case 1: kt = pe_res_key_id(PE_RT_RCDATA); kn = pe_res_key_name(names[rnd(sizeof names / sizeof names[0])]); break;
        case 2: kt = pe_res_key_name(names[rnd(sizeof names / sizeof names[0])]); kn = pe_res_key_id(ids[rnd(sizeof ids / sizeof ids[0])]); break;
        default: kt = pe_res_key_any(); kn = rnd(2) ? pe_res_key_any() : pe_res_key_name(names[rnd(sizeof names / sizeof names[0])]); break;
        }
        r = pe_res_find_ex(in, img, &kt, &kn, pref, &e);
        if (!rc_in_range(r)) die("pe_res_find_ex rc out of range");
        g_rc_stat[r < 0 ? -r : 0]++;
        if (r == 0) { check_entry(&e, img, isz); g_pe_find_ok++; }
        else if (e.data || e.size || e.rva) die("find left data in `out` on failure");
        r = pe_res_find(in, img, kt.name ? PE_RT_RCDATA : kt.id, kn.name ? PE_RES_ID_ANY : kn.id, pref, &d, &sz);
        if (!rc_in_range(r)) die("pe_res_find rc out of range");
        if (r == 0 && !in_image(img, isz, d, sz)) die("pe_res_find data outside the image");
        if (r != 0 && (d || sz)) die("pe_res_find outputs set on failure");
    }

    {
        fuzz_enum_t f;
        pe_res_key_t t;
        f.n = 0; f.stop_at = rnd(3) == 0 ? 1 + rnd(5) : 0; f.img = img; f.isz = isz;
        if (rnd(2)) r = pe_res_enum(in, img, NULL, fuzz_enum_cb, &f, &nv);
        else { t = pe_res_key_id(types[rnd(sizeof types / sizeof types[0])]); r = pe_res_enum(in, img, &t, fuzz_enum_cb, &f, &nv); }
        if (!rc_in_range(r)) die("pe_res_enum rc out of range");
        if (nv != f.n) die("n_visited != callback count");
        if (nv >= PE_RES_MAX_NODES) die("enumeration exceeded the node budget");
        g_pe_leaves += nv;
        if (r == 0) g_pe_enum_ok++; else g_pe_enum_err++;
        g_rc_stat[r < 0 ? -r : 0]++;
    }

    r = pe_version_info(in, img, langs[rnd(sizeof langs / sizeof langs[0])], v);
    if (!rc_in_range(r)) die("pe_version_info rc out of range");
    check_version(v, (r == 0 || r == PE_E_VERSION) ? r : 0);              /* resource-level failures leave `out` empty */
    if (r == 0) g_pe_ver++;

    r = pe_manifest_from_image(in, img, m);
    if (r == 0 || (r <= PE_E_MAN_SYNTAX && r >= PE_E_MAN_UNSAFE)) {      /* a manifest was found and scanned */
        check_manifest(m, PE_MAN_MAX_DOC + 1u, r);
        if (r == 0) g_pe_man++;
    } else {                                                              /* none, or a resource-level failure: `out` is empty */
        if (!rc_in_range(r)) die("pe_manifest_from_image rc out of range");
        if (m->well_formed || m->error || m->n_elements || m->has_identity) die("manifest outputs set although no manifest was scanned");
    }
    free(m);
    free(v);
}

static void run_pe(const u8 *file, size_t len)
{
    pe_info_t info;
    int r = pe_parse(file, len, &info);
    if (r < -12 || r > 0) die("pe_parse rc out of range");
    if (r != PE_OK) { g_pe_rej++; return; }
    if (info.size_of_image > BIG_IMAGE) { g_pe_big++; return; }
    {
        u8 *img = (u8 *)calloc(info.size_of_image, 1);       /* exactly size_of_image bytes */
        r = pe_map(file, len, &info, img);
        if (r != PE_OK) { free(img); return; }
        g_pe_mapped++;
        exercise(&info, img);

        if (rnd(4) == 0) {                                    /* round 2: the same calls over a corrupted pe_info_t */
            pe_info_t bad = info;
            u32 nm = 1 + rnd(3), i;
            for (i = 0; i < nm; i++) {
                switch (rnd(6)) {
                case 0: bad.size_of_image = (u32)pick_value(bad.size_of_image, 4); break;
                case 1: bad.size_of_headers = (u32)pick_value(bad.size_of_headers, 4); break;
                case 2: bad.size_of_image = info.size_of_image - (info.size_of_image ? rnd(info.size_of_image) : 0); break;
                case 3: bad.size_of_image = 0x1000 + rnd(info.size_of_image); break;
                case 4: bad.n_sections = (unsigned short)pick_value(bad.n_sections, 2); break;
                default: bad.is_dll = rnd(2); break;
                }
            }
            if (bad.size_of_image > info.size_of_image) bad.size_of_image = info.size_of_image;    /* never claim more than we allocated */
            exercise(&bad, img);
        }
        free(img);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* mutation                                                                                           */
/* ------------------------------------------------------------------------------------------------ */
static size_t pos_in(u32 lo, u32 hi, size_t len)
{
    if (hi > len) hi = (u32)len;
    if (lo >= hi) return len ? rnd((u32)len) : 0;
    return lo + rnd(hi - lo);
}

static size_t mutate_pe(const seed_t *s, u8 *buf, size_t len, size_t cap)
{
    u32 nmut = 1 + rnd(5), m;
    if (rnd(10) == 0) nmut += rnd(8);
    for (m = 0; m < nmut && len > 8; m++) {
        u32 kind = rnd(100);
        size_t p;
        if (kind < 40) {                                              /* 32-bit word in the resource section <- boundary value */
            p = pos_in(s->lo, s->hi, len) & ~(size_t)3;
            if (p + 4 <= len) put_le(buf, p, 4, pick_value(get_le(buf, p, 4), 4));
        } else if (kind < 52) {                                       /* 16-bit word (counts, language ids, string lengths) */
            p = pos_in(s->lo, s->hi, len) & ~(size_t)1;
            if (p + 2 <= len) put_le(buf, p, 2, pick_value(get_le(buf, p, 2), 2));
        } else if (kind < 60) {                                       /* bit flip in the section */
            p = pos_in(s->lo, s->hi, len);
            buf[p] ^= (u8)(1u << rnd(8));
        } else if (kind < 66) {                                       /* random byte in the section */
            buf[pos_in(s->lo, s->hi, len)] = (u8)rnd(256);
        } else if (kind < 76) {                                       /* clone a chunk over another place in the section: loops / shared nodes */
            size_t a = pos_in(s->lo, s->hi, len), b = pos_in(s->lo, s->hi, len), n = 8 + rnd(56);
            if (a + n <= len && b + n <= len) memmove(buf + b, buf + a, n);
        } else if (kind < 82) {                                       /* headers: data directory / section table / optional header */
            p = rnd(s->hdr_end < len ? s->hdr_end : (u32)len);
            if (rnd(2)) buf[p] ^= (u8)(1u << rnd(8)); else buf[p] = (u8)rnd(256);
        } else if (kind < 86) {                                       /* resource data-directory entry specifically (VA, size) */
            u32 lf = (u32)get_le(buf, 0x3c, 4);
            size_t o = (size_t)lf + 24 + 112 + 16;
            if (o + 8 <= len) put_le(buf, o + 4 * rnd(2), 4, pick_value(get_le(buf, o, 4), 4));
        } else if (kind < 90) {                                       /* insert */
            u32 n = 1 + rnd(16);
            p = pos_in(s->lo, s->hi, len);
            if (len + n <= cap && p <= len) {
                u32 i;
                memmove(buf + p + n, buf + p, len - p);
                for (i = 0; i < n; i++) buf[p + i] = (u8)rnd(256);
                len += n;
            }
        } else if (kind < 94) {                                       /* delete */
            u32 n = 1 + rnd(16);
            p = pos_in(s->lo, s->hi, len);
            if (p + n <= len) { memmove(buf + p, buf + p + n, len - p - n); len -= n; }
        } else if (kind < 98) {                                       /* truncate */
            switch (rnd(3)) {
            case 0: len = len - 1; break;
            case 1: len = s->lo + rnd(len > s->lo ? (u32)(len - s->lo) : 1); break;
            default: len = len > 16 ? len - rnd(16) : len; break;
            }
        } else {                                                      /* 8 x 0xFF / 0x00 in the section */
            u8 v = rnd(2) ? 0xff : 0;
            u32 i;
            p = pos_in(s->lo, s->hi, len);
            for (i = 0; i < 8 && p + i < len; i++) buf[p + i] = v;
        }
    }
    return len;
}

#define TOK(lit) { lit, (u8)(sizeof(lit) - 1) }
static const struct { const char *s; u8 n; } k_tok[] = {
    TOK("&abcdefghijklmnopqrstuvwxyz;"), TOK("&#00000000000000000000;"), TOK("<a><a><a><a><a><a><a><a>"), TOK("<a b=\"1\" c=\"2\" d=\"3\" e=\"4\" f=\"5\" g=\"6\" h=\"7\"/>"),
    { "<", 1 }, { ">", 1 }, { "</", 2 }, { "/>", 2 }, { "<!--", 4 }, { "-->", 3 }, { "<![CDATA[", 9 }, { "]]>", 3 }, { "<?", 2 }, { "?>", 2 },
    { "&amp;", 5 }, { "&lt;", 4 }, { "&#x41;", 6 }, { "&#0;", 4 }, { "&bogus;", 7 }, { "&", 1 }, { ";", 1 }, { "\"", 1 }, { "'", 1 }, { "=", 1 }, { " ", 1 },
    { "\n", 1 }, { "xmlns:", 6 }, { "xmlns=\"x\"", 9 }, { "<a>", 3 }, { "</a>", 4 }, { "<a/>", 4 }, { "<assembly>", 10 }, { "</assembly>", 11 },
    { "<assemblyIdentity ", 18 }, { "<dependentAssembly>", 19 }, { "</dependentAssembly>", 20 }, { "<dependency>", 12 },
    { "<requestedExecutionLevel level=\"requireAdministrator\" uiAccess=\"true\"/>", 66 },
    { "<supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"/>", 54 }, { "<dpiAware>true</dpiAware>", 25 }, { "<!DOCTYPE x>", 12 },
    { "name=\"", 6 }, { "version=\"1.2.3.4\"", 17 }, { "publicKeyToken=\"6595b64144ccf1df\"", 33 }, { "\0", 1 }, { "\xef\xbb\xbf", 3 }, { "\xff\xfe", 2 },
    { "\xc3\xa9", 2 }, { "\xf0\x9f\x98\x80", 4 }, { "\x80", 1 }, { "\xff", 1 }, { "x:", 2 }, { "..", 2 }, { "<x:a xmlns:x=\"u\">", 18 }, { "&#xD800;", 8 },
    { "&#99999999999;", 14 }, { "<trustInfo><security><requestedPrivileges>", 43 }, { "<application><windowsSettings>", 30 },
};
#define N_TOK (sizeof k_tok / sizeof k_tok[0])

/* "flood" operators (UTF-8 only): drive the document / nesting / element / attribute / name caps */
static size_t flood_text(u8 *buf, size_t len, size_t cap)
{
    size_t p = rnd((u32)len), q;
    u32 which = rnd(5), i, n;
    char tmp[1200];
    size_t ins = 0;

    switch (which) {
    case 0:                                                           /* 25..40 attributes right after some tag name */
        for (q = p; q < len && buf[q] != '<'; q++) {}
        for (q = q + 1; q < len && buf[q] != ' ' && buf[q] != '>' && buf[q] != '/'; q++) {}
        p = q < len ? q : len;
        n = 25 + rnd(16);
        for (i = 0; i < n && ins + 16 < sizeof tmp; i++) ins += (size_t)snprintf(tmp + ins, sizeof tmp - ins, " a%u=\"1\"", i);
        break;
    case 1:                                                           /* an over-long name: 100..140 extra characters after a '<' */
        for (q = p; q < len && buf[q] != '<'; q++) {}
        p = q < len ? q + 1 : q;
        n = 100 + rnd(41);
        for (i = 0; i < n; i++) tmp[ins++] = 'x';
        break;
    case 2:                                                           /* many nested openers */
        n = 25 + rnd(15);
        for (i = 0; i < n; i++) { memcpy(tmp + ins, "<a>", 3); ins += 3; }
        break;
    case 3: {                                                         /* element flood: thousands of <a/> */
        size_t total = (size_t)(1500 + rnd(9000)) * 4;
        if (len + total > cap) return len;
        for (q = p; q < len && buf[q] != '>'; q++) {}
        p = q < len ? q + 1 : q;
        memmove(buf + p + total, buf + p, len - p);
        for (i = 0; i < total; i += 4) memcpy(buf + p + i, "<a/>", 4);
        return len + total;
    }
    default: {                                                        /* bloat past the 64 KiB document limit */
        size_t chunk = 1 + rnd(64), have = len;
        if (chunk > len) chunk = len;
        while (have < PE_MAN_MAX_DOC - 3 + rnd(8) && have + chunk <= cap) { memmove(buf + have, buf + (len - chunk), chunk); have += chunk; }
        return have;
    }
    }
    if (len + ins > cap) return len;
    memmove(buf + p + ins, buf + p, len - p);
    memcpy(buf + p, tmp, ins);
    return len + ins;
}

/* mutate manifest text; `wide` => operate on 16-bit units so UTF-16 documents stay mostly UTF-16 */
static size_t mutate_text(u8 *buf, size_t len, size_t cap, int wide)
{
    u32 nmut = 1 + rnd(5), m;
    if (rnd(8) == 0) nmut += rnd(10);
    if (!wide && len > 4 && rnd(40) == 0) len = flood_text(buf, len, cap);
    for (m = 0; m < nmut && len > 0; m++) {
        u32 kind = rnd(100);
        size_t p = rnd((u32)len);
        if (wide) p &= ~(size_t)1;
        if (kind < 15) {                                              /* bit flip */
            buf[p] ^= (u8)(1u << rnd(8));
        } else if (kind < 27) {                                       /* random byte (ASCII-biased) */
            buf[p] = (u8)(rnd(4) ? 0x20 + rnd(0x5f) : rnd(256));
        } else if (kind < 45) {                                       /* a syntax character */
            static const char syn[] = "<>/=\"'&;!?-[]: \n\t#x";
            buf[p] = (u8)syn[rnd(sizeof syn - 1)];
        } else if (kind < 65) {                                       /* insert a token */
            u32 t = rnd((u32)N_TOK), n = k_tok[t].n, i;
            size_t ins = n * (wide ? 2u : 1u);
            if (len + ins <= cap) {
                memmove(buf + p + ins, buf + p, len - p);
                for (i = 0; i < n; i++) {
                    if (wide) { buf[p + 2 * i] = (u8)k_tok[t].s[i]; buf[p + 2 * i + 1] = 0; }
                    else buf[p + i] = (u8)k_tok[t].s[i];
                }
                len += ins;
            }
        } else if (kind < 75) {                                       /* delete a range */
            size_t n = 1 + rnd(16);
            if (wide) n = (n + 1) & ~(size_t)1;
            if (p + n <= len) { memmove(buf + p, buf + p + n, len - p - n); len -= n; }
        } else if (kind < 85) {                                       /* duplicate a range elsewhere (grows nesting, attribute and element counts) */
            size_t n = 1 + rnd(48), q = rnd((u32)len);
            if (wide) { n = (n + 1) & ~(size_t)1; q &= ~(size_t)1; }
            if (p + n <= len && len + n <= cap) {
                u8 tmp[128];
                memcpy(tmp, buf + p, n);
                memmove(buf + q + n, buf + q, len - q);
                memcpy(buf + q, tmp, n);
                len += n;
            }
        } else if (kind < 91) {                                       /* repeat a start tag many times (depth / element caps) */
            size_t a = p, b;
            int step = wide ? 2 : 1;
            while (a + (size_t)step <= len && !(buf[a] == '<' && (!wide || buf[a + 1] == 0))) a += (size_t)step;
            b = a + (size_t)step;
            while (b + (size_t)step <= len && !(buf[b] == '>' && (!wide || buf[b + 1] == 0))) b += (size_t)step;
            if (a + (size_t)step <= len && b + (size_t)step <= len && b - a < 64) {
                size_t n = b + (size_t)step - a, times = 2 + rnd(40);
                if (len + n * times <= cap) {
                    u8 tmp[80];
                    size_t i;
                    memcpy(tmp, buf + a, n);
                    memmove(buf + a + n * times, buf + a, len - a);
                    for (i = 0; i < times; i++) memcpy(buf + a + i * n, tmp, n);
                    len += n * times;
                }
            }
        } else if (kind < 96) {                                       /* truncate */
            len = rnd(3) ? rnd((u32)len + 1) : (len > 1 ? len - 1 : len);
        } else {                                                      /* swap two bytes */
            size_t q = rnd((u32)len);
            u8 t = buf[p]; buf[p] = buf[q]; buf[q] = t;
        }
    }
    return len;
}

static size_t mutate_blob(u8 *buf, size_t len, size_t cap)
{
    u32 nmut = 1 + rnd(5), m;
    for (m = 0; m < nmut && len > 2; m++) {
        u32 kind = rnd(100);
        size_t p = rnd((u32)len);
        if (kind < 40) {                                              /* 16-bit field <- boundary */
            p &= ~(size_t)1;
            if (p + 2 <= len) put_le(buf, p, 2, pick_value(get_le(buf, p, 2), 2));
        } else if (kind < 55) {                                       /* 32-bit field */
            p &= ~(size_t)3;
            if (p + 4 <= len) put_le(buf, p, 4, pick_value(get_le(buf, p, 4), 4));
        } else if (kind < 65) {
            buf[p] ^= (u8)(1u << rnd(8));
        } else if (kind < 72) {
            buf[p] = (u8)rnd(256);
        } else if (kind < 82) {                                       /* insert */
            u32 n = 1 + rnd(12), i;
            if (len + n <= cap) { memmove(buf + p + n, buf + p, len - p); for (i = 0; i < n; i++) buf[p + i] = (u8)rnd(256); len += n; }
        } else if (kind < 90) {                                       /* delete */
            u32 n = 1 + rnd(12);
            if (p + n <= len) { memmove(buf + p, buf + p + n, len - p - n); len -= n; }
        } else if (kind < 96) {                                       /* truncate */
            len = rnd(2) ? rnd((u32)len + 1) : len - 1;
        } else {                                                      /* clone a chunk */
            size_t a = rnd((u32)len), b = rnd((u32)len), n = 6 + rnd(40);
            if (a + n <= len && b + n <= len) memmove(buf + b, buf + a, n);
        }
    }
    return len;
}

/* ------------------------------------------------------------------------------------------------ */
/* manifest run (with the UTF-8 == UTF-16LE metamorphic check)                                        */
/* ------------------------------------------------------------------------------------------------ */
static u64 g_man_equiv;

static void run_man(const u8 *doc, size_t len)
{
    pe_manifest_t *m = (pe_manifest_t *)malloc(sizeof *m);
    int rc = pe_manifest_parse(doc, len, m);
    check_manifest(m, len, rc);

    /* ASCII-only, BOM-less UTF-8 input must parse identically when presented as UTF-16LE with a BOM */
    if (len > 0 && len <= 30000) {
        size_t i;
        int ascii = 1;
        for (i = 0; i < len; i++) if (doc[i] >= 0x80) { ascii = 0; break; }
        if (ascii && !(len >= 2 && ((doc[0] == 0x3C && doc[1] == 0x00) || (doc[0] == 0x00 && doc[1] == 0x3C)))) {   /* not a UTF-16 sniff pattern */
            size_t ul;
            u8 *w = to_utf16le_bom(doc, len, &ul);
            u8 *wx = dup_exact(w, ul);                            /* exact size */
            pe_manifest_t *m2 = (pe_manifest_t *)malloc(sizeof *m2);
            int rc2 = pe_manifest_parse(wx, ul, m2);
            check_manifest(m2, ul, rc2);
            if (rc2 != rc) die("UTF-8 and UTF-16LE parses disagree (return code)");
            {
                pe_manifest_t a = *m, b = *m2;
                a.encoding = b.encoding = 0;
                a.error_pos = b.error_pos = 0;
                if (memcmp(&a, &b, sizeof a) != 0) die("UTF-8 and UTF-16LE parses disagree (extracted data)");
            }
            if (rc != 0 && rc != PE_E_MAN_ROOT && m2->error_pos != 2 * m->error_pos + 2) die("UTF-8 / UTF-16LE error positions inconsistent");
            g_man_equiv++;
            free(m2); free(wx); free(w);
        }
    }
    free(m);
}

/* ------------------------------------------------------------------------------------------------ */
/* hint target                                                                                        */
/* ------------------------------------------------------------------------------------------------ */
static void run_hint(void)
{
    pe_man_assembly_t *a = (pe_man_assembly_t *)malloc(sizeof *a);           /* exact-size heap block */
    static const char alpha[] = "abcXYZ019.-_\\/: ..\x01\xc3";
    char *f[4] = { a->name, a->version, a->type, a->processor_architecture };
    u32 cap[4] = { PE_MAN_NAME_SZ, PE_MAN_VER_SZ, PE_MAN_TYPE_SZ, PE_MAN_ARCH_SZ }, i, j;
    pe_dep_hint_t h;
    int r;
    memset(a, 0, sizeof *a);
    memset(a->public_key_token, rnd(2) ? '6' : 'f', sizeof a->public_key_token - (rnd(2) ? 8 : 0));
    if (rnd(2)) memcpy(a->public_key_token, "6595b64144ccf1df", 17);
    for (i = 0; i < 4; i++) {
        u32 n = rnd(3) == 0 ? cap[i] : rnd(cap[i]);                       /* sometimes fills the WHOLE field: no NUL */
        for (j = 0; j < n; j++) f[i][j] = (char)(rnd(5) ? "abcdefghijklmnopqrstuvwxyzMicrosoft.Windows-0123456789"[rnd(51)] : alpha[rnd(sizeof alpha - 1)]);
        if (n < cap[i]) f[i][n] = 0;
    }
    if (rnd(4) == 0) memcpy(a->name, "Microsoft.Windows.Common-Controls", 34);
    a->truncated = (unsigned char)(rnd(8) == 0);
    g_cur = (const u8 *)a; g_cur_len = sizeof *a;
    r = pe_manifest_dep_hint(a, &h);
    if (r == 0) g_hint_ok++; else g_hint_rej++;
    check_hint(a);
    g_cur = NULL;
    free(a);
}

/* ------------------------------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    u64 iters = 300000, seed = 0x9E3779B97F4A7C15ull, n_pe_it = 0, n_man_it = 0, n_ver_it = 0, n_hint_it = 0;
    const char *e, *exe = NULL, *mpath = NULL;
    u64 inject_at = ~0ull, inject_ub_at = ~0ull;
    struct timespec t0, t1;
    int a = 1;
    u8 *exe_data, *man_data, *man16;
    size_t exe_len = 0, man_len = 0, man16_len = 0;

    if (argc > 1 && (argv[1][0] >= '0' && argv[1][0] <= '9')) { seed = strtoull(argv[1], NULL, 0); a = 2; }
    if (a + 1 >= argc) { fprintf(stderr, "usage: res_fuzz [seed] res_hello.exe res_good.manifest\n"); return 2; }
    exe = argv[a]; mpath = argv[a + 1];
    if ((e = getenv("RES_FUZZ_ITERS")) != NULL) iters = strtoull(e, NULL, 0);
    if ((e = getenv("RES_FUZZ_INJECT")) != NULL) inject_at = strtoull(e, NULL, 0);
    if ((e = getenv("RES_FUZZ_INJECT_UB")) != NULL) inject_ub_at = strtoull(e, NULL, 0);
    if ((e = getenv("RES_FUZZ_CRASH")) != NULL) g_crash_path = e;
    g_rng = seed ? seed : 1;

    exe_data = load_file(exe, &exe_len);
    man_data = load_file(mpath, &man_len);
    if (!exe_data || !man_data) { fprintf(stderr, "RES-FUZZ: cannot read fixtures\n"); return 2; }

    /* PE seeds: the real windres fixture and a minimal synthetic PE */
    add_pe_seed(exe_data, exe_len);
    {
        rt_t r;
        size_t fl;
        u8 *f;
        rt_new(&r, RT_BASE_LEN); rt_base(&r);
        f = make_pe(r.b, RT_BASE_LEN, 0, &fl);
        add_pe_seed(f, fl);
        free(f); rt_free(&r);
    }
    /* the real version blob, plus synthetic ones */
    {
        pe_info_t info;
        u8 *img;
        const u8 *d;
        unsigned sz;
        vbuf_t v;
        pe_parse(exe_data, exe_len, &info);
        img = (u8 *)calloc(info.size_of_image, 1);
        pe_map(exe_data, exe_len, &info, img);
        if (pe_res_find(&info, img, PE_RT_VERSION, PE_RES_ID_ANY, 0, &d, &sz) != 0) { fprintf(stderr, "RES-FUZZ: fixture has no version resource\n"); return 2; }
        add_seed(g_ver, &n_ver, d, sz, 0, 0, 0);
        free(img);
        vb_new(&v, 8192); vb_good(&v, "040904B0", "040704B0", 0x04B00409u); add_seed(g_ver, &n_ver, v.b, v.len, 0, 0, 0); vb_free(&v);
        vb_new(&v, 8192); vb_good(&v, "000004E4", NULL, 0x04E40000u); add_seed(g_ver, &n_ver, v.b, v.len, 0, 0, 0); vb_free(&v);
    }
    /* manifest seeds */
    add_seed(g_man, &n_man, man_data, man_len, 0, 0, 0);
    man16 = to_utf16le_bom(man_data, man_len, &man16_len);
    add_seed(g_man, &n_man, man16, man16_len, 0, 0, 0);
    {
        static const char *const docs[] = {
            "<?xml version='1.0' encoding='UTF-8'?>\n<!-- c -->\n<asm:assembly xmlns:asm=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion='1.0'>\n"
            "<asm:assemblyIdentity name=\"A&amp;B\" version=\"1.0.0.0\" type=\"win32\"/><![CDATA[ <x/> ]]><?pi data?>"
            "<asm:trustInfo><asm:security><asm:requestedPrivileges><asm:requestedExecutionLevel level='requireAdministrator' uiAccess='true'/>"
            "</asm:requestedPrivileges></asm:security></asm:trustInfo></asm:assembly>",
            "<assembly><dependency><dependentAssembly><assemblyIdentity name=\"d1\" version=\"1.2.3.4\" publicKeyToken=\"0123456789abcdef\" processorArchitecture=\"x86\"/></dependentAssembly>"
            "<dependentAssembly><assemblyIdentity name=\"..\\d2\" version=\"1.2.3\" language=\"*\"/></dependentAssembly></dependency></assembly>",
            "<assembly><application><windowsSettings><dpiAware>true/pm</dpiAware><dpiAwareness>PerMonitorV2, system</dpiAwareness>"
            "<longPathAware>true&#x20;</longPathAware><activeCodePage>UTF-8</activeCodePage></windowsSettings></application>"
            "<compatibility><application><supportedOS Id=\"{35138b9a-5d96-4fbd-8e2d-a2440225f93a}\"/><supportedOS Id=\"{E2011457-1546-43C5-A5FE-008DEEE3D3F0}\"/></application></compatibility></assembly>\n\n",
            "<!DOCTYPE assembly [<!ENTITY a \"b\">]><assembly a=\"&a;\"/>",
            "<assembly><a><b><c><d><e><f><g><h>text &amp; &lt;&gt; &#65; &#x42;</h></g></f></e></d></c></b></a></assembly>",
            /* the isolation-container manifests found in Windows itself: well-formed, root is not <assembly> */
            "<?xml version='1.0' encoding='utf-8' standalone='yes'?>\n<isolation>\n<container name=\"C\"><consumes><capability name=\"RegistryRead\" /></consumes></container>\n</isolation>",
            "<x:assembly xmlns:x=\"u\"><x:assemblyIdentity x:name=\"ignored\" name=\"seen\" type=\"win32\" version=\"6.0.0.0\" publicKeyToken=\"6595b64144ccf1df\"/></x:assembly>",
        };
        u32 i;
        for (i = 0; i < sizeof docs / sizeof docs[0]; i++) add_seed(g_man, &n_man, (const u8 *)docs[i], strlen(docs[i]), 0, 0, 0);
        {
            size_t ul;
            u8 *w = to_utf16le_bom((const u8 *)docs[0], strlen(docs[0]), &ul);
            add_seed(g_man, &n_man, w, ul, 0, 0, 0);
            free(w);
        }
    }

    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(save_crash);
    signal(SIGABRT, on_signal);
    if (!__sanitizer_set_death_callback) {
        signal(SIGSEGV, on_signal); signal(SIGBUS, on_signal); signal(SIGFPE, on_signal);
        signal(SIGILL, on_signal); signal(SIGTRAP, on_signal);
    }

    fprintf(stderr, "RES-FUZZ: seed=0x%llx iterations=%llu pe_seeds=%u man_seeds=%u ver_seeds=%u\n", seed, iters, n_pe, n_man, n_ver);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (g_iter = 0; g_iter < iters; g_iter++) {
        u32 which = rnd(100);
        if (which < 18) {                                             /* PE target */
            const seed_t *s = &g_pe[rnd(n_pe)];
            size_t cap = s->len + 128, len = s->len;
            u8 *work = (u8 *)malloc(cap), *exact;
            g_kind = "PE"; n_pe_it++;
            memcpy(work, s->data, len);
            if (g_iter > 0 || rnd(4)) len = mutate_pe(s, work, len, cap);
            exact = dup_exact(work, len);
            free(work);
            g_cur = exact; g_cur_len = len;
            if (g_iter == inject_at) { volatile u8 x = exact[len]; (void)x; }
            if (g_iter == inject_ub_at) { volatile int big = 0x7fffffff; big = big + (int)(g_iter & 1) + 1; (void)big; }
            run_pe(exact, len);
            g_cur = NULL;
            free(exact);
        } else if (which < 74) {                                      /* manifest target */
            const seed_t *s = &g_man[rnd(n_man)];
            int wide = s->len >= 2 && s->data[0] == 0xFF && s->data[1] == 0xFE;
            size_t cap = MAN_CAP + 64, len = s->len;
            u8 *work = (u8 *)malloc(cap), *exact;
            g_kind = "MAN"; n_man_it++;
            memcpy(work, s->data, len);
            if (g_iter > 0 || rnd(4)) len = mutate_text(work, len, MAN_CAP, wide);
            exact = dup_exact(work, len);
            free(work);
            g_cur = exact; g_cur_len = len;
            if (g_iter == inject_at) { volatile u8 x = exact[len]; (void)x; }
            if (g_iter == inject_ub_at) { volatile int big = 0x7fffffff; big = big + (int)(g_iter & 1) + 1; (void)big; }
            run_man(exact, len);
            g_cur = NULL;
            free(exact);
        } else if (which < 94) {                                      /* version target */
            const seed_t *s = &g_ver[rnd(n_ver)];
            size_t cap = s->len + 128, len = s->len;
            u8 *work = (u8 *)malloc(cap), *exact;
            pe_version_t *v = (pe_version_t *)malloc(sizeof *v);
            int r;
            g_kind = "VER"; n_ver_it++;
            memcpy(work, s->data, len);
            if (g_iter > 0 || rnd(4)) len = mutate_blob(work, len, cap);
            exact = dup_exact(work, len);
            free(work);
            g_cur = exact; g_cur_len = len;
            if (g_iter == inject_at) { volatile u8 x = exact[len]; (void)x; }
            if (g_iter == inject_ub_at) { volatile int big = 0x7fffffff; big = big + (int)(g_iter & 1) + 1; (void)big; }
            {
                static const u32 langs[] = { 0, 0x409, 0x407, PE_RES_LANG_ANY, 0x40c };
                r = pe_version_parse(exact, (unsigned)len, langs[rnd(5)], v);
            }
            check_version(v, r);
            g_cur = NULL;
            free(exact); free(v);
        } else {                                                      /* hint target */
            g_kind = "HINT"; n_hint_it++;
            run_hint();
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    fprintf(stderr, "RES-FUZZ: PE: mapped=%llu rejected=%llu big=%llu find_ok=%llu enum_ok=%llu enum_err=%llu leaves=%llu manifests_ok=%llu versions_ok=%llu\n",
            g_pe_mapped, g_pe_rej, g_pe_big, g_pe_find_ok, g_pe_enum_ok, g_pe_enum_err, g_pe_leaves, g_pe_man, g_pe_ver);
    fprintf(stderr, "RES-FUZZ: PE result codes:");
    {
        u32 i;
        for (i = 0; i < 32; i++) if (g_rc_stat[i]) fprintf(stderr, " %d=%llu", i == 0 ? 0 : -(int)i, g_rc_stat[i]);
    }
    fprintf(stderr, "\nRES-FUZZ: MAN: ok=%llu rejected=%llu (syntax=%llu trunc=%llu encoding=%llu toobig=%llu depth=%llu limit=%llu entity=%llu doctype=%llu root=%llu) utf8==utf16 checks=%llu\n",
            g_man_ok, g_man_rej, g_man_class[0], g_man_class[1], g_man_class[2], g_man_class[3], g_man_class[4], g_man_class[5], g_man_class[6],
            g_man_class[7], g_man_class[8], g_man_equiv);
    fprintf(stderr, "RES-FUZZ: HINT: ok=%llu unsafe=%llu   time=%.1fs\n", g_hint_ok, g_hint_rej,
            (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);
    {
        u32 i;                                                        /* leave no allocation behind (LeakSanitizer is on) */
        for (i = 0; i < n_pe; i++) free(g_pe[i].data);
        for (i = 0; i < n_man; i++) free(g_man[i].data);
        for (i = 0; i < n_ver; i++) free(g_ver[i].data);
        free(exe_data); free(man_data); free(man16);
    }
    printf("RES-FUZZ: PASS iterations=%llu pe=%llu man=%llu ver=%llu hint=%llu\n", iters, n_pe_it, n_man_it, n_ver_it, n_hint_it);
    return 0;
}
