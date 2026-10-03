/*
 * pe_manifest.h -- bounded, non-allocating scanner for the Windows application manifest (RT_MANIFEST).
 * =====================================================================================================
 *
 * Freestanding C11 (no libc, no malloc, no globals), companion to pe.h / pe_resources.h.  Written from
 * Microsoft's public application-manifest schema documentation (assemblyIdentity, dependency /
 * dependentAssembly, trustInfo / security / requestedPrivileges / requestedExecutionLevel,
 * compatibility / application / supportedOS, windowsSettings dpiAware / dpiAwareness / longPathAware /
 * activeCodePage).  Original code.
 *
 * THIS IS NOT A GENERAL XML PARSER.  It is a scanner for exactly the XML subset real manifests use, hardened
 * for hostile input:
 *   - input: UTF-8 (optional BOM) or UTF-16LE (BOM, or an unambiguous '<' 0x00 start); UTF-16BE is refused;
 *   - handled: <?...?> declarations/PIs (skipped), comments (skipped), CDATA (skipped, content is NOT
 *     reported), elements, attributes in '...' or "...", the five predefined entities and numeric character
 *     references (decoded; anything else, e.g. a DTD-defined entity, is an error);
 *   - refused: <!DOCTYPE ...> (no DTD => no entity expansion, so no entity bombs), more than one root,
 *     content after the root (NUL padding and whitespace are tolerated), mismatched / unterminated tags;
 *   - namespaces: prefixes are tolerated and element matching is by LOCAL name only; namespace URIs are NOT
 *     resolved.  Attributes with a prefix (and xmlns declarations) are never matched.  (Consequence: an
 *     element in a foreign namespace with a manifest-looking local name is treated like the real one.  The
 *     elevation helper is conservative about duplicates: see pe_manifest_requires_elevation().)
 *   - bounded: document <= PE_MAN_MAX_DOC bytes, nesting <= PE_MAN_MAX_DEPTH (explicit stack, no
 *     recursion), <= PE_MAN_MAX_ELEMENTS elements, <= PE_MAN_MAX_ATTRS attributes per element, names <=
 *     PE_MAN_MAX_NAME characters; every extracted string is bounded and truncation is flagged.  Run time is
 *     linear in the document size.
 *
 * Policy note: this layer only REPORTS what the manifest declares.  Whether to honour requireAdministrator,
 * uiAccess, a missing supportedOS entry, a dependency we cannot satisfy, or a malformed manifest is the
 * caller's (OS loader's) decision.  A manifest with well_formed == 0 must be treated as untrusted: fields
 * extracted before the first error are kept (useful for diagnostics) but may be incomplete or misleading.
 *
 * No file I/O is ever performed; the dependency helper only builds name strings.
 */
#ifndef PE_MANIFEST_H
#define PE_MANIFEST_H

#include "pe.h"
#include "pe_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Limits. */
#define PE_MAN_MAX_DOC       65536u   /* bytes of manifest text accepted                       */
#define PE_MAN_MAX_DEPTH     32u      /* element nesting                                       */
#define PE_MAN_MAX_ELEMENTS  8192u    /* elements per document                                 */
#define PE_MAN_MAX_ATTRS     32u      /* attributes per element                                */
#define PE_MAN_MAX_NAME      128u     /* characters in an element / attribute qualified name   */
#define PE_MAN_MAX_DEPS      16u      /* dependentAssembly entries stored                      */
#define PE_MAN_MAX_OS        8u       /* supportedOS GUIDs stored                              */

/* Field sizes (bytes incl. NUL). */
#define PE_MAN_NAME_SZ  128u
#define PE_MAN_VER_SZ   32u
#define PE_MAN_TYPE_SZ  16u
#define PE_MAN_ARCH_SZ  16u
#define PE_MAN_TOKEN_SZ 24u
#define PE_MAN_LANG_SZ  24u
#define PE_MAN_GUID_SZ  40u           /* "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" is 38 chars  */

/* Error codes.  Own range -50..-59, disjoint from pe.h (-1..-12), pe_exports.h (-13..-16), pe_resources.h (-20..-25) and
 * pe_modules.h (-32..-42), so a loader may funnel all of them through one integer without ambiguity.
 * pe_manifest_strerror() describes these and every code of pe.h / pe_resources.h. */
enum {
    PE_E_MAN_SYNTAX   = -50,   /* not well-formed: bad character / tag / attribute / mismatched end tag / junk    */
    PE_E_MAN_TRUNC    = -51,   /* ends inside a construct, element left open, or no root element at all          */
    PE_E_MAN_ENCODING = -52,   /* UTF-16BE / odd-length UTF-16                                                   */
    PE_E_MAN_TOOBIG   = -53,   /* larger than PE_MAN_MAX_DOC                                                     */
    PE_E_MAN_DEPTH    = -54,   /* nesting deeper than PE_MAN_MAX_DEPTH                                           */
    PE_E_MAN_LIMIT    = -55,   /* too many elements / attributes, or a name longer than PE_MAN_MAX_NAME          */
    PE_E_MAN_ENTITY   = -56,   /* unknown or malformed entity / character reference                              */
    PE_E_MAN_DOCTYPE  = -57,   /* <!DOCTYPE> present (DTDs are refused)                                          */
    PE_E_MAN_ROOT     = -58,   /* well-formed, but the root element is not <assembly>                            */
    PE_E_MAN_UNSAFE   = -59    /* assembly name unusable as a file name (pe_manifest_dep_hint)                   */
};

const char *pe_manifest_strerror(int err);

enum { PE_MAN_ENC_UNKNOWN = 0, PE_MAN_ENC_UTF8 = 1, PE_MAN_ENC_UTF16LE = 2 };

/* requestedExecutionLevel values. */
enum {
    PE_MAN_EXEC_NONE = 0,              /* no element, or no level attribute                */
    PE_MAN_EXEC_AS_INVOKER = 1,
    PE_MAN_EXEC_REQUIRE_ADMIN = 2,     /* level="requireAdministrator"                     */
    PE_MAN_EXEC_HIGHEST = 3,           /* level="highestAvailable"                         */
    PE_MAN_EXEC_UNKNOWN = 4            /* level present but not one of the three           */
};

/* supportedOS GUIDs known from Microsoft's documentation (bits of pe_manifest_t.os_mask). */
#define PE_MAN_OS_VISTA 0x01u   /* {e2011457-1546-43c5-a5fe-008deee3d3f0}  Vista / Server 2008   */
#define PE_MAN_OS_WIN7  0x02u   /* {35138b9a-5d96-4fbd-8e2d-a2440225f93a}  7 / Server 2008 R2    */
#define PE_MAN_OS_WIN8  0x04u   /* {4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}  8 / Server 2012       */
#define PE_MAN_OS_WIN81 0x08u   /* {1f676c76-80e1-4239-95bb-83d0f6d0da78}  8.1 / Server 2012 R2  */
#define PE_MAN_OS_WIN10 0x10u   /* {8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}  10 / 11 / Server 2016+*/

/* One assemblyIdentity (the application's own, or a dependentAssembly's).  Strings are ASCII (non-ASCII
 * characters become '?') and always NUL-terminated; an absent attribute is "". */
typedef struct {
    char name[PE_MAN_NAME_SZ];
    char version[PE_MAN_VER_SZ];
    char type[PE_MAN_TYPE_SZ];
    char processor_architecture[PE_MAN_ARCH_SZ];
    char public_key_token[PE_MAN_TOKEN_SZ];
    char language[PE_MAN_LANG_SZ];
    unsigned short ver[4];              /* version parsed as a.b.c.d (each 0..65535) when ver_valid          */
    unsigned char ver_valid;
    unsigned char token_valid;          /* exactly 16 hex digits                                              */
    unsigned char truncated;            /* some attribute was longer than its field and was cut               */
    unsigned char reserved;
} pe_man_assembly_t;

typedef struct {
    int      well_formed;               /* the document scanned to its end with no syntax error              */
    int      error;                     /* 0, or the PE_E_MAN_* code (== the function's return value)        */
    unsigned error_pos;                 /* BYTE offset in the input of the first problem                      */
    int      encoding;                  /* PE_MAN_ENC_*                                                       */
    unsigned n_elements, n_attributes, max_depth;
    int      is_assembly;               /* root element's local name is "assembly"                           */

    int has_identity;
    pe_man_assembly_t identity;         /* <assembly>/<assemblyIdentity>: first one wins                      */

    unsigned n_deps;                    /* entries stored in deps[] (<= PE_MAN_MAX_DEPS)                      */
    unsigned n_deps_total;              /* dependentAssembly identities seen (may exceed n_deps)              */
    pe_man_assembly_t deps[PE_MAN_MAX_DEPS];

    int      has_exec_level;            /* at least one requestedExecutionLevel element                      */
    int      exec_level;                /* PE_MAN_EXEC_*: FIRST occurrence                                    */
    unsigned exec_levels_seen;          /* bit (1u << PE_MAN_EXEC_x) for EVERY occurrence                    */
    unsigned n_exec_level;              /* number of such elements                                            */
    int      has_ui_access;
    int      ui_access;                 /* 1 if ANY occurrence says uiAccess="true"                           */

    unsigned n_os;                      /* GUIDs stored in os_guid[]                                          */
    unsigned n_os_total;                /* supportedOS elements seen                                          */
    char     os_guid[PE_MAN_MAX_OS][PE_MAN_GUID_SZ];   /* as written, lowercased, bounded                    */
    unsigned os_mask;                   /* PE_MAN_OS_* among ALL supportedOS elements seen                    */

    int  has_dpi_aware, has_dpi_awareness, has_long_path_aware, has_active_code_page;
    char dpi_aware[32];                 /* element text, trimmed: "true", "true/pm", "per monitor", ...       */
    char dpi_awareness[64];             /* "permonitorv2, permonitor"                                         */
    char long_path_aware[16];
    char active_code_page[32];          /* "UTF-8", "Legacy", ...                                             */
    unsigned truncated;                 /* nonzero: some string above was cut                                  */
} pe_manifest_t;

/* Scan `len` bytes of manifest text.  `out` is zeroed first and filled as far as the scan got.  Returns 0 if the
 * document is well-formed and its root is <assembly>; otherwise the PE_E_MAN_* code (also in out->error).
 * out->well_formed is 1 even for PE_E_MAN_ROOT (it is a syntax statement, not a schema statement). */
int pe_manifest_parse(const unsigned char *text, unsigned long len, pe_manifest_t *out);

/* Find the embedded manifest (RT_MANIFEST id 1 for an EXE, id 2 for a DLL, then 3, then any) in a mapped image
 * and pe_manifest_parse it.  Returns 1 (PE_RES_NOTFOUND) when the image has no manifest resource, a resource
 * error code, or the pe_manifest_parse result.  `out` is always initialised. */
int pe_manifest_from_image(const pe_info_t *in, const unsigned char *image, pe_manifest_t *out);

/* Elevation the manifest asks for (regardless of well_formed; duplicates are resolved conservatively: if ANY
 * requestedExecutionLevel element asked for administrator rights the answer says so). */
enum { PE_MAN_ELEV_NONE = 0, PE_MAN_ELEV_REQUIRED = 1 /* requireAdministrator */, PE_MAN_ELEV_IF_AVAILABLE = 2 /* highestAvailable */ };
int pe_manifest_requires_elevation(const pe_manifest_t *m);

/* DPI mode the manifest declares (dpiAwareness takes precedence over dpiAware; first recognised token wins). */
enum { PE_MAN_DPI_UNSPECIFIED = 0, PE_MAN_DPI_UNAWARE = 1, PE_MAN_DPI_SYSTEM = 2, PE_MAN_DPI_PER_MONITOR = 3,
       PE_MAN_DPI_PER_MONITOR_V2 = 4 };
int pe_manifest_dpi_mode(const pe_manifest_t *m);

/* ------------------------------------------------------------------------------------------------ */
/* dependentAssembly -> DLL / side-by-side search hint (pure string building, NO file I/O)           */
/* ------------------------------------------------------------------------------------------------ */

enum {
    PE_DEP_PRIVATE = 0,        /* no public key token: a private (application-local) assembly            */
    PE_DEP_WINSXS = 1,         /* has a public key token: normally installed side by side (WinSxS)       */
    PE_DEP_KNOWN_SYSTEM = 2    /* a well-known Microsoft assembly we know the implementing DLL of        */
};

#define PE_DEP_SXS_SZ   192u
#define PE_DEP_PATH_SZ  (PE_MAN_NAME_SZ * 2u + 24u)

typedef struct {
    int  kind;                              /* PE_DEP_*                                                      */
    int  arch_compatible;                   /* processorArchitecture is "*", "" or "amd64"                   */
    char sxs_prefix[PE_DEP_SXS_SZ];         /* "<arch>_<name>_<token>_" lowercased: the leading part of a WinSxS
                                             * directory name "<arch>_<name>_<token>_<version>_<culture>_<hash>";
                                             * "" when there is no usable token                              */
    char sxs_version[PE_MAN_VER_SZ];        /* requested version, e.g. "6.0.0.0"                             */
    char system_dll[32];                    /* DLL implementing a known system assembly, e.g. "comctl32.dll"  */
    /* Application-local locations of the assembly (Microsoft's documented private-assembly probing order),
     * relative to the application directory, '\\'-separated, never absolute, never containing "..". */
    char dll_candidate[2][PE_DEP_PATH_SZ];       /* "<name>.dll", "<name>\<name>.dll"                        */
    char manifest_candidate[2][PE_DEP_PATH_SZ];  /* "<name>.manifest", "<name>\<name>.manifest"              */
} pe_dep_hint_t;

/* Returns 0, or PE_E_MAN_UNSAFE if dep->name is empty, too long, or contains anything but [A-Za-z0-9._-] or a
 * ".." sequence (path traversal); `out` is always zeroed first and left empty on error.  Reserved Windows
 * device names (CON, NUL, ...) are not special-cased here: the file-system layer must handle them. */
int pe_manifest_dep_hint(const pe_man_assembly_t *dep, pe_dep_hint_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PE_MANIFEST_H */
