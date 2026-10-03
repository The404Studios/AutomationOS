# Running Windows `.exe`, Linux programs, Steam, Unreal, Unity and Blender -- feasibility

Date: 2026-10-02. Evidence-based audit of the tree (file:line citations are relative to the repo root);
engineer-week figures are estimates, not commitments. Nothing here was changed in the kernel.

## Bottom line

* **The OS cannot run a Windows `.exe` or a Linux ELF program today.** The workspace notes describe
  "native Windows PE execution via a custom loader"; that is not accurate for this tree. The PE loader is
  dormant source that is not compiled into the kernel.
* **Steam, Unreal Editor, Unity Editor and Blender are not feasible** on this OS (or on a T410) -- see
  "Why the big four are out" below. A path that gives you the *experience* of them (streaming) is listed
  at the end.
* What *is* realistic, in order of payoff: a streaming/remote-play client, small native game ports, a
  static-Linux-binary personality, and Windows console `.exe` support.

## What exists today

### PE (Windows) loader -- dormant
* `kernel/pe/*.c` (~3k lines) is **not in the kernel build** (`scripts/quick_build.sh:470-710`);
  `pe_load_and_execute` has no caller and no spawn/exec path looks for `MZ`.
* It could not link as written: `process_switch`, `scheduler_add_thread`, `thread_exit` are defined
  nowhere; `process_create(exe_path)` (`pe_loader.c:632`) has the wrong arity (`exec.c:324`).
* Parsing is naive: it casts to the PE32+ optional header with no magic/machine/subsystem check
  (`pe_loader.c:84`). No delay-imports, no `.pdata`/SEH, no TEB/PEB, no ntdll/msvcrt.
* Import resolution does not use the emulated functions: it searches for real `.dll` files
  (`dll_loader.c:21-27,179-186`). The emulated Win32 surface (~24 kernel32, 11 user32, 12 gdi32 functions,
  `pe_win32.h:351-430`) is unreachable; `GetStdHandle`, `HeapAlloc`, `GetProcAddress`, `LoadLibrary`,
  `GetModuleHandle` do not exist. `docs/WIN32_COMPATIBILITY.md` claiming `HeapAlloc` is "FULL" is false.
* The calling convention is wrong (`WINAPI` is empty, no `ms_abi`), the functions live in kernel space but
  would be called from ring 3, and the GDI draws into a private 800x600 buffer that is never presented.
* The only test (`build_test/pe_check.sh`) host-compiles two files with warnings suppressed; there is no
  `.exe` and no smoke check.

### ELF / Linux
* The loader is **static only** (`exec.c:343-347`: PT_LOAD only; PT_INTERP/PT_TLS ignored; no auxv;
  no FS-base/TLS). User programs must link at 0x800000 (`userspace/userspace.ld`) because the kernel image
  reaches ~4.6 MB.
* `userspace/ld.so` (~2k lines) is not built by any script and is not glibc/musl compatible.
* **Syscall numbers are the kernel's own** (EXIT=0, FORK=1, READ=2, WRITE=3, MMAP=37 ...) and collide with
  Linux's (read=0, write=1, mmap=9). There is no Linux compatibility shim. Missing outright: lseek, dup,
  pipe, fstat, getcwd/chdir, mprotect, brk, readv/writev, fcntl, getdents, uname, clock_gettime,
  nanosleep, arch_prctl, set_tid_address, exit_group, sigaltstack, clone, AF_UNIX, /proc.
* `sys_mmap` is anonymous and eager (ignores hint/flags, 256 MB cap, no demand paging, no file-backed or
  MAP_FIXED); signals are 1-31 only with a custom frame; futex has WAIT/WAKE only; threads are a custom
  `thread_create`, not `clone`.
* No OpenGL/Vulkan/D3D. `userspace/lib/g3d` is a 667-line integer software rasterizer. Display =
  firmware framebuffer; the NVIDIA code only detects the card.

## Verdicts

| target | verdict | rough effort |
|---|---|---|
| static Linux `hello` (musl) | feasible | 3-4 weeks (personality table, arch_prctl+FS base, auxv, brk/mprotect/writev, relink) |
| static busybox | feasible | +6-8 weeks (~60 more calls and an FS layer) |
| static Rust (musl) | feasible | +2 weeks single-threaded |
| threads + TLS (clone/futex timeouts) | feasible | +4-6 weeks |
| VM rewrite (demand paging, MAP_FIXED, file mmap, mprotect, brk) | required for anything big | 8-12 weeks |
| static Python | feasible after the VM work | +10-14 weeks |
| static Go | feasible, hard | 15-20 weeks |
| dynamic linking (musl/glibc ld.so) | after the VM work | +4-6 weeks |
| Windows console `.exe` (MinGW) | feasible as a ring-3 shim | 6-8 weeks (+6-8 for MSVC CRT/SEH) |
| Win32 GUI `.exe` (simple apps) mapped onto the compositor | feasible, big | 12-20 weeks |
| broad Windows compatibility (a Wine port) | needs the full Linux ABI first | multi-year |

## Why the big four are out

* **Steam** -- glibc dynamic linking, Chromium/CEF, hundreds of threads, an X11 server, AF_UNIX, /proc,
  32-bit support, and (for games) a Vulkan driver.
* **Unreal Editor** -- D3D11/12 or Vulkan at shader model 5. The T410's GT218 is DX10.1-class and has no
  Vulkan driver; the editor also wants 16 GB+ RAM and a very large Win32/thread surface.
* **Unity Editor** -- a .NET/Mono JIT, GL 3.2+ or Vulkan, and the full Linux ABI.
* **Blender** -- GL 4.3, glibc, an X11/Wayland server. Only a software-GL path (Mesa softpipe) over a
  full Linux ABI could ever run it, at slideshow speed: 2+ years.
* A hypervisor alternative (run Linux in a VM) has no VT-x support in this kernel today and would make
  Linux, not this OS, run the software.

## The realistic way to get the experience: stream it

Run the heavy software on a host PC and stream it to the OS:

1. **Streaming client (4-8 weeks, provable in QEMU).** Pieces already exist: TCP/TLS/HTTP
   (`userspace/lib/net`), a JPEG decoder (`stb_image`), UDP, evdev input, HDA PCM streaming. Missing: an
   MJPEG-over-TCP viewer with an input back-channel and a host-side capture/encode script. A launcher
   dashboard sits on top. H.264 is another 8-12 weeks and its CPU cost on a 2010 laptop is unverified.
2. **Native small-game ports** (1-2 weeks each; needs a real `lseek` first).
3. **Linux personality chain** (static hello -> busybox -> threads/TLS -> VM rewrite): ~5-8 months total,
   each step provable with a QEMU smoke marker (e.g. `LINUX-HELLO: PASS`).
4. **Windows console `.exe`** as a ring-3 shim (6-8 weeks), provable with a MinGW hello in QEMU.

## Recommendation

Do the streaming client first (it delivers "Steam / Unreal / Unity / Blender on my OS" visually, today),
then the static-Linux personality, because it also unlocks real native ports. Do not start a Windows GUI
or Wine effort before the Linux ABI and VM work exist.
