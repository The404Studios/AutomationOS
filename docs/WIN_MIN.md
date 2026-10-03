# WIN-MIN -- the bare minimum for native Windows program compatibility

Status 2026-10-02. Companion to `docs/COMPAT_FEASIBILITY_2026-10-02.md` (which says what is *not* feasible).
This file defines the smallest thing that is real, how it is built, and how every stage is asserted.

## What "minimum" means

A **Windows x64 console program (`.exe`) runs, prints, and exits with its own exit code.** Nothing else is
claimed. The minimum is a pipeline of five stages; each one is a measurable claim:

| stage | claim | where | marker |
|---|---|---|---|
| L0 parse | the PE32+ image is validated (hostile-input safe) | `userspace/lib/pe` | `WINRUN: L0 parse OK ...` |
| L1 map | sections placed at their RVAs; base relocations applied (image runs at any address) | `winrun.c` + `pe_map/pe_relocate` | `WINRUN: L1 mapped at ... relocs=applied` |
| L2 imports | every import resolves to a working function (or a loud stub) | `win_shim.c` | `WINRUN: L2 imports total=N unresolved=M` |
| L3 run | control enters through the **Windows x64 ABI** (`ms_abi`); `ExitProcess`/return ends the process | `winrun.c` | `WINRUN: L3 entry ...`, `WINRUN: exit code=N` |
| hostile | a truncated / non-PE / wrong-architecture file is refused and never reaches an entry point | `pe.c` | `WINRUN: REJECT <reason>` |

Everything runs in **user space**, with **no kernel change**: `vmm_mmap_anon` already honours an explicit
`PROT_EXEC`, and a PE is position-independent through its relocation table. (The image is mapped RWX because
there is no `mprotect` syscall yet -- an accepted, documented weakening of W^X for this tier.)

## Why this is the right floor

It is the smallest program that exercises every hard part of "running foreign code": a foreign binary
format, relocation, a foreign calling convention, an API boundary, and process exit. A freestanding console
program imports three `kernel32` functions (`GetStdHandle`, `WriteFile`, `ExitProcess`), so the whole surface
is small enough to *prove*. Everything above it is more surface, not a new kind of problem.

## The ladder (build from here)

| tier | target | adds | status |
|---|---|---|---|
| **1** | freestanding console `.exe` (no CRT) | the pipeline above + ~55 kernel32 functions (console I/O, heap, VirtualAlloc, time, TLS slots, critical-section no-ops) | **PROVEN in QEMU 2026-10-02** (22/22 checks, `proofkit run wincompat`) |
| 2 | **TEB**: a per-thread `GS` base | `SYS_SET_GS_BASE` (+ set on every switch-in), a minimal TEB/PEB in user memory (`gs:[0x30]` self, `gs:[0x60]` PEB, `gs:[0x58]` TLS array, `gs:[0x08/0x10]` stack bounds) | **measured prerequisite for tier 3** -- see below; kernel brick |
| 3 | ordinary MinGW program (`printf`, `malloc`, `argc/argv`) | the UCRT import set -- **32 functions measured** (list below): `_initterm`, `__p___argc`, `__acrt_iob_func`, `__stdio_common_vfprintf`, malloc/free, `strlen/strncmp/memcpy`, `exit/abort`, `signal`, ... | next after tier 2 |
| 4 | SEH + threads | x64 `.pdata` unwinding (`__C_specific_handler`), `CreateThread` on `thread_create`, TLS directory | kernel + shim |
| 5 | kernel `binfmt`: `./prog.exe` just works | `exec.c` recognises `MZ`/`PE` and launches `winrun` | small |
| 6 | Win32 GUI (`user32`/`gdi32`) mapped onto `wl_client` windows | message loop, window class table, GDI -> surface | large (12-20 wk) |

### What the first real measurement taught (2026-10-02)

Running the MinGW `crt_hello.exe` through the tier-1 loader showed two things the plan had wrong:

1. **MinGW GCC 16 links against the UCRT, not msvcrt.** Imports arrive via the `api-ms-win-crt-*-l1-1-0.dll`
   family (runtime, stdio, heap, string, locale, math, environment, private) plus `KERNEL32`. The shim must
   resolve those API-set names (by function name; the API-set DLL is only a routing label).
2. **The CRT's own startup code faults before `main`**, on `mov rax, gs:[0x30]` (the TEB self-pointer; CR2 = 0x30
   because GS base is 0). So a GS/TEB brick is a *prerequisite* of the UCRT shim, not something that can follow
   it. The fault was contained (the kernel killed only `winrun`; `boot.no_fault` still passed).

The 32 unresolved imports at the time of measurement: `KERNEL32!FreeLibrary`;
`environment!__p__environ`; `heap!_set_new_mode calloc free malloc`; `locale!_configthreadlocale`;
`math!__setusermatherr`; `private!__C_specific_handler memcpy`; `runtime!_set_app_type __p___argc __p___argv _cexit
_configure_narrow_argv _crt_atexit _exit _initialize_narrow_environment _initterm _initterm_e
_set_invalid_parameter_handler abort exit signal`; `stdio!__acrt_iob_func __p__commode __p__fmode
__stdio_common_vfprintf fflush setvbuf`; `string!strlen strncmp`. The `tier2.*` proof assertions track this number.

### Bugs the proof found in its own first run

* `winrun` read the file with one 16 MiB `read`; `sys_read` rejects `count > MAX_READ_SIZE` (1 MiB) with `EINVAL`,
  so **every** `.exe` -- valid or hostile -- was reported as "truncated". Fixed (chunked reads; an I/O error is now
  reported as an I/O error). The first version of the hostile-input assertion *passed anyway* because it counted
  `REJECT` lines; it now names each bad file and also asserts the good file is **not** rejected.
* The tier-1 fixture had no `.reloc` table (GCC folded the pointer away), so it could only load at its preferred
  base and never exercised relocation. Now `volatile`; the fixture builder prints the reloc directory size and
  the L1 marker proves `delta=nonzero` (mapped at `0x101000000`, preferred `0x140000000`).

Steam, Unreal/Unity editors and Blender are **not** on this ladder (see the feasibility doc): they need a GPU
API, a near-complete Windows/Linux userland and threads far beyond tier 5.

## How it is asserted (the process framework)

`tools/proofkit/scenarios/wincompat.json` boots a build with `WIN_TEST=1` (fixtures compiled by MinGW-w64 on the
host and staged under `/usr/share/winfix`), then asserts, with `depends_on` so a failure names the *first*
broken stage:

* host: ASAN/UBSAN unit tests + a 300 000-iteration mutation fuzzer over the parser;
* L0 -> L1 -> L2 -> L3 markers **in order** (`serial_order`), the program's own output (which also proves a
  relocated pointer was fixed up correctly), and the exact exit code (42);
* hostile files are rejected and **never** reach `L3 entry`; no kernel fault;
* tier 2 is an *expected-red* measurement (`severity: warn`): the number of unresolved CRT imports, which turns
  green when tier 2 lands.

Run: `wsl -d Arch python3 tools/proofkit/proofkit.py run wincompat` (add `--expect-fail ID` to prove a test can
fail before trusting it).

## Known limits at tier 1 (by design)

No GS/TEB (code reading `gs:` faults), no SEH, single thread, no GUI, no DLLs other than the built-in shim,
no delay-imports, `VirtualAlloc` ignores the requested address, all memory RWX.
