/*
 * pe_resources.c -- hostile-input-safe PE resource directory walker + VS_VERSIONINFO reader.
 * See pe_resources.h for the API contract and README.md for the security rules.
 *
 * Freestanding build gate (warning-free, NO undefined symbols, NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 *       -mno-red-zone -O2 -Wall -Wextra -Werror -c userspace/lib/pe/pe_resources.c
 *
 * Design rules (same as pe.c): byte-wise little-endian loads, every offset widened to u64 and checked with
 * rs_range_ok() before use, every loop capped, nothing read outside [image, image + size_of_image).
 *
 * Test-only: -DPE_RES_INJECT_BUG=N deliberately plants library bug N (1: the bounds checks on resource-name strings
 * are removed; 2: the version-node length check against its parent is removed).  The fuzz runner
 * (tests/win/run_res_tests.sh) builds the fuzzer that way to prove it detects each of them.  Never defined in a real
 * build (the RS_BUG() tests fold to 0).
 */
#include "pe_resources.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define RS_BARRIER() __asm__ volatile("" ::: "memory")

#ifdef PE_RES_INJECT_BUG
#define RS_BUG(n) (PE_RES_INJECT_BUG == (n))
#else
#define RS_BUG(n) 0
#endif

#if defined(__GNUC__) && (__GNUC__ >= 11) && !defined(__clang__)
#define RS_NO_SSP __attribute__((no_stack_protector))
#else
#define RS_NO_SSP
#endif

/* ------------------------------------------------------------------------------------------------ */
/* tiny helpers (private copies; pe.c's are static)                                                  */
/* ------------------------------------------------------------------------------------------------ */

static void rs_zero(void *p, u64 n)
{
    u8 *d = (u8 *)p;
    while (n) { *d++ = 0; n--; RS_BARRIER(); }
}

static inline u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static inline u32 rd32(const u8 *p) { return rd16(p) | (rd16(p + 2) << 16); }

/* off+n <= limit without overflow. */
static inline int rs_range_ok(u64 off, u64 n, u64 limit) { return off <= limit && n <= limit - off; }

static inline u32 rs_fold(u32 c) { return (c >= 'a' && c <= 'z') ? c - 32u : c; }

const char *pe_res_strerror(int err)
{
    switch (err) {
    case PE_RES_NOTFOUND:  return "resource not found";
    case PE_E_RES_BOUNDS:  return "resource structure outside the resource tree or image";
    case PE_E_RES_FORMAT:  return "malformed resource directory";
    case PE_E_RES_LOOP:    return "resource directory loop or overlap";
    case PE_E_RES_DEPTH:   return "resource directory deeper than 3 levels";
    case PE_E_RES_LIMIT:   return "resource directory limit exceeded";
    case PE_E_VERSION:     return "malformed version resource";
    default:               return pe_strerror(err);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* locating the resource data directory in the mapped headers                                         */
/* ------------------------------------------------------------------------------------------------ */

int pe_res_dir(const pe_info_t *in, const unsigned char *image, unsigned *rva, unsigned *size)
{
    u64 isz, hdr, lfanew, optoff, sizeopt;
    const u8 *coff, *opt;
    u32 nrva, r_rva, r_size;

    if (rva) *rva = 0;
    if (size) *size = 0;
    if (!in || !image) return PE_E_BOUNDS;
    isz = in->size_of_image;
    hdr = in->size_of_headers;
    if (isz == 0 || isz > PE_MAX_IMAGE) return PE_E_SIZE;
    if (hdr > isz) return PE_E_SIZE;
    if (hdr < 0x40u) return PE_E_BOUNDS;
    if (image[0] != 'M' || image[1] != 'Z') return PE_E_MAGIC;
    lfanew = rd32(image + 0x3c);
    if (lfanew < 0x40u || !rs_range_ok(lfanew, 24u, hdr)) return PE_E_BOUNDS;
    if (image[lfanew] != 'P' || image[lfanew + 1] != 'E' || image[lfanew + 2] != 0 || image[lfanew + 3] != 0)
        return PE_E_MAGIC;
    coff = image + lfanew + 4u;
    sizeopt = rd16(coff + 16);
    optoff = lfanew + 24u;
    if (sizeopt < 112u || !rs_range_ok(optoff, sizeopt, hdr)) return PE_E_BOUNDS;
    opt = image + optoff;
    if (rd16(opt) != 0x20bu) return PE_E_MAGIC;
    nrva = rd32(opt + 108);
    if (nrva > 16u || sizeopt < 112u + 8u * (u64)nrva) return PE_E_BOUNDS;
    if (nrva <= 2u) return 1;                                /* no resource slot in the directory array */
    r_rva = rd32(opt + 112u + 16u);
    r_size = rd32(opt + 116u + 16u);
    if ((r_rva | r_size) == 0) return 1;
    if (r_rva == 0 || r_size == 0) return PE_E_BOUNDS;       /* same rule as pe.h: both zero or both nonzero */
    if (!rs_range_ok(r_rva, r_size, isz)) return PE_E_BOUNDS;
    if (rva) *rva = r_rva;
    if (size) *size = r_size;
    return PE_OK;
}

/* ------------------------------------------------------------------------------------------------ */
/* tree walking                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

#define RS_DIR_HDR  16u
#define RS_ENT      8u
#define RS_DATA_ENT 16u
#define RS_HIGH     0x80000000u

typedef struct {
    const u8 *img;      /* mapped image                                                              */
    u64 isz;            /* size_of_image                                                             */
    u64 base;           /* RVA of the resource tree (data directory RVA)                             */
    u64 lim;            /* bytes of tree that may be addressed (data directory Size)                 */
    u32 nodes;          /* entries examined so far (whole-call budget)                               */
} rs_t;

typedef struct { u64 off, end; u32 n; } rs_span_t;   /* one opened directory table: [off,end) incl. header */

static int rs_init(rs_t *rs, const pe_info_t *in, const u8 *image)
{
    unsigned rva, size;
    int r = pe_res_dir(in, image, &rva, &size);
    if (r != PE_OK) return r;                                /* 1 = none, <0 = error */
    rs->img = image;
    rs->isz = in->size_of_image;
    rs->base = rva;
    rs->lim = size;
    rs->nodes = 0;
    return PE_OK;
}

/* Open the directory table at tree offset `off`; anc[0..nanc) are the enclosing tables. */
static int rs_open(rs_t *rs, u64 off, const rs_span_t *anc, u32 nanc, rs_span_t *d)
{
    const u8 *p;
    u64 n, end;
    u32 a;

    if (!rs_range_ok(off, RS_DIR_HDR, rs->lim)) return PE_E_RES_BOUNDS;
    p = rs->img + rs->base + off;
    n = (u64)rd16(p + 12) + (u64)rd16(p + 14);               /* named + id entries */
    if (n > PE_RES_MAX_ENTRIES) return PE_E_RES_LIMIT;
    end = off + RS_DIR_HDR + RS_ENT * n;
    if (end > rs->lim) return PE_E_RES_BOUNDS;
    for (a = 0; a < nanc; a++)                               /* equal or overlapping an ancestor == loop */
        if (off < anc[a].end && anc[a].off < end) return PE_E_RES_LOOP;
    rs->nodes += (u32)n + 1u;                                /* n <= 32768, nodes <= 131072+32769: no overflow */
    if (rs->nodes > PE_RES_MAX_NODES) return PE_E_RES_LIMIT;
    d->off = off;
    d->end = end;
    d->n = (u32)n;
    return PE_OK;
}

/* entry k < d->n of an opened table (bounds were proven by rs_open) */
static void rs_entry(const rs_t *rs, const rs_span_t *d, u32 k, u32 *namef, u32 *offf)
{
    const u8 *p = rs->img + rs->base + d->off + RS_DIR_HDR + (u64)RS_ENT * k;
    *namef = rd32(p);
    *offf = rd32(p + 4);
}

/* IMAGE_RESOURCE_DIR_STRING_U at the offset in a named entry: u16 length, then that many UTF-16 units.
 * Validates BOTH the length word and the characters against the tree.  Returns the byte offset of the first
 * character in *coff and the length in *len. */
static int rs_str(const rs_t *rs, u32 namef, u32 *len, u64 *coff)
{
    u64 off = namef & ~RS_HIGH;
    u32 l;
    if (!RS_BUG(1) && !rs_range_ok(off, 2u, rs->lim)) return PE_E_RES_BOUNDS;
    l = rd16(rs->img + rs->base + off);
    if (!RS_BUG(1) && !rs_range_ok(off + 2u, 2u * (u64)l, rs->lim)) return PE_E_RES_BOUNDS;
    *len = l;
    *coff = off + 2u;
    return PE_OK;
}

/* 1 = entry name matches key, 0 = no, <0 = error.  A numeric key never reads a name string. */
RS_NO_SSP
static int rs_match(const rs_t *rs, u32 namef, const pe_res_key_t *key)
{
    u32 klen, len, i;
    u64 coff;
    int r;

    if (!key) return 1;
    if (key->name == 0) {
        if (key->id == PE_RES_ID_ANY) return 1;
        if (namef & RS_HIGH) return 0;
        return namef == key->id;
    }
    if (!(namef & RS_HIGH)) return 0;
    for (klen = 0; key->name[klen]; klen++)
        if (klen >= PE_RES_KEY_MAX) return PE_E_RES_LIMIT;
    if (!rs_range_ok(namef & ~RS_HIGH, 2u, rs->lim)) return 0;   /* unreadable length: cannot be our name */
    len = rd16(rs->img + rs->base + (namef & ~RS_HIGH));
    if (len != klen) return 0;                                   /* cheap reject: no string bytes touched */
    r = rs_str(rs, namef, &len, &coff);
    if (r != PE_OK) return r;
    for (i = 0; i < len; i++) {
        u32 u = rd16(rs->img + rs->base + coff + 2u * i);
        u32 k = (u8)key->name[i];
        if (rs_fold(u) != rs_fold(k)) return 0;
    }
    return 1;
}

/* Linear scan (no reliance on sort order).  Returns 0 + *k, or 1 if no entry matches, or an error. */
static int rs_scan(const rs_t *rs, const rs_span_t *d, const pe_res_key_t *key, u32 *k)
{
    u32 i;
    for (i = 0; i < d->n; i++) {
        u32 namef, offf;
        int m;
        rs_entry(rs, d, i, &namef, &offf);
        m = rs_match(rs, namef, key);
        if (m < 0) return m;
        if (m) { *k = i; return PE_OK; }
    }
    return PE_RES_NOTFOUND;
}

/* Render a node identity (id or ASCII-bounded name).  Validates the name string. */
RS_NO_SSP
static int rs_fill_id(const rs_t *rs, u32 namef, pe_res_id_t *o)
{
    u32 len, i;
    u64 coff;
    int r;

    rs_zero(o, sizeof *o);
    if (!(namef & RS_HIGH)) { o->id = namef; return PE_OK; }
    r = rs_str(rs, namef, &len, &coff);
    if (r != PE_OK) return r;
    o->is_named = 1;
    o->name_len = len;
    for (i = 0; i < len && i + 1u < PE_RES_NAME_MAX; i++) {
        u32 u = rd16(rs->img + rs->base + coff + 2u * i);
        o->str[i] = (u >= 0x20u && u < 0x7fu) ? (char)u : '?';
    }
    o->str[i] = 0;
    return PE_OK;
}

/* IMAGE_RESOURCE_DATA_ENTRY at tree offset `offf` (no high bit).  The data block must be inside the image. */
static int rs_leaf(const rs_t *rs, u32 offf, pe_res_entry_t *e)
{
    const u8 *p;
    u32 rva, size;
    if (offf & RS_HIGH) return PE_E_RES_DEPTH;               /* a directory where a data entry belongs */
    if (!rs_range_ok(offf, RS_DATA_ENT, rs->lim)) return PE_E_RES_BOUNDS;
    p = rs->img + rs->base + offf;
    rva = rd32(p);
    size = rd32(p + 4);
    if (!rs_range_ok(rva, size, rs->isz)) return PE_E_RES_BOUNDS;
    e->rva = rva;
    e->size = size;
    e->codepage = rd32(p + 8);
    e->data = rs->img + rva;
    return PE_OK;
}

/* exact > same primary language > neutral > en-US > anything (the order Microsoft documents for FindResource) */
static u32 rs_lang_score(u32 lang, u32 pref)
{
    if (pref == PE_RES_LANG_ANY) return 1;
    if (lang == pref) return 5;
    if (lang != 0 && (lang & 0x3ffu) == (pref & 0x3ffu)) return 4;
    if (lang == 0) return 3;
    if (lang == 0x0409u) return 2;
    return 1;
}

/* A string key is caller data, but cap it anyway (and reject it before any tree access). */
static int rs_key_ok(const pe_res_key_t *key)
{
    u32 n;
    if (!key || !key->name) return PE_OK;
    for (n = 0; key->name[n]; n++)
        if (n >= PE_RES_KEY_MAX) return PE_E_RES_LIMIT;
    return PE_OK;
}

RS_NO_SSP
int pe_res_find_ex(const pe_info_t *in, const unsigned char *image, const pe_res_key_t *type,
                   const pe_res_key_t *name, unsigned lang_pref, pe_res_entry_t *out)
{
    rs_t rs;
    rs_span_t root, tdir, ldir, anc[2];
    pe_res_entry_t tmp;
    pe_res_entry_t *e = out ? out : &tmp;
    u32 k = 0, tnamef, toff, nnamef, noff, lnamef = 0, loff = 0, i, best_score = 0;
    int r;

    rs_zero(e, sizeof *e);
    r = rs_key_ok(type);
    if (r == PE_OK) r = rs_key_ok(name);
    if (r != PE_OK) return r;
    r = rs_init(&rs, in, image);
    if (r != PE_OK) return r;
    r = rs_open(&rs, 0, 0, 0, &root);
    if (r != PE_OK) return r;

    r = rs_scan(&rs, &root, type, &k);                       /* level 1: type */
    if (r != PE_OK) return r;
    rs_entry(&rs, &root, k, &tnamef, &toff);
    if (!(toff & RS_HIGH)) return PE_E_RES_FORMAT;           /* a data entry where a directory belongs */
    anc[0] = root;
    r = rs_open(&rs, toff & ~RS_HIGH, anc, 1, &tdir);
    if (r != PE_OK) return r;

    r = rs_scan(&rs, &tdir, name, &k);                       /* level 2: name / id */
    if (r != PE_OK) return r;
    rs_entry(&rs, &tdir, k, &nnamef, &noff);
    if (!(noff & RS_HIGH)) return PE_E_RES_FORMAT;
    anc[1] = tdir;
    r = rs_open(&rs, noff & ~RS_HIGH, anc, 2, &ldir);
    if (r != PE_OK) return r;

    if (ldir.n == 0) return PE_RES_NOTFOUND;                 /* level 3: language */
    for (i = 0; i < ldir.n; i++) {
        u32 ln, lo, lang, sc;
        rs_entry(&rs, &ldir, i, &ln, &lo);
        if (lo & RS_HIGH) return PE_E_RES_DEPTH;
        lang = (ln & RS_HIGH) ? 0u : (ln & 0xffffu);
        sc = rs_lang_score(lang, lang_pref);
        if (sc > best_score) { best_score = sc; lnamef = ln; loff = lo; }
    }

    r = rs_leaf(&rs, loff, e);
    if (r != PE_OK) { rs_zero(e, sizeof *e); return r; }
    e->lang = (lnamef & RS_HIGH) ? 0u : (lnamef & 0xffffu);
    r = rs_fill_id(&rs, tnamef, &e->type);
    if (r == PE_OK) r = rs_fill_id(&rs, nnamef, &e->name);
    if (r != PE_OK) { rs_zero(e, sizeof *e); return r; }
    return PE_OK;
}

RS_NO_SSP
int pe_res_find(const pe_info_t *in, const unsigned char *image, unsigned type, unsigned id,
                unsigned lang_pref, const unsigned char **data, unsigned *size)
{
    pe_res_key_t kt = pe_res_key_id(type), kn = pe_res_key_id(id);
    pe_res_entry_t e;
    int r;

    if (data) *data = 0;
    if (size) *size = 0;
    r = pe_res_find_ex(in, image, &kt, &kn, lang_pref, &e);
    if (r == PE_OK) {
        if (data) *data = e.data;
        if (size) *size = e.size;
    }
    return r;
}

RS_NO_SSP
int pe_res_enum(const pe_info_t *in, const unsigned char *image, const pe_res_key_t *type,
                pe_res_enum_fn fn, void *user, unsigned *n_visited)
{
    rs_t rs;
    rs_span_t root, tdir, ldir, anc[2];
    pe_res_entry_t e;
    u32 ti, ni, li, visited = 0;
    int r;

    if (n_visited) *n_visited = 0;
    if (!fn) return PE_E_BOUNDS;
    r = rs_key_ok(type);
    if (r != PE_OK) return r;
    r = rs_init(&rs, in, image);
    if (r != PE_OK) return r;
    r = rs_open(&rs, 0, 0, 0, &root);
    if (r != PE_OK) return r;
    anc[0] = root;

    for (ti = 0; ti < root.n; ti++) {
        u32 tnamef, toff;
        rs_entry(&rs, &root, ti, &tnamef, &toff);
        r = rs_match(&rs, tnamef, type);
        if (r < 0) goto done;
        if (r == 0) continue;
        if (!(toff & RS_HIGH)) { r = PE_E_RES_FORMAT; goto done; }
        r = rs_open(&rs, toff & ~RS_HIGH, anc, 1, &tdir);
        if (r != PE_OK) goto done;
        anc[1] = tdir;

        for (ni = 0; ni < tdir.n; ni++) {
            u32 nnamef, noff;
            rs_entry(&rs, &tdir, ni, &nnamef, &noff);
            if (!(noff & RS_HIGH)) { r = PE_E_RES_FORMAT; goto done; }
            r = rs_open(&rs, noff & ~RS_HIGH, anc, 2, &ldir);
            if (r != PE_OK) goto done;

            for (li = 0; li < ldir.n; li++) {
                u32 lnamef, loff;
                rs_entry(&rs, &ldir, li, &lnamef, &loff);
                r = rs_leaf(&rs, loff, &e);
                if (r != PE_OK) goto done;
                e.lang = (lnamef & RS_HIGH) ? 0u : (lnamef & 0xffffu);
                r = rs_fill_id(&rs, tnamef, &e.type);
                if (r != PE_OK) goto done;
                r = rs_fill_id(&rs, nnamef, &e.name);
                if (r != PE_OK) goto done;
                visited++;
                if (fn(&e, user) != 0) { r = PE_OK; goto done; }
            }
        }
    }
    r = PE_OK;
done:
    if (n_visited) *n_visited = visited;
    if (r == PE_RES_NOTFOUND) r = PE_OK;                     /* defensive: scan codes never leak from enum */
    return r;
}

/* ------------------------------------------------------------------------------------------------ */
/* VS_VERSIONINFO                                                                                    */
/* ------------------------------------------------------------------------------------------------ */

/* Layout (public docs): every node is  WORD wLength, WORD wValueLength, WORD wType, WCHAR szKey[],
 * pad to DWORD, Value, pad to DWORD, Children.  wLength excludes the padding that follows the node.
 * Alignment is taken relative to the start of the blob (the resource data is DWORD aligned in an image). */

#define VER_KEY_MAX 64u

typedef struct {
    u64 off, end;               /* [off, end): the node, end = off + wLength (clamped for the root only)    */
    u32 vlen, type;
    u64 key_off, key_len;       /* byte offset of szKey and its length in UTF-16 units (without the NUL)   */
    u64 val_off;                /* DWORD-aligned start of the value, clamped to end                         */
} vnode_t;

static inline u64 ver_align4(u64 v) { return (v + 3u) & ~(u64)3u; }

static int ver_node(const u8 *b, u64 off, u64 limit, int clamp, vnode_t *n)
{
    u64 wlen, k, i, voff;

    if (!rs_range_ok(off, 6u, limit)) return PE_E_VERSION;
    wlen = rd16(b + off);
    if (wlen < 6u) return PE_E_VERSION;
    if (!RS_BUG(2) && !rs_range_ok(off, wlen, limit)) {
        if (!clamp) return PE_E_VERSION;
        wlen = limit - off;                                  /* root only: tolerate an over-long wLength */
        if (wlen < 6u) return PE_E_VERSION;
    }
    n->off = off;
    n->end = off + wlen;
    n->vlen = rd16(b + off + 2);
    n->type = rd16(b + off + 4);
    k = off + 6u;
    for (i = 0;; i++) {                                      /* szKey: NUL-terminated UTF-16, inside the node */
        if (i > VER_KEY_MAX) return PE_E_VERSION;
        if (!rs_range_ok(k + 2u * i, 2u, n->end)) return PE_E_VERSION;
        if (rd16(b + k + 2u * i) == 0) break;
    }
    n->key_off = k;
    n->key_len = i;
    voff = ver_align4(k + 2u * (i + 1u));
    n->val_off = voff > n->end ? n->end : voff;
    return PE_OK;
}

/* case-insensitive ASCII compare of the node's UTF-16 key with a C string */
static int ver_key_is(const u8 *b, const vnode_t *n, const char *s)
{
    u64 i;
    for (i = 0; i < n->key_len; i++) {
        u32 u = rd16(b + n->key_off + 2u * i);
        u32 c = (u8)s[i];
        if (c == 0 || rs_fold(u) != rs_fold(c)) return 0;
    }
    return s[i] == 0;
}

/* First child of `p` whose value area has `vbytes` bytes. */
static u64 ver_child_start(const vnode_t *p, u64 vbytes)
{
    u64 s = ver_align4(p->val_off + vbytes);
    return s > p->end ? p->end : s;
}

static int ver_hexval(u32 c) { return c >= '0' && c <= '9' ? (int)(c - '0') : c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10u) : c >= 'A' && c <= 'F' ? (int)(c - 'A' + 10u) : -1; }

/* "04090 4B0" -> lang, codepage; 0xFFFFFFFF both when the key is not 8 hex digits. */
static void ver_table_key(const u8 *b, const vnode_t *t, u32 *lang, u32 *cp)
{
    u32 v = 0, i;
    *lang = *cp = 0xFFFFFFFFu;
    if (t->key_len != 8u) return;
    for (i = 0; i < 8u; i++) {
        int h = ver_hexval(rd16(b + t->key_off + 2u * i));
        if (h < 0) return;
        v = (v << 4) | (u32)h;
    }
    *lang = v >> 16;
    *cp = v & 0xffffu;
}

/* UTF-16 (nchars units max, stops at NUL) -> bounded printable ASCII.  Returns 1 if the string was cut. */
static int ver_ascii(char *dst, const u8 *src, u64 nchars)
{
    u32 o = 0;
    u64 i;
    int cut = 0;
    for (i = 0; i < nchars; i++) {
        u32 u = rd16(src + 2u * i);
        char c;
        if (u == 0) break;
        if (u >= 0xDC00u && u <= 0xDFFFu) continue;          /* low surrogate: the lead already produced '?' */
        c = (u >= 0x20u && u < 0x7fu) ? (char)u : (u < 0x80u ? ' ' : '?');
        if (o + 1u >= PE_VER_STR_MAX) { cut = 1; break; }
        dst[o++] = c;
    }
    dst[o] = 0;
    return cut;
}

typedef struct { u64 node_off; u32 lang, cp; } ver_table_t;

RS_NO_SSP
int pe_version_parse(const unsigned char *blob, unsigned size, unsigned lang_pref, pe_version_t *out)
{
    enum { MAX_TABLES = 16 };
    ver_table_t tab[MAX_TABLES];
    u32 ntab = 0, nodes = 0, sel, i;
    u32 trans_lang = 0xFFFFFFFFu;
    int have_trans = 0;
    vnode_t root, c, t, s;
    u64 off, toff, soff, vb, limit = size;
    int r;

    if (!out) return PE_E_BOUNDS;
    rs_zero(out, sizeof *out);
    out->lang = out->codepage = 0xFFFFFFFFu;
    if (!blob) return PE_E_VERSION;

    r = ver_node(blob, 0, limit, 1, &root);
    if (r != PE_OK) return r;
    if (!ver_key_is(blob, &root, "VS_VERSION_INFO")) return PE_E_VERSION;

    /* fixed file info: 0 bytes (absent) or >= 52 bytes starting with 0xFEEF04BD */
    vb = root.vlen;
    if (vb != 0) {
        const u8 *f;
        if (vb < 52u || !rs_range_ok(root.val_off, 52u, root.end)) return PE_E_VERSION;
        f = blob + root.val_off;
        if (rd32(f) != 0xFEEF04BDu) return PE_E_VERSION;
        out->has_fixed = 1;
        out->struct_version = rd32(f + 4);
        out->file_version_ms = rd32(f + 8);
        out->file_version_ls = rd32(f + 12);
        out->product_version_ms = rd32(f + 16);
        out->product_version_ls = rd32(f + 20);
        out->file_flags_mask = rd32(f + 24);
        out->file_flags = rd32(f + 28);
        out->file_os = rd32(f + 32);
        out->file_type = rd32(f + 36);
        out->file_subtype = rd32(f + 40);
        out->file_version[0] = (unsigned short)(out->file_version_ms >> 16);
        out->file_version[1] = (unsigned short)(out->file_version_ms & 0xffffu);
        out->file_version[2] = (unsigned short)(out->file_version_ls >> 16);
        out->file_version[3] = (unsigned short)(out->file_version_ls & 0xffffu);
        out->product_version[0] = (unsigned short)(out->product_version_ms >> 16);
        out->product_version[1] = (unsigned short)(out->product_version_ms & 0xffffu);
        out->product_version[2] = (unsigned short)(out->product_version_ls >> 16);
        out->product_version[3] = (unsigned short)(out->product_version_ls & 0xffffu);
    }

    /* children of the root: StringFileInfo (tables) and VarFileInfo (Translation) */
    for (off = ver_child_start(&root, vb); rs_range_ok(off, 6u, root.end); off = ver_align4(c.end)) {
        if (++nodes > PE_VER_MAX_NODES) return PE_E_VERSION;
        r = ver_node(blob, off, root.end, 0, &c);
        if (r != PE_OK) return r;

        if (ver_key_is(blob, &c, "StringFileInfo")) {
            for (toff = ver_child_start(&c, 0); rs_range_ok(toff, 6u, c.end); toff = ver_align4(t.end)) {
                if (++nodes > PE_VER_MAX_NODES) return PE_E_VERSION;
                r = ver_node(blob, toff, c.end, 0, &t);
                if (r != PE_OK) return r;
                if (ntab < MAX_TABLES) {
                    tab[ntab].node_off = toff;
                    ver_table_key(blob, &t, &tab[ntab].lang, &tab[ntab].cp);
                    ntab++;
                }
            }
        } else if (ver_key_is(blob, &c, "VarFileInfo")) {
            for (toff = ver_child_start(&c, 0); rs_range_ok(toff, 6u, c.end); toff = ver_align4(t.end)) {
                if (++nodes > PE_VER_MAX_NODES) return PE_E_VERSION;
                r = ver_node(blob, toff, c.end, 0, &t);
                if (r != PE_OK) return r;
                if (!have_trans && ver_key_is(blob, &t, "Translation") && t.vlen >= 4u &&
                    rs_range_ok(t.val_off, 4u, t.end)) {
                    trans_lang = rd16(blob + t.val_off);     /* DWORD: low word LANGID, high word code page */
                    have_trans = 1;
                }
            }
        }
    }

    if (ntab == 0) return PE_OK;                             /* fixed info only: fine */

    /* choose the table: exact lang_pref, primary-language match, first Translation, first table */
    sel = ntab;
    if (lang_pref != PE_RES_LANG_ANY) {
        for (i = 0; i < ntab && sel == ntab; i++) if (tab[i].lang == lang_pref) sel = i;
        for (i = 0; i < ntab && sel == ntab; i++)
            if (tab[i].lang != 0xFFFFFFFFu && (tab[i].lang & 0x3ffu) == (lang_pref & 0x3ffu)) sel = i;
    }
    if (sel == ntab && have_trans)
        for (i = 0; i < ntab && sel == ntab; i++) if (tab[i].lang == trans_lang) sel = i;
    if (sel == ntab) sel = 0;

    r = ver_node(blob, tab[sel].node_off, root.end, 0, &t);  /* re-read (cheap); it validated once already */
    if (r != PE_OK) return r;
    out->has_strings = 1;
    out->lang = tab[sel].lang;
    out->codepage = tab[sel].cp;

    for (soff = ver_child_start(&t, 0); rs_range_ok(soff, 6u, t.end); soff = ver_align4(s.end)) {
        u64 nch, avail;
        char *dst = 0;
        u32 bit = 0;
        if (++nodes > PE_VER_MAX_NODES) return PE_E_VERSION;
        r = ver_node(blob, soff, t.end, 0, &s);
        if (r != PE_OK) return r;

        if (ver_key_is(blob, &s, "FileDescription"))      { bit = PE_VER_S_DESCRIPTION; dst = out->file_description; }
        else if (ver_key_is(blob, &s, "ProductName"))      { bit = PE_VER_S_PRODUCT;     dst = out->product_name; }
        else if (ver_key_is(blob, &s, "CompanyName"))      { bit = PE_VER_S_COMPANY;     dst = out->company_name; }
        else if (ver_key_is(blob, &s, "OriginalFilename")) { bit = PE_VER_S_ORIGINAL;    dst = out->original_filename; }
        else if (ver_key_is(blob, &s, "FileVersion"))      { bit = PE_VER_S_FILEVER;     dst = out->file_version_str; }
        else if (ver_key_is(blob, &s, "ProductVersion"))   { bit = PE_VER_S_PRODVER;     dst = out->product_version_str; }
        if (!dst || (out->have_str & bit)) continue;         /* first occurrence wins (VerQueryValue order) */

        avail = (s.end - s.val_off) / 2u;                    /* UTF-16 units that physically exist */
        nch = s.vlen < avail ? s.vlen : avail;               /* wValueLength counts WORDs for text values */
        if (ver_ascii(dst, blob + s.val_off, nch)) out->truncated |= bit;
        out->have_str |= bit;
    }
    return PE_OK;
}

RS_NO_SSP
int pe_version_info(const pe_info_t *in, const unsigned char *image, unsigned lang_pref, pe_version_t *out)
{
    const unsigned char *d = 0;
    unsigned sz = 0;
    int r;

    if (out) {
        rs_zero(out, sizeof *out);
        out->lang = out->codepage = 0xFFFFFFFFu;
    }
    if (!out) return PE_E_BOUNDS;
    r = pe_res_find(in, image, PE_RT_VERSION, PE_RES_ID_ANY, lang_pref, &d, &sz);
    if (r != PE_OK) return r;
    return pe_version_parse(d, sz, lang_pref, out);
}
