/*
 * pe_modules.h -- PE module-graph loader: DLL dependencies, forwarders, LoadLibrary/GetProcAddress semantics,
 * DllMain ordering.  Written against CALLBACKS so it is OS-agnostic and host-testable.
 * ================================================================================================================
 *
 * Freestanding C11 on top of pe.h / pe_exports.h: no libc, no malloc, no globals; every byte of state lives in
 * the caller-provided pe_mod_ctx_t (~103 KiB, sizeof == 105712: keep it static or mmap it, never on the stack).
 * Original code written from the public Microsoft "PE Format" specification.  Hostile-input safe: all file-derived
 * data goes through pe.c / pe_exports.c (bounds-checked, capped); names are validated and NEVER truncated into a
 * different match.
 *
 * Caller flow (what winrun does):
 *
 *     static pe_mod_ctx_t ctx;                      // zero-init is fine, pe_mod_ctx_init() is explicit
 *     pe_mod_ops_t ops = { read_file, alloc_image, builtin_resolve, call_entry, user, <optional...> };
 *     pe_mod_ctx_init(&ctx);
 *     int m = pe_mod_load_main(&ctx, &ops, exe_bytes, exe_len, "prog.exe");   // map+reloc+link the exe and,
 *                                                                              // recursively, every DLL it needs
 *     if (m < 0) -> reject (pe_mod_strerror(m));
 *     // ctx.unresolved / ctx.first_unresolved report unresolved imports (they are NOT fatal)
 *     if (pe_mod_run_inits(&ctx, &ops) < 0) -> a DllMain returned FALSE (ctx.failed_module); already rolled back
 *     entry = pe_mod_entry_va(&ctx, m);  ((unsigned (__attribute__((ms_abi)) *)(void))entry)();
 *     ... on ExitProcess:  pe_mod_run_detach(&ctx, &ops); pe_mod_free_all(&ctx, &ops);
 *
 *   LoadLibraryA(name):      i = pe_mod_load(&ctx,&ops,name); if (i >= 0) { pe_mod_run_inits(&ctx,&ops); return base }
 *                            (on a DllMain failure call pe_mod_free(&ctx,&ops,i) and return NULL)
 *   GetProcAddress(h,name):  i = pe_mod_find_base(&ctx,(u64)h); pe_mod_getproc(&ctx,&ops,i,name,0,0,&a)
 *                            (an LPCSTR < 0x10000 is an ordinal: by_ordinal = 1)
 *   FreeLibrary(h):          pe_mod_free(&ctx,&ops,pe_mod_find_base(...))
 *
 * Semantics (all covered by tests/win/pe_dll_test.c):
 *   - Import resolution, per imported DLL: builtin_resolve FIRST (system / shim DLLs win even if a file of that name
 *     exists); if it declines, the DLL is loaded through read_file (recursively, cycle-safe: a DLL still being
 *     linked is bound to by base address, as Windows does) and each name/ordinal is resolved against its export
 *     table, FOLLOWING FORWARDERS (to other loaded DLLs or to builtin_resolve) with hop cap + cycle detection.
 *   - Unresolved imports never abort and never crash the loader: they are counted (ctx.unresolved), the first one
 *     is recorded as "dll!name" / "dll!#ordinal" in ctx.first_unresolved, and the IAT slot points at
 *     ops->unresolved_stub's address if provided, else 0.
 *   - DLL names are matched case-INSENSITIVELY, with or without ".dll"; export names case-SENSITIVELY.  A DLL name
 *     must be 1..63 printable-ASCII bytes, no path separators / ':' / wildcards (a bare file name); longer or junk
 *     names are rejected (the import stays unresolved), never truncated.
 *   - DLL_PROCESS_ATTACH runs in DEPENDENCY order (a module's imports and forwarder targets first); in a cycle
 *     the member reached second is initialised first.  A DllMain returning FALSE aborts: modules attached by that
 *     pe_mod_run_inits() call are detached in reverse order; the failing DLL itself gets no DETACH (its initialisation
 *     never completed, so there is nothing to clean up; this is the observed ntdll behaviour).
 *     pe_mod_run_detach() detaches in reverse attach order.  The exe's entry point is never called by the loader.
 *   - Load at the preferred base is only a hint to alloc_image; pe_relocate is applied with the ACTUAL base.
 *
 * NOT implemented (documented in README.md): TLS (a TLS directory is recorded as PE_MOD_F_TLS, never executed),
 * SEH / .pdata registration, DLL_THREAD_ATTACH/DETACH notifications, bound imports, API-set schema redirection,
 * side-by-side/manifest search, load-config / CFG, resources, LOAD_LIBRARY_AS_DATAFILE, reference-counted unload of
 * import CYCLES (a cycle is only released by pe_mod_free_all).
 *
 * Stack: each import-recursion level costs 2-3 KiB (pe_resolve_imports alone keeps 1.4 KiB of name buffers; measured
 * with -fstack-usage: link_module 240 + pe_resolve_imports 1440 + link_resolve 112 + load_dll_by_name 96, and a
 * forwarder-triggered load adds getproc_chain 928).  A realistic graph (depth <= 4) needs < 12 KiB; the cap
 * PE_MOD_MAX_DEPTH = 16 bounds the pathological worst case at about 50 KiB, so build with -DPE_MOD_MAX_DEPTH=8 (~25 KiB)
 * if the process stack is only 64 KiB (kernel/fs/exec.c USER_STACK_SIZE).
 * Not re-entrant: do not call pe_mod_* from inside read_file / alloc_image / builtin_resolve (calling pe_mod_load +
 * pe_mod_run_inits from inside call_entry, i.e. from a DllMain, IS supported).
 */
#ifndef PE_MODULES_H
#define PE_MODULES_H

#include "pe.h"
#include "pe_exports.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caps.  PE_MOD_MAX_DEPTH and PE_MOD_MAX_FWD_HOPS may be lowered at build time (-DPE_MOD_MAX_DEPTH=8) if the user
 * stack is tight; PE_MOD_MAX_MODULES is fixed (a dependency set is a u32 bitmask). */
#define PE_MOD_MAX_MODULES   32u     /* loaded modules (incl. the exe); also the width of the deps bitmask     */
#define PE_MOD_NAME_MAX      64u     /* bytes incl. NUL => names of at most 63 characters                      */
#ifndef PE_MOD_MAX_DEPTH
#define PE_MOD_MAX_DEPTH     16u     /* import / load nesting depth (the exe is depth 0)                       */
#endif
#ifndef PE_MOD_MAX_FWD_HOPS
#define PE_MOD_MAX_FWD_HOPS  16u     /* forwarder chain length                                                 */
#endif
#define PE_MOD_MSG_MAX       192u    /* ctx.first_unresolved buffer                                            */

#define PE_MOD_DLL_PROCESS_DETACH 0u /* `reason` values handed to call_entry                                   */
#define PE_MOD_DLL_PROCESS_ATTACH 1u

/* Error codes (negative).  Parse / map / relocate / import / export failures are passed through unchanged as the
 * PE_E_* codes of pe.h and pe_exports.h (-1 .. -16). */
enum {
    PE_MOD_E_ARG       = -32,   /* NULL / invalid argument, missing required callback, bad module index        */
    PE_MOD_E_NAME      = -33,   /* DLL name empty, too long, or contains junk / path separators                */
    PE_MOD_E_FULL      = -34,   /* module table full (PE_MOD_MAX_MODULES)                                      */
    PE_MOD_E_DEPTH     = -35,   /* import nesting deeper than PE_MOD_MAX_DEPTH                                 */
    PE_MOD_E_NOFILE    = -36,   /* read_file could not supply the DLL                                          */
    PE_MOD_E_ALLOC     = -37,   /* alloc_image failed / returned a non page-aligned or wrapping region         */
    PE_MOD_E_KIND      = -38,   /* main module is a DLL, or a dependency is not a DLL                          */
    PE_MOD_E_FWD_CYCLE = -39,   /* forwarder chain revisits an export                                          */
    PE_MOD_E_FWD_DEPTH = -40,   /* forwarder chain longer than PE_MOD_MAX_FWD_HOPS                             */
    PE_MOD_E_INIT      = -41,   /* DllMain(DLL_PROCESS_ATTACH) returned FALSE                                  */
    PE_MOD_E_STATE     = -42    /* e.g. a main module is already loaded                                        */
};

typedef struct {
    /* REQUIRED.  Supplies the file for `dll_name` (canonical form: an extension is present, e.g. "foo.dll"; the
     * search policy lives here).  Return 0 and set *buf / *len; the bytes must stay valid until the loader has
     * mapped the image (it copies them immediately) -- i.e. until the next callback invocation or release_file.
     * Nonzero = not found. */
    int (*read_file)(const char *dll_name, const unsigned char **buf, unsigned long *len, void *u);

    /* REQUIRED.  Returns a ZERO-FILLED, readable+writable+executable, page-aligned region of `size` bytes, or NULL.
     * `preferred_base` is a hint: honouring it only saves the relocation pass (the loader relocates for whatever address
     * it gets) -- EXCEPT for an image that carries no base-relocation directory (common for exes linked without
     * /DYNAMICBASE): such an image can only run at exactly its preferred base and fails with PE_E_RELOC otherwise. */
    unsigned char *(*alloc_image)(unsigned long size, unsigned long long preferred_base, void *u);

    /* OPTIONAL (NULL = nothing is builtin).  System / shim DLLs.  `dll` is canonical ("KERNEL32.dll"); for an
     * ordinal import name == "" and by_ordinal != 0.  *out_addr is pre-zeroed.
     *   return 0                          : resolved, *out_addr valid
     *   return !=0, *out_addr != 0        : this IS a builtin DLL but the function is unimplemented; *out_addr is a
     *                                       loud stub.  Counted unresolved; the stub address goes into the IAT; the
     *                                       loader does NOT try to load a file DLL of that name.
     *   return !=0, *out_addr == 0        : not a builtin DLL (or no stub): the loader goes on to read_file.
     * Must have no side effects for DLLs it does not own (the loader may probe it once per imported DLL). */
    int (*builtin_resolve)(const char *dll, const char *name, unsigned short ordinal, int by_ordinal,
                           unsigned long long *out_addr, void *u);

    /* REQUIRED for pe_mod_run_inits / pe_mod_run_detach.  Calls the DLL entry point DllMain(hinst = module_base,
     * reason, NULL) through the Windows x64 ABI; returns nonzero for TRUE.  (The return value of a DETACH call is
     * ignored.) */
    int (*call_entry)(unsigned long long entry_va, unsigned long long module_base, unsigned reason, void *u);

    void *user;                                   /* passed back as `u` to every callback */

    /* OPTIONAL */
    void (*release_file)(const unsigned char *buf, unsigned long len, void *u);   /* called after mapping a DLL file */
    void (*free_image)(unsigned char *image, unsigned long size, void *u);        /* unload / failed load / free_all */
    /* Address for an import that could not be resolved from a FILE dll (missing DLL, missing export, bad name):
     * return 0 and set *out_addr to a loud stub, nonzero = no stub (slot stays 0).  Not used for delay imports. */
    int (*unresolved_stub)(const char *dll, const char *name, unsigned short ordinal, int by_ordinal,
                           unsigned long long *out_addr, void *u);
} pe_mod_ops_t;

/* pe_mod_t.state */
enum { PE_MOD_S_FREE = 0, PE_MOD_S_LINKING = 1, PE_MOD_S_LINKED = 2, PE_MOD_S_UNLOADING = 3 };
/* pe_mod_t.init */
enum { PE_MOD_I_NONE = 0, PE_MOD_I_ATTACHING = 1, PE_MOD_I_ATTACHED = 2, PE_MOD_I_DETACHED = 3 };
/* pe_mod_t.flags */
#define PE_MOD_F_MAIN      0x1u   /* the exe                                                                    */
#define PE_MOD_F_TLS       0x2u   /* has a TLS directory (validated, NOT executed / allocated)                  */
#define PE_MOD_F_LINK_ERR  0x4u   /* the import table turned malformed while being linked (see ctx.last_error)  */
/* pe_mod_ctx_t.flags */
#define PE_MOD_CF_NO_DELAY 0x1u   /* do not bind delay-load imports eagerly                                     */

typedef struct {
    pe_info_t info;                         /* parse result (sec[] included); keep: exports are looked up with it */
    unsigned long long base;                /* actual load address == (u64)image == HMODULE                       */
    unsigned long long entry;               /* absolute entry-point VA, 0 if none                                 */
    unsigned char *image;
    unsigned long size;                     /* info.size_of_image                                                 */
    unsigned deps;                          /* bit j set: this module needs module j (init-before, ref held)      */
    int refcount;                           /* pub_refs + the number of modules that depend on it                  */
    int pub_refs;                           /* explicit loads (pe_mod_load / load_main) not yet pe_mod_free'd       */
    unsigned attach_seq;                    /* order of DLL_PROCESS_ATTACH                                        */
    unsigned unresolved;                    /* unresolved imports of THIS module                                  */
    unsigned flags;
    unsigned char state, init;
    char name[PE_MOD_NAME_MAX];             /* as first requested; "" if unusable                                 */
} pe_mod_t;

typedef struct {
    pe_mod_t mod[PE_MOD_MAX_MODULES];
    unsigned flags;                         /* PE_MOD_CF_* */
    int has_main, main_idx;
    unsigned attach_counter;
    unsigned unresolved;                    /* unresolved imports, all modules, since pe_mod_ctx_init             */
    unsigned delay_unresolved;              /* delay-load imports that could not be bound (not an error)          */
    unsigned link_errors;                   /* modules whose import table broke mid-link (PE_MOD_F_LINK_ERR)      */
    int last_error;                         /* last non-fatal code: a dependency that failed to load, a link error */
    int failed_module;                      /* valid after PE_MOD_E_INIT: the module whose DllMain said FALSE     */
    int first_unresolved_module;            /* module that owns first_unresolved (-1 none)                        */
    int first_unresolved_why;               /* why it failed: PE_MOD_E_NOFILE / PE_MOD_E_NAME / PE_MOD_E_DEPTH / PE_MOD_E_FULL /
                                               PE_E_NOTFOUND (no such export or builtin stub) / PE_E_* of a bad image / ...  */
    char first_unresolved[PE_MOD_MSG_MAX];  /* "dll!name" or "dll!#ordinal" (display text; very long names are cut) */
} pe_mod_ctx_t;

/* Static, never-NULL description of any pe_mod / pe_export / pe code. */
const char *pe_mod_strerror(int err);

/* Resets the context (all module slots free).  A zeroed context is also valid, but failed_module / main_idx read 0. */
void pe_mod_ctx_init(pe_mod_ctx_t *ctx);

/* Maps + relocates + links the exe `file` (the caller keeps ownership of the bytes) and every DLL it needs.
 * Returns the module index (>= 0) or a negative error.  `name` (may be a path; only the last component is kept)
 * is how other modules can import from it by name.  Unresolved imports are not errors (see ctx->unresolved). */
int pe_mod_load_main(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, const unsigned char *file, unsigned long len,
                     const char *name);

/* LoadLibrary: a case-insensitive name match against a loaded module just bumps its refcount; otherwise the DLL is
 * read, mapped and linked.  Returns the module index (>= 0) or a negative error.  Does NOT run DllMain: call
 * pe_mod_run_inits() afterwards. */
int pe_mod_load(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, const char *dll_name);

/* GetProcAddress for module `mod`: by name (exact, case-sensitive) or by public ordinal; FOLLOWS FORWARDERS across
 * modules (loading forwarder-target DLLs on demand) and into builtin_resolve.
 *   returns 0   : *out_addr = absolute address
 *           1   : unresolved, but a builtin DLL supplied a stub (*out_addr = stub) -- GetProcAddress should
 *                 return NULL, an import binder should use the stub
 *           < 0 : PE_E_NOTFOUND (no such export), PE_E_EXPORT / PE_E_FWD / PE_MOD_E_FWD_CYCLE / PE_MOD_E_FWD_DEPTH /
 *                 PE_MOD_E_* ; *out_addr = 0 */
int pe_mod_getproc(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, int mod, const char *name, unsigned ordinal,
                   int by_ordinal, unsigned long long *out_addr);

/* DLL_PROCESS_ATTACH for every loaded, not-yet-attached module, dependencies first (idempotent: attached modules
 * are skipped).  Returns 0, or PE_MOD_E_INIT after rolling back (see header comment); needs ops->call_entry. */
int pe_mod_run_inits(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops);

/* DLL_PROCESS_DETACH for every attached module in reverse attach order (idempotent). */
int pe_mod_run_detach(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops);

/* FreeLibrary: releases one EXPLICIT load (pub_refs--).  A module with no explicit load left is refused with
 * PE_MOD_E_ARG, so a hostile or buggy extra FreeLibrary can never pull a module out from under its dependents.  When the
 * total refcount (explicit loads + dependents) reaches zero the module is detached (if attached), its dependencies are
 * released in turn, its image is handed to ops->free_image and the slot is freed.  Returns 0 or PE_MOD_E_ARG. */
int pe_mod_free(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops, int mod);

/* Detaches anything still attached (if ops->call_entry), frees every image (ops->free_image) and resets ctx. */
void pe_mod_free_all(pe_mod_ctx_t *ctx, const pe_mod_ops_t *ops);

/* Lookups.  pe_mod_find: GetModuleHandle by name (no refcount change) -> index or PE_E_NOTFOUND / PE_MOD_E_NAME.
 * pe_mod_find_base: HMODULE (== base address) -> index or PE_E_NOTFOUND.  pe_mod_get: module record or NULL.
 * pe_mod_entry_va: absolute entry VA of a loaded module (0 = none / invalid index). */
int pe_mod_find(const pe_mod_ctx_t *ctx, const char *name);
int pe_mod_find_base(const pe_mod_ctx_t *ctx, unsigned long long base);
const pe_mod_t *pe_mod_get(const pe_mod_ctx_t *ctx, int mod);
unsigned long long pe_mod_entry_va(const pe_mod_ctx_t *ctx, int mod);

#ifdef __cplusplus
}
#endif

#endif /* PE_MODULES_H */
