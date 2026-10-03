/*
 * pe.c -- hostile-input-safe PE32+ (x86-64) parser / mapper / relocator / import resolver.
 * See pe.h for the API contract and README.md for the security rules.
 *
 * Freestanding build gate (must be warning-free, must have NO undefined symbols, NO fs:0x28 canary):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 *       -mno-red-zone -O2 -Wall -Wextra -Werror -c userspace/lib/pe/pe.c
 *
 * Design rules used throughout:
 *   - every multi-byte read/write is done byte-wise (no unaligned/aliasing UB, endian-explicit);
 *   - every offset/size/RVA is widened to u64 and range-checked with pe_range_ok() (never `a + b <= c`);
 *   - nothing is ever read outside [file, file+len) or [image, image+size_of_image);
 *   - every loop has a hard cap; every string is bounded and copied before it is handed to a callback.
 */
#include "pe.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* Loop-idiom barrier: stops GCC from turning our copy/zero loops back into calls to memcpy/memset
 * (which a freestanding build does not provide). */
#define PE_BARRIER() __asm__ volatile("" ::: "memory")

#if defined(__GNUC__) && (__GNUC__ >= 11) && !defined(__clang__)
#define PE_NO_SSP __attribute__((no_stack_protector))
#else
#define PE_NO_SSP
#endif

/* Unaligned 64-bit word for fast bulk copy/zero (alignment 1 => no UB on any host). */
typedef u64 __attribute__((may_alias, aligned(1))) pe_u64u;

/* ------------------------------------------------------------------------------------------------ */
/* tiny helpers                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

static inline void pe_zero(void *p, u64 n)
{
    u8 *d = (u8 *)p;
    while (n >= 8) { *(pe_u64u *)d = 0; d += 8; n -= 8; PE_BARRIER(); }
    while (n) { *d++ = 0; n--; PE_BARRIER(); }
}

/* dst and src must not overlap (file buffer vs image buffer). */
static inline void pe_copy(void *dst, const void *src, u64 n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n >= 8) { *(pe_u64u *)d = *(const pe_u64u *)s; d += 8; s += 8; n -= 8; PE_BARRIER(); }
    while (n) { *d++ = *s++; n--; PE_BARRIER(); }
}

static inline u16 rd16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }
static inline u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }

static inline void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void wr32(u8 *p, u32 v) { wr16(p, (u16)v); wr16(p + 2, (u16)(v >> 16)); }
static inline void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }

/* off+n <= limit without overflow. */
static inline int pe_range_ok(u64 off, u64 n, u64 limit)
{
    return off <= limit && n <= limit - off;
}

/* v <= 2^32, a <= 2^16 power of two in every caller: no overflow possible. */
static inline u64 pe_align_up(u64 v, u64 a) { return (v + a - 1u) & ~(a - 1u); }

static inline int pe_is_pow2(u64 v) { return v != 0 && (v & (v - 1u)) == 0; }

/* A directory is absent when RVA and Size are both 0, otherwise both must be nonzero and inside the image. */
static inline int pe_dir_valid(u32 rva, u32 size, u64 image_size)
{
    if ((rva | size) == 0) return 1;
    if (rva == 0 || size == 0) return 0;
    return pe_range_ok(rva, size, image_size);
}

static inline u64 pe_sec_extent(const pe_info_t *in, u32 i)
{
    return pe_align_up(in->sec[i].vsize, in->section_alignment ? in->section_alignment : 1u);
}

const char *pe_strerror(int err)
{
    switch (err) {
    case 1:                 return "no such directory";
    case PE_OK:             return "ok";
    case PE_E_TRUNC:        return "file truncated";
    case PE_E_MAGIC:        return "bad magic (not a PE image)";
    case PE_E_NOT_PE32PLUS: return "not a PE32+ (64-bit) image";
    case PE_E_MACHINE:      return "unsupported machine (not AMD64)";
    case PE_E_BOUNDS:       return "field or directory outside the image";
    case PE_E_SECTIONS:     return "bad section table";
    case PE_E_SIZE:         return "bad SizeOfImage/SizeOfHeaders";
    case PE_E_ALIGN:        return "bad alignment";
    case PE_E_RELOC:        return "bad base-relocation table";
    case PE_E_IMPORT:       return "bad import table";
    case PE_E_ORDINAL:      return "bad ordinal import";
    case PE_E_UNSUPPORTED:  return "unsupported image kind (managed/.NET)";
    default:                return "unknown PE error";
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* pe_parse                                                                                          */
/* ------------------------------------------------------------------------------------------------ */

#define PE_MACHINE_AMD64 0x8664u
#define PE_MAGIC_PE32    0x010bu
#define PE_MAGIC_PE32P   0x020bu
#define PE_FILE_DLL      0x2000u

#define PE_DIR_EXPORT  0u
#define PE_DIR_IMPORT  1u
#define PE_DIR_RELOC   5u
#define PE_DIR_TLS     9u
#define PE_DIR_DELAY   13u
#define PE_DIR_CLR     14u

static void pe_get_dir(const u8 *opt, u32 nrva, u32 idx, u32 *rva, u32 *size)
{
    if (idx < nrva) {
        *rva = rd32(opt + 112u + 8u * idx);
        *size = rd32(opt + 116u + 8u * idx);
    } else {
        *rva = 0;
        *size = 0;
    }
}

PE_NO_SSP
static int pe_parse_impl(const u8 *file, unsigned long len, pe_info_t *out)
{
    const u64 flen = (u64)len;
    const u8 *coff, *opt, *tab;
    u64 lfanew, optoff, sizeopt, taboff, imgbase, hdr_pages;
    u32 i, j, nrva, sa, fa, soi, soh, clr_rva, clr_size, delay_size, entry;
    u16 machine, nsec, magic, chars;

    if (flen < 0x40u) return PE_E_TRUNC;
    if (file[0] != 'M' || file[1] != 'Z') return PE_E_MAGIC;

    lfanew = rd32(file + 0x3c);
    if (lfanew < 0x40u) return PE_E_BOUNDS;                 /* NT header would overlap the DOS header */
    if (!pe_range_ok(lfanew, 24u, flen)) return PE_E_TRUNC; /* signature + COFF header past EOF */
    if (file[lfanew] != 'P' || file[lfanew + 1] != 'E' || file[lfanew + 2] != 0 || file[lfanew + 3] != 0)
        return PE_E_MAGIC;

    coff = file + lfanew + 4u;
    machine = rd16(coff);
    nsec = rd16(coff + 2);
    sizeopt = rd16(coff + 16);
    chars = rd16(coff + 18);
    optoff = lfanew + 24u;

    if (sizeopt < 2u) return PE_E_BOUNDS;
    if (!pe_range_ok(optoff, sizeopt, flen)) return PE_E_TRUNC;
    opt = file + optoff;
    magic = rd16(opt);
    if (magic == PE_MAGIC_PE32) return PE_E_NOT_PE32PLUS;
    if (magic != PE_MAGIC_PE32P) return PE_E_MAGIC;
    if (machine != PE_MACHINE_AMD64) return PE_E_MACHINE;
    if (nsec == 0 || nsec > PE_MAX_SECTIONS) return PE_E_SECTIONS;
    if (sizeopt < 112u) return PE_E_BOUNDS;

    nrva = rd32(opt + 108);
    if (nrva > 16u) return PE_E_BOUNDS;
    if (sizeopt < 112u + 8u * (u64)nrva) return PE_E_BOUNDS;

    /* Managed images are rejected before anything else is looked at. */
    pe_get_dir(opt, nrva, PE_DIR_CLR, &clr_rva, &clr_size);
    if ((clr_rva | clr_size) != 0) return PE_E_UNSUPPORTED;

    imgbase = rd64(opt + 24);
    sa = rd32(opt + 32);
    fa = rd32(opt + 36);
    soi = rd32(opt + 56);
    soh = rd32(opt + 60);
    entry = rd32(opt + 16);

    /* Alignment policy: FileAlignment pow2 in [0x200,0x10000]; SectionAlignment pow2 in [0x1000,0x10000]
     * and >= FileAlignment (page-granular images only; sub-page "old style" images are rejected). */
    if (!pe_is_pow2(fa) || fa < 0x200u || fa > 0x10000u) return PE_E_ALIGN;
    if (!pe_is_pow2(sa) || sa < 0x1000u || sa > 0x10000u || sa < fa) return PE_E_ALIGN;

    if (soi == 0 || soi > PE_MAX_IMAGE) return PE_E_SIZE;
    if ((soi & (sa - 1u)) != 0) return PE_E_SIZE;
    if (soh == 0 || soh > soi) return PE_E_SIZE;
    if (soh > flen) return PE_E_TRUNC;

    if ((imgbase & 0xffffu) != 0) return PE_E_ALIGN;
    if (imgbase + soi < imgbase) return PE_E_BOUNDS;        /* image would wrap the address space */

    /* Section table. */
    taboff = optoff + sizeopt;
    if (!pe_range_ok(taboff, 40u * (u64)nsec, flen)) return PE_E_TRUNC;
    if (taboff + 40u * (u64)nsec > soh) return PE_E_SECTIONS;
    tab = file + taboff;
    hdr_pages = pe_align_up(soh, sa);

    out->image_base = imgbase;
    out->size_of_image = soi;
    out->size_of_headers = soh;
    out->entry_rva = entry;
    out->section_alignment = sa;
    out->file_alignment = fa;
    out->subsystem = rd16(opt + 68);
    out->dll_characteristics = rd16(opt + 70);
    out->n_sections = nsec;
    out->is_dll = (chars & PE_FILE_DLL) ? 1u : 0u;
    out->stack_reserve = rd64(opt + 72);
    out->stack_commit = rd64(opt + 80);

    for (i = 0; i < nsec; i++) {
        const u8 *s = tab + 40u * (u64)i;
        u32 vsize = rd32(s + 8), vrva = rd32(s + 12), rawsz = rd32(s + 16), rawoff = rd32(s + 20);
        u32 veff = vsize ? vsize : rawsz;
        u64 ext;

        for (j = 0; j < 8; j++) {
            u8 c = s[j];
            if (c == 0) break;
            out->sec[i].name[j] = (c >= 0x20u && c < 0x7fu) ? (char)c : '?';
        }
        out->sec[i].name[8] = 0;
        out->sec[i].vsize = veff;
        out->sec[i].vrva = vrva;
        out->sec[i].raw_size = rawsz;
        out->sec[i].raw_off = rawoff;
        out->sec[i].flags = rd32(s + 36);

        if ((vrva & (sa - 1u)) != 0) return PE_E_ALIGN;
        ext = pe_align_up(veff, sa);
        if (ext != 0 && (vrva < hdr_pages || (u64)vrva + ext > soi)) return PE_E_SECTIONS;
        if (rawsz != 0) {
            if ((rawoff & (fa - 1u)) != 0) return PE_E_ALIGN;
            if (!pe_range_ok(rawoff, rawsz, flen)) return PE_E_TRUNC;
        }
    }

    /* Overlap check on page-aligned virtual extents (out-of-order but disjoint tables are fine). */
    for (i = 0; i < nsec; i++) {
        u64 ei = pe_sec_extent(out, i);
        if (ei == 0) continue;
        for (j = 0; j < i; j++) {
            u64 ej = pe_sec_extent(out, j);
            if (ej == 0) continue;
            if (!((u64)out->sec[i].vrva + ei <= out->sec[j].vrva || (u64)out->sec[j].vrva + ej <= out->sec[i].vrva))
                return PE_E_SECTIONS;
        }
    }

    /* Entry point: 0 only for DLLs, otherwise inside some section's extent. */
    if (entry == 0) {
        if (!out->is_dll) return PE_E_BOUNDS;
    } else {
        int found = 0;
        for (i = 0; i < nsec; i++) {
            u64 e = pe_sec_extent(out, i);
            if (e != 0 && entry >= out->sec[i].vrva && (u64)entry < (u64)out->sec[i].vrva + e) { found = 1; break; }
        }
        if (!found) return PE_E_BOUNDS;
    }

    /* Directories we consume. */
    pe_get_dir(opt, nrva, PE_DIR_EXPORT, &out->export_rva, &out->export_size);
    pe_get_dir(opt, nrva, PE_DIR_IMPORT, &out->import_rva, &out->import_size);
    pe_get_dir(opt, nrva, PE_DIR_RELOC, &out->reloc_rva, &out->reloc_size);
    pe_get_dir(opt, nrva, PE_DIR_TLS, &out->tls_rva, &out->tls_size);
    pe_get_dir(opt, nrva, PE_DIR_DELAY, &out->delay_import_rva, &delay_size);
    if (!pe_dir_valid(out->delay_import_rva, delay_size, soi)) return PE_E_BOUNDS;
    if (!pe_dir_valid(out->export_rva, out->export_size, soi)) return PE_E_BOUNDS;
    if (!pe_dir_valid(out->import_rva, out->import_size, soi)) return PE_E_BOUNDS;
    if (!pe_dir_valid(out->reloc_rva, out->reloc_size, soi)) return PE_E_BOUNDS;
    if (!pe_dir_valid(out->tls_rva, out->tls_size, soi)) return PE_E_BOUNDS;
    return PE_OK;
}

PE_NO_SSP
int pe_parse(const unsigned char *file, unsigned long len, pe_info_t *out)
{
    int r;
    if (!out) return PE_E_BOUNDS;
    pe_zero(out, sizeof *out);
    if (!file) return PE_E_TRUNC;
    r = pe_parse_impl(file, len, out);
    if (r != PE_OK) pe_zero(out, sizeof *out);
    return r;
}

/* ------------------------------------------------------------------------------------------------ */
/* shared sanity check for every pe_info_t consumer (the struct is caller-owned and may have been    */
/* edited; we never trust it beyond what is re-verified here and at each use).                       */
/* ------------------------------------------------------------------------------------------------ */

static int pe_info_check(const pe_info_t *in)
{
    if (!in) return PE_E_BOUNDS;
    if (in->size_of_image == 0 || in->size_of_image > PE_MAX_IMAGE) return PE_E_SIZE;
    if (in->size_of_headers > in->size_of_image) return PE_E_SIZE;
    if (in->n_sections == 0 || in->n_sections > PE_MAX_SECTIONS) return PE_E_SECTIONS;
    return PE_OK;
}

/* ------------------------------------------------------------------------------------------------ */
/* pe_map                                                                                            */
/* ------------------------------------------------------------------------------------------------ */

int pe_map(const unsigned char *file, unsigned long len, const pe_info_t *in, unsigned char *image)
{
    const u64 flen = (u64)len;
    u64 isz;
    u32 i;
    int r = pe_info_check(in);

    if (r != PE_OK) return r;
    if (!file || !image) return PE_E_BOUNDS;
    isz = in->size_of_image;
    if (!pe_range_ok(0, in->size_of_headers, flen)) return PE_E_TRUNC;

    /* Pass 1: validate every copy so a failure leaves the image untouched. */
    for (i = 0; i < in->n_sections; i++) {
        u64 veff = in->sec[i].vsize ? in->sec[i].vsize : in->sec[i].raw_size;
        u64 n = in->sec[i].raw_size < veff ? in->sec[i].raw_size : veff;
        if (n == 0) continue;
        if (!pe_range_ok(in->sec[i].vrva, n, isz)) return PE_E_SECTIONS;
        if (!pe_range_ok(in->sec[i].raw_off, n, flen)) return PE_E_TRUNC;
    }

    /* Pass 2: copy. */
    pe_copy(image, file, in->size_of_headers);
    for (i = 0; i < in->n_sections; i++) {
        u64 veff = in->sec[i].vsize ? in->sec[i].vsize : in->sec[i].raw_size;
        u64 n = in->sec[i].raw_size < veff ? in->sec[i].raw_size : veff;
        if (n == 0) continue;
        pe_copy(image + in->sec[i].vrva, file + in->sec[i].raw_off, n);
    }
    return PE_OK;
}

/* ------------------------------------------------------------------------------------------------ */
/* pe_relocate                                                                                       */
/* ------------------------------------------------------------------------------------------------ */

#define PE_REL_ABSOLUTE 0u
#define PE_REL_HIGH     1u
#define PE_REL_LOW      2u
#define PE_REL_HIGHLOW  3u
#define PE_REL_DIR64    10u

static int pe_reloc_walk(const pe_info_t *in, u8 *image, u64 delta, int apply)
{
    const u64 isz = in->size_of_image;
    const u64 rlo = in->reloc_rva;
    const u64 rhi = rlo + in->reloc_size;
    u64 pos = rlo;
    u64 count = 0;

    while (pos < rhi) {
        u32 page, bsz;
        u64 nent, k;

        if (rhi - pos < 8u) return PE_E_RELOC;              /* trailing bytes too short for a block header */
        page = rd32(image + pos);
        bsz = rd32(image + pos + 4);
        if (bsz < 8u || (bsz & 3u) != 0) return PE_E_RELOC; /* size 0 / too small / not 32-bit multiple */
        if ((u64)bsz > rhi - pos) return PE_E_RELOC;        /* block runs past the directory */
        nent = ((u64)bsz - 8u) >> 1;

        for (k = 0; k < nent; k++) {
            u32 e = rd16(image + pos + 8u + 2u * k);
            u32 type = e >> 12, off = e & 0xfffu;
            u64 tgt, w;
            u8 *p;

            if (++count > PE_MAX_RELOCS) return PE_E_RELOC;
            if (type == PE_REL_ABSOLUTE) continue;
            switch (type) {
            case PE_REL_DIR64:   w = 8; break;
            case PE_REL_HIGHLOW: w = 4; break;
            case PE_REL_HIGH:
            case PE_REL_LOW:     w = 2; break;
            default:             return PE_E_RELOC;
            }
            tgt = (u64)page + off;
            if (!pe_range_ok(tgt, w, isz)) return PE_E_RELOC;
            if (tgt < rhi && tgt + w > rlo) return PE_E_RELOC; /* may not patch the table being walked */
            if (!apply) continue;
            p = image + tgt;
            switch (type) {
            case PE_REL_DIR64:   wr64(p, rd64(p) + delta); break;
            case PE_REL_HIGHLOW: wr32(p, rd32(p) + (u32)delta); break;
            case PE_REL_HIGH:    wr16(p, (u16)(rd16(p) + (u16)(delta >> 16))); break;
            default:             wr16(p, (u16)(rd16(p) + (u16)delta)); break;
            }
        }
        pos += bsz;
    }
    return PE_OK;
}

int pe_relocate(const pe_info_t *in, unsigned char *image, unsigned long long actual_base)
{
    u64 delta;
    int r = pe_info_check(in);

    if (r != PE_OK) return r;
    if (!image) return PE_E_BOUNDS;
    delta = actual_base - in->image_base;                   /* modular on purpose (negative deltas) */

    if ((in->reloc_rva | in->reloc_size) == 0) return delta ? PE_E_RELOC : PE_OK;
    if (!pe_dir_valid(in->reloc_rva, in->reloc_size, in->size_of_image)) return PE_E_RELOC;

    r = pe_reloc_walk(in, image, delta, 0);                 /* validate everything first ... */
    if (r != PE_OK || delta == 0) return r;
    return pe_reloc_walk(in, image, delta, 1);              /* ... then patch */
}

/* ------------------------------------------------------------------------------------------------ */
/* pe_resolve_imports                                                                                */
/* ------------------------------------------------------------------------------------------------ */

/* Copies the NUL-terminated string at image[rva] into buf (bufsz incl. NUL).  Returns its length, or -1 if
 * it is not terminated inside the image or does not fit. */
static int pe_copy_cstr(const u8 *image, u64 isz, u64 rva, char *buf, u32 bufsz)
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

PE_NO_SSP
int pe_resolve_imports(const pe_info_t *in, unsigned char *image, pe_resolve_fn fn, void *user,
                       unsigned int *n_total, unsigned int *n_unresolved)
{
    char dll[PE_MAX_DLL_NAME];
    char fname[PE_MAX_FUNC_NAME];
    u32 total = 0, unres = 0, d;
    u64 isz;
    int r;

    if (n_total) *n_total = 0;
    if (n_unresolved) *n_unresolved = 0;
    r = pe_info_check(in);
    if (r != PE_OK) return r;
    if (!image) return PE_E_IMPORT;
    isz = in->size_of_image;
    if ((in->import_rva | in->import_size) == 0) return PE_OK;
    if (!pe_dir_valid(in->import_rva, in->import_size, isz)) return PE_E_IMPORT;

    /* The descriptor array is terminated by an all-zero entry (the directory Size is not trusted to include
     * it); the walk is bounded by the image and by PE_MAX_IMPORT_DESCRIPTORS. */
    for (d = 0;; d++) {
        u64 doff = (u64)in->import_rva + 20ull * d;
        u32 oft, name_rva, ft, i;
        const u8 *dp;
        u64 lookup;

        if (!pe_range_ok(doff, 20u, isz)) { r = PE_E_IMPORT; goto done; }
        dp = image + doff;
        oft = rd32(dp);
        name_rva = rd32(dp + 12);
        ft = rd32(dp + 16);
        if ((oft | rd32(dp + 4) | rd32(dp + 8) | name_rva | ft) == 0) break;
        if (d >= PE_MAX_IMPORT_DESCRIPTORS) { r = PE_E_IMPORT; goto done; }
        if (name_rva == 0 || ft == 0) { r = PE_E_IMPORT; goto done; }
        if (pe_copy_cstr(image, isz, name_rva, dll, sizeof dll) <= 0) { r = PE_E_IMPORT; goto done; }
        lookup = oft ? oft : ft;

        for (i = 0;; i++) {
            u64 loff = lookup + 8ull * i, foff = (u64)ft + 8ull * i;
            u64 entry, addr = 0;
            unsigned short ordinal;
            int by_ord, rc = 0;

            if (!pe_range_ok(loff, 8u, isz) || !pe_range_ok(foff, 8u, isz)) { r = PE_E_IMPORT; goto done; }
            entry = rd64(image + loff);
            if (entry == 0) break;
            if (i >= PE_MAX_THUNKS) { r = PE_E_IMPORT; goto done; }

            if (entry >> 63) {                              /* import by ordinal */
                if ((entry & 0x7fffffffffff0000ull) != 0) { r = PE_E_ORDINAL; goto done; }
                by_ord = 1;
                ordinal = (unsigned short)(entry & 0xffffu);
                fname[0] = 0;
            } else {                                        /* import by name: IMAGE_IMPORT_BY_NAME */
                u64 nrva = entry;
                if ((entry >> 31) != 0) { r = PE_E_IMPORT; goto done; }
                if (!pe_range_ok(nrva, 2u, isz)) { r = PE_E_IMPORT; goto done; }
                by_ord = 0;
                ordinal = rd16(image + nrva);               /* the hint */
                if (pe_copy_cstr(image, isz, nrva + 2u, fname, sizeof fname) <= 0) { r = PE_E_IMPORT; goto done; }
            }

            total++;
            if (fn) rc = fn(dll, fname, ordinal, by_ord, &addr, user);
            if (fn) {
                if (rc != 0) unres++;
                wr64(image + foff, addr);                   /* addr stays 0 unless the resolver set it */
            }
        }
    }
    r = PE_OK;
done:
    if (n_total) *n_total = total;
    if (n_unresolved) *n_unresolved = unres;
    return r;
}

/* ------------------------------------------------------------------------------------------------ */
/* pe_tls_info                                                                                       */
/* ------------------------------------------------------------------------------------------------ */

int pe_tls_info(const pe_info_t *in, const unsigned char *image, unsigned long long actual_base,
                unsigned long long *start, unsigned long long *end, unsigned long long *index_addr,
                unsigned long long *callbacks_va)
{
    u64 isz, lo, hi, s, e, idx, cb;
    const u8 *d;
    int r = pe_info_check(in);

    if (r != PE_OK) return r;
    if (!image) return PE_E_BOUNDS;
    if ((in->tls_rva | in->tls_size) == 0) return 1;
    isz = in->size_of_image;
    if (in->tls_rva == 0 || in->tls_size < 40u || !pe_range_ok(in->tls_rva, 40u, isz)) return PE_E_BOUNDS;
    if (actual_base > ~0ull - isz) return PE_E_BOUNDS;
    lo = actual_base;
    hi = actual_base + isz;

    d = image + in->tls_rva;                                /* IMAGE_TLS_DIRECTORY64 */
    s = rd64(d);
    e = rd64(d + 8);
    idx = rd64(d + 16);
    cb = rd64(d + 24);

    if (s > e) return PE_E_BOUNDS;
    if (!(s == 0 && e == 0) && (s < lo || e > hi)) return PE_E_BOUNDS;
    if (idx < lo || !pe_range_ok(idx - lo, 4u, isz)) return PE_E_BOUNDS;

    if (cb != 0) {                                          /* null-terminated array of callback VAs */
        u32 n;
        for (n = 0;; n++) {
            u64 va, cbf;
            if (n > PE_MAX_TLS_CALLBACKS) return PE_E_BOUNDS;   /* <= 64 callbacks + the null terminator */
            va = cb + 8ull * n;
            if (va < cb || va < lo || !pe_range_ok(va - lo, 8u, isz)) return PE_E_BOUNDS;
            cbf = rd64(image + (va - lo));
            if (cbf == 0) break;
            if (cbf < lo || cbf >= hi) return PE_E_BOUNDS;
        }
    }

    if (start) *start = s;
    if (end) *end = e;
    if (index_addr) *index_addr = idx;
    if (callbacks_va) *callbacks_va = cb;
    return PE_OK;
}
