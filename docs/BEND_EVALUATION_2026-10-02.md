# Bend as the main language of AutomationOS -- evaluation

Date: 2026-10-02. Research only (public sources, cited); nothing from Bend/HVM was copied or built into this tree.

## What Bend is *today*

* **Bend 2** launched 2026-09-17 (latest v2.0.34 on 2026-09-28), repo `bendlang/bend`, **Apache-2.0**. Its own
  README says "BEND IS YOUNG. EXPECT BUGS" and that the compiler is "99% AI-written and not yet fully audited".
  Bend 1 programs do not load in Bend 2.
* Bend 2 **no longer evaluates interaction nets at runtime**: it emits one C file per program built with
  **clang**, plus Metal/CUDA/JS backends. Compiler is TypeScript/Bun. Python-like syntax, dependent types,
  affine values.
* Limits stated by the project: only `Nat`, `U32`, `F32` (U64/F64 rejected); strings are slow linked lists;
  no TLS/DNS/HTTP/JSON/regex in the language; effects = print/env/time/sleep/spawn/channels/files/TCP/UDP;
  **clang only**, **glibc 2.34+**, Linux/macOS; whole-program builds (~62 s per 1000 lines); FFI has no ABI promise.
* Runtime needs: an mmap'd heap (8 GiB..8 TB reserved), **2 GB guard-paged stacks per worker**, a pthread pool
  (up to 128), C11 atomics, a single-threaded effect event loop.
* Performance (authors' numbers, M4 Max): sequential ~0.8-1.5x C time; 16 threads ~9-12x over one thread.

## Verdict for this OS

| role | verdict | why |
|---|---|---|
| kernel / drivers | **no** | no pointers, no MMIO, no inline asm, no C ABI, needs a huge heap + 2 GB stacks, whole-program, runtime sits *on top of* an OS |
| userland, shell, UI, apps | **no** | no library output / ABI, 32-bit numbers, slow strings, GCC unsupported, 2-week-old toolchain |
| high-level **parallel compute layer** | plausible, costly | port the runtime to our syscalls (mmap/VMA, threads, clang cross toolchain: weeks); our kernel's *eager* VMAs fight its lazily-faulted huge reservations |
| on-device compiler | **no** | compiler needs Bun/TypeScript + clang; cross-compile on the host |

"It should operate the kernel": a language runtime can only operate a kernel through the kernel's interface --
it cannot *be* the kernel. The kernel stays C (it needs MMIO, interrupts, page tables, atomics). What *can* be
high-level is the **operating layer above the syscall boundary**, and this OS already has the pieces:

* typed syscalls incl. the new `SYS_FW_CTL` / `SYS_CAP_DROP` (kernel-enforced privilege tiers);
* the gated agent rail (`agentd`) and the MCP bridge (`scripts/chainlayer_mcp_bridge.py`);
* an in-OS JavaScript interpreter.

## Recommended path (cheapest first, each provable in QEMU)

1. **Take Bend's real lesson now, in C:** a two-lane fork-join (`par2(f,g)` / `par_for`) on the existing CPU1
   offload (`matmuljobs`), with per-lane bump allocators and equal-split fork-join (no work stealing is
   needed for two lanes). Real multicore benefit for T410 compute, no new language.
2. **A control language for operating the system:** bind a small `os.*` object into the existing JS engine
   (`os.spawn`, `os.fw.allow`, `os.cap.drop`, `os.net.info`, `os.ps`) so scripts can orchestrate the kernel
   through typed, privilege-checked syscalls. Proof: a script that opens a firewall port and the kernel
   rule table changes (the same "effect on the wire" style as `proofkit dns_lease`).
3. **Optional, host-side experiment (the smallest step to learn if Bend is viable here):** on the WSL host build
   Bend 2's hello-world and a 2-thread tree-sum, then `nm -u` the output. If the undefined-symbol list is
   short (mmap, pthread, write, clock), try the single-thread build under a ring-3 loader. Otherwise stop.

Do not build the OS around Bend now: a two-week-old, AI-written, clang+glibc-only whole-program compiler
cannot sit under a freestanding kernel, and the gain on a 2-core 2010 CPU would be at most ~2x.

Sources: github.com/bendlang/bend (README, LICENSE, CHANGELOG, paper/BendRT.pdf), github.com/HigherOrderCO/HVM2,
akitaonrails.com review (2026-09-19). Items the researcher could not verify: HVM4 license, exact clang version
floor, and whether the emitted runtime can be built freestanding.
