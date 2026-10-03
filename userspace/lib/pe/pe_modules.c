/*
 * pe_modules.c -- PE module-graph loader (DLL dependencies, forwarders, LoadLibrary/GetProcAddress, DllMain order).
 * See pe_modules.h for the API contract and README.md for the security rules.
 *
 * Freestanding build gate (must be warning-free; linked with pe.c + pe_exports.c: no undefined symbols, no fs:0x28):
 *   gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 *       -mno-red-zone -O2 -Wall -Wextra -Werror -c userspace/lib/pe/pe_modules.c
 *
 * Per-module life cycle:   slot FREE -> [parse, alloc, map, relocate, validate imports/exports/tls] -> LINKING
 *                          -> [resolve imports; recursion loads dependencies] -> LINKED
 * A module becomes visible to name lookups at LINKING, i.e. AFTER it is mapped and relocated, so an import cycle
 * can bind to its exports by address before its own IAT is complete (the Windows rule).
 */
#include "pe_modules.h"

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

#if defined(__GNUC__) && (__GNUC__ >= 11) && !defined(__clang__)
#define MOD_FN __attribute__((no_stack_protector))
#else
#define MOD_FN
#endif

/* Loop-idiom barrier: stops GCC from turning zero / copy loops into memset / memcpy calls. */
#define MOD_BARRIER() __asm__ volatile("" ::: "memory")

typedef u64 __attribute__((may_alias, aligned(1))) mod_u64u;

MOD_FN static void mod_zero(void *p, u64 n)
{
    u8 *d = (u8 *)p;
    while (n >= 8) { *(mod_u64u *)d = 0; d += 8; n -= 8; MOD_BARRIER(); }
    while (n) { *d++ = 0; n--; MOD_BARRIER(); }
}

static inline int mod_active(const pe_mod_t *m) { return m->state == PE_MOD_S_LINKING || m->state == PE_MOD_S_LINKED; }

const char *pe_mod_strerror(int err)
{
    switch (err) {
    case PE_MOD_E_ARG:       return "bad argument / missing callback / bad module index";
    case PE_MOD_E_NAME:      return "bad DLL name (empty, too long, junk or path separators)";
    case PE_MOD_E_FULL:      return "module table full";
    case PE_MOD_E_DEPTH:     return "import nesting too deep";
    case PE_MOD_E_NOFILE:    return "DLL file not found";
    case PE_MOD_E_ALLOC:     return "cannot allocate image memory";
    case PE_MOD_E_KIND:      return "wrong image kind (exe vs dll)";
    case PE_MOD_E_FWD_CYCLE: return "forwarder cycle";
    case PE_MOD_E_FWD_DEPTH: return "forwarder chain too long";
    case PE_MOD_E_INIT:      return "DllMain(DLL_PROCESS_ATTACH) returned FALSE";
    case PE_MOD_E_STATE:     return "invalid state (main module already loaded)";
    default:                 return pe_export_strerror(err);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* names                                                                                             */
/* ------------------------------------------------------------------------------------------------ */

static inline int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Both NUL-terminated.  ASCII case-insensitive equality. */
static int ci_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = lc((u8)*a), y = lc((u8)*b);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Length of s, or `cap` if there is no NUL in the first cap bytes. */
static u32 slen_cap(const char *s, u32 cap)
{
    u32 n;
    for (n = 0; n < cap && s[n]; n++) { }
    return n;
}

/* Printable ASCII, no path separators / drive colon / wildcards / quotes / redirection characters. */
static int name_chars_ok(const char *s, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) {
        u8 c = (u8)s[i];
        if (c < 0x20u || c >= 0x7fu) return 0;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return 0;
    }
    return 1;
}

/* Validates `in` as a bare DLL file name and writes its canonical form (an extension is guaranteed: ".dll" is
 * appended when `force_ext` or when there is no '.' at all) into out[PE_MOD_NAME_MAX].  Names that are empty, have
 * 64+ characters (after appending), junk, or no real stem are REJECTED -- never truncated. */
static int canon_name(const char *in, int force_ext, char *out)
{
    u32 n, i, stem;
    int dot = 0;

    out[0] = 0;
    if (!in) return PE_MOD_E_NAME;
    n = slen_cap(in, PE_MOD_NAME_MAX);
    if (n == 0 || n >= PE_MOD_NAME_MAX) return PE_MOD_E_NAME;
    if (!name_chars_ok(in, n)) return PE_MOD_E_NAME;
    for (i = 0; i < n; i++) if (in[i] == '.') dot = 1;
    for (i = 0; i < n; i++) { out[i] = in[i]; MOD_BARRIER(); }
    if (force_ext || !dot) {
        if (n + 4u >= PE_MOD_NAME_MAX) { out[0] = 0; return PE_MOD_E_NAME; }
        out[n] = '.'; out[n + 1] = 'd'; out[n + 2] = 'l'; out[n + 3] = 'l';
        n += 4u;
    }
    out[n] = 0;
    stem = n;
    if (n >= 4u && out[n - 4u] == '.' && lc((u8)out[n - 3u]) == 'd' && lc((u8)out[n - 2u]) == 'l' && lc((u8)out[n - 1u]) == 'l')
        stem = n - 4u;
    for (i = 0; i < stem; i++) if (out[i] != '.') break;
    if (i == stem) { out[0] = 0; return PE_MOD_E_NAME; }       /* "", ".dll", "..dll", "." ... */
    return 0;
}

static int find_mod(const pe_mod_ctx_t *ctx, const char *canon)
{
    u32 i;
    for (i = 0; i < PE_MOD_MAX_MODULES; i++)
        if (mod_active(&ctx->mod[i]) && ci_eq(ctx->mod[i].name, canon)) return (int)i;
    return PE_E_NOTFOUND;
}

/* ------------------------------------------------------------------------------------------------ */
/* unresolved-import bookkeeping                                                                     */
/* ------------------------------------------------------------------------------------------------ */

typedef struct { char *b; u32 n; u32 cap; } app_t;

/* Appends at most maxlen characters of s (then "..." if more follow), replacing control / non-ASCII bytes by '?'
 * (the text is printed by callers; never let a hostile import name inject terminal escapes). */
static void app_str(app_t *a, const char *s, u32 maxlen)
{
    u32 i;
    for (i = 0; s[i]; i++) {
        u8 c = (u8)s[i];
        if (i >= maxlen) {
            u32 k;
            for (k = 0; k < 3u && a->n + 1u < a->cap; k++) a->b[a->n++] = '.';
            return;
        }
        if (a->n + 1u >= a->cap) return;
        a->b[a->n++] = (c < 0x20u || c >= 0x7fu) ? '?' : (char)c;
    }
}

static void app_u(app_t *a, u32 v)
{
    char t[12];
    u32 k = 0;
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10u); v /= 10u; }
    while (k && a->n + 1u < a->cap) a->b[a->n++] = t[--k];
}

MOD_FN static void note_unres(pe_mod_ctx_t *ctx, int self, const char *dll, const char *name, unsigned ord, int by_ord,
                              int delay, int why)
{
    if (delay) { ctx->delay_unresolved++; return; }
    if (ctx->unresolved == 0) {
        app_t a;
        a.b = ctx->first_unresolved; a.n = 0; a.cap = PE_MOD_MSG_MAX;
        app_str(&a, dll, 63);
        app_str(&a, "!", 1);
        if (by_ord) { app_str(&a, "#", 1); app_u(&a, ord); }
        else app_str(&a, name, 96);
        a.b[a.n] = 0;
        ctx->first_unresolved_module = self;
        ctx->first_unresolved_why = why;
    }
    ctx->unresolved++;
    if (self >= 0 && self < (int)PE_MOD_MAX_MODULES) ctx->mod[self].unresolved++;
}

/* ------------------------------------------------------------------------------------------------ */
/* dependency edges                                                                                  */
/* ------------------------------------------------------------------------------------------------ */

/* `from` needs `to` (to is initialised first; a reference is held on `to`).  Once per pair. */
static void add_dep(pe_mod_ctx_t *ctx, int from, int to)
{
    u32 bit;
    if (from == to || from < 0 || to < 0 || from >= (int)PE_MOD_MAX_MODULES || to >= (int)PE_MOD_MAX_MODULES) return;
    bit = 1u << (u32)to;
    if (ctx->mod[from].deps & bit) return;
    ctx->mod[from].deps |= bit;
    ctx->mod[to].refcount++;
}

/* ------------------------------------------------------------------------------------------------ */
/* mapping one image                                                                                 */
/* ------------------------------------------------------------------------------------------------ */

static void mod_free_image(const pe_mod_ops_t *ops, u8 *img, unsigned long size)
{
    if (img && ops->free_image) ops->free_image(img, size, ops->user);
}

/* parse + allocate + map + relocate + validate everything the linker will walk.  On success the slot is LINKING
 * (visible to lookups) with refcount 0; on failure the slot is untouched/free and the image released. */
MOD_FN static int mod_map(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, const u8 *file, unsigned long len, int is_main,
                          const char *name, int *out_idx)
{
    int idx = -1, r;
    u32 i, n, un;
    u64 base;
    unsigned long size;
    u8 *img;
    pe_mod_t *M;

    for (i = 0; i < PE_MOD_MAX_MODULES; i++)
        if (ctx->mod[i].state == PE_MOD_S_FREE) { idx = (int)i; break; }
    if (idx < 0) return PE_MOD_E_FULL;
    M = &ctx->mod[idx];
    mod_zero(M, sizeof *M);

    r = pe_parse(file, len, &M->info);
    if (r != PE_OK) return r;                                   /* pe_parse zeroed M->info */
    if (is_main ? M->info.is_dll : !M->info.is_dll) { mod_zero(M, sizeof *M); return PE_MOD_E_KIND; }

    size = M->info.size_of_image;
    img = ops->alloc_image(size, M->info.image_base, ops->user);
    if (!img) { mod_zero(M, sizeof *M); return PE_MOD_E_ALLOC; }
    base = (u64)(__UINTPTR_TYPE__)img;
    if ((base & 0xfffu) != 0 || base > ~0ull - size) { r = PE_MOD_E_ALLOC; goto fail; }

    r = pe_map(file, len, &M->info, img);
    if (r != PE_OK) goto fail;
    r = pe_relocate(&M->info, img, base);
    if (r != PE_OK) goto fail;
    r = pe_resolve_imports(&M->info, img, 0, 0, &n, &un);       /* dry run: reject a malformed import table up front */
    if (r != PE_OK) goto fail;
    r = pe_delay_imports(&M->info, img, 0, 0, &n, &un);
    if (r != PE_OK) goto fail;
    r = pe_export_validate(&M->info, img);                      /* 1 = no exports */
    if (r < 0) goto fail;
    r = pe_tls_info(&M->info, img, base, 0, 0, 0, 0);           /* 1 = no TLS */
    if (r < 0) goto fail;
    if (r == PE_OK) M->flags |= PE_MOD_F_TLS;

    for (i = 0; i + 1u < PE_MOD_NAME_MAX && name[i]; i++) M->name[i] = name[i];
    M->name[i] = 0;
    M->image = img;
    M->base = base;
    M->size = size;
    M->entry = M->info.entry_rva ? base + M->info.entry_rva : 0;
    M->refcount = 0;
    M->init = PE_MOD_I_NONE;
    if (is_main) M->flags |= PE_MOD_F_MAIN;
    M->state = PE_MOD_S_LINKING;
    *out_idx = idx;
    return 0;
fail:
    mod_free_image(ops, img, size);
    mod_zero(M, sizeof *M);
    return r;
}

/* ------------------------------------------------------------------------------------------------ */
/* export resolution with forwarders                                                                 */
/* ------------------------------------------------------------------------------------------------ */

MOD_FN static int load_dll_by_name(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, const char *canon, unsigned depth, int *out_idx);

/* Resolves one export of `mod` and follows forwarder chains.  Returns 0 (*out = address), 1 (builtin stub in *out),
 * or a negative code.  `depth` = nesting depth of the requester; DLLs loaded on the way sit at depth + 1. */
MOD_FN static int getproc_chain(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, int mod, const char *name, unsigned ord,
                                int by_ord, unsigned depth, u64 *out)
{
    char fwd[PE_MAX_FWD], fname[PE_MAX_FWD], dllp[PE_MOD_NAME_MAX], canon[PE_MOD_NAME_MAX];
    int seen_mod[PE_MOD_MAX_FWD_HOPS + 1];
    u32 seen_rva[PE_MOD_MAX_FWD_HOPS + 1];
    const char *cname = name;
    unsigned cord = ord;
    int cby = by_ord, cur = mod;
    u32 hop, k;

    *out = 0;
    for (hop = 0;; hop++) {
        const pe_mod_t *M;
        unsigned int rva = 0, ford = 0;
        int r, fby = 0, t;
        u64 addr;

        if (cur < 0 || cur >= (int)PE_MOD_MAX_MODULES || !mod_active(&ctx->mod[cur])) return PE_MOD_E_ARG;
        M = &ctx->mod[cur];
        r = pe_export_find(&M->info, M->image, cname, cord, cby, &rva, fwd, sizeof fwd);
        if (r == PE_OK) { *out = M->base + rva; return 0; }
        if (r != PE_EXPORT_FORWARDER) return r;                 /* not found / malformed table */

        if (hop >= PE_MOD_MAX_FWD_HOPS) return PE_MOD_E_FWD_DEPTH;
        for (k = 0; k < hop; k++)
            if (seen_mod[k] == cur && seen_rva[k] == rva) return PE_MOD_E_FWD_CYCLE;
        seen_mod[hop] = cur;
        seen_rva[hop] = rva;

        if (pe_export_parse_forwarder(fwd, dllp, sizeof dllp, fname, sizeof fname, &ford, &fby) != PE_OK)
            return PE_E_FWD;
        if (canon_name(dllp, 1, canon) != 0) return PE_E_FWD;   /* forwarder DLL names carry no extension */

        if (ops->builtin_resolve) {                             /* system DLL? (same protocol as for imports) */
            addr = 0;
            r = ops->builtin_resolve(canon, fby ? "" : fname, (unsigned short)ford, fby, &addr, ops->user);
            if (r == 0 && addr != 0) { *out = addr; return 0; }
            if (addr != 0) { *out = addr; return 1; }
        }
        t = find_mod(ctx, canon);
        if (t < 0) {
            r = load_dll_by_name(ctx, ops, canon, depth + 1u, &t);
            if (r < 0) return r;
        }
        add_dep(ctx, cur, t);                                   /* the forwarder target initialises first */
        cur = t;
        cname = fby ? "" : fname;
        cord = ford;
        cby = fby;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* import linking                                                                                    */
/* ------------------------------------------------------------------------------------------------ */

enum { LV_NONE = 0, LV_BUILTIN, LV_MODULE, LV_FAIL };

typedef struct {
    pe_mod_ctx_t *ctx;
    const pe_mod_ops_t *ops;
    int self, target, verdict, have, delay, why;
    unsigned depth;
    char raw[PE_MOD_NAME_MAX];      /* the import DLL string as given (when it fits), for "same descriptor?" */
    char canon[PE_MOD_NAME_MAX];
} link_t;

/* The import DLL string changed: classify it (name validity) and reset the per-DLL verdict. */
MOD_FN static void begin_dll(link_t *L, const char *dll)
{
    u32 n = slen_cap(dll, PE_MOD_NAME_MAX), i;

    L->verdict = LV_NONE;
    L->target = -1;
    L->have = 0;
    L->raw[0] = 0;
    L->canon[0] = 0;
    if (n < PE_MOD_NAME_MAX) {                                  /* short enough to cache */
        for (i = 0; i < n; i++) { L->raw[i] = dll[i]; MOD_BARRIER(); }
        L->raw[n] = 0;
        L->have = 1;
    }
    L->why = PE_MOD_E_NAME;
    if (canon_name(dll, 0, L->canon) != 0) L->verdict = LV_FAIL; /* junk / too long: unresolved, never truncated */
}

static int raw_same(const char *cached, const char *dll)
{
    u32 k;
    for (k = 0;; k++) {
        if (cached[k] != dll[k]) return 0;
        if (cached[k] == 0) return 1;
    }
}

/* pe_resolve_fn handed to pe_resolve_imports / pe_delay_imports.  One call per imported function. */
MOD_FN static int link_resolve(const char *dll, const char *name, unsigned short ord, int by_ord,
                               unsigned long long *out, void *user)
{
    link_t *L = (link_t *)user;
    pe_mod_ctx_t *ctx = L->ctx;
    const pe_mod_ops_t *ops = L->ops;
    u64 addr;
    int rc, t, why = PE_E_NOTFOUND;

    *out = 0;
    if (!L->have || !raw_same(L->raw, dll)) begin_dll(L, dll);
    if (L->verdict == LV_FAIL) { why = L->why; goto unresolved; }

    if (L->verdict == LV_NONE || L->verdict == LV_BUILTIN) {
        if (ops->builtin_resolve) {
            addr = 0;
            rc = ops->builtin_resolve(L->canon, name, ord, by_ord, &addr, ops->user);
            if (rc == 0 && addr != 0) { L->verdict = LV_BUILTIN; *out = addr; return 0; }
            if (addr != 0) {                                    /* builtin DLL, unimplemented function: loud stub */
                L->verdict = LV_BUILTIN;
                *out = addr;
                note_unres(ctx, L->self, dll, name, ord, by_ord, L->delay, PE_E_NOTFOUND);
                return 1;
            }
        }
        if (L->verdict == LV_BUILTIN) goto unresolved;          /* a builtin DLL that declined this name */

        t = find_mod(ctx, L->canon);
        if (t < 0) {
            if (L->depth + 1u > PE_MOD_MAX_DEPTH) { L->verdict = LV_FAIL; L->why = why = PE_MOD_E_DEPTH; goto unresolved; }
            rc = load_dll_by_name(ctx, ops, L->canon, L->depth + 1u, &t);
            if (rc < 0) { L->verdict = LV_FAIL; L->why = why = rc; ctx->last_error = rc; goto unresolved; }
        }
        L->verdict = LV_MODULE;
        L->target = t;
        add_dep(ctx, L->self, t);
    }

    rc = getproc_chain(ctx, ops, L->target, name, ord, by_ord, L->depth + 1u, &addr);
    if (rc == 0) { *out = addr; return 0; }
    if (rc == 1 && addr != 0) {                                 /* forwarded into a builtin DLL's unimplemented name */
        *out = addr;
        note_unres(ctx, L->self, dll, name, ord, by_ord, L->delay, PE_E_NOTFOUND);
        return 1;
    }
    why = rc < 0 ? rc : PE_E_NOTFOUND;
unresolved:
    note_unres(ctx, L->self, dll, name, ord, by_ord, L->delay, why);
    if (!L->delay && ops->unresolved_stub) {
        addr = 0;
        if (ops->unresolved_stub(dll, name, ord, by_ord, &addr, ops->user) == 0 && addr != 0) *out = addr;
    }
    return 1;
}

MOD_FN static void link_module(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, int idx, unsigned depth)
{
    pe_mod_t *M = &ctx->mod[idx];
    link_t L;
    u32 n = 0, un = 0;
    int r;

    L.ctx = ctx; L.ops = ops; L.self = idx; L.target = -1; L.verdict = LV_NONE; L.have = 0; L.delay = 0; L.why = 0;
    L.depth = depth; L.raw[0] = 0; L.canon[0] = 0;
    r = pe_resolve_imports(&M->info, M->image, link_resolve, &L, &n, &un);
    if (r != PE_OK) {                                           /* only if the table changed under us (overlapping IAT) */
        M->flags |= PE_MOD_F_LINK_ERR;
        ctx->link_errors++;
        ctx->last_error = r;
    }
    if (M->info.delay_import_rva && !(ctx->flags & PE_MOD_CF_NO_DELAY)) {
        L.verdict = LV_NONE; L.target = -1; L.have = 0; L.delay = 1; L.raw[0] = 0; L.canon[0] = 0;
        r = pe_delay_imports(&M->info, M->image, link_resolve, &L, &n, &un);
        if (r != PE_OK) {
            M->flags |= PE_MOD_F_LINK_ERR;
            ctx->link_errors++;
            ctx->last_error = r;
        }
    }
    M->state = PE_MOD_S_LINKED;
}

MOD_FN static int load_dll_by_name(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, const char *canon, unsigned depth, int *out_idx)
{
    const u8 *buf = 0;
    unsigned long len = 0;
    int idx = -1, r;

    if (depth > PE_MOD_MAX_DEPTH) return PE_MOD_E_DEPTH;
    if (!ops->read_file || !ops->alloc_image) return PE_MOD_E_ARG;
    if (ops->read_file(canon, &buf, &len, ops->user) != 0 || !buf) return PE_MOD_E_NOFILE;
    r = mod_map(ctx, ops, buf, len, 0, canon, &idx);
    if (ops->release_file) ops->release_file(buf, len, ops->user);   /* mapped: the file bytes are no longer needed */
    if (r < 0) return r;
    link_module(ctx, ops, idx, depth);
    *out_idx = idx;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* public API: loading                                                                               */
/* ------------------------------------------------------------------------------------------------ */

void pe_mod_ctx_init(pe_mod_ctx_t *ctx)
{
    if (!ctx) return;
    mod_zero(ctx, sizeof *ctx);
    ctx->main_idx = 0;
    ctx->failed_module = -1;
    ctx->first_unresolved_module = -1;
    ctx->first_unresolved_why = 0;
}

MOD_FN int pe_mod_load_main(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, const unsigned char *file, unsigned long len,
                            const char *name)
{
    const char *b = "", *p;
    char nm[PE_MOD_NAME_MAX];
    u32 n, i, scanned = 0;
    int idx = -1, r;

    if (!ctx || !ops || !file || !ops->alloc_image) return PE_MOD_E_ARG;
    if (ctx->has_main) return PE_MOD_E_STATE;

    nm[0] = 0;
    if (name) {                                                 /* keep only the last path component, if usable */
        b = name;
        for (p = name; *p && scanned < 4096u; p++, scanned++) if (*p == '/' || *p == '\\') b = p + 1;
        n = slen_cap(b, PE_MOD_NAME_MAX);
        if (n > 0 && n < PE_MOD_NAME_MAX && name_chars_ok(b, n)) {
            for (i = 0; i < n; i++) { nm[i] = b[i]; MOD_BARRIER(); }
            nm[n] = 0;
        }
    }
    r = mod_map(ctx, ops, file, len, 1, nm, &idx);
    if (r < 0) return r;
    ctx->has_main = 1;
    ctx->main_idx = idx;
    ctx->mod[idx].refcount = 1;
    ctx->mod[idx].pub_refs = 1;
    link_module(ctx, ops, idx, 0);
    return idx;
}

MOD_FN int pe_mod_load(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, const char *dll_name)
{
    char canon[PE_MOD_NAME_MAX];
    int idx, r;

    if (!ctx || !ops) return PE_MOD_E_ARG;
    r = canon_name(dll_name, 0, canon);
    if (r != 0) return r;
    idx = find_mod(ctx, canon);
    if (idx < 0) {
        r = load_dll_by_name(ctx, ops, canon, 1u, &idx);
        if (r < 0) return r;
    }
    ctx->mod[idx].refcount++;
    ctx->mod[idx].pub_refs++;
    return idx;
}

MOD_FN int pe_mod_getproc(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, int mod, const char *name, unsigned ordinal,
                          int by_ordinal, unsigned long long *out_addr)
{
    if (out_addr) *out_addr = 0;
    if (!ctx || !ops || !out_addr) return PE_MOD_E_ARG;
    if (mod < 0 || mod >= (int)PE_MOD_MAX_MODULES || !mod_active(&ctx->mod[mod])) return PE_MOD_E_ARG;
    if (!by_ordinal && !name) return PE_MOD_E_ARG;
    return getproc_chain(ctx, ops, mod, by_ordinal ? "" : name, ordinal, by_ordinal != 0, 0u, out_addr);
}

/* ------------------------------------------------------------------------------------------------ */
/* DllMain ordering                                                                                  */
/* ------------------------------------------------------------------------------------------------ */

static int mod_callable(const pe_mod_t *M) { return M->info.is_dll && M->entry != 0; }

/* Detach, in reverse attach order, every ATTACHED module whose attach_seq > min_seq; module ends in `end_state`. */
MOD_FN static void detach_batch(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, unsigned min_seq, int end_state)
{
    for (;;) {
        int best = -1;
        u32 i;
        pe_mod_t *M;
        for (i = 0; i < PE_MOD_MAX_MODULES; i++) {
            const pe_mod_t *c = &ctx->mod[i];
            if (mod_active(c) && c->init == PE_MOD_I_ATTACHED && c->attach_seq > min_seq &&
                (best < 0 || c->attach_seq > ctx->mod[best].attach_seq))
                best = (int)i;
        }
        if (best < 0) return;
        M = &ctx->mod[best];
        M->init = (unsigned char)end_state;                     /* before the call: a re-entrant detach must skip it */
        if (end_state == PE_MOD_I_NONE) M->attach_seq = 0;
        if (mod_callable(M) && ops->call_entry) ops->call_entry(M->entry, M->base, PE_MOD_DLL_PROCESS_DETACH, ops->user);
    }
}

/* Depth-first: dependencies first.  `visiting` marks the DFS path (a dependency that is on it is a cycle member
 * reached second: it is linked by address but initialised later). */
MOD_FN static int init_visit(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, int i, u32 *visiting)
{
    pe_mod_t *M = &ctx->mod[i];
    u32 j;
    int ok = 1;

    *visiting |= 1u << (u32)i;
    for (j = 0; j < PE_MOD_MAX_MODULES; j++) {
        pe_mod_t *D;
        int r;
        if (!(M->deps & (1u << j))) continue;
        if (*visiting & (1u << j)) continue;
        D = &ctx->mod[j];
        if (D->state != PE_MOD_S_LINKED || D->init != PE_MOD_I_NONE) continue;
        r = init_visit(ctx, ops, (int)j, visiting);
        if (r < 0) { *visiting &= ~(1u << (u32)i); return r; }
    }
    *visiting &= ~(1u << (u32)i);

    if (M->state != PE_MOD_S_LINKED || M->init != PE_MOD_I_NONE) return 0;   /* e.g. attached by a nested call */
    M->init = PE_MOD_I_ATTACHING;                               /* guards against re-entrant pe_mod_run_inits (DllMain -> LoadLibrary) */
    if (mod_callable(M)) ok = ops->call_entry(M->entry, M->base, PE_MOD_DLL_PROCESS_ATTACH, ops->user) != 0;
    if (!ok) {
        M->init = PE_MOD_I_NONE;
        ctx->failed_module = i;
        return PE_MOD_E_INIT;
    }
    M->init = PE_MOD_I_ATTACHED;
    M->attach_seq = ++ctx->attach_counter;
    return 0;
}

MOD_FN int pe_mod_run_inits(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops)
{
    u32 i, visiting = 0;
    unsigned start;

    if (!ctx || !ops || !ops->call_entry) return PE_MOD_E_ARG;
    start = ctx->attach_counter;
    for (i = 0; i < PE_MOD_MAX_MODULES; i++) {
        int r;
        if (ctx->mod[i].state != PE_MOD_S_LINKED || ctx->mod[i].init != PE_MOD_I_NONE) continue;
        r = init_visit(ctx, ops, (int)i, &visiting);
        if (r < 0) {
            detach_batch(ctx, ops, start, PE_MOD_I_NONE);       /* roll back what THIS call attached, newest first */
            return r;
        }
    }
    return 0;
}

MOD_FN int pe_mod_run_detach(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops)
{
    if (!ctx || !ops || !ops->call_entry) return PE_MOD_E_ARG;
    detach_batch(ctx, ops, 0, PE_MOD_I_DETACHED);
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* unloading                                                                                         */
/* ------------------------------------------------------------------------------------------------ */

MOD_FN static void unload_mod(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, int idx)
{
    pe_mod_t *M = &ctx->mod[idx];
    u32 deps, j;

    M->state = PE_MOD_S_UNLOADING;                              /* invisible to lookups; breaks release cycles */
    if (M->init == PE_MOD_I_ATTACHED && mod_callable(M) && ops->call_entry)
        ops->call_entry(M->entry, M->base, PE_MOD_DLL_PROCESS_DETACH, ops->user);
    M->init = PE_MOD_I_DETACHED;
    deps = M->deps;
    M->deps = 0;
    for (j = 0; j < PE_MOD_MAX_MODULES; j++) {
        pe_mod_t *D;
        if (!(deps & (1u << j))) continue;
        D = &ctx->mod[j];
        if (!mod_active(D)) continue;
        if (--D->refcount <= 0) unload_mod(ctx, ops, (int)j);
    }
    mod_free_image(ops, M->image, M->size);
    if (ctx->has_main && ctx->main_idx == idx) ctx->has_main = 0;
    mod_zero(M, sizeof *M);
}

MOD_FN int pe_mod_free(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, int mod)
{
    if (!ctx || !ops) return PE_MOD_E_ARG;
    if (mod < 0 || mod >= (int)PE_MOD_MAX_MODULES || !mod_active(&ctx->mod[mod])) return PE_MOD_E_ARG;
    if (ctx->mod[mod].pub_refs <= 0) return PE_MOD_E_ARG;       /* nothing explicit left to release */
    ctx->mod[mod].pub_refs--;
    if (--ctx->mod[mod].refcount <= 0) unload_mod(ctx, ops, mod);
    return 0;
}

MOD_FN void pe_mod_free_all(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops)
{
    u32 i;
    if (!ctx) return;
    if (ops && ops->call_entry) pe_mod_run_detach(ctx, ops);
    for (i = 0; i < PE_MOD_MAX_MODULES; i++)
        if (ctx->mod[i].state != PE_MOD_S_FREE && ops) mod_free_image(ops, ctx->mod[i].image, ctx->mod[i].size);
    pe_mod_ctx_init(ctx);
}

/* ------------------------------------------------------------------------------------------------ */
/* lookups                                                                                           */
/* ------------------------------------------------------------------------------------------------ */

MOD_FN int pe_mod_find(const pe_mod_ctx_t *ctx, const char *name)
{
    char canon[PE_MOD_NAME_MAX];
    int r;
    if (!ctx) return PE_MOD_E_ARG;
    r = canon_name(name, 0, canon);
    if (r != 0) return r;
    return find_mod(ctx, canon);
}

int pe_mod_find_base(const pe_mod_ctx_t *ctx, unsigned long long base)
{
    u32 i;
    if (!ctx) return PE_MOD_E_ARG;
    for (i = 0; i < PE_MOD_MAX_MODULES; i++)
        if (mod_active(&ctx->mod[i]) && ctx->mod[i].base == base) return (int)i;
    return PE_E_NOTFOUND;
}

const pe_mod_t *pe_mod_get(const pe_mod_ctx_t *ctx, int mod)
{
    if (!ctx || mod < 0 || mod >= (int)PE_MOD_MAX_MODULES || !mod_active(&ctx->mod[mod])) return 0;
    return &ctx->mod[mod];
}

unsigned long long pe_mod_entry_va(const pe_mod_ctx_t *ctx, int mod)
{
    const pe_mod_t *M = pe_mod_get(ctx, mod);
    return M ? M->entry : 0;
}
