# Changelog

All notable changes to AutomationOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### 2026-10 (later) — Multi-core made real, Windows loader, ChainLayerTwo on the OS, hardware research

**Verified in QEMU (proofkit)**
- `multicore` scenario: **57 passed, 0 failed, 1 warning**. Single-core lean desktop healthy; **two-core** (`MULTICORE=1`) desktop healthy at
  ~5 s with frame rate equal to single-core, real DNS/HTTP/verified-HTTPS on both (8 real sites; 4/4 broken-certificate classes refused;
  no site ever trusted wrongly); the SMP-H1 BKL storms (two 60 s marked-syscall storms) complete with zero corruption and no lock watchdog,
  also under the full self-test suite. The one warning: one run had 7/8 sites (a transient `letsencrypt.org` connect error); two immediate
  re-boots were 8/8.
- `scripts/bkl_smoke.sh` (the project's own storm arbiter): `BKL: PASS ... deadlock=0 panic=0`.
- `wincompat`: a real MinGW Windows x64 console `.exe` runs through `winrun` (parse, map + relocate at a non-preferred base, imports, entry,
  exit code 42); 3 hostile files refused and never run; 300k-iteration parser fuzz passes.
- `build_test/chainlayer_e2e.sh`: **ChainLayerTwo's own MCP client code** created a file on the booted OS, wrote C, compiled it on the device,
  ran it (`exit=55`), and the OS refused writes to `/etc` and path traversal. Bridge self-test 19/19.

**Fixed**
- **Multi-core serial tearing**: two CPUs printing to one serial port tore log lines (kernel `kprintf` vs user writes; init built lines from
  several writes). Init is now line-buffered (`print_flush`); `[INIT] Process N exited` / `[SUP] ...` are atomic.
- **BKL is now a FIFO ticket lock** (was test-and-set): a chatty CPU could starve the other for seconds.
- **Boot-time proof storms no longer ship**: new `MULTICORE=1` (one switch for the full SMP stack) implies `SMP_PRODUCT=1`, which keeps the
  BKL but does not launch the two 60 s `bklstorm` processes at every boot. `MULTICORE=1 SMP_PRODUCT=0` is the proof kernel.
- **Early-boot DNS race**: the first lookup right after boot failed (`DNS_ERR_SEND`) before the stack was ready. The resolver now retries
  that same attempt with a bounded sleep + socket poll (3 s budget per process); new `dns_wait_net()`.
- **Bridge peer authentication** (`scripts/chainlayer_mcp_bridge.py`): the OS-facing listener accepted any peer; it now has `--os-allow-ip` and
  refuses a non-loopback listen address without one.
- `winrun` read the whole file in one 16 MiB `read` (the kernel caps reads at 1 MiB), so every `.exe` looked "truncated"; now chunked.
- proofkit: `no_kernel_fault` no longer matches `Total panics detected: 0`; live serial/pcap are written to the fast disk and published
  afterwards (a guest serial write blocks the vCPU; writing to the Windows-mounted artifact dir throttled chatty guests ~60x and looked like a
  kernel hang); `eval` re-judges recorded artifacts.

**Added**
- `scripts/make_multicore_isos.sh`, `scripts/make_release_isos.sh`; proofkit scenarios `multicore`, `bkl_storm`.
- Host-tested PE libraries (not yet wired into `winrun`): `pe_exports` + `pe_modules` (DLL graph, forwarders, DllMain order; 1,964 checks, 200k
  fuzz) and `pe_resources` + `pe_manifest` (resource tree, version info, application manifest; 7,120 checks, 300k fuzz; 0 resource rejects over
  4,279 real System32 binaries).
- Docs: `LOADING_INTERNALS.md`, `CHAINLAYER_INTEGRATION.md`, `T410_COMPAT_RESEARCH_2026-10-02.md`, addenda in `T410_IWLWIFI.md` and
  `GPU_ROADMAP_2026-10-02.md`, `WIN_MIN.md` (tier-2 measurement), gap register.

**Known limits (stated, not hidden)**
- **Real hardware is unverified**: nothing here has run on the ThinkPad T410. The multi-core ISO needs topology-based AP selection and MTRR/PAT
  parity before it is trusted there (single-core is the baseline); the onboard-NIC reset sequence has a documented hang risk (`PCH_NIC` stays
  default OFF); USB on the EHCI-only PCH needs hub + split-transaction support that does not exist yet.
- **Wi-Fi**: card identification + firmware parsing only; no association or data path. Compliant **WPA3 is impossible on the Intel 6000-series**
  cards the T410 shipped with (firmware has no management-frame protection); use a WPA2/WPA3 transition SSID or wired Ethernet.
- **Windows**: tier 1 only. Ordinary MinGW programs fault in CRT start-up (`gs:[0x30]`: no TEB); 32 UCRT imports unresolved; no SEH/TLS/threads.
- **NVIDIA/GPU**: no native driver; the VBE framebuffer + software compositor is what runs (write-combining via PAT is the next win).
- Node.js, git, Blender, Unreal and Steam do not run inside the OS; ChainLayerTwo and git run on the host and the OS is the machine they
  build for and drive (the on-device compiler is a C subset).

### 2026-10 — Build repair, paging fix, firewall + enforced privileges, MCP tool host

**Fixed**
- **Build was broken on GCC 16**: the compiler lowers aggregate copies/initialisers to
  `memcpy`/`memset` calls even under `-ffreestanding -fno-builtin`, so crt0-only apps failed to
  link and `build_all.sh` (`set -e`) produced no ISO. `userspace/lib/c/fsmem.c` provides weak
  `memcpy/memset/memmove/memcmp`, linked into every userspace ELF.
- **PT-DIRECTMAP-0 (kernel)**: the page-table walkers dereferenced frames through the low identity
  alias, which under a process CR3 is overlaid by that process's own user pages. `browser2`
  (33 MB BSS) shadowed its own PML4, so a stack demand-fault "resolved" without installing a PTE,
  re-faulted forever and leaked a frame per retry until all RAM was gone (any RAM size). All 152
  table dereferences in `paging.c` now go through the dedicated direct map.
- Self-heal v3 (PID 1 supervises the compositor) verified under the T410 profile:
  forced freeze -> recovery -> all windows restored.

**Added**
- **FW-0 packet filter** (`kernel/net/firewall.c`, `uapi/fw.h`, `SYS_FW_CTL`=136, `fwctl`):
  stateful IPv4, default inbound DROP / outbound ACCEPT, connection tracking, built-in DHCP/ICMP
  handling, ingress sanity drops (spoofed sources, NULL/SYN+FIN/SYN+RST scans, L4 fragments), boot
  `FW-SELFTEST`. Hooks `net_recv`/`net_send` **and** the stack's `nif->tx` transmit branch.
  `FW_OPEN=1 quick_build.sh` builds with inbound default ACCEPT for harnesses.
- **PCAP-0 privilege mask** (`SYS_CAP_DROP`=137, `SYS_CAP_QUERY`=138): monotonic per-process
  `cap_denied`, inherited by spawn/fork/thread, enforced on raw frame send/receive, network
  configuration and firewall writes. Default is "nothing dropped", so behaviour is unchanged until a
  process drops privileges.
- **Agent rail**: `fw_status` (read-only, auto) and `firewall` (operator-confirmed) tools; tools
  that launch programs (`tool_exec`, `tool_spawn`) drop all kernel privileges first; `agentd`
  serve mode (`/etc/agentd.conf`, `AGENTD_SERVE=1`) as a long-lived tool host with `PING` keepalive.
- `scripts/chainlayer_mcp_bridge.py`: exposes the OS tool rail to ChainLayerTwo / any MCP client
  (Streamable HTTP, Bearer token, observe/mutate/control tiers). `build_test/mcp_bridge_e2e.sh`
  proves MCP -> bridge -> guest agent -> kernel end to end.
- Smoke suite is now **52 checks** (`FW-SELFTEST`, `FWTEST`).
- `docs/GAP_REGISTER_2026-10-02.md`: audit register (YouTube, sound, GPU, scheduler/locks, WiFi,
  ChainLayer) with FIXED / OPEN-SW / OPEN-HW status.

### 2026-07 — Signature Dark redesign + robustness/completeness hardening

**Desktop UI**
- Unified **"Signature Dark"** design language: one token header
  (`userspace/lib/ui/theme.h`, graphite surfaces + teal accent) replacing the
  fragmented per-app palettes across the compositor, `lib/ui`, and all ~44 GUI
  apps.
- Release hardening: a WCAG-AA contrast gate (`scripts/theme_contrast_check.py`),
  a statistical chrome pixel-regression (`scripts/chrome_pixel_check.py`), a
  desktop golden baseline, and a manual QA checklist
  (`docs/ui/desktop-redesign-manual-qa.md`).

**Security / robustness (audit → fix → PROVE bricks)**
- KERNEL-ROBUST-0, CRYPTO-ROBUST-0, KERNEL-SYSCALL-ROBUST-0, NET-RX-ROBUST-0:
  multi-agent adversarial audits fixing ~40 verified defects (x509 exponent
  overflow, embedded-NUL hostname bypass, X25519 low-order acceptance,
  cross-process `shmdt` UAF, ELF `e_phoff` overflow, and more), each with
  negative-test discriminators and smoke gates.
- Whole-system completeness sweep: closed default-reachable memory-safety gaps —
  sleep-list timer-IRQ UAF, `sendfile` socket-ownership bypass, page-cache CLOCK
  UAF, and W^X (NX) on anonymous/heap pages. Wired previously-inert desktop
  controls (settings persistence, volume/mute, notifications, filemanager error
  surfacing).
- See **SECURITY.md** for the honest posture (deferred SMP-gated items, fail-open
  TLS trust in progress, non-enforcing capability/seccomp/rlimit scaffolding).

**Boot smoke suite:** now **50 checks** (was 43); build the full suite with
`FULL=1 bash scripts/build_all.sh`.

### Phase 1: Core Foundation (95% Complete)

#### Added (Phase 1 Implementation)

**Boot & Initialization**
- GRUB Multiboot2 boot with framebuffer setup (the shipped path; an experimental ABL loader also exists in-tree)
- x86_64 long mode initialization
- Higher-half kernel mapping (0xFFFFFFFF80000000)
- Bootloader-kernel protocol with memory map, RSDP, framebuffer info

**Memory Management**
- Physical Memory Manager (PMM) with buddy allocator
- Virtual Memory Manager (VMM) with 4-level paging
- Kernel heap allocator with slab-based allocation
- Memory zone management and allocation statistics
- Higher-half virtual addressing

**CPU & Interrupts**
- GDT (Global Descriptor Table) with kernel/user code/data segments
- IDT (Interrupt Descriptor Table) with 256 entries
- Exception handlers for all CPU exceptions
- SYSCALL/SYSRET fast system call mechanism
- Context switching with full register state save/restore

**Device Drivers**
- Serial driver (COM1, 115200 baud) for debug output
- PIT (Programmable Interval Timer) at 100Hz
- PS/2 keyboard driver with scancode translation
- Framebuffer driver with 8x8 bitmap font
- Basic VGA text mode support

**Process Management**
- Process structure with PID, state, registers, page tables
- Round-robin scheduler with 10ms time slices
- Process creation and termination
- Kernel and user mode processes
- Process state management (READY, RUNNING, BLOCKED, TERMINATED)

**System Calls**
- System call dispatcher infrastructure
- Basic system calls: exit, write, read, getpid
- User/kernel privilege separation
- System call parameter validation with copy_from_user/copy_to_user

**Userspace**
- User mode initialization
- Init process (PID 1)
- Interactive shell with command execution
- Minimal libc (printf, scanf, string functions, system call wrappers)

**Testing & Validation**
- Integration testing framework with Python test harness
- Automated boot tests in QEMU
- Unit tests for memory allocators
- Performance benchmarking infrastructure
- CI/CD-ready test suite

**Documentation**
- Comprehensive architecture documentation (26KB)
- Complete API reference (19KB)
- Build and development guides (35KB combined)
- Troubleshooting guide (17KB)
- Integration testing documentation
- Performance analysis reports
- Phase 1 and Phase 2 implementation plans
- AI service architecture specification
- Driver expansion roadmap

#### Fixed (Wave 4 Bug Fixes)

**Critical Fixes**
- Heap allocator implementation in kernel/core/mem/heap.c
- Build system integration for all components
- Context switch RSI register corruption
- SYSCALL/SYSRET MSR configuration
- User/kernel privilege level transitions
- Stack pointer alignment in context switches

**Security Fixes**
- Added copy_from_user/copy_to_user for safe parameter passing
- Fixed buffer validation in system call handlers
- Implemented proper privilege checks
- Added stack canaries (-fstack-protector-strong)
- Enabled NX (No-Execute) bit support
- Fixed NULL pointer dereferences (20+ instances)

**Scheduler Fixes**
- Fixed time slice reset bug (time_slice never reset after quantum expiration)
- Fixed race conditions in scheduler state transitions
- Proper handling of BLOCKED and TERMINATED states
- TSS RSP0 update on context switch

**Memory Management Fixes**
- Memory leak fixes in process termination
- Proper cleanup of page tables on process exit
- Frame allocator validation
- Heap corruption prevention

**Driver Fixes**
- Race condition in keyboard input buffer
- Serial port initialization reliability
- Framebuffer scrolling edge cases

#### Security

**Vulnerabilities Identified (from Security Analysis)**
- 27 total vulnerabilities identified in codebase analysis
- 9 CRITICAL severity issues
- 11 HIGH severity issues
- 7 MEDIUM severity issues

**Security Improvements**
- Implemented copy_from_user/copy_to_user for kernel/user boundary
- Stack canaries enabled (-fstack-protector-strong)
- NX bit support for non-executable pages
- Proper privilege level checking in system calls
- Input validation in all user-facing interfaces

**Remaining Security Work (Phase 2)**
- Full ASLR (Address Space Layout Randomization)
- Capability-based security
- Mandatory Access Control (MAC)
- Secure boot chain
- Encrypted storage

#### Performance

**Instrumentation Added**
- Boot time measurement and profiling
- Context switch latency tracking
- System call latency measurement
- CPU frequency calibration via RDTSC
- Memory allocation statistics

**Performance Characteristics (Baseline)**
- Boot time: ~500ms (QEMU, estimated)
- Context switch: ~1-2 μs (hardware-dependent)
- System call overhead: ~50-100 ns (SYSCALL/SYSRET)
- Scheduler quantum: 10ms (100Hz timer)
- Memory allocation: O(log n) for buddy allocator

---

## [0.1.0] - 2026-05-26

### Phase 1 Initial Release

**Status:** Development snapshot, 95% feature complete

This release represents the completion of Phase 1 (Core Foundation) of the AutomationOS project. It provides a minimal but functional operating system that boots on x86_64 hardware (QEMU and bare metal via USB).

#### Deliverables

1. **Bootable System**
   - UEFI bootloader
   - x86_64 kernel
   - Boots in QEMU and on bare metal

2. **Core Features**
   - Memory management (PMM, VMM, heap)
   - Process scheduler
   - Basic device drivers
   - System call interface
   - Userspace with init and shell

3. **Development Infrastructure**
   - Complete build system
   - Integration testing framework
   - Performance profiling tools
   - Comprehensive documentation

4. **Documentation**
   - 10+ documentation files
   - ~20,000 lines of documentation
   - 100+ code examples
   - Complete API reference

#### Known Limitations

**Feature Limitations**
- No file system (Phase 2)
- No networking (Phase 2)
- No multi-threading (Phase 2)
- No IPC mechanisms (Phase 2)
- Single-core only (Phase 2)
- No disk I/O (Phase 2)

**Stability Issues**
- Some edge cases in scheduler state transitions
- Limited error recovery in drivers
- No graceful handling of out-of-memory conditions

**Performance Limitations**
- No SMP (Symmetric Multi-Processing)
- No CPU frequency scaling
- No power management
- Basic round-robin scheduler (no priority)

#### Upgrade Path

Phase 1 (0.1.0) → Phase 2 (0.2.0):
- File system (AutoFS)
- Disk drivers (AHCI/NVMe)
- Network stack
- IPC mechanisms
- Enhanced security (capabilities, MAC)
- Multi-core support

---

## Project Milestones

### Phase 1: Core Foundation (Completed - May 2026)
**Duration:** 8 weeks  
**Goal:** Bootable kernel with minimal shell  
**Status:** ✅ Complete (95%)

### Phase 2: Security & Isolation (In Progress)
**Duration:** 6-8 weeks  
**Goal:** Capabilities, namespaces, MAC  
**Status:** 📋 Planning complete, implementation starting

### Phase 3: Storage & Networking (Planned)
**Duration:** 8-10 weeks  
**Goal:** File system, disk I/O, network stack  
**Status:** 📝 Design phase

### Phase 4: AI Integration (Planned)
**Duration:** 6-8 weeks  
**Goal:** AI service daemon, ML model loading  
**Status:** 📝 Design phase

### Phase 5: Advanced Features (Planned)
**Duration:** 8-10 weeks  
**Goal:** GPU acceleration, distributed features  
**Status:** 📝 Concept phase

### Phase 6: Production Hardening (Planned)
**Duration:** 6-8 weeks  
**Goal:** Optimization, monitoring, documentation  
**Status:** 📝 Concept phase

---

## Version History

| Version | Date | Phase | Status | Highlights |
|---------|------|-------|--------|------------|
| 0.1.0 | 2026-05-26 | Phase 1 | Dev Snapshot | First bootable release |
| 0.2.0 | TBD | Phase 2 | Planned | Security & isolation |
| 0.3.0 | TBD | Phase 3 | Planned | Storage & networking |
| 0.4.0 | TBD | Phase 4 | Planned | AI integration |

---

## Contributing

See [DEVELOPMENT_GUIDE.md](docs/DEVELOPMENT_GUIDE.md) for contribution guidelines.

### Reporting Issues

When reporting bugs, please include:
- AutomationOS version (from this file)
- Host system (Linux/macOS/Windows WSL2)
- Steps to reproduce
- Expected vs actual behavior
- Relevant log output

---

## References

- [Project README](README.md)
- [Documentation Index](docs/INDEX.md)
- [Phase 1 Completion Report](docs/PHASE1_COMPLETION_REPORT.md)
- [Phase 2 Implementation Plan](docs/superpowers/plans/2026-05-26-phase2-security-isolation.md)

---

**Last Updated:** 2026-05-26  
**Maintained by:** AutomationOS Development Team
