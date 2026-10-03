/*
 * pe_resources.h -- hostile-input-safe PE resource directory walker + VS_VERSIONINFO reader.
 * =========================================================================================
 *
 * Freestanding C11 (no libc, no malloc, no globals), companion to pe.h.  Written from Microsoft's public PE
 * format documentation ("The .rsrc Section": IMAGE_RESOURCE_DIRECTORY / _ENTRY / _DIR_STRING_U /
 * _DATA_ENTRY) and the public VS_VERSIONINFO / VS_FIXEDFILEINFO documentation.  Original code.
 *
 * Input contract: `in` comes from pe_parse(), `image` is the MAPPED image from pe_map() (size_of_image bytes,
 * headers + sections at their RVAs).  Nothing is ever read outside [image, image + in->size_of_image); every
 * RVA, offset and length found in the (attacker controlled) resource tree is range-checked first, every
 * walk is capped, and every returned pointer/size is inside the image.  Returned `data` pointers point INTO
 * `image` (no copies); they stay valid as long as the caller keeps the image buffer.
 *
 * The resource data directory (IMAGE_DIRECTORY_ENTRY_RESOURCE, index 2) is re-read from the mapped image's own
 * headers (pe_info_t does not record it), and re-validated against in->size_of_image on every call.
 *
 * Walk model: a 3-level tree -- type -> name/id -> language -> IMAGE_RESOURCE_DATA_ENTRY.  Hard caps:
 *   - directory depth is exactly 3 (a directory entry at the language level is PE_E_RES_DEPTH),
 *   - PE_RES_MAX_ENTRIES entries in one directory table, PE_RES_MAX_NODES entries across one whole call,
 *   - a child directory may not equal or overlap one of its ancestors (PE_E_RES_LOOP),
 *   - resource names are compared (never copied) with the key, or copied bounded to PE_RES_NAME_MAX.
 *
 * Return-code policy (own range -20..-29, plus the pe.h codes; use pe_res_strerror() for any of them):
 *    0                 found / ok
 *    PE_RES_NOTFOUND   (1) no resource directory, or no such type / name / language
 *    PE_E_RES_BOUNDS   a table, string, data entry or data block lies outside the resource tree / the image
 *    PE_E_RES_FORMAT   structurally malformed (a leaf where a directory belongs, bad header, ...)
 *    PE_E_RES_LOOP     a child directory equals/overlaps an ancestor
 *    PE_E_RES_DEPTH    the tree is deeper than 3 levels
 *    PE_E_RES_LIMIT    PE_RES_MAX_ENTRIES / PE_RES_MAX_NODES / key-length cap exceeded
 *    PE_E_VERSION      malformed VS_VERSIONINFO
 *  plus PE_E_BOUNDS / PE_E_SIZE / PE_E_MAGIC from pe.h for a corrupted pe_info_t / image header.
 */
#ifndef PE_RESOURCES_H
#define PE_RESOURCES_H

#include "pe.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard resource types (winuser.h RT_*). */
#define PE_RT_CURSOR       1u
#define PE_RT_BITMAP       2u
#define PE_RT_ICON         3u
#define PE_RT_MENU         4u
#define PE_RT_DIALOG       5u
#define PE_RT_STRING       6u
#define PE_RT_RCDATA       10u
#define PE_RT_MESSAGETABLE 11u
#define PE_RT_GROUP_CURSOR 12u
#define PE_RT_GROUP_ICON   14u
#define PE_RT_VERSION      16u
#define PE_RT_HTML         23u
#define PE_RT_MANIFEST     24u

/* Manifest resource ids (winuser.h / Microsoft docs). */
#define PE_MANIFEST_ID_EXE      1u   /* CREATEPROCESS_MANIFEST_RESOURCE_ID */
#define PE_MANIFEST_ID_DLL      2u   /* ISOLATIONAWARE_MANIFEST_RESOURCE_ID */
#define PE_MANIFEST_ID_DLL_NOSI 3u   /* ISOLATIONAWARE_NOSTATICIMPORT_MANIFEST_RESOURCE_ID */

/* "any": take the first entry of the level / any language. */
#define PE_RES_ID_ANY   0xFFFFFFFFu
#define PE_RES_LANG_ANY 0xFFFFFFFFu

/* Caps. */
#define PE_RES_MAX_ENTRIES 32768u    /* entries in one directory table (named + id)                    */
#define PE_RES_MAX_NODES   131072u   /* directory entries examined across one find / enum call          */
#define PE_RES_NAME_MAX    64u       /* bytes incl. NUL of an ASCII-rendered resource name              */
#define PE_RES_KEY_MAX     255u      /* longest string key accepted by pe_res_find_ex                   */

#define PE_RES_NOTFOUND 1

enum {
    PE_E_RES_BOUNDS = -20,
    PE_E_RES_FORMAT = -21,
    PE_E_RES_LOOP   = -22,
    PE_E_RES_DEPTH  = -23,
    PE_E_RES_LIMIT  = -24,
    PE_E_VERSION    = -25
};

/* Static, never-NULL description of any code above, any pe.h code, or PE_RES_NOTFOUND. */
const char *pe_res_strerror(int err);

/* A lookup key for one level: numeric id, or ASCII name (case-insensitive compare against the UTF-16 name),
 * or "any".  Build with the helpers below.  The name pointer is only read during the call. */
typedef struct {
    const char *name;   /* non-NULL => match a string-named entry                        */
    unsigned    id;     /* used when name == NULL; PE_RES_ID_ANY => first entry           */
} pe_res_key_t;

static inline pe_res_key_t pe_res_key_id(unsigned id)        { pe_res_key_t k; k.name = 0; k.id = id; return k; }
static inline pe_res_key_t pe_res_key_name(const char *name) { pe_res_key_t k; k.name = name; k.id = 0; return k; }
static inline pe_res_key_t pe_res_key_any(void)              { pe_res_key_t k; k.name = 0; k.id = PE_RES_ID_ANY; return k; }

/* Identity of one tree node, as found in the image. */
typedef struct {
    unsigned is_named;                  /* 1: str/name_len valid; 0: id valid                          */
    unsigned id;
    unsigned name_len;                  /* TRUE length in UTF-16 units (may exceed what str holds)      */
    char     str[PE_RES_NAME_MAX];      /* ASCII rendering: non-printable/non-ASCII -> '?', NUL-terminated */
} pe_res_id_t;

/* One resource (a leaf: type + name + language). */
typedef struct {
    pe_res_id_t type, name;
    unsigned lang;                      /* LANGID of the leaf; a string-named language entry reports 0   */
    unsigned codepage;
    unsigned rva;                       /* RVA of the data, inside the image                             */
    unsigned size;                      /* bytes, rva + size <= size_of_image                            */
    const unsigned char *data;          /* image + rva                                                   */
} pe_res_entry_t;

/* RVA/size of the resource data directory read from the mapped image's headers.
 * Returns 0 and fills rva and size, 1 if the image has no resource directory, or a negative error. */
int pe_res_dir(const pe_info_t *in, const unsigned char *image, unsigned *rva, unsigned *size);

/* Core lookup.  `type` / `name` are keys (NULL == any).  Language choice among the leaves of the chosen name:
 *   exact lang_pref > same primary language (lang & 0x3ff) > neutral (0) > en-US (0x0409) > first entry;
 *   lang_pref == PE_RES_LANG_ANY takes the first entry.  Ties go to the earlier entry.
 * `out` (nullable) receives the full description.  Returns 0 or PE_RES_NOTFOUND or a negative error. */
int pe_res_find_ex(const pe_info_t *in, const unsigned char *image, const pe_res_key_t *type,
                   const pe_res_key_t *name, unsigned lang_pref, pe_res_entry_t *out);

/* Convenience: numeric type and numeric id (PE_RES_ID_ANY = first).  On success *data / *size (both nullable)
 * describe the resource, inside the image.  Same return codes. */
int pe_res_find(const pe_info_t *in, const unsigned char *image, unsigned type, unsigned id,
                unsigned lang_pref, const unsigned char **data, unsigned *size);

/* Enumerator: calls fn for every LEAF (type/name/language) of `type` (NULL or pe_res_key_any() => every type),
 * in table order.  fn returns 0 to continue, nonzero to stop early (the call then returns 0).  The whole path
 * to each leaf is validated; on a negative return, leaves already reported were valid but the walk is
 * incomplete.  *n_visited (nullable) = number of fn calls.  Returns 0, PE_RES_NOTFOUND (no directory), or error. */
typedef int (*pe_res_enum_fn)(const pe_res_entry_t *e, void *user);
int pe_res_enum(const pe_info_t *in, const unsigned char *image, const pe_res_key_t *type,
                pe_res_enum_fn fn, void *user, unsigned *n_visited);

/* ------------------------------------------------------------------------------------------------ */
/* VS_VERSIONINFO                                                                                    */
/* ------------------------------------------------------------------------------------------------ */

#define PE_VER_STR_MAX  128u        /* bytes incl. NUL of each extracted string */
#define PE_VER_MAX_NODES 2048u      /* version-tree nodes visited per parse      */

/* bits of pe_version_t.truncated / .have_str */
#define PE_VER_S_DESCRIPTION 0x01u
#define PE_VER_S_PRODUCT     0x02u
#define PE_VER_S_COMPANY     0x04u
#define PE_VER_S_ORIGINAL    0x08u
#define PE_VER_S_FILEVER     0x10u
#define PE_VER_S_PRODVER     0x20u

typedef struct {
    int has_fixed;                          /* VS_FIXEDFILEINFO present with signature 0xFEEF04BD            */
    unsigned file_version_ms, file_version_ls, product_version_ms, product_version_ls;
    unsigned short file_version[4];         /* major, minor, build, revision (from the fixed block)           */
    unsigned short product_version[4];
    unsigned struct_version, file_flags_mask, file_flags, file_os, file_type, file_subtype;
    int has_strings;                        /* a StringTable was found and read                                */
    unsigned lang, codepage;                /* of that table (from its 8-hex-digit key); 0xFFFFFFFF if unparsable */
    unsigned have_str;                      /* PE_VER_S_* bits: string present (even if empty)                */
    unsigned truncated;                     /* PE_VER_S_* bits: string longer than PE_VER_STR_MAX-1, cut      */
    char file_description[PE_VER_STR_MAX];
    char product_name[PE_VER_STR_MAX];
    char company_name[PE_VER_STR_MAX];
    char original_filename[PE_VER_STR_MAX];
    char file_version_str[PE_VER_STR_MAX];
    char product_version_str[PE_VER_STR_MAX];
} pe_version_t;

/* Parse a raw RT_VERSION blob (exactly `size` readable bytes).  `lang_pref` picks the StringTable (exact
 * LANGID, else same primary language, else the first VarFileInfo\Translation, else the first table);
 * PE_RES_LANG_ANY = Translation, else first.  `out` is zeroed first.  Returns 0 or PE_E_VERSION; on
 * error `out` holds whatever was extracted before the problem (has_fixed / has_strings tell what). */
int pe_version_parse(const unsigned char *blob, unsigned size, unsigned lang_pref, pe_version_t *out);

/* pe_res_find(RT_VERSION) + pe_version_parse.  Returns 1 (PE_RES_NOTFOUND) when the image has no version resource. */
int pe_version_info(const pe_info_t *in, const unsigned char *image, unsigned lang_pref, pe_version_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PE_RESOURCES_H */
