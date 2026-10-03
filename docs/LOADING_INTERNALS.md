# How programs are loaded -- the interworkings, and where AutomationOS stands

Status 2026-10-02. Written from public documentation plus measurements; nothing here required decompiling Windows. Tags: **[V]** stated by a
primary source (Microsoft docs, a spec, a project's own file), **[E]** measured by a research agent on the Windows 11 machine this repo is
developed on (read-only, in memory; *not re-run by me*), **[I]** inference to be tested. Companion docs: `WIN_MIN.md` (the PE ladder and
proof), `userspace/lib/pe/README.md`, `COMPAT_FEASIBILITY_2026-10-02.md`.

## 1. The shape (it is the same on every OS)

```
 exec(path)
   |  kernel: recognise the format (magic), create the address space, map the *interpreter/loader*, build the initial stack
   v
 user-mode loader  (Windows: ntdll; Linux: ld.so / PT_INTERP; AutomationOS: static ELF today, bin/winrun for PE)
   |  1 map + relocate image(s)        2 resolve imports (recursively loading dependencies)   3 static TLS
   |  4 TLS callbacks (reason 1)       5 DllMain(PROCESS_ATTACH) leaf-first                    6 enter the program
   v
 program runs ... exit -> DLL_PROCESS_DETACH in reverse order -> process teardown
```
Windows' *kernel* maps the image as a section and starts the user-mode loader, which does steps 1-6 -- so "kernel recognises MZ/PE and starts a
user-space loader" is **not a hack, it is the real structure** (Windows Internals, ch. 3 "Image loader": early process init, DLL name resolution
and redirection, loaded-module database, import parsing, post-import init [V2]). That is option **A** below.

## 2. What is documented vs only observable (PE)

| topic | status | source |
|---|---|---|
| headers, sections, data directories, DllCharacteristics (DYNAMIC_BASE, NX_COMPAT, GUARD_CF, ...) | documented | MS Learn "PE Format" (CC BY 4.0 source) |
| export directory incl. **forwarders**, import tables incl. ordinal/hint, **delay-load**, `.reloc`, `.tls` directory + callback prototype, `.rsrc`, load config, certificate table | documented | same |
| x64 unwind data (`RUNTIME_FUNCTION`, `UNWIND_INFO`, handler prototype) and the x64 calling convention | documented | MS Learn x64 exception handling / calling convention |
| `DllMain` reasons, loader-lock serialisation, Kernel32 guaranteed loaded | documented | MS Learn DllMain |
| DLL search order; **API sets** (`api-*`, `ext-*`) resolved before the loaded-module list; UCRT `api-ms-win-crt-*` forwarders -> `ucrtbase.dll` | documented | MS Learn |
| security cookie (`/GS`), CFG (a custom loader may ignore it) | documented | MS Learn |
| **order** of dependency load -> TLS callbacks -> DllMain -> entry | observable only; expected [I]: map+relocate, imports (recursive), static TLS, TLS callbacks, DllMain leaf-first, entry | differential test |
| TEB: `StackBase +8, StackLimit +0x10, Self +0x30, TLS array +0x58, PEB +0x60` (documented/mirrored); `LastError +0x68`, `TlsSlots +0x1480`, PEB sub-offsets (observable only) | partly documented | MS Learn TEB (opaque), Wikipedia TIB |
| modern delay-load helpers call into the OS (`api-ms-win-core-delayload-*`), so Pietrek's "not an OS feature" is dated [E] | observable | measured |

Real tools' footprint [E]: `hostname/whoami/where/findstr/tree` import `msvcrt.dll` + `api-ms-win-core-*`, all carry a load config, `.pdata` and
DllCharacteristics 0xC160; none has bound imports; only `whoami` has TLS.

## 3. Where AutomationOS is today

| stage | AutomationOS | next |
|---|---|---|
| format recognition | ELF in `kernel/fs/exec.c`; **PE only through `bin/winrun <exe>`** | `T2-5` binfmt: MZ -> start `bin/winrun` with the exe as argv[1] (the shebang-style hook; mirrors Windows) |
| parse / map / relocate | **done, proven** (`lib/pe`, 22/22 in QEMU; 300k-iteration fuzz) | -- |
| imports / DLLs | `winrun` still resolves only against the built-in kernel32 shim (unresolved -> loud stubs). **Library built and host-tested, not yet wired in:** `pe_exports` (export lookup, forwarders `DLL.Func`/`DLL.#n`, bounded, hostile-input safe) + `pe_modules` (recursive dependency graph, LoadLibrary/GetProcAddress, DllMain attach leaf-first + rollback, detach in reverse, cycles, depth/module caps, delay imports bound eagerly) -- 1,964 host checks under ASAN/UBSAN, 200k-iteration fuzzer, real MinGW DLLs executed; not done: TLS in DLLs, SEH registration, thread attach/detach, bound imports, API-set redirection | wire into `winrun` (callbacks: read_file, alloc_image, builtin_resolve, call_entry; `shim_owns_dll()` in front of the shim) + `LoadLibrary`/`GetProcAddress` in the shim |
| manifest / resources | not used by `winrun` yet. **Library built and host-tested:** `pe_resources` (3-level resource walk, VS_VERSIONINFO) + `pe_manifest` (bounded XML-subset scanner: dependentAssembly, requestedExecutionLevel, supportedOS, DPI; dependency->search-path hints with no file I/O) -- 7,120 host checks, 300k-iteration fuzzer, 100% line coverage; read-only scan of 4,279 real System32 binaries: 0 resource rejects, 820 manifests parsed (the 4 refused are genuinely malformed/isolation manifests) | call `pe_manifest_from_image` after `pe_map` in `winrun`; feed `pe_manifest_dep_hint` into the DLL search; elevation policy stays the OS's |
| TLS / TEB / SEH / threads | none (CRT startup faults at `gs:[0x30]`) | `T2-0` GS base per thread -> `T2-2` TLS -> `T2-3` x64 SEH -> `T2-4` threads |
| loader trace | markers `WINRUN: L0..L3` only | `WINRUN_TRACE`: every stage + every dependency decision, assertable by proofkit |
| ELF dynamic linking | static, non-PIE only | out of scope for now |

## 4. Method (and its legal frame -- not legal advice)

1. **Public sources first**: the PE Format docs, the Authenticode spec, the x64 docs. Nothing in sections 2-3 needs decompilation.
2. **Black-box differential testing**: write small test PEs (MinGW/clang), run them on real Windows 11 *here* and on our loader, commit the golden
   transcripts. This is the "tear out" that is both useful and safe: it measures behaviour instead of copying code. Wine's conformance-test practice
   is the precedent.
3. **Provenance log**: for each behaviour record a doc URL or a test id.
4. **Keep Windows material out of the repo**: no Windows binaries (EULA bars redistribution [I]), no SDK/WDK headers; write structs from the CC-BY docs
   and assert them with `offsetof` tests. Do not read leaked Windows/WRK source.
5. **No GPL/LGPL code**: ReactOS (GPL) and Wine (LGPL) are behaviour references only. Permissive references: LLVM `Win64EH.h` (Apache-2.0+exception),
   signify (MIT), LIEF (Apache-2.0), google/authenticode-rs (Apache/MIT), EDK2 (BSD-2-Clause-Patent), Mono.Security (MIT) -- with attribution.
6. The Windows 11 OEM terms forbid reverse engineering/decompiling "except ... permitted by applicable law" (2(c)(vi)); EU Directive 2009/24/EC
   Art. 5(3)/6 and US 17 U.S.C. 1201(f) allow narrow interoperability work. We avoid the question by not decompiling.
7. The Authenticode spec's licence forbids reproducing/deriving from its text: write from understanding, not by pasting.

## 5. Design decision: A (recommended) vs B

* **A -- ELF stays native; PE is a guest format** via kernel binfmt + user-space loader. Keeps the kernel small, keeps ELF's simpler linking, matches Windows'
  own structure, and our `lib/pe` already has this shape.
* **B -- PE as the native format** (UEFI-style: UEFI is PE). Buys one format and an existing signature scheme; costs adopting Windows semantics (SEH tables,
  import-by-name, API sets, KnownDLLs, SxS), a PE-emitting native toolchain, and Authenticode's weaknesses. Revisit only if "run Windows apps" becomes the product.

## 6. Signing -- two separate things

**(1) Native-exec signing (do first; format-agnostic).** SHA-256 over the file bytes + a signed trailer (magic, version, algorithm, key id, signature), or a
detached signed catalog (a signed list of {id, hash, version, flags}; works for ELF and PE, no binary modification, mirrors Windows catalogs). Modes
`off | audit | enforce` (start in audit; enforce for system directories). Kernel keyring for the public key. Pitfalls to design against: **TOCTOU**
(verify the exact bytes that get mapped: read once into a kernel buffer, verify, map from it, deny writes while mapped -- Windows' PPLFault is the cautionary
case), **signature stripping** (unsigned = denied in enforce), **rollback** (version floor + hash deny-list, as UEFI `dbx`), **key storage** (private key
offline; embed the public key in the signed kernel), SHA-1 rejected for native signatures. Bricks: `SIG-0` threat model + format, `SIG-1` signing tool,
`SIG-2` kernel verifier at exec, `SIG-3` negative tests (tamper, strip, wrong key, rollback, swap-after-verify), `SIG-4` PE binfmt uses the same policy,
`SIG-5` strict default for system directories + signed policy file.

**(2) Authenticode verification (optional PE feature; never the trust basis for native execution).** Public spec: "Windows Authenticode Portable
Executable Signature Format" v1.0. Structures [V]: security data directory (index 4, a *file offset*), `WIN_CERTIFICATE` (type 0x0002 PKCS_SIGNED_DATA), CMS
SignedData with `SpcIndirectDataContent` (OID 1.3.6.1.4.1.311.2.1.4), PE image hash, RFC 3161 or legacy-countersignature timestamps, chain with the
code-signing EKU 1.3.6.1.5.5.7.3.3. Measured facts [E]: of 1058 embedded-signed System32 files the spec's hash procedure reproduced the digest inside the
signature for **1058/1058**; digests 903 SHA-256 / 151 SHA-1 / 4 SHA-384 (so SHA-1 is needed for the *hash* step even if chains stay strict); every signature RSA
(Microsoft's code-signing roots do not use ECC, so our ECDSA is not needed here); the two Microsoft documents **disagree** on hashing data after the last
section (the PE Format Appendix A sentence is wrong for real files; the simple model "every byte except the 4-byte CheckSum, the 8-byte certificate-table
directory entry and the certificate-table bytes" matched all tested files) -- differential-test files with data after the certificate table. Only ~25% of
System32 files carry embedded signatures; the rest are **catalog (.cat) signed**. Trust: Microsoft's code-signing roots are public (CCADB report) and are a
*separate* store from the TLS bundle (Mozilla disabled code-signing trust bits). Effort [I]: parse + hash + chain for SHA-256 embedded signatures ~2-3k LOC.
Bricks: `AUTH-0` host vectors (a manifest of name/size/alg/expected digest -- commit the manifest, never the binaries), `AUTH-1` locate + hash (prove
against local signed files by recomputing the digest inside the signature), `AUTH-2` DER/CMS parse + signer-signature verify ("integrity only"), `AUTH-3`
chain with a code-signing root store, `AUTH-4` timestamps, `AUTH-5` nested signatures / page hashes / catalogs / deny-list. Page hashes (`SPC_PE_IMAGE_PAGE_HASHES`)
are intentionally undocumented by Microsoft; windows checks them at page-in for FORCE_INTEGRITY images.

## 7. Ordered brick plan (each provable in QEMU; differential-test against real Windows where marked)

1. `T2-0` TEB/GS base (kernel: per-thread GS base set at every switch-in; the ~9 sites that set the kernel stack are the chokepoints) + minimal TEB/PEB; test
   exe reads `gs:[0x30]`/`gs:[0x60]` [diff vs Windows].
2. `T2-1` imports + API sets + forwarders (shim table keyed by lowercase module name; unresolved imports fail the load by default) [mingw printf program].
3. DLL graph (`pe_modules`) + manifest (`pe_manifest`) -- *in progress*; `LoadLibrary/GetProcAddress/FreeLibrary` in the shim.
4. `T2-2` TLS (implicit + callbacks + `TlsAlloc`) [diff]. 5. `T2-3` x64 SEH (`RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `__C_specific_handler`;
   scope-table layout is *not* documented -> differential test) [diff]. 6. `T2-4` threads/sync. 7. `T2-5` binfmt. 8. `T2-6` cookie + load config.
9. Loader trace mode (alongside each brick). 10. `AUTH-0..5` and `SIG-0..5` as above.
