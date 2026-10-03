/*
 * pe.h -- hostile-input-safe Windows PE32+ (x86-64) parser / mapper / relocator / import resolver.
 * =============================================================================================
 *
 * Freestanding C11: no libc, no malloc, no standard headers, no globals.  Everything operates on
 * caller-provided buffers.  The same pe.c also builds hosted (tests/win/run_pe_tests.sh runs it under
 * AddressSanitizer + UBSan against a corpus of hand-built malicious images and a mutation fuzzer).
 *
 * Written from the public Microsoft "PE Format" specification.  Original code.
 *
 * Typical loader flow:
 *
 *     pe_info_t info;
 *     int r = pe_parse(file, len, &info);                       // validate headers/sections/dir RVAs
 *     unsigned char *img = <alloc + ZERO info.size_of_image bytes>;
 *     r = pe_map(file, len, &info, img);                        // headers + sections -> RVAs
 *     r = pe_relocate(&info, img, actual_base);                 // only needed when actual_base != image_base
 *     r = pe_resolve_imports(&info, img, my_resolver, ctx, &n, &unres);
 *     r = pe_tls_info(&info, img, actual_base, &s, &e, &idx, &cb);   // AFTER pe_relocate
 *
 * Error-code policy (stable; see README.md):
 *   PE_E_TRUNC        the file is too short for a structure/section the headers promise
 *   PE_E_MAGIC        bad 'MZ' / 'PE\0\0' / unknown optional-header magic
 *   PE_E_NOT_PE32PLUS optional-header magic 0x10b (32-bit PE)
 *   PE_E_MACHINE      Machine != IMAGE_FILE_MACHINE_AMD64 (ARM64, i386, ...)
 *   PE_E_BOUNDS       header field / directory range / entry point / TLS VA outside the image
 *   PE_E_SECTIONS     bad section count, table does not fit headers, extent past SizeOfImage, overlap
 *   PE_E_SIZE         SizeOfImage / SizeOfHeaders out of range
 *   PE_E_ALIGN        FileAlignment / SectionAlignment / ImageBase / section RVA / raw offset misaligned
 *   PE_E_RELOC        malformed base-relocation table, or a rebase is needed but there is no table
 *   PE_E_IMPORT       malformed import table (bad RVA, unterminated string/chain, cap exceeded)
 *   PE_E_ORDINAL      ordinal thunk with reserved bits set
 *   PE_E_UNSUPPORTED  managed (.NET / CLR) image
 */
#ifndef PE_H
#define PE_H

#ifdef __cplusplus
extern "C" {
#endif

#define PE_MAX_IMAGE   (64u*1024u*1024u)   /* SizeOfImage cap */
#define PE_MAX_SECTIONS 96

/* Walk caps (denial-of-service / infinite-chain guards). */
#define PE_MAX_IMPORT_DESCRIPTORS 4096u    /* real descriptors, excluding the null terminator   */
#define PE_MAX_THUNKS             65536u   /* thunks per descriptor, excluding the terminator   */
#define PE_MAX_RELOCS             1048576u /* relocation entries (incl. ABSOLUTE padding), total */
#define PE_MAX_DLL_NAME           256u     /* bytes incl. NUL; longer DLL names are rejected    */
#define PE_MAX_FUNC_NAME          1024u    /* bytes incl. NUL; longer import names are rejected */
#define PE_MAX_TLS_CALLBACKS      64u      /* callbacks validated per TLS directory             */

typedef struct {
    unsigned long long image_base;      /* preferred base */
    unsigned int  size_of_image, size_of_headers, entry_rva, section_alignment, file_alignment;
    unsigned short subsystem;           /* 3 = console, 2 = GUI */
    unsigned short dll_characteristics;
    unsigned short n_sections;
    /* name is sanitised (non-printable bytes -> '?') and always NUL-terminated.
     * vsize is the EFFECTIVE virtual size: the header's VirtualSize, or SizeOfRawData when VirtualSize==0. */
    struct { char name[9]; unsigned int vsize, vrva, raw_size, raw_off, flags; } sec[PE_MAX_SECTIONS];
    unsigned int import_rva, import_size, reloc_rva, reloc_size, tls_rva, tls_size, export_rva, export_size, delay_import_rva;
    unsigned int is_dll;                /* IMAGE_FILE_DLL */
    unsigned long long stack_reserve, stack_commit;
} pe_info_t;

enum { PE_OK=0, PE_E_TRUNC=-1, PE_E_MAGIC=-2, PE_E_NOT_PE32PLUS=-3, PE_E_MACHINE=-4, PE_E_BOUNDS=-5, PE_E_SECTIONS=-6,
       PE_E_SIZE=-7, PE_E_ALIGN=-8, PE_E_RELOC=-9, PE_E_IMPORT=-10, PE_E_ORDINAL=-11, PE_E_UNSUPPORTED=-12 };

/* Static, never-NULL description of an error code (also valid for PE_OK and unknown codes). */
const char *pe_strerror(int err);

/* Full validation of headers + section table + directory RVA ranges.  `out` is zeroed on failure. */
int pe_parse(const unsigned char *file, unsigned long len, pe_info_t *out);

/* image = caller buffer of in->size_of_image bytes, ZEROED by caller.  Copies headers + each section's raw
 * data to its RVA.  All checks run BEFORE the first byte is written, so a failing call leaves `image` untouched. */
int pe_map(const unsigned char *file, unsigned long len, const pe_info_t *in, unsigned char *image);

/* Applies base relocations (ABSOLUTE, DIR64, HIGHLOW, HIGH, LOW) for delta = actual_base - in->image_base.
 * The whole table is validated before any byte is patched.  A nonzero delta with NO reloc dir => PE_E_RELOC. */
int pe_relocate(const pe_info_t *in, unsigned char *image, unsigned long long actual_base);

/* dll / name are ALWAYS valid NUL-terminated strings (private copies, never NULL; name == "" for ordinal imports).
 * For by-name imports `ordinal` carries the hint.  Return 0 and set *out_addr on success. */
typedef int (*pe_resolve_fn)(const char *dll, const char *name, unsigned short ordinal, int by_ordinal, unsigned long long *out_addr, void *user);

/* Walks import descriptors (OriginalFirstThunk if nonzero, else FirstThunk) and writes each resolved address
 * into the FirstThunk slot.  fn != 0 => slot left pointing at *out_addr if fn set it (else zeroed), counted
 * unresolved; the walk continues.  n_total/n_unresolved may be NULL; they reflect progress even on error.
 * fn == NULL is a validate-and-count dry run (nothing written).  Delay imports are NOT processed. */
int pe_resolve_imports(const pe_info_t *in, unsigned char *image, pe_resolve_fn fn, void *user, unsigned int *n_total, unsigned int *n_unresolved);

/* Returns PE_OK and fills the (nullable) outputs with ACTUAL virtual addresses, or 1 if the image has no TLS
 * directory, or a PE_E_* error.  Call AFTER pe_relocate(actual_base): the VAs stored in the TLS directory must
 * already point into [actual_base, actual_base + size_of_image).  Callbacks are validated, never executed. */
int pe_tls_info(const pe_info_t *in, const unsigned char *image, unsigned long long actual_base, unsigned long long *start, unsigned long long *end, unsigned long long *index_addr, unsigned long long *callbacks_va);

#ifdef __cplusplus
}
#endif

#endif /* PE_H */
