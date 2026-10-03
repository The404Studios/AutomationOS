/*
 * pe_dll_builder.h -- tiny synthetic PE32+ DLL/EXE builder for the export / module-loader tests (header only).
 *
 * Builds a structurally valid image with: .text (optional DllMain stub + one `mov eax,imm32; ret` stub per
 * function), .edata (export directory, EAT, name pointer table, ordinal table, strings, forwarder strings),
 * .idata (import tables), .didat (delay-load tables), .reloc (always present, so the image is relocatable).  Every
 * knob a hostile-input test needs is exposed: unsorted names, unused slots, forwarders, arbitrary ordinal base, no
 * export directory at all.  Tests then poke bytes at the RVAs reported in bout_t (bl_off() converts RVA -> file offset).
 */
#ifndef PE_DLL_BUILDER_H
#define PE_DLL_BUILDER_H

#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wunused-function"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#define BL_MAXF 64
#define BL_MAXI 48

typedef struct { u32 val; const char *fwd; } bfunc_t;        /* val != 0: stub returning val; fwd: forwarder; neither: unused slot */
typedef struct { const char *name; u32 index; } bname_t;     /* name -> EAT index (unbiased), in table order (may be unsorted) */
/* names[i]: "Func" = import by name, "#7" = import by ordinal 7 */
typedef struct { const char *dll; int n; const char *const *names; } bimp_t;

typedef struct {
    u64 image_base;               /* 0 -> 0x180000000 */
    u32 base;                     /* export ordinal base, 0 -> 1 */
    int nfuncs;  const bfunc_t *funcs;
    int nnames;  const bname_t *names;
    int nimps;   const bimp_t *imps;
    int ndelay;  const bimp_t *delay;
    int no_exports;               /* omit the export directory */
    int has_entry;                /* DllMain stub (mov eax,1; ret) as entry point */
    int is_exe;                   /* clear IMAGE_FILE_DLL; entry = start of .text */
    const char *dll_name;         /* name stored in the export directory (default "synth.dll") */
} bspec_t;

typedef struct {
    u8 *file;
    size_t len;
    u32 size_of_image, entry_rva;
    u32 text_rva, edata_rva, idata_rva, didat_rva, reloc_rva;
    u32 export_rva, export_size, eat_rva, names_rva, ords_rva, strs_rva;
    u32 stub_rva[BL_MAXF], fwd_rva[BL_MAXF], name_rva[BL_MAXF];
    u32 import_rva, import_size, iat_rva[BL_MAXI], ilt_rva[BL_MAXI];
    u32 delay_rva, diat_rva[BL_MAXI], dint_rva[BL_MAXI];
    struct { u32 vrva, vsize, raw, rawsz; const char *name; } sec[6];
    int nsec;
} bout_t;

static void bl_p16(u8 *b, size_t o, u32 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
static void bl_p32(u8 *b, size_t o, u32 v) { bl_p16(b, o, v & 0xffffu); bl_p16(b, o + 2, v >> 16); }
static void bl_p64(u8 *b, size_t o, u64 v) { bl_p32(b, o, (u32)v); bl_p32(b, o + 4, (u32)(v >> 32)); }
static u32 bl_g32(const u8 *b, size_t o) { return (u32)b[o] | ((u32)b[o + 1] << 8) | ((u32)b[o + 2] << 16) | ((u32)b[o + 3] << 24); }
static u32 bl_align(u32 v, u32 a) { return (v + a - 1u) & ~(a - 1u); }

/* RVA -> file offset (0 if the RVA is in no section). */
static size_t bl_off(const bout_t *o, u32 rva)
{
    int i;
    for (i = 0; i < o->nsec; i++)
        if (rva >= o->sec[i].vrva && rva - o->sec[i].vrva < o->sec[i].rawsz) return o->sec[i].raw + (rva - o->sec[i].vrva);
    return 0;
}

static void bl_free(bout_t *o) { free(o->file); memset(o, 0, sizeof *o); }

/* section placement: sections follow each other in the file (after 0x400 of headers) and in memory (page aligned) */
static int bl_place(bout_t *o, u32 *vr, u32 *fo, const char *name, u32 used, u32 *rva_out)
{
    int i = o->nsec++;
    o->sec[i].name = name;
    o->sec[i].vrva = *vr;
    o->sec[i].vsize = used;
    o->sec[i].rawsz = bl_align(used ? used : 1u, 0x200u);
    o->sec[i].raw = *fo;
    *fo += o->sec[i].rawsz;
    if (rva_out) *rva_out = *vr;
    *vr += bl_align(used ? used : 1u, 0x1000u);
    return i;
}

static u32 bl_imp_size(const bimp_t *imps, int n, u32 desc_size)
{
    u32 sz = desc_size * (u32)(n + 1);
    int i, k;
    for (i = 0; i < n; i++) {
        sz += 8u * (u32)(imps[i].n + 1) * 2u + 8u;             /* ILT + IAT (+ hmod slot for delay) */
        for (k = 0; k < imps[i].n; k++)
            if (imps[i].names[k][0] != '#') sz += 2u + (u32)strlen(imps[i].names[k]) + 1u + 2u;
        sz += (u32)strlen(imps[i].dll) + 1u + 2u;
    }
    return sz + 16u;
}

static int bl_dll(const bspec_t *s, bout_t *o)
{
    const u64 ib = s->image_base ? s->image_base : 0x180000000ull;
    const u32 base = s->base ? s->base : 1u;
    const u32 nf = (u32)s->nfuncs, nn = (u32)s->nnames;
    const u32 ent = s->has_entry ? 8u : 0u;
    const u32 text_used = ent + 8u * nf + 8u;
    const u32 e_eat = 40u, e_names = e_eat + 4u * nf, e_ords = e_names + 4u * nn, e_str = e_ords + 2u * nn;
    const char *dllname = s->dll_name ? s->dll_name : "synth.dll";
    u32 e_used, i_used = 0, d_used = 0, nthunks = 0, rl_cap;
    u32 vr = 0x1000u, fo = 0x400u, i, k;
    int i_text, i_edata = -1, i_idata = -1, i_didat = -1, i_reloc;
    u8 *f;
    static u32 relocs[BL_MAXI * 64 + 8];
    u32 nrel = 0;

    memset(o, 0, sizeof *o);
    if (nf > BL_MAXF || nn > BL_MAXF || s->nimps > BL_MAXI || s->ndelay > BL_MAXI) return -1;

    e_used = e_str + (u32)strlen(dllname) + 1u;
    for (i = 0; i < nn; i++) e_used += (u32)strlen(s->names[i].name) + 1u;
    for (i = 0; i < nf; i++) if (s->funcs[i].fwd) e_used += (u32)strlen(s->funcs[i].fwd) + 1u;
    if (s->nimps) i_used = bl_imp_size(s->imps, s->nimps, 20u);
    if (s->ndelay) d_used = bl_imp_size(s->delay, s->ndelay, 32u);
    for (i = 0; i < (u32)s->ndelay; i++) nthunks += (u32)s->delay[i].n;
    if (nthunks > BL_MAXI * 64u) return -1;
    rl_cap = 16u + 12u * nthunks;

    i_text = bl_place(o, &vr, &fo, ".text", text_used, &o->text_rva);
    if (!s->no_exports) i_edata = bl_place(o, &vr, &fo, ".edata", e_used, &o->edata_rva);
    if (s->nimps) i_idata = bl_place(o, &vr, &fo, ".idata", i_used, &o->idata_rva);
    if (s->ndelay) i_didat = bl_place(o, &vr, &fo, ".didat", d_used, &o->didat_rva);
    i_reloc = bl_place(o, &vr, &fo, ".reloc", rl_cap, &o->reloc_rva);
    o->size_of_image = vr;
    o->len = fo;
    o->file = (u8 *)calloc(fo, 1);
    if (!o->file) return -1;
    f = o->file;

    /* ---- headers ---- */
    {
        const u32 OPT = 0x98u, SECT = OPT + 240u;
        f[0] = 'M'; f[1] = 'Z';
        bl_p32(f, 0x3c, 0x80);
        f[0x80] = 'P'; f[0x81] = 'E';
        bl_p16(f, 0x84 + 0, 0x8664);
        bl_p16(f, 0x84 + 2, (u32)o->nsec);
        bl_p16(f, 0x84 + 16, 240);
        bl_p16(f, 0x84 + 18, s->is_exe ? 0x0022u : 0x2022u);
        bl_p16(f, OPT + 0, 0x20b);
        o->entry_rva = (s->is_exe || s->has_entry) ? o->text_rva : 0u;
        bl_p32(f, OPT + 16, o->entry_rva);
        bl_p64(f, OPT + 24, ib);
        bl_p32(f, OPT + 32, 0x1000);
        bl_p32(f, OPT + 36, 0x200);
        bl_p32(f, OPT + 56, o->size_of_image);
        bl_p32(f, OPT + 60, 0x400);
        bl_p16(f, OPT + 68, 3);
        bl_p16(f, OPT + 70, 0x160);
        bl_p64(f, OPT + 72, 0x100000); bl_p64(f, OPT + 80, 0x1000);
        bl_p32(f, OPT + 108, 16);
        for (i = 0; i < (u32)o->nsec; i++) {
            u32 so = SECT + 40u * i;
            memcpy(f + so, o->sec[i].name, strlen(o->sec[i].name));
            bl_p32(f, so + 8, o->sec[i].vsize);
            bl_p32(f, so + 12, o->sec[i].vrva);
            bl_p32(f, so + 16, o->sec[i].rawsz);
            bl_p32(f, so + 20, o->sec[i].raw);
            bl_p32(f, so + 36, i == 0 ? 0x60000020u : 0xC0000040u);
        }
    }

    /* ---- .text ---- */
    {
        u8 *t = f + o->sec[i_text].raw;
        if (s->has_entry) { t[0] = 0xB8; bl_p32(t, 1, 1); t[5] = 0xC3; t[6] = 0xCC; t[7] = 0xCC; }
        for (i = 0; i < nf; i++) {
            u32 o8 = ent + 8u * i;
            if (s->funcs[i].val != 0 && !s->funcs[i].fwd) {
                t[o8] = 0xB8; bl_p32(t, o8 + 1, s->funcs[i].val); t[o8 + 5] = 0xC3; t[o8 + 6] = 0xCC; t[o8 + 7] = 0xCC;
                o->stub_rva[i] = o->text_rva + o8;
            }
        }
    }

    /* ---- .edata ---- */
    if (i_edata >= 0) {
        u8 *e = f + o->sec[i_edata].raw;
        u32 cur;
        o->export_rva = o->edata_rva;
        o->export_size = e_used;
        o->eat_rva = o->edata_rva + e_eat;
        o->names_rva = o->edata_rva + e_names;
        o->ords_rva = o->edata_rva + e_ords;
        o->strs_rva = o->edata_rva + e_str;
        bl_p32(e, 16, base);
        bl_p32(e, 20, nf);
        bl_p32(e, 24, nn);
        bl_p32(e, 28, o->eat_rva);
        bl_p32(e, 32, o->names_rva);
        bl_p32(e, 36, o->ords_rva);
        cur = e_str;
        bl_p32(e, 12, o->edata_rva + cur);
        memcpy(e + cur, dllname, strlen(dllname) + 1u);
        cur += (u32)strlen(dllname) + 1u;
        for (i = 0; i < nn; i++) {
            size_t l = strlen(s->names[i].name) + 1u;
            o->name_rva[i] = o->edata_rva + cur;
            memcpy(e + cur, s->names[i].name, l);
            bl_p32(e, e_names + 4u * i, o->edata_rva + cur);
            bl_p16(e, e_ords + 2u * i, s->names[i].index);
            cur += (u32)l;
        }
        for (i = 0; i < nf; i++) {
            if (s->funcs[i].fwd) {
                size_t l = strlen(s->funcs[i].fwd) + 1u;
                o->fwd_rva[i] = o->edata_rva + cur;
                memcpy(e + cur, s->funcs[i].fwd, l);
                bl_p32(e, e_eat + 4u * i, o->edata_rva + cur);
                cur += (u32)l;
            } else if (s->funcs[i].val != 0) {
                bl_p32(e, e_eat + 4u * i, o->stub_rva[i]);
            }
        }
        bl_p32(f, 0x98 + 112 + 0, o->export_rva);
        bl_p32(f, 0x98 + 112 + 4, o->export_size);
    }

    /* ---- .idata (normal imports, k == 0) and .didat (delay imports, k == 1) ---- */
    for (k = 0; k < 2; k++) {
        const bimp_t *imps = k ? s->delay : s->imps;
        const int n = k ? s->ndelay : s->nimps;
        const u32 dsz = k ? 32u : 20u;
        const int si = k ? i_didat : i_idata;
        int d, q;
        u8 *sb;
        u32 srva, cur;
        if (!n) continue;
        sb = f + o->sec[si].raw;
        srva = o->sec[si].vrva;
        cur = dsz * (u32)(n + 1);
        if (k) o->delay_rva = srva; else { o->import_rva = srva; o->import_size = dsz * (u32)(n + 1); }
        for (d = 0; d < n; d++) {
            const u32 nth = (u32)imps[d].n;
            u32 ilt, iat, hmod = 0, dn;
            u8 *desc = sb + dsz * (u32)d;
            ilt = srva + cur; cur += 8u * (nth + 1u);
            iat = srva + cur; cur += 8u * (nth + 1u);
            if (k) { hmod = srva + cur; cur += 8u; }
            for (q = 0; q < (int)nth; q++) {
                const char *nmq = imps[d].names[q];
                u64 thunk;
                if (nmq[0] == '#') {
                    thunk = 0x8000000000000000ull | (u64)strtoul(nmq + 1, 0, 10);
                    bl_p64(sb, ilt - srva + 8u * (u32)q, thunk);
                } else {
                    u32 hn = srva + cur;
                    bl_p16(sb, cur, (u32)q);
                    memcpy(sb + cur + 2u, nmq, strlen(nmq) + 1u);
                    cur += 2u + (u32)strlen(nmq) + 1u;
                    cur = (cur + 1u) & ~1u;
                    thunk = hn;
                    bl_p64(sb, ilt - srva + 8u * (u32)q, thunk);
                }
                if (k) {                                       /* delay IAT starts at a fake thunk: an absolute VA => reloc */
                    bl_p64(sb, iat - srva + 8u * (u32)q, ib + o->text_rva);
                    relocs[nrel++] = iat + 8u * (u32)q;
                } else {
                    bl_p64(sb, iat - srva + 8u * (u32)q, thunk);
                }
            }
            dn = srva + cur;
            memcpy(sb + cur, imps[d].dll, strlen(imps[d].dll) + 1u);
            cur += (u32)strlen(imps[d].dll) + 1u;
            if (k) {
                bl_p32(desc, 0, 1);                            /* grAttrs: RVA form */
                bl_p32(desc, 4, dn); bl_p32(desc, 8, hmod); bl_p32(desc, 12, iat); bl_p32(desc, 16, ilt);
                o->diat_rva[d] = iat; o->dint_rva[d] = ilt;
            } else {
                bl_p32(desc, 0, ilt); bl_p32(desc, 12, dn); bl_p32(desc, 16, iat);
                o->iat_rva[d] = iat; o->ilt_rva[d] = ilt;
            }
        }
        if (k) {
            bl_p32(f, 0x98 + 112 + 8 * 13, o->delay_rva);
            bl_p32(f, 0x98 + 112 + 8 * 13 + 4, 32u * (u32)(n + 1));
        } else {
            bl_p32(f, 0x98 + 112 + 8, o->import_rva);
            bl_p32(f, 0x98 + 112 + 12, o->import_size);
        }
    }

    /* ---- .reloc: DIR64 entries for the delay thunk pointers, else a single ABSOLUTE no-op entry ---- */
    {
        u8 *rl = f + o->sec[i_reloc].raw;
        u32 cur = 0, bi = 0;
        for (i = 1; i < nrel; i++) { u32 v = relocs[i]; int j = (int)i - 1; while (j >= 0 && relocs[j] > v) { relocs[j + 1] = relocs[j]; j--; } relocs[j + 1] = v; }
        if (nrel == 0) {
            bl_p32(rl, 0, o->text_rva); bl_p32(rl, 4, 12); bl_p16(rl, 8, 0); bl_p16(rl, 10, 0);
            cur = 12;
        } else {
            while (bi < nrel) {
                u32 page = relocs[bi] & ~0xfffu, cnt = 0, j = bi, bs;
                while (j < nrel && (relocs[j] & ~0xfffu) == page) { cnt++; j++; }
                bs = (8u + 2u * cnt + 3u) & ~3u;
                if (cur + bs > o->sec[i_reloc].rawsz) return -1;
                bl_p32(rl, cur, page); bl_p32(rl, cur + 4, bs);
                for (k = 0; k < cnt; k++) bl_p16(rl, cur + 8u + 2u * k, 0xA000u | (relocs[bi + k] & 0xfffu));
                cur += bs;
                bi = j;
            }
        }
        o->sec[i_reloc].vsize = cur;
        bl_p32(f, 0x98 + 240 + 40u * (u32)i_reloc + 8, cur);
        bl_p32(f, 0x98 + 112 + 8 * 5, o->reloc_rva);
        bl_p32(f, 0x98 + 112 + 8 * 5 + 4, cur);
    }
    return 0;
}

#endif /* PE_DLL_BUILDER_H */
