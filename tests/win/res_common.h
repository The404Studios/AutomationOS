/*
 * res_common.h -- shared helpers for the pe_resources / pe_manifest host tests and fuzzer (tests/win/res_*.c).
 *
 *   - little-endian poke/peek, file loading
 *   - a tiny resource-tree "assembler" (rt_*) and a PE32+ wrapper (make_pe) so tests can hand-build both valid and
 *     hostile resource sections with exact control over every offset
 *   - a VS_VERSIONINFO writer (vb_*)
 *   - UTF-8 -> UTF-16LE conversion for manifest tests
 *
 * Header-only (static functions); hosted C only (never part of the freestanding library).
 */
#ifndef RES_COMMON_H
#define RES_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"   /* each test program uses a different subset of these helpers */
#endif

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

static void p16(u8 *b, size_t o, u32 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
static void p32(u8 *b, size_t o, u32 v) { p16(b, o, v & 0xffffu); p16(b, o + 2, v >> 16); }
static void p64(u8 *b, size_t o, u64 v) { p32(b, o, (u32)v); p32(b, o + 4, (u32)(v >> 32)); }
static u32 g16(const u8 *b, size_t o) { return (u32)b[o] | ((u32)b[o + 1] << 8); }
static u32 g32(const u8 *b, size_t o) { return g16(b, o) | (g16(b, o + 2) << 16); }

static u32 align_up_u32(u32 v, u32 a) { return (v + a - 1u) & ~(a - 1u); }

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

/* EXACT-size heap copy: ASan sees any read past the end. */
static u8 *dup_exact(const u8 *p, size_t n)
{
    u8 *c = (u8 *)malloc(n ? n : 1);
    if (n) memcpy(c, p, n);
    return c;
}

/* UTF-8 (ASCII only) -> UTF-16LE with BOM; returns malloc'd buffer, *outlen bytes */
static u8 *to_utf16le_bom(const u8 *s, size_t n, size_t *outlen)
{
    u8 *o = (u8 *)malloc(2 + 2 * n + 2);
    size_t i;
    o[0] = 0xFF; o[1] = 0xFE;
    for (i = 0; i < n; i++) { o[2 + 2 * i] = s[i]; o[3 + 2 * i] = 0; }
    o[2 + 2 * n] = o[3 + 2 * n] = 0;           /* 2 spare, zeroed bytes: callers may probe an odd length of len+1 */
    *outlen = 2 + 2 * n;
    return o;
}

/* ------------------------------------------------------------------------------------------------ */
/* resource-tree assembler: a zeroed block of `cap` bytes, poked field by field                       */
/* ------------------------------------------------------------------------------------------------ */
#define RSRC_RVA 0x2000u            /* RVA at which make_pe() maps the resource block */
#define RT_HIGH  0x80000000u

typedef struct { u8 *b; u32 cap; } rt_t;

static void rt_new(rt_t *r, u32 cap) { r->b = (u8 *)calloc(cap, 1); r->cap = cap; }
static void rt_free(rt_t *r) { free(r->b); r->b = NULL; r->cap = 0; }
static void rt_dir(rt_t *r, u32 off, u32 n_named, u32 n_id) { p16(r->b, off + 12, n_named); p16(r->b, off + 14, n_id); }
static void rt_ent(rt_t *r, u32 dir_off, u32 idx, u32 namef, u32 offf)
{
    p32(r->b, dir_off + 16 + 8 * idx, namef);
    p32(r->b, dir_off + 20 + 8 * idx, offf);
}
static void rt_dat(rt_t *r, u32 off, u32 rva, u32 size, u32 cp) { p32(r->b, off, rva); p32(r->b, off + 4, size); p32(r->b, off + 8, cp); }
static void rt_str(rt_t *r, u32 off, const char *s)
{
    u32 n = (u32)strlen(s), i;
    p16(r->b, off, n);
    for (i = 0; i < n; i++) p16(r->b, off + 2 + 2 * i, (u8)s[i]);
}

/* Minimal valid tree: type 24 / id 1 / lang 0x409 -> 8-byte blob "MANIFEST".
 *   0x00 root (1 id entry)   0x20 type dir (1 id entry)   0x38 lang dir (1 id entry)
 *   0x50 data entry           0x60 blob (8 bytes)          total 0x68 */
#define RT_BASE_LEN 0x68u
static void rt_base(rt_t *r)
{
    rt_dir(r, 0x00, 0, 1); rt_ent(r, 0x00, 0, 24, RT_HIGH | 0x20);
    rt_dir(r, 0x20, 0, 1); rt_ent(r, 0x20, 0, 1, RT_HIGH | 0x38);
    rt_dir(r, 0x38, 0, 1); rt_ent(r, 0x38, 0, 0x409, 0x50);
    rt_dat(r, 0x50, RSRC_RVA + 0x60, 8, 1252);
    memcpy(r->b + 0x60, "MANIFEST", 8);
}

/* ------------------------------------------------------------------------------------------------ */
/* PE32+ wrapper: .text + .rsrc.  Returns a malloc'd file image.                                      */
/* ------------------------------------------------------------------------------------------------ */
#define PE_COFF 0x84u
#define PE_OPT  0x98u
#define PE_SECT (PE_OPT + 240u)
#define PE_DIRO(i) (PE_OPT + 112u + 8u * (u32)(i))

/* dir_size: value for the resource data-directory Size (0 => use rsrc_len). */
static u8 *make_pe(const u8 *rsrc, u32 rsrc_len, u32 dir_size, size_t *flen)
{
    u32 raw = align_up_u32(rsrc_len ? rsrc_len : 1u, 0x200u);
    u32 vsz = rsrc_len ? rsrc_len : 1u;
    u32 soi = RSRC_RVA + align_up_u32(vsz, 0x1000u);
    size_t n = 0x600u + raw;
    u8 *f = (u8 *)calloc(n, 1);
    u32 i;
    f[0] = 'M'; f[1] = 'Z';
    p32(f, 0x3c, 0x80);
    f[0x80] = 'P'; f[0x81] = 'E';
    p16(f, PE_COFF + 0, 0x8664);
    p16(f, PE_COFF + 2, 2);
    p16(f, PE_COFF + 16, 240);
    p16(f, PE_COFF + 18, 0x0022);
    p16(f, PE_OPT + 0, 0x20b);
    p32(f, PE_OPT + 16, 0x1000);
    p64(f, PE_OPT + 24, 0x140000000ull);
    p32(f, PE_OPT + 32, 0x1000);
    p32(f, PE_OPT + 36, 0x200);
    p32(f, PE_OPT + 56, soi);
    p32(f, PE_OPT + 60, 0x400);
    p16(f, PE_OPT + 68, 3);
    p16(f, PE_OPT + 70, 0x160);
    p64(f, PE_OPT + 72, 0x200000);
    p64(f, PE_OPT + 80, 0x1000);
    p32(f, PE_OPT + 108, 16);
    /* .text */
    memcpy(f + PE_SECT, ".text", 6);
    p32(f, PE_SECT + 8, 0x100); p32(f, PE_SECT + 12, 0x1000); p32(f, PE_SECT + 16, 0x200); p32(f, PE_SECT + 20, 0x400);
    p32(f, PE_SECT + 36, 0x60000020u);
    /* .rsrc */
    memcpy(f + PE_SECT + 40, ".rsrc", 6);
    p32(f, PE_SECT + 40 + 8, vsz); p32(f, PE_SECT + 40 + 12, RSRC_RVA); p32(f, PE_SECT + 40 + 16, raw);
    p32(f, PE_SECT + 40 + 20, 0x600); p32(f, PE_SECT + 40 + 36, 0x40000040u);
    p32(f, PE_DIRO(2), RSRC_RVA);
    p32(f, PE_DIRO(2) + 4, dir_size ? dir_size : rsrc_len);
    for (i = 0; i < 0x200; i++) f[0x400 + i] = 0xC3;
    if (rsrc_len) memcpy(f + 0x600, rsrc, rsrc_len);
    *flen = n;
    return f;
}

/* ------------------------------------------------------------------------------------------------ */
/* VS_VERSIONINFO writer                                                                              */
/* ------------------------------------------------------------------------------------------------ */
typedef struct { u8 *b; u32 len, cap; } vbuf_t;

static void vb_new(vbuf_t *v, u32 cap) { v->b = (u8 *)calloc(cap, 1); v->len = 0; v->cap = cap; }
static void vb_free(vbuf_t *v) { free(v->b); v->b = NULL; v->len = v->cap = 0; }
static void vb_pad4(vbuf_t *v) { while (v->len & 3u) v->b[v->len++] = 0; }
static void vb_w16(vbuf_t *v, u32 x) { p16(v->b, v->len, x); v->len += 2; }
static void vb_w32(vbuf_t *v, u32 x) { p32(v->b, v->len, x); v->len += 4; }
static void vb_wstr(vbuf_t *v, const char *s) { while (*s) vb_w16(v, (u8)*s++); vb_w16(v, 0); }

/* node header + key; the value (if any) is written by the caller right after (call vb_pad4 first if the
 * value is binary and must be aligned -- it already is: begin pads after the key). Returns the node offset. */
static u32 vb_begin(vbuf_t *v, u32 vlen, u32 type, const char *key)
{
    u32 off;
    vb_pad4(v);
    off = v->len;
    vb_w16(v, 0);               /* wLength, patched by vb_end */
    vb_w16(v, vlen);
    vb_w16(v, type);
    vb_wstr(v, key);
    vb_pad4(v);
    return off;
}
static void vb_end(vbuf_t *v, u32 off) { p16(v->b, off, v->len - off); }

static void vb_string(vbuf_t *v, const char *key, const char *val)
{
    u32 o = vb_begin(v, (u32)strlen(val) + 1u, 1, key);
    vb_wstr(v, val);
    vb_end(v, o);
}

/* A complete, valid version resource.  lang_hex = 8-digit table key; the second table (if non-NULL) is added after. */
static void vb_good(vbuf_t *v, const char *tab1, const char *tab2, u32 translation_dword)
{
    u32 root = vb_begin(v, 52, 0, "VS_VERSION_INFO"), sfi, st, vfi, var;
    vb_w32(v, 0xFEEF04BDu); vb_w32(v, 0x00010000u);
    vb_w32(v, 0x00010002u); vb_w32(v, 0x00030004u);       /* file 1.2.3.4 */
    vb_w32(v, 0x00050006u); vb_w32(v, 0x00070008u);       /* product 5.6.7.8 */
    vb_w32(v, 0x3f); vb_w32(v, 0); vb_w32(v, 0x40004); vb_w32(v, 1); vb_w32(v, 0); vb_w32(v, 0); vb_w32(v, 0);
    vb_pad4(v);
    sfi = vb_begin(v, 0, 1, "StringFileInfo");
    st = vb_begin(v, 0, 1, tab1);
    vb_string(v, "CompanyName", "Example Corp");
    vb_string(v, "FileDescription", "Resource Test App");
    vb_string(v, "FileVersion", "1.2.3.4");
    vb_string(v, "ProductName", "Res Test Product");
    vb_string(v, "ProductVersion", "5.6.7.8");
    vb_end(v, st);
    if (tab2) {
        st = vb_begin(v, 0, 1, tab2);
        vb_string(v, "FileDescription", "Zweite Beschreibung");
        vb_string(v, "ProductName", "Zweites Produkt");
        vb_end(v, st);
    }
    vb_end(v, sfi);
    vfi = vb_begin(v, 0, 1, "VarFileInfo");
    var = vb_begin(v, 4, 0, "Translation");
    vb_w32(v, translation_dword);
    vb_end(v, var);
    vb_end(v, vfi);
    vb_end(v, root);
}

#endif /* RES_COMMON_H */
