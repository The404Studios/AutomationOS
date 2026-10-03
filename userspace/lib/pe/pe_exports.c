/*
 * pe_exports.c -- hostile-input-safe PE export-directory lookup + delay-load import binding.
 * See pe_exports.h for the API contract and README.md for the security rules.
 *
 * Freestanding build gate (must be warning-free, no undefined symbols once linked with pe.c, no fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 *       -mno-red-zone -O2 -Wall -Wextra -Werror -c userspace/lib/pe/pe_exports.c
 *
 * Same design rules as pe.c: byte-wise little-endian loads, every offset widened to u64 and range-checked with
 * ex_range_ok(), every loop capped, nothing read outside [image, image + size_of_image).
 */
#include "pe_exports.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#if defined(__GNUC__) && (__GNUC__ >= 11) && !defined(__clang__)
#define EX_NO_SSP __attribute__((no_stack_protector))
#else
#define EX_NO_SSP
#endif

static inline u16 rd16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static inline void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static inline void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }

/* Loop-idiom barrier: stops GCC from turning the copy loops below back into memcpy calls (a freestanding build
 * does not provide memcpy). */
#define EX_BARRIER() __asm__ volatile("" ::: "memory")
static inline void ex_copy(char *d, const char *s, u32 n) { while (n) { *d++ = *s++; n--; EX_BARRIER(); } }
static inline void ex_copy_img(char *d, const u8 *s, u32 n) { while (n) { *d++ = (char)*s++; n--; EX_BARRIER(); } }

/* off+n <= limit without overflow. */
static inline int ex_range_ok(u64 off, u64 n, u64 limit) { return off <= limit && n <= limit - off; }

const char *pe_export_strerror(int err)
{
    switch (err) {
    case PE_E_EXPORT:   return "malformed export directory";
    case PE_E_NOTFOUND: return "no such export";
    case PE_E_BUFSIZE:  return "output buffer too small";
    case PE_E_FWD:      return "malformed forwarder string";
    default:            return pe_strerror(err);
    }
}

/* Same sanity gate pe.c applies to every pe_info_t consumer (the struct is caller-owned). */
static int ex_info_check(const pe_info_t *in)
{
    if (!in) return PE_E_BOUNDS;
    if (in->size_of_image == 0 || in->size_of_image > PE_MAX_IMAGE) return PE_E_SIZE;
    if (in->size_of_headers > in->size_of_image) return PE_E_SIZE;
    if (in->n_sections == 0 || in->n_sections > PE_MAX_SECTIONS) return PE_E_SECTIONS;
    return PE_OK;
}

/* ------------------------------------------------------------------------------------------------ */
/* export directory                                                                                  */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    u64 isz;
    u64 dir_lo, dir_hi;                 /* [export_rva, export_rva + export_size): forwarder RVAs live here */
    u32 base, nfuncs, nnames;
    u32 eat, names, ords;
} exp_dir_t;

/* Returns PE_OK, 1 = no export directory, or a negative code.  O(1): checks the header and the table extents. */
static int exp_dir(const pe_info_t *in, const u8 *image, exp_dir_t *d)
{
    const u8 *p;
    int r = ex_info_check(in);

    if (r != PE_OK) return r;
    if (!image) return PE_E_BOUNDS;
    d->isz = in->size_of_image;
    if ((in->export_rva | in->export_size) == 0) return 1;
    if (in->export_rva == 0 || in->export_size < 40u || !ex_range_ok(in->export_rva, in->export_size, d->isz))
        return PE_E_EXPORT;
    d->dir_lo = in->export_rva;
    d->dir_hi = (u64)in->export_rva + in->export_size;
    p = image + in->export_rva;
    d->base = rd32(p + 16);
    d->nfuncs = rd32(p + 20);
    d->nnames = rd32(p + 24);
    d->eat = rd32(p + 28);
    d->names = rd32(p + 32);
    d->ords = rd32(p + 36);
    if (d->nfuncs > PE_MAX_EXPORTS || d->nnames > PE_MAX_EXPORTS) return PE_E_EXPORT;
    if (d->nfuncs && (d->eat == 0 || !ex_range_ok(d->eat, 4ull * d->nfuncs, d->isz))) return PE_E_EXPORT;
    if (d->nnames) {
        if (d->names == 0 || !ex_range_ok(d->names, 4ull * d->nnames, d->isz)) return PE_E_EXPORT;
        if (d->ords == 0 || !ex_range_ok(d->ords, 2ull * d->nnames, d->isz)) return PE_E_EXPORT;
    }
    return PE_OK;
}

/* Walks the NUL-terminated string at image[rva] (at most `cap` bytes including the NUL, all inside the image).
 * If cmp != NULL also compares it for EXACT equality.  Returns -1 malformed (outside the image / not terminated
 * within cap), 0 differs, 1 equal (cmp == NULL: 1).  Always scans to the terminator so a malformed string is
 * reported regardless of the comparison outcome.  *len (nullable) = string length on 0/1. */
static int exp_str(const u8 *image, u64 isz, u64 rva, u32 cap, const char *cmp, u32 *len)
{
    u64 k;
    int eq = cmp != 0;

    for (k = 0; k < cap; k++) {
        u8 c;
        if (rva >= isz || k >= isz - rva) return -1;
        c = image[rva + k];
        if (c == 0) {
            if (len) *len = (u32)k;
            return (cmp == 0) ? 1 : (eq && (u8)cmp[k] == 0);
        }
        /* cmp[k] is only read while eq is still set, i.e. every earlier byte matched a NONZERO cmp byte, so
         * index k is at most the position of cmp's own terminator. */
        if (eq && (u8)cmp[k] != c) eq = 0;
    }
    return -1;
}

/* Resolve EAT slot idx (< nfuncs, caller-checked) into the pe_export_find result convention. */
static int exp_slot(const exp_dir_t *d, const u8 *image, u32 idx, u32 *out_rva, char *fwd, u32 fwd_cap)
{
    u32 rva = rd32(image + d->eat + 4ull * idx);
    u32 n;

    if (rva == 0) return PE_E_NOTFOUND;                         /* unused slot */
    if (!((u64)rva >= d->dir_lo && (u64)rva < d->dir_hi)) {     /* ordinary export */
        if ((u64)rva >= d->isz) return PE_E_EXPORT;
        if (out_rva) *out_rva = rva;
        return PE_OK;
    }
    /* forwarder: the string must terminate inside the image within PE_MAX_FWD bytes */
    if (exp_str(image, d->isz, rva, PE_MAX_FWD, 0, &n) != 1) return PE_E_EXPORT;
    if (n == 0) return PE_E_EXPORT;                             /* empty forwarder */
    if (fwd) {
        if (fwd_cap == 0 || n + 1u > fwd_cap) return PE_E_BUFSIZE;
        ex_copy_img(fwd, image + rva, n);
        fwd[n] = 0;
    }
    if (out_rva) *out_rva = rva;
    return PE_EXPORT_FORWARDER;
}

EX_NO_SSP
int pe_export_find(const pe_info_t *in, const unsigned char *image, const char *name, unsigned int ordinal,
                   int by_ordinal, unsigned int *out_rva, char *fwd, unsigned int fwd_cap)
{
    exp_dir_t d;
    u32 idx = 0, i;
    int r;

    if (out_rva) *out_rva = 0;
    if (fwd && fwd_cap) fwd[0] = 0;
    r = exp_dir(in, image, &d);
    if (r == 1) return PE_E_NOTFOUND;
    if (r != PE_OK) return r;

    if (by_ordinal) {
        u64 off;
        if (ordinal < d.base) return PE_E_NOTFOUND;
        off = (u64)ordinal - d.base;
        if (off >= d.nfuncs) return PE_E_NOTFOUND;
        idx = (u32)off;
    } else {
        u32 nlen;
        int found = 0;

        if (!name || name[0] == 0) return PE_E_NOTFOUND;
        for (nlen = 0; nlen < PE_MAX_EXPORT_NAME && name[nlen]; nlen++) { }
        if (nlen >= PE_MAX_EXPORT_NAME) return PE_E_NOTFOUND;   /* longer than any export name: cannot match */
        for (i = 0; i < d.nnames; i++) {
            u32 nrva = rd32(image + d.names + 4ull * i);
            int m = exp_str(image, d.isz, nrva, PE_MAX_EXPORT_NAME, name, 0);
            if (m < 0) return PE_E_EXPORT;
            if (m == 1) {
                u32 oi = rd16(image + d.ords + 2ull * i);
                if (oi >= d.nfuncs) return PE_E_EXPORT;
                idx = oi;
                found = 1;
                break;
            }
        }
        if (!found) return PE_E_NOTFOUND;
    }
    return exp_slot(&d, image, idx, out_rva, fwd, fwd_cap);
}

EX_NO_SSP
int pe_export_validate(const pe_info_t *in, const unsigned char *image)
{
    exp_dir_t d;
    u32 i;
    int r = exp_dir(in, image, &d);

    if (r != PE_OK) return r;                                   /* includes 1 = nothing to validate */
    for (i = 0; i < d.nfuncs; i++) {
        u32 rva = rd32(image + d.eat + 4ull * i);
        if (rva == 0) continue;
        if ((u64)rva >= d.dir_lo && (u64)rva < d.dir_hi) {
            u32 n;
            if (exp_str(image, d.isz, rva, PE_MAX_FWD, 0, &n) != 1 || n == 0) return PE_E_EXPORT;
        } else if ((u64)rva >= d.isz) {
            return PE_E_EXPORT;
        }
    }
    for (i = 0; i < d.nnames; i++) {
        u32 nrva = rd32(image + d.names + 4ull * i);
        u32 oi = rd16(image + d.ords + 2ull * i);
        u32 n;
        if (exp_str(image, d.isz, nrva, PE_MAX_EXPORT_NAME, 0, &n) != 1 || n == 0) return PE_E_EXPORT;
        if (oi >= d.nfuncs) return PE_E_EXPORT;
    }
    return PE_OK;
}

EX_NO_SSP
int pe_export_count(const pe_info_t *in, const unsigned char *image, unsigned int *n_funcs, unsigned int *n_names)
{
    exp_dir_t d;
    int r;

    if (n_funcs) *n_funcs = 0;
    if (n_names) *n_names = 0;
    r = exp_dir(in, image, &d);
    if (r != PE_OK) return r;
    if (n_funcs) *n_funcs = d.nfuncs;
    if (n_names) *n_names = d.nnames;
    return PE_OK;
}

EX_NO_SSP
int pe_export_at(const pe_info_t *in, const unsigned char *image, unsigned int index, unsigned int *ordinal,
                 unsigned int *rva, int *is_forwarder)
{
    exp_dir_t d;
    u32 v;
    int r;

    if (ordinal) *ordinal = 0;
    if (rva) *rva = 0;
    if (is_forwarder) *is_forwarder = 0;
    r = exp_dir(in, image, &d);
    if (r == 1) return PE_E_NOTFOUND;
    if (r != PE_OK) return r;
    if (index >= d.nfuncs) return PE_E_NOTFOUND;
    v = rd32(image + d.eat + 4ull * index);
    if (v != 0 && !((u64)v >= d.dir_lo && (u64)v < d.dir_hi) && (u64)v >= d.isz) return PE_E_EXPORT;
    if (ordinal) *ordinal = (u32)((u64)d.base + index);        /* wraps like the on-disk u32 would; callers range-check */
    if (rva) *rva = v;
    if (is_forwarder) *is_forwarder = (v != 0 && (u64)v >= d.dir_lo && (u64)v < d.dir_hi);
    return PE_OK;
}

EX_NO_SSP
int pe_export_name_at(const pe_info_t *in, const unsigned char *image, unsigned int index, char *name,
                      unsigned int name_cap, unsigned int *ordinal, unsigned int *rva)
{
    exp_dir_t d;
    u32 nrva, oi, n, v;
    int r;

    if (ordinal) *ordinal = 0;
    if (rva) *rva = 0;
    if (name && name_cap) name[0] = 0;
    r = exp_dir(in, image, &d);
    if (r == 1) return PE_E_NOTFOUND;
    if (r != PE_OK) return r;
    if (index >= d.nnames) return PE_E_NOTFOUND;
    nrva = rd32(image + d.names + 4ull * index);
    oi = rd16(image + d.ords + 2ull * index);
    if (exp_str(image, d.isz, nrva, PE_MAX_EXPORT_NAME, 0, &n) != 1) return PE_E_EXPORT;
    if (oi >= d.nfuncs) return PE_E_EXPORT;
    v = rd32(image + d.eat + 4ull * oi);
    if (v != 0 && !((u64)v >= d.dir_lo && (u64)v < d.dir_hi) && (u64)v >= d.isz) return PE_E_EXPORT;
    if (name) {
        if (name_cap == 0 || n + 1u > name_cap) return PE_E_BUFSIZE;
        ex_copy_img(name, image + nrva, n);
        name[n] = 0;
    }
    if (ordinal) *ordinal = (u32)((u64)d.base + oi);
    if (rva) *rva = v;
    return PE_OK;
}

/* ------------------------------------------------------------------------------------------------ */
/* forwarder strings                                                                                 */
/* ------------------------------------------------------------------------------------------------ */

EX_NO_SSP
int pe_export_parse_forwarder(const char *fwd, char *dll, unsigned int dll_cap, char *func, unsigned int func_cap,
                              unsigned int *ordinal, int *by_ordinal)
{
    u32 n, i, dot = 0, dlen, flen;
    int have_dot = 0;
    int is_ord;
    u32 ord = 0;

    if (dll && dll_cap) dll[0] = 0;
    if (func && func_cap) func[0] = 0;
    if (ordinal) *ordinal = 0;
    if (by_ordinal) *by_ordinal = 0;
    if (!fwd) return PE_E_FWD;
    for (n = 0; n < PE_MAX_FWD && fwd[n]; n++) {
        u8 c = (u8)fwd[n];
        if (c < 0x20u || c >= 0x7fu) return PE_E_FWD;           /* printable ASCII only */
        if (c == '.') { dot = n; have_dot = 1; }
    }
    if (n >= PE_MAX_FWD) return PE_E_FWD;
    if (!have_dot || dot == 0 || dot + 1u >= n) return PE_E_FWD;
    dlen = dot;
    flen = n - dot - 1u;

    is_ord = (fwd[dot + 1u] == '#');
    if (is_ord) {
        if (flen < 2u || flen > 6u) return PE_E_FWD;            /* '#' + 1..5 digits */
        for (i = dot + 2u; i < n; i++) {
            if (fwd[i] < '0' || fwd[i] > '9') return PE_E_FWD;
            ord = ord * 10u + (u32)(fwd[i] - '0');
        }
        if (ord == 0 || ord > 0xffffu) return PE_E_FWD;
    }
    if (dll && dll_cap < dlen + 1u) return PE_E_BUFSIZE;
    if (!is_ord && func && func_cap < flen + 1u) return PE_E_BUFSIZE;
    if (dll) { ex_copy(dll, fwd, dlen); dll[dlen] = 0; }
    if (is_ord) {
        if (ordinal) *ordinal = ord;
        if (by_ordinal) *by_ordinal = 1;
    } else if (func) {
        ex_copy(func, fwd + dot + 1u, flen);
        func[flen] = 0;
    }
    return PE_OK;
}

/* ------------------------------------------------------------------------------------------------ */
/* delay-load imports                                                                                */
/* ------------------------------------------------------------------------------------------------ */

/* Copies the NUL-terminated string at image[rva] into buf (bufsz incl. NUL); returns its length or -1. */
static int ex_copy_cstr(const u8 *image, u64 isz, u64 rva, char *buf, u32 bufsz)
{
    u32 n;
    if (rva >= isz) return -1;
    for (n = 0;; n++) {
        u8 c;
        if (rva + n >= isz) return -1;
        c = image[rva + n];
        if (c == 0) { buf[n] = 0; return (int)n; }
        if (n + 1u >= bufsz) return -1;
        buf[n] = (char)c;
    }
}

EX_NO_SSP
int pe_delay_imports(const pe_info_t *in, unsigned char *image, pe_resolve_fn fn, void *user,
                     unsigned int *n_total, unsigned int *n_unresolved)
{
    char dll[PE_MAX_DLL_NAME];
    char fname[PE_MAX_FUNC_NAME];
    u32 total = 0, unres = 0, d;
    u64 isz;
    int r;

    if (n_total) *n_total = 0;
    if (n_unresolved) *n_unresolved = 0;
    r = ex_info_check(in);
    if (r != PE_OK) return r;
    if (!image) return PE_E_IMPORT;
    isz = in->size_of_image;
    if (in->delay_import_rva == 0) return PE_OK;

    for (d = 0;; d++) {
        u64 doff = (u64)in->delay_import_rva + 32ull * d;
        u32 attrs, name_rva, iat, int_, k, nz;
        const u8 *dp;

        if (!ex_range_ok(doff, 32u, isz)) { r = PE_E_IMPORT; goto done; }
        dp = image + doff;
        nz = 0;
        for (k = 0; k < 32u; k++) nz |= dp[k];
        if (nz == 0) break;                                     /* all-zero terminator */
        if (d >= PE_MAX_IMPORT_DESCRIPTORS) { r = PE_E_IMPORT; goto done; }
        attrs = rd32(dp);
        name_rva = rd32(dp + 4);
        iat = rd32(dp + 12);
        int_ = rd32(dp + 16);
        if ((attrs & 1u) == 0) { r = PE_E_IMPORT; goto done; }  /* VA-form descriptors (pre-VC7) unsupported */
        if (name_rva == 0 || iat == 0 || int_ == 0) { r = PE_E_IMPORT; goto done; }
        if (ex_copy_cstr(image, isz, name_rva, dll, sizeof dll) <= 0) { r = PE_E_IMPORT; goto done; }

        for (k = 0;; k++) {
            u64 loff = (u64)int_ + 8ull * k, foff = (u64)iat + 8ull * k;
            u64 entry, addr = 0;
            unsigned short ordinal;
            int by_ord, rc = 0;

            if (!ex_range_ok(loff, 8u, isz) || !ex_range_ok(foff, 8u, isz)) { r = PE_E_IMPORT; goto done; }
            entry = rd64(image + loff);
            if (entry == 0) break;
            if (k >= PE_MAX_THUNKS) { r = PE_E_IMPORT; goto done; }
            if (entry >> 63) {
                if ((entry & 0x7fffffffffff0000ull) != 0) { r = PE_E_ORDINAL; goto done; }
                by_ord = 1;
                ordinal = (unsigned short)(entry & 0xffffu);
                fname[0] = 0;
            } else {
                u64 nrva = entry;
                if ((entry >> 31) != 0) { r = PE_E_IMPORT; goto done; }
                if (!ex_range_ok(nrva, 2u, isz)) { r = PE_E_IMPORT; goto done; }
                by_ord = 0;
                ordinal = rd16(image + nrva);                   /* the hint */
                if (ex_copy_cstr(image, isz, nrva + 2u, fname, sizeof fname) <= 0) { r = PE_E_IMPORT; goto done; }
            }
            total++;
            if (fn) {
                rc = fn(dll, fname, ordinal, by_ord, &addr, user);
                if (rc != 0) unres++;
                else wr64(image + foff, addr);                  /* unresolved: keep the compiler's delay thunk */
            }
        }
    }
    r = PE_OK;
done:
    if (n_total) *n_total = total;
    if (n_unresolved) *n_unresolved = unres;
    return r;
}
