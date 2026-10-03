/*
 * pe_exports.h -- hostile-input-safe PE export-directory lookup (+ delay-load import binding).
 * ==========================================================================================
 *
 * Companion to pe.h.  Freestanding C11: no libc, no malloc, no globals, no standard headers.  Operates on a
 * pe_info_t produced by pe_parse() and the IMAGE that pe_map() filled (RVA-indexed, size_of_image bytes); it
 * never touches the original file.  Written from the public Microsoft "PE Format" specification (".edata:
 * export directory table, export address table, name pointer table, ordinal table, forwarder RVAs" and
 * ".didat: delay-load directory").  Original code.
 *
 * Export data model (all RVAs are image-relative):
 *   IMAGE_EXPORT_DIRECTORY (40 bytes at export_rva):  +16 Base   +20 NumberOfFunctions   +24 NumberOfNames
 *                                                      +28 AddressOfFunctions (EAT, u32[nfuncs])
 *                                                      +32 AddressOfNames     (u32[nnames], RVAs of NUL-terminated names)
 *                                                      +36 AddressOfNameOrdinals (u16[nnames], UNBIASED EAT index)
 *   public ordinal = Base + EAT index.  An EAT entry of 0 is an unused slot.  An EAT entry that lies INSIDE
 *   [export_rva, export_rva + export_size) is not code: it is the RVA of a forwarder string "DLL.Func" or
 *   "DLL.#ordinal" (the DLL part has no extension).
 *
 * Safety rules (enforced and tested; see tests/win/pe_dll_test.c and pe_dll_fuzz.c):
 *   - the directory, the three tables, every name, every forwarder string and every EAT target are bounds-checked
 *     against size_of_image; all RVA arithmetic is 64-bit with off <= limit && n <= limit - off;
 *   - NumberOfFunctions / NumberOfNames <= PE_MAX_EXPORTS; every name must NUL-terminate within PE_MAX_EXPORT_NAME
 *     bytes (incl. NUL) inside the image; forwarder strings within PE_MAX_FWD bytes (incl. NUL);
 *   - name lookup is a bounded LINEAR scan (the name table is NOT assumed sorted) and an EXACT byte comparison:
 *     case-sensitive, never truncating (a name longer than the cap simply cannot match anything);
 *   - a forwarder string that does not fit the caller's buffer is an error (PE_E_BUFSIZE), never truncated.
 *   - nothing is ever written to the image by the export functions (pe_delay_imports patches the delay IAT only).
 *
 * Error codes extend pe.h's enum (-1..-12) with -13..-16; use pe_export_strerror() for any code.
 */
#ifndef PE_EXPORTS_H
#define PE_EXPORTS_H

#include "pe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PE_MAX_EXPORTS      65536u               /* NumberOfFunctions / NumberOfNames cap                 */
#define PE_MAX_EXPORT_NAME  PE_MAX_FUNC_NAME     /* bytes incl. NUL (== the longest import name pe.c accepts) */
#define PE_MAX_FWD          256u                 /* forwarder string bytes incl. NUL                      */

enum {
    PE_E_EXPORT   = -13,   /* malformed export directory / table / name / EAT entry                          */
    PE_E_NOTFOUND = -14,   /* no such export (also: image has no export directory)                           */
    PE_E_BUFSIZE  = -15,   /* caller buffer too small (name / forwarder string); never truncated             */
    PE_E_FWD      = -16    /* malformed forwarder string (no '.', empty part, junk, bad #ordinal)            */
};

/* pe_export_find() returns this (nonzero, positive) for a forwarder; PE_OK (0) for a normal export. */
#define PE_EXPORT_FORWARDER 1

/* Static, never-NULL description of any pe.h / pe_exports.h code (also valid for unknown codes). */
const char *pe_export_strerror(int err);

/* Full validation of the export directory and EVERY name / ordinal / EAT entry.
 * Returns PE_OK, 1 = image has no export directory, or PE_E_EXPORT (or PE_E_BOUNDS/PE_E_SIZE for a bad pe_info_t).
 * Cost O(nnames * name length + nfuncs) -- call once per module (the loader does it at load time). */
int pe_export_validate(const pe_info_t *in, const unsigned char *image);

/* Counts.  Returns PE_OK (outputs filled, NULL allowed), 1 = no export directory (outputs zeroed), or an error. */
int pe_export_count(const pe_info_t *in, const unsigned char *image, unsigned int *n_funcs, unsigned int *n_names);

/* The core lookup (GetProcAddress semantics for one module, forwarders reported, not followed).
 *
 *   by_ordinal == 0:  `name` (NUL-terminated, exact match) is looked up in the name table; `ordinal` is ignored.
 *   by_ordinal != 0:  `ordinal` is the PUBLIC ordinal (>= Base); `name` is ignored (may be NULL).
 *
 * On success *out_rva (nullable) = the EAT value: the function's RVA (return 0) or the forwarder string's RVA
 * (return PE_EXPORT_FORWARDER, and the string is copied NUL-terminated into fwd[0..fwd_cap) if fwd != NULL; a
 * string that does not fit gives PE_E_BUFSIZE).  Unused slots, unknown names / ordinals and an image with no
 * export directory give PE_E_NOTFOUND; malformed tables give PE_E_EXPORT.  Validation is LAZY here (only the
 * entries the lookup touches); run pe_export_validate() once for a whole-table verdict. */
int pe_export_find(const pe_info_t *in, const unsigned char *image, const char *name, unsigned int ordinal,
                   int by_ordinal, unsigned int *out_rva, char *fwd, unsigned int fwd_cap);

/* Enumeration helpers.
 *  pe_export_at(): EAT slot `index` (0 <= index < nfuncs).  *ordinal = Base + index; *rva = EAT value (0 = unused);
 *                  *is_forwarder = 1 when the value points at a forwarder string.  Any output may be NULL.
 *  pe_export_name_at(): entry `index` of the NAME table: copies the name (bounded; PE_E_BUFSIZE if it does not fit)
 *                  and reports its public ordinal and EAT value.  PE_E_NOTFOUND if index >= nnames. */
int pe_export_at(const pe_info_t *in, const unsigned char *image, unsigned int index, unsigned int *ordinal,
                 unsigned int *rva, int *is_forwarder);
int pe_export_name_at(const pe_info_t *in, const unsigned char *image, unsigned int index, char *name,
                      unsigned int name_cap, unsigned int *ordinal, unsigned int *rva);

/* Splits a forwarder string at its LAST '.' (function names contain no '.', DLL names may):
 *   "NTDLL.RtlFoo"  -> dll "NTDLL" (no extension), func "RtlFoo", *by_ordinal = 0
 *   "DLL.#12"       -> dll "DLL",  func "",        *by_ordinal = 1, *ordinal = 12   (1..65535)
 * Every output pointer may be NULL.  Only printable ASCII (0x20..0x7e) is accepted.
 * Returns PE_OK, PE_E_FWD (malformed) or PE_E_BUFSIZE (an output buffer is too small -- never truncated). */
int pe_export_parse_forwarder(const char *fwd, char *dll, unsigned int dll_cap, char *func, unsigned int func_cap,
                              unsigned int *ordinal, int *by_ordinal);

/* ---- delay-load imports (ImgDelayDescr, RVA form) ---------------------------------------------------------
 * EAGER binding: walks the delay-import directory (in->delay_import_rva; the 32-byte descriptor array is zero-
 * terminated, <= PE_MAX_IMPORT_DESCRIPTORS descriptors, <= PE_MAX_THUNKS thunks each), asks fn for every imported
 * name / ordinal exactly like pe_resolve_imports, and writes the resolved address into the delay IAT slot
 * (descriptor +12 rvaIAT).  fn returning nonzero leaves the slot UNTOUCHED (it still points at the compiler's
 * delay-load thunk, so a program that carries its own delay helper keeps working) and counts as unresolved.
 * Returns PE_OK (also when there is no delay directory), PE_E_IMPORT / PE_E_ORDINAL on malformed tables
 * (grAttrs bit 0 clear = old VA-form descriptors are not supported -> PE_E_IMPORT).  fn == NULL = validate+count only.
 * n_total / n_unresolved are nullable and reflect progress even on error. */
int pe_delay_imports(const pe_info_t *in, unsigned char *image, pe_resolve_fn fn, void *user,
                     unsigned int *n_total, unsigned int *n_unresolved);

#ifdef __cplusplus
}
#endif

#endif /* PE_EXPORTS_H */
