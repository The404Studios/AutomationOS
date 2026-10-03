# userspace/lib/pe -- hostile-input-safe PE32+ (x86-64) library

Parses, maps, relocates and import-resolves Windows PE32+ images for the hobby-OS Windows loader. Freestanding C11
(no libc, no malloc, no globals, no standard headers); the same `pe.c` builds hosted for the ASan/UBSan tests.
It processes attacker-controlled files, so every operation is bounds-checked and every loop is capped. Original code
written from the public Microsoft "PE Format" specification.

## API (`pe.h`)
| call | purpose |
|---|---|
| `pe_parse(file,len,&info)` | validate DOS/NT headers, section table, directory RVAs; `info` is zeroed on failure |
| `pe_map(file,len,&info,image)` | `image` = caller buffer of `size_of_image` bytes, ZEROED; copies headers + sections to their RVAs (all checks before the first write) |
| `pe_relocate(&info,image,base)` | applies ABSOLUTE/DIR64/HIGHLOW/HIGH/LOW for `delta = base - image_base`; whole table validated before any patch |
| `pe_resolve_imports(&info,image,fn,user,&n,&unres)` | walks descriptors, calls `fn` per thunk, writes the IAT (`FirstThunk`) |
| `pe_tls_info(&info,image,base,...)` | returns TLS start/end/index/callback VAs (0), 1 = no TLS, or an error; nothing is executed |
| `pe_strerror(code)` | static description of any code |

Flow: parse -> allocate+zero -> map -> relocate (if `base != image_base`) -> resolve imports -> `pe_tls_info` (AFTER relocate).

## Behaviour the loader must know
- **Alignment policy**: FileAlignment pow2 in [0x200,0x10000]; SectionAlignment pow2 in [0x1000,0x10000] and >= FileAlignment
  (sub-page "old style" images are rejected, PE_E_ALIGN); ImageBase multiple of 64 KiB; section RVAs multiples of SectionAlignment.
- **Sections**: 1..96; page-aligned extents must be disjoint and inside SizeOfImage; out-of-order tables are fine if disjoint.
  `sec[].vsize` is the EFFECTIVE size (VirtualSize, or SizeOfRawData when VirtualSize==0). `pe_map` copies `min(raw_size, vsize)`.
- **Directories**: absent = RVA and Size both 0; otherwise both nonzero and inside the image (else PE_E_BOUNDS). Only export,
  import, reloc, TLS, delay-import are recorded; CLR (dir 14) nonzero => PE_E_UNSUPPORTED.
- **Relocation**: a nonzero delta with no reloc dir => PE_E_RELOC (e.g. the shipped `nocrt_hello.exe` has no `.reloc`, so it can
  only load at 0x140000000). Validation runs even when delta == 0. A block must be >= 8 bytes, a multiple of 4 and inside the
  directory; unknown types (anything but 0,1,2,3,10), targets outside the image and targets overlapping the table are PE_E_RELOC.
  On error the image content is unspecified; discard it.
- **Imports**: `dll`/`name` handed to `fn` are private NUL-terminated copies (never NULL; `name==""` for ordinals; for by-name
  imports `ordinal` carries the hint). Unresolved slot = `*out_addr` if `fn` set it, else 0. The OFT/IAT is bounded by the image,
  not the directory Size. `fn==NULL` is a validate-and-count dry run.
- **TLS**: stored VAs must already be relocated (`[base, base+size_of_image)`); callback array (<= 64 + null) is validated, never run.
- **Error codes**: TRUNC = file too short (incl. e_lfanew past EOF, raw data past EOF); MAGIC = bad MZ/PE/optional magic;
  NOT_PE32PLUS = magic 0x10b; MACHINE = not AMD64 (ARM64 included); BOUNDS = a header/directory/entry/TLS value outside the
  image; SECTIONS; SIZE; ALIGN; RELOC; IMPORT; ORDINAL (reserved bits set in an ordinal thunk); UNSUPPORTED (CLR).

## Security rules (enforced and tested)
All RVA/size/offset math is widened to 64-bit and checked with `off <= limit && n <= limit - off`; byte-wise little-endian loads;
no read/write outside `[file,file+len)` or `[image,image+size_of_image)`; consumers re-validate `pe_info_t`; hard caps:
`PE_MAX_IMAGE` 64 MiB, 96 sections, 4096 import descriptors, 65536 thunks/descriptor, 2^20 relocation entries, 255-char DLL and
1023-char function names, 64 TLS callbacks. Strings must terminate inside the image.

## Tests
`wsl.exe -d Arch -- bash /mnt/c/Users/wilde/Desktop/Kernel/tests/win/run_pe_tests.sh` builds with
`-fsanitize=address,undefined -fno-sanitize-recover=all` and prints `PE-FREESTANDING-BUILD: PASS`, `PE-HOST-TEST: PASS`,
`PE-FUZZ: PASS iterations=300000 ...`. Fixtures come from `/tmp/winfix` (rebuilt with MinGW if missing). A failing fuzz input is
saved to `/tmp/pe_fuzz_crash.bin`. Freestanding gate: `-ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror`, no undefined symbols, no `fs:0x28`.

## NOT supported
SEH / `.pdata` unwind tables (ignored), delay-load imports (recorded in `delay_import_rva`, NOT processed), TLS callback
*execution* (info only), export parsing/forwarders (only `export_rva/size` recorded), resources, bound imports, load-config/CFG,
PE32 (32-bit), ARM64, .NET/CLR, certificates/Authenticode, sub-page section alignment, relocation types other than 0,1,2,3,10.

## DLL support: `pe_exports.[ch]` + `pe_modules.[ch]` (added on top of `pe.c`, which is unchanged)
Both are freestanding like `pe.c` (no libc/malloc/globals; they link with `pe.c` and need nothing else) and hostile-input safe.
This **supersedes** the two "NOT supported" lines above for *export parsing/forwarders* and *delay-load imports*.

**`pe_exports.h`** -- `pe_export_find(info,image,name,ordinal,by_ordinal,&rva,fwd,fwd_cap)` (0 = normal export, `PE_EXPORT_FORWARDER`
= 1 and the `"DLL.Func"` / `"DLL.#n"` string copied into `fwd`, negative = `PE_E_EXPORT` malformed / `PE_E_NOTFOUND` /
`PE_E_BUFSIZE`), `pe_export_validate` (whole-table check, run once at load), `pe_export_count/at/name_at` (enumeration),
`pe_export_parse_forwarder`, `pe_delay_imports` (eager delay-load binding; unresolved slots keep the compiler's delay thunk).
Name lookup is a bounded **linear** scan (the name table is not assumed sorted), exact and case-sensitive; every name, forwarder
string, table and EAT target is bounds-checked against `size_of_image`; caps: 65536 exports, 1023-char export names, 255-char
forwarder strings. New error codes -13..-16 (`pe_export_strerror`).

**`pe_modules.h`** -- the module graph, written against callbacks (`read_file`, `alloc_image`, `builtin_resolve`, `call_entry`;
optional `release_file`, `free_image`, `unresolved_stub`): `pe_mod_load_main` (exe + every DLL it needs, recursively),
`pe_mod_load` (LoadLibrary: case-insensitive, with/without `.dll`, refcounted), `pe_mod_getproc` (GetProcAddress: follows
forwarders across modules and into `builtin_resolve`; hop cap 16 + cycle detection), `pe_mod_run_inits` (DllMain
`DLL_PROCESS_ATTACH` in dependency order; a FALSE return rolls back this call's attaches in reverse), `pe_mod_run_detach`
(reverse order), `pe_mod_free` (FreeLibrary), `pe_mod_free_all`. State lives in a caller-provided `pe_mod_ctx_t` (~103 KiB:
static or mmap it). Caps: 32 modules, 63-char DLL names, nesting depth 16 (`-DPE_MOD_MAX_DEPTH=n` to lower it; ~3 KiB of stack per
level), forwarder chain 16. Builtin (system/shim) DLLs win over a file of the same name; an import that cannot be resolved is
counted (`ctx.unresolved`), named (`ctx.first_unresolved` = `"dll!name"`, reason in `ctx.first_unresolved_why`) and pointed at
`unresolved_stub`'s address -- never fatal. DLL names must be bare printable-ASCII file names (no `/ \ : * ? " < > |`);
anything else is rejected, never truncated. An image with no `.reloc` can only be loaded at exactly its preferred base
(`alloc_image` must honour the hint for those, else `PE_E_RELOC`). The exe's entry point is never run by the loader.

**Not implemented**: TLS in DLLs (a TLS directory is validated and flagged `PE_MOD_F_TLS`, never allocated or run), SEH /
`.pdata` registration, `DLL_THREAD_ATTACH/DETACH`, bound imports, API-set schema redirection (do it in `builtin_resolve`),
manifests / side-by-side search (do it in `read_file`), lazy delay-load (binding is eager; `PE_MOD_CF_NO_DELAY` disables it),
refcounted unload of import *cycles* (released by `pe_mod_free_all` only).

**Tests** (host only, `/tmp/pe_dll_dev`): `wsl.exe -d Arch -- bash /mnt/c/Users/wilde/Desktop/Kernel/tests/win/run_dll_tests.sh`
prints `PE-DLL-FIXTURES`, `PE-DLL-FREESTANDING-BUILD`, `PE-DLL-HOST-TEST: PASS checks=N` (real MinGW DLLs *executed* via ms_abi on
the host + synthetic hostile graphs, under ASan/UBSan), `PE-DLL-FUZZ: PASS iterations=200000 ...` (export directory + module
loader, with a libc differential oracle) and `PE-DLL-FUZZ-HARNESS-SELFTEST: PASS` (the fuzzer is proven to catch injected bugs).

## Resources, version info and manifests: `pe_resources.[ch]` + `pe_manifest.[ch]` (added on top of `pe.c`, which is unchanged)
Freestanding like `pe.c`, hostile-input safe, link with `pe.c` and nothing else. This **supersedes** the "resources" entry of the
"NOT supported" list. Input is `pe_info_t` from `pe_parse` plus the **mapped** image from `pe_map` (the resource data directory is
re-read from the image's own headers; `pe_info_t` does not record it). Returned pointers point into the image (no copies).

**`pe_resources.h`** -- `pe_res_find(info,image,type,id,lang_pref,&data,&size)` (0 / `PE_RES_NOTFOUND`=1 / negative), `pe_res_find_ex`
(string-named types/names via `pe_res_key_name("MYTYPE")`, `pe_res_key_any()`, full `pe_res_entry_t` result), `pe_res_enum`
(callback per type/name/language leaf), `pe_res_dir`, `pe_version_info` / `pe_version_parse` (VS_FIXEDFILEINFO numbers + FileDescription,
ProductName, CompanyName, OriginalFilename, FileVersion, ProductVersion as bounded ASCII). Language choice: exact > same primary language >
neutral > en-US > first. Walk is exactly 3 levels; caps: 32768 entries per table, 131072 entries per call, child tables may not equal or
overlap an ancestor (`PE_E_RES_LOOP`), a table where a data entry belongs is `PE_E_RES_DEPTH`; every table/string/data entry must lie in the
resource data directory and every data block in the image. Codes -20..-25 (`pe_res_strerror`).

**`pe_manifest.h`** -- `pe_manifest_from_image` (RT_MANIFEST id 1 for an exe, 2 for a DLL, then 3, then any) / `pe_manifest_parse` (any text):
a bounded, non-allocating scanner for the XML subset real manifests use (UTF-8 or UTF-16LE, comments/PIs/CDATA skipped, the 5 entities +
numeric references, prefixes tolerated, **DOCTYPE refused**, <= 64 KiB, depth <= 32, <= 8192 elements, <= 32 attributes per element, explicit
stack, no recursion). Fills `pe_manifest_t`: `identity`, up to 16 `deps[]` (name/version/type/arch/publicKeyToken/language + parsed `ver[4]`),
`exec_level` + `ui_access`, up to 8 `supportedOS` GUIDs (+ `os_mask` of the known Vista..Win10 ones), `dpi_aware` / `dpi_awareness` /
`long_path_aware` / `active_code_page` text, `well_formed` + `error` + `error_pos`. Helpers: `pe_manifest_requires_elevation` (conservative about
duplicates), `pe_manifest_dpi_mode`, `pe_manifest_dep_hint` (dependentAssembly -> WinSxS directory-name prefix, known system DLL such as
`comctl32.dll`, and application-local `<name>.dll` / `<name>\<name>.dll` / `.manifest` candidates; pure string building, **no file I/O**, names with
separators or `..` are refused). Codes -50..-59 (`pe_manifest_strerror`). **Policy is the caller's**: this layer only reports what the manifest
declares (requireAdministrator, uiAccess, missing supportedOS, unsatisfied dependency, malformed manifest, ...).

```c
pe_info_t info;  pe_manifest_t m;  pe_version_t v;
pe_parse(file, len, &info);  /* ... alloc + zero, pe_map(file, len, &info, img) ... */
if (pe_manifest_from_image(&info, img, &m) == 0 && m.well_formed) {
    if (pe_manifest_requires_elevation(&m) == PE_MAN_ELEV_REQUIRED) { /* the OS decides: refuse / prompt / run as invoker */ }
    for (i = 0; i < m.n_deps; i++) { pe_dep_hint_t h; if (!pe_manifest_dep_hint(&m.deps[i], &h)) /* probe h.dll_candidate[], h.system_dll */; }
}
if (pe_version_info(&info, img, 0x0409, &v) == 0) /* v.file_version[0..3], v.product_name */;
```

**Not parsed**: a general XML parser (no DTDs/custom entities, no namespace URI resolution -- element matching is by *local name*, so a
foreign-namespace look-alike counts; UTF-16BE/UTF-32; `<file>` / `<comClass>` / `<application>` extensions other than the ones above), external
`.manifest` files (feed the text to `pe_manifest_parse` yourself), `<isolation>` container manifests (well-formed, reported as `PE_E_MAN_ROOT`),
icons / bitmaps / dialogs / string tables (reachable with `pe_res_find`, not decoded), 32-bit and managed images (`pe_parse` refuses them first).

**Tests** (host only): `wsl.exe -d Arch -- bash /mnt/c/Users/wilde/Desktop/Kernel/tests/win/run_res_tests.sh` prints `RES-FREESTANDING-BUILD: PASS`,
`RES-HOST-TEST: PASS checks=N` (real windres/MinGW fixture + hand-built hostile images, ASan/UBSan), `RES-FUZZ: PASS iterations=300000 ...`
(resource tree, manifest text with a UTF-8==UTF-16 oracle, version blobs, dependency hints) and `RES-FUZZ-HARNESS-SELFTEST: PASS` (five bugs planted in
the library are each caught). `tests/win/res_corpus.sh [dir] [n]` is a read-only scan of real binaries (`res_scan` + `res_corpus_report.py`).
