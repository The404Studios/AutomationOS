/*
 * pe_manifest.c -- bounded, non-allocating scanner for the Windows application manifest XML subset.
 * See pe_manifest.h for the contract (what is and is not parsed) and README.md for the security rules.
 *
 * Freestanding build gate (warning-free, NO undefined symbols, NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 *       -mno-red-zone -O2 -Wall -Wextra -Werror -c userspace/lib/pe/pe_manifest.c
 *
 * Structure: a reader abstracts UTF-8 / UTF-16LE into "code units" addressed by index; ONE loop scans the
 * document linearly with an explicit element stack (no recursion).  Each element gets a "role" from its
 * parent's role and its local name; roles decide which attributes / text are captured.  Every captured value is
 * decoded straight into a bounded buffer.  Nothing is ever read at an index >= the unit count (mr_get).
 *
 * Test-only: -DPE_MAN_INJECT_BUG=N deliberately plants library bug N (1: mr_get() reads one unit past the end;
 * 2: the nesting-depth cap is removed; 3: the entity-name length cap is removed).  The fuzz runner builds the fuzzer
 * that way to prove it detects each of them.  Never defined in a real build (the MN_BUG() tests fold to 0).
 */
#include "pe_manifest.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define MN_BARRIER() __asm__ volatile("" ::: "memory")

#ifdef PE_MAN_INJECT_BUG
#define MN_BUG(n) (PE_MAN_INJECT_BUG == (n))
#else
#define MN_BUG(n) 0
#endif

#if defined(__GNUC__) && (__GNUC__ >= 11) && !defined(__clang__)
#define MN_NO_SSP __attribute__((no_stack_protector))
#else
#define MN_NO_SSP
#endif

static void mn_zero(void *p, u64 n)
{
    u8 *d = (u8 *)p;
    while (n) { *d++ = 0; n--; MN_BARRIER(); }
}

static inline u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }

static inline u32 mn_fold(u32 c) { return (c >= 'A' && c <= 'Z') ? c + 32u : c; }   /* to lower */

/* case-insensitive (ASCII) equality of two C strings */
static int mn_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        if (mn_fold((u8)*a) != mn_fold((u8)*b)) return 0;
        if (*a == 0) return 1;
    }
}

const char *pe_manifest_strerror(int err)
{
    switch (err) {
    case PE_E_MAN_SYNTAX:   return "manifest is not well-formed";
    case PE_E_MAN_TRUNC:    return "manifest truncated (open construct, unclosed element or no root)";
    case PE_E_MAN_ENCODING: return "unsupported or malformed manifest encoding";
    case PE_E_MAN_TOOBIG:   return "manifest larger than the 64 KiB limit";
    case PE_E_MAN_DEPTH:    return "manifest nesting too deep";
    case PE_E_MAN_LIMIT:    return "manifest element/attribute/name limit exceeded";
    case PE_E_MAN_ENTITY:   return "unknown or malformed entity reference";
    case PE_E_MAN_DOCTYPE:  return "DOCTYPE/DTD not allowed in a manifest";
    case PE_E_MAN_ROOT:     return "root element is not <assembly>";
    case PE_E_MAN_UNSAFE:   return "assembly name is not usable as a file name";
    default:                return pe_res_strerror(err);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* reader: UTF-8 bytes or UTF-16LE units, addressed by index                                          */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    const u8 *p;
    u32 n;          /* number of code units (bytes for UTF-8, 16-bit units for UTF-16)                   */
    u32 start;      /* first unit after the BOM                                                         */
    int wide;
} mr_t;

/* code unit at i, or -1 past the end */
static inline int mr_get(const mr_t *r, u32 i)
{
    if (MN_BUG(1) ? i > r->n : i >= r->n) return -1;     /* bug 1 (TEST-ONLY): i == n reads one unit past the buffer */
    return r->wide ? (int)rd16(r->p + 2u * (u64)i) : (int)r->p[i];
}

/* 1 if the ASCII string s occurs at unit index i */
static int mr_match(const mr_t *r, u32 i, const char *s)
{
    u32 k;
    for (k = 0; s[k]; k++)
        if (mr_get(r, i + k) != (int)(u8)s[k]) return 0;
    return 1;
}

/* 1 if the len-unit span at `off` equals the ASCII string s */
static int mr_span_is(const mr_t *r, u32 off, u32 len, const char *s)
{
    u32 k;
    for (k = 0; k < len; k++)
        if (s[k] == 0 || mr_get(r, off + k) != (int)(u8)s[k]) return 0;
    return s[len] == 0;
}

static int mr_span_eq(const mr_t *r, u32 a, u32 b, u32 len)
{
    u32 k;
    for (k = 0; k < len; k++)
        if (mr_get(r, a + k) != mr_get(r, b + k)) return 0;
    return 1;
}

static inline int is_ws(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static inline int is_name_start(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == ':' || c >= 0x80; }
static inline int is_name_char(int c) { return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.'; }

/* ------------------------------------------------------------------------------------------------ */
/* roles                                                                                              */
/* ------------------------------------------------------------------------------------------------ */

enum {
    R_NONE = 0, R_DOC, R_ASSEMBLY, R_ID_APP, R_DEPENDENCY, R_DEPASM, R_ID_DEP, R_TRUSTINFO, R_SECURITY,
    R_REQPRIV, R_REL, R_COMPAT, R_COMPAT_APP, R_OS, R_APPLICATION, R_WINSET,
    R_T_DPIAWARE, R_T_DPIAWARENESS, R_T_LONGPATH, R_T_ACP          /* R_T_* capture their text content */
};
#define R_T_FIRST R_T_DPIAWARE

typedef struct { u8 parent, child; char name[28]; } role_t;

static const role_t k_roles[] = {
    { R_DOC,         R_ASSEMBLY,       "assembly" },
    { R_ASSEMBLY,    R_ID_APP,         "assemblyIdentity" },
    { R_ASSEMBLY,    R_DEPENDENCY,     "dependency" },
    { R_ASSEMBLY,    R_TRUSTINFO,      "trustInfo" },
    { R_ASSEMBLY,    R_COMPAT,         "compatibility" },
    { R_ASSEMBLY,    R_APPLICATION,    "application" },
    { R_DEPENDENCY,  R_DEPASM,         "dependentAssembly" },
    { R_DEPASM,      R_ID_DEP,         "assemblyIdentity" },
    { R_TRUSTINFO,   R_SECURITY,       "security" },
    { R_SECURITY,    R_REQPRIV,        "requestedPrivileges" },
    { R_REQPRIV,     R_REL,            "requestedExecutionLevel" },
    { R_COMPAT,      R_COMPAT_APP,     "application" },
    { R_COMPAT_APP,  R_OS,             "supportedOS" },
    { R_APPLICATION, R_WINSET,         "windowsSettings" },
    { R_WINSET,      R_T_DPIAWARE,     "dpiAware" },
    { R_WINSET,      R_T_DPIAWARENESS, "dpiAwareness" },
    { R_WINSET,      R_T_LONGPATH,     "longPathAware" },
    { R_WINSET,      R_T_ACP,          "activeCodePage" },
};
#define N_ROLES (sizeof k_roles / sizeof k_roles[0])

static int role_of(const mr_t *r, int parent, u32 loff, u32 llen)
{
    u32 i;
    for (i = 0; i < N_ROLES; i++)
        if (k_roles[i].parent == parent && mr_span_is(r, loff, llen, k_roles[i].name)) return k_roles[i].child;
    return R_NONE;
}

enum { A_NONE = 0, A_NAME, A_VERSION, A_TYPE, A_ARCH, A_TOKEN, A_LANG, A_LEVEL, A_UIACCESS, A_OSID, A__COUNT };

static int attr_kind(int role, const mr_t *r, u32 off, u32 len)
{
    if (role == R_ID_APP || role == R_ID_DEP) {
        if (mr_span_is(r, off, len, "name")) return A_NAME;
        if (mr_span_is(r, off, len, "version")) return A_VERSION;
        if (mr_span_is(r, off, len, "type")) return A_TYPE;
        if (mr_span_is(r, off, len, "processorArchitecture")) return A_ARCH;
        if (mr_span_is(r, off, len, "publicKeyToken")) return A_TOKEN;
        if (mr_span_is(r, off, len, "language")) return A_LANG;
    } else if (role == R_REL) {
        if (mr_span_is(r, off, len, "level")) return A_LEVEL;
        if (mr_span_is(r, off, len, "uiAccess")) return A_UIACCESS;
    } else if (role == R_OS) {
        if (mr_span_is(r, off, len, "Id")) return A_OSID;
    }
    return A_NONE;
}

/* ------------------------------------------------------------------------------------------------ */
/* scanner state                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

typedef struct { u32 name_off, name_len; u8 role; } frame_t;

typedef struct {
    mr_t r;
    pe_manifest_t *out;
    u32 depth, elements, attrs;
    int root_seen, root_done;
    u32 errpos;                         /* unit index of the first problem                              */
    /* text capture of the element on frame cap_frame */
    char *cap_dst;
    u32 cap_sz, cap_len, cap_frame;
    frame_t st[PE_MAN_MAX_DEPTH];       /* explicit element stack; LAST member so an overrun leaves the object */
} ctx_t;

#define FAIL(code, pos) do { c->errpos = (pos); return (code); } while (0)

/* Map one input unit (or an already-decoded code point) to a printable ASCII char; 0 = produce nothing. */
static char map_unit(const ctx_t *c, int unit, int is_cp)
{
    if (unit == '\t' || unit == '\r' || unit == '\n') return ' ';
    if (unit >= 0x20 && unit < 0x7f) return (char)unit;
    if (unit < 0x20 || unit == 0x7f) return ' ';
    if (is_cp) return '?';
    if (c->r.wide) return (unit >= 0xDC00 && unit <= 0xDFFF) ? 0 : '?';   /* low surrogate: lead gave '?' */
    return (unit >= 0x80 && unit <= 0xBF) ? 0 : '?';                      /* UTF-8 continuation byte      */
}

/* ------------------------------------------------------------------------------------------------ */
/* lexical pieces                                                                                     */
/* ------------------------------------------------------------------------------------------------ */

/* Qualified name at *pi.  *loff = start of the local part (after the last ':'). */
static int read_name(ctx_t *c, u32 *pi, u32 *off, u32 *len, u32 *loff)
{
    u32 i = *pi, n = 0, lo;
    int ch = mr_get(&c->r, i);
    if (ch < 0) FAIL(PE_E_MAN_TRUNC, i);
    if (!is_name_start(ch)) FAIL(PE_E_MAN_SYNTAX, i);
    *off = i;
    lo = i;
    for (;;) {
        ch = mr_get(&c->r, i);
        if (!is_name_char(ch)) break;
        if (ch == ':') lo = i + 1u;
        if (++n > PE_MAN_MAX_NAME) FAIL(PE_E_MAN_LIMIT, i);
        i++;
    }
    *len = n;
    *loff = lo;
    *pi = i;
    return 0;
}

/* '&' at *pi: predefined entities and numeric references only.  Advances *pi past the ';'. */
MN_NO_SSP
static int read_entity(ctx_t *c, u32 *pi, u32 *cp)
{
    char nm[12];
    u32 n = 0, v = 0, i;
    int ch;

    for (;;) {
        ch = mr_get(&c->r, *pi + 1u + n);
        if (ch < 0) FAIL(PE_E_MAN_TRUNC, *pi);
        if (ch == ';') break;
        if ((!MN_BUG(3) && n >= 10u) || !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '#'))
            FAIL(PE_E_MAN_ENTITY, *pi);
        nm[n++] = (char)ch;
    }
    nm[n] = 0;
    if (n == 0) FAIL(PE_E_MAN_ENTITY, *pi);

    if (nm[0] == '#') {
        int hex = (n >= 2u && nm[1] == 'x');
        i = hex ? 2u : 1u;
        if (i >= n) FAIL(PE_E_MAN_ENTITY, *pi);
        for (; i < n; i++) {
            u32 d;
            ch = nm[i];
            if (ch >= '0' && ch <= '9') d = (u32)(ch - '0');
            else if (hex && ch >= 'a' && ch <= 'f') d = (u32)(ch - 'a' + 10);
            else if (hex && ch >= 'A' && ch <= 'F') d = (u32)(ch - 'A' + 10);
            else FAIL(PE_E_MAN_ENTITY, *pi);
            v = v * (hex ? 16u : 10u) + d;                   /* <= 8 hex / 9 decimal digits: fits u32 */
        }
        if (v == 0 || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu)) FAIL(PE_E_MAN_ENTITY, *pi);
    } else if (n == 3u && nm[0] == 'a' && nm[1] == 'm' && nm[2] == 'p') v = '&';
    else if (n == 2u && nm[0] == 'l' && nm[1] == 't') v = '<';
    else if (n == 2u && nm[0] == 'g' && nm[1] == 't') v = '>';
    else if (n == 4u && nm[0] == 'q' && nm[1] == 'u' && nm[2] == 'o' && nm[3] == 't') v = '"';
    else if (n == 4u && nm[0] == 'a' && nm[1] == 'p' && nm[2] == 'o' && nm[3] == 's') v = '\'';
    else FAIL(PE_E_MAN_ENTITY, *pi);

    *cp = v;
    *pi = *pi + 1u + n + 1u;
    return 0;
}

/* Quoted attribute value at *pi.  dst may be NULL (validate only).  The value is trimmed of leading/trailing
 * blanks; *trunc is set if it did not fit. */
MN_NO_SSP
static int read_value(ctx_t *c, u32 *pi, char *dst, u32 dstsz, int *trunc)
{
    u32 i = *pi, len = 0;
    int q = mr_get(&c->r, i);

    if (q < 0) FAIL(PE_E_MAN_TRUNC, i);
    if (q != '"' && q != '\'') FAIL(PE_E_MAN_SYNTAX, i);
    i++;
    for (;;) {
        int ch = mr_get(&c->r, i), unit, is_cp = 0;
        char o;
        if (ch < 0) FAIL(PE_E_MAN_TRUNC, i);
        if (ch == q) { i++; break; }
        if (ch == '<' || ch == 0) FAIL(PE_E_MAN_SYNTAX, i);
        unit = ch;
        if (ch == '&') {
            u32 cp;
            int r = read_entity(c, &i, &cp);
            if (r) return r;
            unit = (int)cp;
            is_cp = 1;
        } else i++;
        o = map_unit(c, unit, is_cp);
        if (!o || !dst) continue;
        if (o == ' ' && len == 0) continue;                  /* trim leading blanks */
        if (len + 1u < dstsz) dst[len++] = o; else *trunc = 1;
    }
    if (dst) {
        while (len > 0 && dst[len - 1] == ' ') len--;        /* trim trailing blanks */
        dst[len] = 0;
    }
    *pi = i;
    return 0;
}

/* Find `pat` at or after `from`; *pi = index just past it.  Not found => truncated construct starting at `start`. */
static int skip_past(ctx_t *c, u32 start, u32 from, const char *pat, u32 *pi)
{
    u32 j, k;
    for (j = from; mr_get(&c->r, j) >= 0; j++) {
        if (mr_match(&c->r, j, pat)) {
            for (k = 0; pat[k]; k++) {}
            *pi = j + k;
            return 0;
        }
    }
    FAIL(PE_E_MAN_TRUNC, start);
}

/* ------------------------------------------------------------------------------------------------ */
/* value helpers                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

static void put_str(char *dst, u32 dstsz, const char *src, unsigned char *trunc)
{
    u32 i = 0;
    while (src[i] && i + 1u < dstsz) { dst[i] = src[i]; i++; }
    if (src[i]) *trunc = 1;
    dst[i] = 0;
}

static int hexdig(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* "a.b.c.d", each 1..5 decimal digits <= 65535 */
static int parse_ver(const char *s, unsigned short *v)
{
    u32 part = 0, i = 0;
    for (part = 0; part < 4u; part++) {
        u32 n = 0, digits = 0;
        while (s[i] >= '0' && s[i] <= '9') {
            n = n * 10u + (u32)(s[i] - '0');
            if (++digits > 5u || n > 65535u) return 0;
            i++;
        }
        if (digits == 0) return 0;
        v[part] = (unsigned short)n;
        if (part < 3u) { if (s[i] != '.') return 0; i++; }
    }
    return s[i] == 0;
}

static void finish_assembly(pe_man_assembly_t *a)
{
    u32 i, n = 0;
    a->ver_valid = (unsigned char)parse_ver(a->version, a->ver);
    if (!a->ver_valid) { a->ver[0] = a->ver[1] = a->ver[2] = a->ver[3] = 0; }
    for (i = 0; a->public_key_token[i]; i++) {
        if (!hexdig((u8)a->public_key_token[i])) { n = 0; break; }
        n++;
    }
    a->token_valid = (unsigned char)(n == 16u && a->public_key_token[16] == 0);
}

static int parse_level(const char *v)
{
    if (mn_ieq(v, "asInvoker")) return PE_MAN_EXEC_AS_INVOKER;
    if (mn_ieq(v, "requireAdministrator")) return PE_MAN_EXEC_REQUIRE_ADMIN;
    if (mn_ieq(v, "highestAvailable")) return PE_MAN_EXEC_HIGHEST;
    return PE_MAN_EXEC_UNKNOWN;
}

static const char k_os_guid[5][40] = {
    "{e2011457-1546-43c5-a5fe-008deee3d3f0}",
    "{35138b9a-5d96-4fbd-8e2d-a2440225f93a}",
    "{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}",
    "{1f676c76-80e1-4239-95bb-83d0f6d0da78}",
    "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}",
};

static void os_add(pe_manifest_t *o, const char *val)
{
    u32 k;
    o->n_os_total++;
    for (k = 0; k < 5u; k++)
        if (mn_ieq(val, k_os_guid[k])) o->os_mask |= 1u << k;
    if (o->n_os < PE_MAN_MAX_OS) {
        char *d = o->os_guid[o->n_os];
        u32 i = 0;
        while (val[i] && i + 1u < PE_MAN_GUID_SZ) { d[i] = (char)mn_fold((u8)val[i]); i++; }
        if (val[i]) o->truncated |= 1u;
        d[i] = 0;
        o->n_os++;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* text capture                                                                                       */
/* ------------------------------------------------------------------------------------------------ */

static void cap_begin(ctx_t *c, int role)
{
    pe_manifest_t *o = c->out;
    char *d = 0;
    u32 sz = 0;
    int *has = 0;

    switch (role) {
    case R_T_DPIAWARE:     d = o->dpi_aware;        sz = sizeof o->dpi_aware;        has = &o->has_dpi_aware; break;
    case R_T_DPIAWARENESS: d = o->dpi_awareness;    sz = sizeof o->dpi_awareness;    has = &o->has_dpi_awareness; break;
    case R_T_LONGPATH:     d = o->long_path_aware;  sz = sizeof o->long_path_aware;  has = &o->has_long_path_aware; break;
    default:               d = o->active_code_page; sz = sizeof o->active_code_page; has = &o->has_active_code_page; break;
    }
    c->cap_dst = 0;
    if (*has) return;                                        /* first occurrence wins */
    *has = 1;
    d[0] = 0;
    c->cap_dst = d;
    c->cap_sz = sz;
    c->cap_len = 0;
    c->cap_frame = c->depth - 1u;                            /* the frame just pushed */
}

static void cap_unit(ctx_t *c, int unit, int is_cp)
{
    char o;
    if (!c->cap_dst || c->depth == 0 || c->depth - 1u != c->cap_frame) return;
    o = map_unit(c, unit, is_cp);
    if (!o) return;
    if (o == ' ' && c->cap_len == 0) return;
    if (c->cap_len + 1u < c->cap_sz) { c->cap_dst[c->cap_len++] = o; c->cap_dst[c->cap_len] = 0; }
    else c->out->truncated |= 1u;
}

static void cap_end(ctx_t *c)
{
    if (!c->cap_dst) return;
    while (c->cap_len > 0 && c->cap_dst[c->cap_len - 1u] == ' ') c->cap_len--;
    c->cap_dst[c->cap_len] = 0;
    c->cap_dst = 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* tags                                                                                               */
/* ------------------------------------------------------------------------------------------------ */

MN_NO_SSP
static int start_tag(ctx_t *c, u32 *pi)
{
    pe_manifest_t *o = c->out;
    const u32 lt = *pi;
    u32 i = lt + 1u, noff, nlen, nloff, nattr = 0, seen = 0;
    int parent, role, empty = 0, first_rel = 0, lvl = PE_MAN_EXEC_NONE, ui_present = 0, ui_true = 0, r;
    pe_man_assembly_t *as = 0;

    r = read_name(c, &i, &noff, &nlen, &nloff);
    if (r) return r;
    if (!MN_BUG(2) && c->depth >= PE_MAN_MAX_DEPTH) FAIL(PE_E_MAN_DEPTH, lt);
    if (c->elements >= PE_MAN_MAX_ELEMENTS) FAIL(PE_E_MAN_LIMIT, lt);
    c->elements++;
    if (c->depth == 0 && c->root_done) FAIL(PE_E_MAN_SYNTAX, lt);        /* a second root element */

    parent = c->depth ? c->st[c->depth - 1u].role : R_DOC;
    role = role_of(&c->r, parent, nloff, nlen - (nloff - noff));
    if (c->depth == 0) { c->root_seen = 1; o->is_assembly = (role == R_ASSEMBLY); }

    switch (role) {
    case R_ID_APP:
        if (!o->has_identity) { o->has_identity = 1; as = &o->identity; }
        break;
    case R_ID_DEP:
        o->n_deps_total++;
        if (o->n_deps < PE_MAN_MAX_DEPS) as = &o->deps[o->n_deps++];
        break;
    case R_REL:
        first_rel = (o->n_exec_level == 0);
        o->n_exec_level++;
        break;
    default:
        break;
    }

    for (;;) {                                                           /* attributes */
        int had_ws = 0, ch, kind, trunc = 0;
        u32 aoff, alen, aloff;
        char val[PE_MAN_NAME_SZ];

        while (is_ws(ch = mr_get(&c->r, i))) { i++; had_ws = 1; }
        if (ch < 0) FAIL(PE_E_MAN_TRUNC, i);
        if (ch == '>') { i++; break; }
        if (ch == '/') {
            int nx = mr_get(&c->r, i + 1u);
            if (nx == '>') { i += 2u; empty = 1; break; }
            FAIL(nx < 0 ? PE_E_MAN_TRUNC : PE_E_MAN_SYNTAX, i);
        }
        if (!had_ws) FAIL(PE_E_MAN_SYNTAX, i);
        if (++nattr > PE_MAN_MAX_ATTRS) FAIL(PE_E_MAN_LIMIT, i);
        c->attrs++;

        r = read_name(c, &i, &aoff, &alen, &aloff);
        if (r) return r;
        while (is_ws(ch = mr_get(&c->r, i))) i++;
        if (ch < 0) FAIL(PE_E_MAN_TRUNC, i);
        if (ch != '=') FAIL(PE_E_MAN_SYNTAX, i);
        i++;
        while (is_ws(ch = mr_get(&c->r, i))) i++;

        kind = (aloff == aoff) ? attr_kind(role, &c->r, aoff, alen) : A_NONE;   /* prefixed attrs never match */
        if (kind != A_NONE) {
            if (seen & (1u << kind)) FAIL(PE_E_MAN_SYNTAX, aoff);        /* duplicate attribute */
            seen |= 1u << kind;
        }
        val[0] = 0;
        r = read_value(c, &i, kind != A_NONE ? val : 0, sizeof val, &trunc);
        if (r) return r;
        if (trunc) {                                                     /* cut while decoding (value longer than the temp buffer) */
            o->truncated |= 1u;
            if (as && kind != A_NONE) as->truncated = 1;
        }

        switch (kind) {
        case A_NAME:    if (as) put_str(as->name, sizeof as->name, val, &as->truncated); break;
        case A_VERSION: if (as) put_str(as->version, sizeof as->version, val, &as->truncated); break;
        case A_TYPE:    if (as) put_str(as->type, sizeof as->type, val, &as->truncated); break;
        case A_ARCH:    if (as) put_str(as->processor_architecture, sizeof as->processor_architecture, val, &as->truncated); break;
        case A_TOKEN:   if (as) put_str(as->public_key_token, sizeof as->public_key_token, val, &as->truncated); break;
        case A_LANG:    if (as) put_str(as->language, sizeof as->language, val, &as->truncated); break;
        case A_LEVEL:   lvl = parse_level(val); break;
        case A_UIACCESS: ui_present = 1; ui_true = mn_ieq(val, "true"); break;
        case A_OSID:    os_add(o, val); break;
        default:        break;
        }
    }

    if (role == R_REL) {
        o->has_exec_level = 1;
        o->exec_levels_seen |= 1u << lvl;
        if (first_rel) o->exec_level = lvl;
        if (ui_present) { o->has_ui_access = 1; if (ui_true) o->ui_access = 1; }
    }

    if (c->depth + 1u > o->max_depth) o->max_depth = c->depth + 1u;
    if (empty) {
        if (role >= R_T_FIRST) { cap_begin(c, role); cap_end(c); }       /* <dpiAware/> : present, empty */
        if (c->depth == 0) c->root_done = 1;
    } else {
        c->st[c->depth].name_off = noff;
        c->st[c->depth].name_len = nlen;
        c->st[c->depth].role = (u8)role;
        c->depth++;
        if (role >= R_T_FIRST) cap_begin(c, role);
    }
    *pi = i;
    return 0;
}

static int end_tag(ctx_t *c, u32 *pi)
{
    const u32 lt = *pi;
    u32 i = lt + 2u, noff, nlen, nloff;
    int ch, r;
    const frame_t *f;

    r = read_name(c, &i, &noff, &nlen, &nloff);
    if (r) return r;
    while (is_ws(ch = mr_get(&c->r, i))) i++;
    if (ch < 0) FAIL(PE_E_MAN_TRUNC, i);
    if (ch != '>') FAIL(PE_E_MAN_SYNTAX, i);
    i++;
    if (c->depth == 0) FAIL(PE_E_MAN_SYNTAX, lt);                        /* end tag with nothing open */
    f = &c->st[c->depth - 1u];
    if (f->name_len != nlen || !mr_span_eq(&c->r, f->name_off, noff, nlen)) FAIL(PE_E_MAN_SYNTAX, lt);
    if (f->role >= R_T_FIRST) cap_end(c);
    c->depth--;
    if (c->depth == 0) c->root_done = 1;
    *pi = i;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* the scan loop                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

MN_NO_SSP
static int scan(ctx_t *c)
{
    u32 i = c->r.start;
    int r;

    for (;;) {
        int ch = mr_get(&c->r, i);
        if (ch < 0) break;
        if (ch == '<') {
            int n1 = mr_get(&c->r, i + 1u);
            if (n1 == '?') r = skip_past(c, i, i + 2u, "?>", &i);
            else if (n1 == '!') {
                if (mr_match(&c->r, i + 2u, "--")) r = skip_past(c, i, i + 4u, "-->", &i);
                else if (mr_match(&c->r, i + 2u, "[CDATA[")) {
                    if (c->depth == 0) FAIL(PE_E_MAN_SYNTAX, i);
                    r = skip_past(c, i, i + 9u, "]]>", &i);
                } else if (mr_match(&c->r, i + 2u, "DOCTYPE")) FAIL(PE_E_MAN_DOCTYPE, i);
                else FAIL(PE_E_MAN_SYNTAX, i);
            } else if (n1 == '/') r = end_tag(c, &i);
            else if (n1 < 0) FAIL(PE_E_MAN_TRUNC, i);
            else r = start_tag(c, &i);
            if (r) return r;
        } else {
            int unit = ch, is_cp = 0;
            if (c->depth == 0) {                                         /* outside the root: blanks (and NUL padding after it) only */
                if (is_ws(ch) || (ch == 0 && c->root_done)) { i++; continue; }
                FAIL(PE_E_MAN_SYNTAX, i);
            }
            if (ch == 0) FAIL(PE_E_MAN_SYNTAX, i);
            if (ch == '&') {
                u32 cp;
                r = read_entity(c, &i, &cp);
                if (r) return r;
                unit = (int)cp;
                is_cp = 1;
            } else i++;
            cap_unit(c, unit, is_cp);
        }
    }
    if (!c->root_seen || c->depth > 0) FAIL(PE_E_MAN_TRUNC, c->r.n);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* public API                                                                                         */
/* ------------------------------------------------------------------------------------------------ */

MN_NO_SSP
int pe_manifest_parse(const unsigned char *text, unsigned long len, pe_manifest_t *out)
{
    ctx_t c;
    u32 k, errbytes = 0;
    int r, odd = 0;

    if (!out) return PE_E_BOUNDS;
    mn_zero(out, sizeof *out);
    if (!text || len == 0) { out->error = PE_E_MAN_TRUNC; return out->error; }
    if (len > PE_MAN_MAX_DOC) { out->error = PE_E_MAN_TOOBIG; out->error_pos = PE_MAN_MAX_DOC; return out->error; }

    mn_zero(&c, sizeof c);
    c.out = out;
    c.r.p = text;
    if (len >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) { c.r.start = 3; out->encoding = PE_MAN_ENC_UTF8; }
    else if (len >= 2 && text[0] == 0xFF && text[1] == 0xFE) { c.r.wide = 1; c.r.start = 1; out->encoding = PE_MAN_ENC_UTF16LE; }
    else if (len >= 2 && ((text[0] == 0xFE && text[1] == 0xFF) || (text[0] == 0x00 && text[1] == 0x3C))) {
        out->error = PE_E_MAN_ENCODING;                                  /* UTF-16BE (BOM or '<' 0x00 pattern) */
        return out->error;
    } else if (len >= 2 && text[0] == 0x3C && text[1] == 0x00) { c.r.wide = 1; out->encoding = PE_MAN_ENC_UTF16LE; }
    else out->encoding = PE_MAN_ENC_UTF8;
    c.r.n = c.r.wide ? (u32)(len / 2u) : (u32)len;
    odd = c.r.wide && (len & 1u);

    r = scan(&c);
    if (r == 0) {
        out->well_formed = 1;
        if (odd) { out->well_formed = 0; r = PE_E_MAN_ENCODING; errbytes = (u32)len - 1u; }
        else if (!out->is_assembly) { r = PE_E_MAN_ROOT; errbytes = 0; }
    } else {
        errbytes = c.r.wide ? 2u * c.errpos : c.errpos;
    }
    if (errbytes > len) errbytes = (u32)len;

    out->n_elements = c.elements;
    out->n_attributes = c.attrs;
    out->error = r;
    out->error_pos = r ? errbytes : 0;
    finish_assembly(&out->identity);
    for (k = 0; k < out->n_deps; k++) finish_assembly(&out->deps[k]);
    for (k = 0; k < out->n_deps; k++) if (out->deps[k].truncated) out->truncated |= 1u;
    if (out->identity.truncated) out->truncated |= 1u;
    return r;
}

MN_NO_SSP
int pe_manifest_from_image(const pe_info_t *in, const unsigned char *image, pe_manifest_t *out)
{
    static const unsigned exe_order[3] = { PE_MANIFEST_ID_EXE, PE_MANIFEST_ID_DLL, PE_MANIFEST_ID_DLL_NOSI };
    static const unsigned dll_order[3] = { PE_MANIFEST_ID_DLL, PE_MANIFEST_ID_EXE, PE_MANIFEST_ID_DLL_NOSI };
    const unsigned *order;
    const unsigned char *d = 0;
    unsigned sz = 0, k;
    int r = 1;

    if (!out) return PE_E_BOUNDS;
    mn_zero(out, sizeof *out);
    if (!in) return PE_E_BOUNDS;
    order = in->is_dll ? dll_order : exe_order;
    for (k = 0; k < 4u; k++) {
        r = pe_res_find(in, image, PE_RT_MANIFEST, k < 3u ? order[k] : PE_RES_ID_ANY, 0, &d, &sz);
        if (r != 1) break;                                               /* found (0) or hard error (<0) */
    }
    if (r != PE_OK) return r;
    return pe_manifest_parse(d, sz, out);
}

int pe_manifest_requires_elevation(const pe_manifest_t *m)
{
    if (!m) return PE_MAN_ELEV_NONE;
    if (m->exec_levels_seen & (1u << PE_MAN_EXEC_REQUIRE_ADMIN)) return PE_MAN_ELEV_REQUIRED;
    if (m->exec_levels_seen & (1u << PE_MAN_EXEC_HIGHEST)) return PE_MAN_ELEV_IF_AVAILABLE;
    return PE_MAN_ELEV_NONE;
}

/* first recognised token of a comma/blank separated list */
MN_NO_SSP
static int dpi_token_mode(const char *s)
{
    char tok[24];
    u32 i = 0;
    for (;;) {
        u32 n = 0;
        while (s[i] == ',' || s[i] == ' ') i++;
        if (!s[i]) return PE_MAN_DPI_UNSPECIFIED;
        while (s[i] && s[i] != ',' && s[i] != ' ') { if (n + 1u < sizeof tok) tok[n++] = s[i]; i++; }
        tok[n] = 0;
        if (mn_ieq(tok, "permonitorv2")) return PE_MAN_DPI_PER_MONITOR_V2;
        if (mn_ieq(tok, "permonitor")) return PE_MAN_DPI_PER_MONITOR;
        if (mn_ieq(tok, "system")) return PE_MAN_DPI_SYSTEM;
        if (mn_ieq(tok, "unaware")) return PE_MAN_DPI_UNAWARE;
    }
}

MN_NO_SSP
int pe_manifest_dpi_mode(const pe_manifest_t *m)
{
    int r;
    if (!m) return PE_MAN_DPI_UNSPECIFIED;
    if (m->has_dpi_awareness) {
        r = dpi_token_mode(m->dpi_awareness);
        if (r != PE_MAN_DPI_UNSPECIFIED) return r;
    }
    if (m->has_dpi_aware) {
        if (mn_ieq(m->dpi_aware, "true")) return PE_MAN_DPI_SYSTEM;
        if (mn_ieq(m->dpi_aware, "false")) return PE_MAN_DPI_UNAWARE;
        if (mn_ieq(m->dpi_aware, "true/pm") || mn_ieq(m->dpi_aware, "per monitor") || mn_ieq(m->dpi_aware, "permonitor"))
            return PE_MAN_DPI_PER_MONITOR;
    }
    return PE_MAN_DPI_UNSPECIFIED;
}

/* ------------------------------------------------------------------------------------------------ */
/* dependency hints                                                                                   */
/* ------------------------------------------------------------------------------------------------ */

typedef struct { char asm_name[36]; char dll[20]; } known_t;
static const known_t k_known[] = {
    { "Microsoft.Windows.Common-Controls", "comctl32.dll" },
    { "Microsoft.Windows.GdiPlus",         "gdiplus.dll" },
    { "Microsoft.VC80.CRT",                "msvcr80.dll" },
    { "Microsoft.VC90.CRT",                "msvcr90.dll" },
    { "Microsoft.VC80.MFC",                "mfc80.dll" },
    { "Microsoft.VC90.MFC",                "mfc90.dll" },
    { "Microsoft.VC80.ATL",                "atl80.dll" },
    { "Microsoft.VC90.ATL",                "atl90.dll" },
};
#define N_KNOWN (sizeof k_known / sizeof k_known[0])

/* append src to dst (capacity cap incl. NUL); the caller sized things so that this never truncates, but it is
 * bounded regardless */
static void app(char *dst, u32 cap, u32 *len, const char *src)
{
    while (*src && *len + 1u < cap) dst[(*len)++] = *src++;
    dst[*len] = 0;
}

static int safe_name(const char *n, u32 trunc)
{
    u32 i, len = 0;
    if (trunc) return 0;
    for (i = 0; n[i]; i++) {
        u8 c = (u8)n[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return 0;
        if (c == '.' && n[i + 1] == '.') return 0;
        len++;
    }
    if (len == 0 || len >= PE_MAN_NAME_SZ) return 0;
    if (n[0] == '.' || n[len - 1u] == '.') return 0;
    return 1;
}

/* bounded copy: reads at most cap-1 chars of a fixed-size field (so even an unterminated, hand-built struct is safe) */
static void bcopy_str(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (i + 1u < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* lowercase copy of src (src is a NUL-terminated local buffer; bounded by cap incl. NUL) */
static void lower_copy(char *dst, u32 cap, const char *src)
{
    u32 i = 0;
    while (src[i] && i + 1u < cap) { dst[i] = (char)mn_fold((u8)src[i]); i++; }
    dst[i] = 0;
}

static int all_alnum(const char *s)
{
    u32 i;
    for (i = 0; s[i]; i++) {
        u8 c = (u8)s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return 0;
    }
    return i != 0;
}

MN_NO_SSP
int pe_manifest_dep_hint(const pe_man_assembly_t *dep, pe_dep_hint_t *out)
{
    u32 k, i, ntok = 0, len;
    char name[PE_MAN_NAME_SZ], arch[PE_MAN_ARCH_SZ], tok[PE_MAN_TOKEN_SZ], ver[PE_MAN_VER_SZ];
    char lname[PE_MAN_NAME_SZ], ltok[PE_MAN_TOKEN_SZ], larch[PE_MAN_ARCH_SZ];
    int token_ok, arch_any;

    if (!out) return PE_E_BOUNDS;
    mn_zero(out, sizeof *out);
    if (!dep) return PE_E_BOUNDS;
    bcopy_str(name, sizeof name, dep->name);
    bcopy_str(arch, sizeof arch, dep->processor_architecture);
    bcopy_str(tok, sizeof tok, dep->public_key_token);
    bcopy_str(ver, sizeof ver, dep->version);
    if (!safe_name(name, dep->truncated)) return PE_E_MAN_UNSAFE;

    arch_any = (arch[0] == 0 || (arch[0] == '*' && arch[1] == 0));
    out->arch_compatible = (arch_any || mn_ieq(arch, "amd64"));
    bcopy_str(out->sxs_version, sizeof out->sxs_version, ver);

    /* the token must be exactly 16 hex digits (recomputed: the caller may have built `dep` by hand) */
    for (i = 0; tok[i]; i++) {
        if (!hexdig((u8)tok[i])) { ntok = 0; break; }
        ntok++;
    }
    token_ok = (ntok == 16u && tok[16] == 0);

    out->kind = token_ok ? PE_DEP_WINSXS : PE_DEP_PRIVATE;
    for (k = 0; k < N_KNOWN; k++) {
        if (mn_ieq(name, k_known[k].asm_name)) {
            out->kind = PE_DEP_KNOWN_SYSTEM;
            len = 0;
            app(out->system_dll, sizeof out->system_dll, &len, k_known[k].dll);
            break;
        }
    }

    /* WinSxS directory-name prefix: "<arch>_<name>_<token>_", all lowercase */
    lower_copy(lname, sizeof lname, name);
    lower_copy(ltok, sizeof ltok, tok);
    lower_copy(larch, sizeof larch, arch_any ? "amd64" : arch);
    if (token_ok && all_alnum(larch)) {
        len = 0;
        app(out->sxs_prefix, sizeof out->sxs_prefix, &len, larch);
        app(out->sxs_prefix, sizeof out->sxs_prefix, &len, "_");
        app(out->sxs_prefix, sizeof out->sxs_prefix, &len, lname);
        app(out->sxs_prefix, sizeof out->sxs_prefix, &len, "_");
        app(out->sxs_prefix, sizeof out->sxs_prefix, &len, ltok);
        app(out->sxs_prefix, sizeof out->sxs_prefix, &len, "_");
    }

    /* application-local candidates (the name was validated by safe_name: no separators, no "..") */
    len = 0; app(out->dll_candidate[0], PE_DEP_PATH_SZ, &len, name); app(out->dll_candidate[0], PE_DEP_PATH_SZ, &len, ".dll");
    len = 0; app(out->dll_candidate[1], PE_DEP_PATH_SZ, &len, name); app(out->dll_candidate[1], PE_DEP_PATH_SZ, &len, "\\");
             app(out->dll_candidate[1], PE_DEP_PATH_SZ, &len, name); app(out->dll_candidate[1], PE_DEP_PATH_SZ, &len, ".dll");
    len = 0; app(out->manifest_candidate[0], PE_DEP_PATH_SZ, &len, name); app(out->manifest_candidate[0], PE_DEP_PATH_SZ, &len, ".manifest");
    len = 0; app(out->manifest_candidate[1], PE_DEP_PATH_SZ, &len, name); app(out->manifest_candidate[1], PE_DEP_PATH_SZ, &len, "\\");
             app(out->manifest_candidate[1], PE_DEP_PATH_SZ, &len, name); app(out->manifest_candidate[1], PE_DEP_PATH_SZ, &len, ".manifest");
    return 0;
}
