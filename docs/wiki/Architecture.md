# Architecture

AutomationOS is a from-scratch x86_64 operating system that boots through GRUB into a 64-bit long-mode kernel and lands on a Windows-like graphical desktop — the same image runs under QEMU and on a physical 2010 Lenovo ThinkPad T410. This page is the high-level map: how the layers stack, how a boot actually proceeds, and where every piece lives in the tree.

![The AutomationOS desktop](../../screenshots/desktop.png)

> Scope note: this document describes what the code *does today*, in the **default** build. The scheduler is **cooperative** (processes yield via `SYS_YIELD`; there is no timer preemption), the system is **single-core**, and boot is **GRUB Multiboot1 on legacy BIOS** (not UEFI). Preemptive (`PREEMPT=1`) and multi-core SMP (`SMP=1`) builds exist behind env-gated flags and are separately validated, but are **not** the shipped default. A growing list of subsystems — the real Intel iwlwifi WiFi driver, Intel HD Audio, the AI agent rail, TLS 1.3 — are likewise gated behind build flags or post-desktop triggers and are **off in a plain boot** (the reasons are explained below). See [Roadmap](../ROADMAP.md) for what is planned.
>
> The boot smoke test (`scripts/smoke_boot.sh`) runs **43 invariant checks** against a real QEMU boot of the default ISO; a separate `GAMETEST=1` harness proves **25 games + apps** spawn and survive.

---

## The layering

From firmware up to the apps, each layer hands off to the next:

```
Legacy BIOS / firmware
        │  (El Torito ISO, GRUB stage built by grub-mkrescue)
        ▼
GRUB 2  ──  Multiboot1 loader   iso/boot/grub/grub.cfg
        │   loads kernel.elf + initrd.img as module 0
        ▼
boot.asm  ──  32-bit entry → long mode    kernel/arch/x86_64/boot.asm
        │   CPUID checks, PAE, page tables, EFER.LME, paging on,
        │   initrd-in-.bss rescue, .bss clear, call kernel_main
        ▼
kernel_main(C)  ──  subsystem bring-up     kernel/kernel.c
        │   GDT/IDT, PMM/VMM/heap, framebuffer+splash, drivers,
        │   VFS, mount initrd, load /sbin/init, scheduler_start()
        ▼
/sbin/init (PID 1, ring 3)   userspace/init/main.c
        │   SYS_SPAWN the compositor + apps; reap children
        ▼
/sbin/compositor (ring 3)    userspace/compositor/compositor_m8.c
        │   owns the framebuffer; draws panel, dock, wallpaper;
        │   composites client windows over SHM; forwards input
        ▼
GUI apps (ring 3)            userspace/apps/*
            terminal, filemanager, editor, browser2, ide,
            netman, soundman, cockpit, ~18 games…
```

Everything above `boot.asm` runs in 64-bit long mode. Everything from `init` down runs in **ring 3** — the kernel reaches userspace exactly once (loading PID 1) and thereafter serves it through the syscall interface.

The model holds for the newer subsystems too: WiFi, audio, and the AI agent are all userspace clients of narrow kernel syscall surfaces (`SYS_WLAN_*`, `SYS_AUDIO_*`, the gated tool rail) — the radio driver and the HDA codec talk are the only pieces that live in the kernel, behind a swap seam.

---

## Boot sequence (with on-screen `[NN]` markers)

On real hardware the serial `kprintf` log is invisible, so `kernel_main` paints numbered progress markers straight to the framebuffer via `boot_mark()` (`kernel/kernel.c`). Each call stacks one green `[NN] label` line under the boot splash; the **last marker on screen** pinpoints exactly which init step froze if a boot ever hangs. The compositor harmlessly paints over them once the desktop comes up.

The sequence (`kernel_main`, `kernel/kernel.c`):

1. `serial_init()` + banner.
2. `parse_multiboot()` — read the GRUB info struct: memory map, initrd module, framebuffer geometry.
3. `gdt_init()`, `idt_init()`.
4. `pmm_reserve_initrd()` **before** PMM init, so the page allocator never hands out the initrd's pages.
5. `pmm_init()` → `vmm_init()` → `tlb_init()` → `pmm_add_remaining_pages()` → `heap_init()` → `cow_init()`.
6. Self-tests/benchmarks: heap, slab.
7. `pci_init()` — enumerate the PCI bus.
8. Framebuffer: map it (kernel + a userspace alias at `0x40000000`), `framebuffer_init()`, draw the **"Welcome to AutomationOS"** splash. From here on `boot_mark()` is live.
9. `[NN] timer (PIT)` — `pit_init(1000)`. **The PIT is armed as a monotonic 1000 Hz tick counter only.** IRQ0 increments a counter and never reschedules, which is what keeps scheduling cooperative; it gives userspace a real time source via `SYS_GET_TICKS_MS`.
10. `[NN] network (e1000)` — `net_init()` (NIC detect, ARP). Bounded spins so a T410 with no link bails fast. `sock_init()`, `rtc_init()`, `rng_init()`, clipboard/notify.
11. `[NN] vfs` → `[NN] fs drivers (ext2/fat32)` → `[NN] mount root (ramfs)`; create `/mnt`, `/home`, `/dev`.
12. `[NN] input subsystem` → `[NN] keyboard/mouse (PS/2)` → `[NN] pty`.
13. `process_init()`, `scheduler_init()`, `perf_init()`.
14. Load the initrd (`initrd_init` / `initrd_mount`), find `sbin/init`, `elf_load_and_exec()` it as PID 1.
15. `tss_init()`, `syscall_init()`, `shm_init()`/`msg_init()`, `syscall_msr_init()` (SYSCALL/SYSRET MSRs).
16. `[NN] starting services (scheduler)` — `scheduler_start()` enables interrupts and switches into PID 1. Control never returns; `kernel_main` would otherwise fall into an `hlt` idle loop.

### Safe-boot gate

`kernel.c` defines `T410_SAFE_BOOT` (currently on). Under it the kernel **skips the NVIDIA GPU register probe and the AHCI/SATA + diskfs init** — both poke real device MMIO that QEMU never exercises and were prime suspects for a post-splash hang. A RAM-rooted boot reaches the desktop without either; you'll see `[NN] gpu SKIPPED (safe boot)` / `[NN] storage SKIPPED (safe boot)` markers. Networking stays enabled because its spins are hard-bounded.

The same "off until proven on the real device" discipline gates the newer hardware paths: **HD Audio** (`HDA_ENABLE=1`), the **real Intel WiFi radio** (`IWLWIFI=1`, plus a post-desktop trigger — see below), durable **disk persistence** (`DISK_PERSIST=1`), and the **T410 PCH NIC** (`PCH_NIC=1`) are all compiled out of the default image because each can hardware-stall an un-validated machine at boot. QEMU is unaffected either way.

---

## Major subsystems (and how they're gated)

Beyond the classic kernel/desktop core, the tree carries several larger subsystems. Each follows the same rule: **default-safe, opt-in via a build flag, proven by a self-test or KAT before it is trusted on real hardware.** The build flags are listed under [What is actually compiled](#what-is-actually-compiled).

### WiFi — a from-scratch Intel iwlwifi DVM driver

WiFi is structured around a single swap seam, `wifi_ops_t` in `kernel/include/wifi.h`, which hangs off a `netif_t`. Everything above the seam — the Network Manager GUI (`userspace/apps/netman`), the `wpasupp` supplicant, the `SYS_WLAN_*` control plane — talks only to the seam; a concrete backend supplies the radio. There are two backends implementing the identical contract:

- **`kernel/drivers/net/wireless/sim/wifisim.c`** — a simulated `wlan0` (a fixed AP list + a scripted connect), compiled under `WIFI_SIM=1`. QEMU has no real WiFi, so this is the only WiFi during development; it exercises the entire scan → WPA2/WPA3 → DHCP → IP flow and the GUI with no radio.
- **`kernel/drivers/net/wireless/intel/iwlwifi/`** — a **real, from-scratch Intel iwlwifi DVM driver**, compiled under `IWLWIFI=1`. The files map to the bring-up ladder: `iwl-pci.c` (card detect + safe BAR probe), `iwl-trans.c` (APM power-up, prepare-card-hw, DMA rings), `iwl-fw.c` / `iwl-fw-load.c` (firmware TLV parse + DMA load → ALIVE → calibration), `iwl-hostcmd.c` (the host-command spine + scheduler), `iwl-nvm.c` (MAC + channel list from EEPROM/OTP), `iwl-rxon.c` (RXON config), `iwl-scan.c` (passive scan), and `iwl-ops.c` (the `wifi_ops` implementation + `iwl_wifi_bringup()`). Firmware is **auto-selected** by card family (`iwl_fw_candidates`, families 1000/5000/6000/6000-G2), and **RF-kill is detected** via `CSR_GP_CNTRL`.

The real radio is **never brought up at boot.** `iwl_wifi_bringup()` is called only by the post-desktop trigger tool `userspace/apps/iwlup` (via `SYS_NET_CONFIG`'s WLAN_BRINGUP flag) — a stall on a live machine with serial costs a retry, never the boot. Boot KATs (IWL-RXON / IWL-SCAN / IWL-FWSEL / IWL-FW) pass in QEMU, but the **RF tail has no emulator** and is iterated on the physical T410.

The control plane is six syscalls in `kernel/net/wlansyscall.c` (ABI in `kernel/include/uapi/wlan.h`): `SYS_WLAN_SCAN` (113), `CONNECT` (114), `STATUS` (115), `DISCONNECT` (116), `SET_KEY` (117), `DIAG` (124). `SYS_WLAN_DIAG` + `kernel/net/wifidiag.c` surface live bring-up diagnostics (the staged DETECTED → TRANS_OK → ALIVE → NVM_OK → REGISTERED ladder, family, RF-kill) as a "Radio:" line in the Network Manager. The supplicant `userspace/apps/wpasupp` does **WPA2** (PBKDF2, the 4-way handshake, CCMP/GCMP) and **WPA3-SAE** (dragonfly: PWE → commit → confirm → PMK), with the crypto in `userspace/lib/crypto/` and KAT-proven in the boot crypto battery. Full operator guide: `docs/T410_IWLWIFI.md`.

![Network Manager with live radio diagnostics](../../screenshots/netman_diag.png)

### Sound — Intel HD Audio

`HDA_ENABLE=1` compiles in the Intel HDA driver (`kernel/drivers/hda.c`, `hda_stream.c`, `kernel/drivers/audio/`). The codec is reached through the **Immediate Command Interface** (the CORB/RIRB ring path broke, so commands go through the ICI), and playback runs over a real DMA stream. The mixer is exposed as `SYS_AUDIO_*` syscalls (volume / mute / test-tone / status), and the **Sound Manager** app is `userspace/apps/soundman`. HDA is off by default because the real T410 controller can hang on reset; QEMU's emulated HDA works fine, and `AUDIO_SELFTEST=1` plays a boot tone to prove real DMA playback headlessly.

### AI agent rail — the OS automates itself, gated

`userspace/apps/agentd` is an OS-side, gated agentic loop: a host LLM (over the slirp seam, via `scripts/*_broker.py` / `nemotron_broker.js`) issues a `GOAL`/`TOOL`/`RESULT`/`DONE` protocol, and `agentd` is its **hands** — it dispatches capability-gated typed tools (the `tool_*` set: shell, file ops, spawn/kill, synthetic mouse + keyboard). **Every byte from the model is treated as hostile text:** each tool call passes a strict whitelist + path policy *before* any dispatch. `userspace/apps/cockpit` is the human-supervised GUI (live goal + steps, Allow/Deny/STOP); actions are recorded in a tamper-evident hash-chained audit ledger and are rollback-capable. The `aibroker` self-test (policy / tool-bus / ledger / rollback) is one of the 43 smoke checks.

![The AI cockpit](../../screenshots/cockpit.png)

### TLS — 1.2 **and** 1.3

The hand-rolled TLS stack (`userspace/lib/tls/`) now speaks **both TLS 1.2 and TLS 1.3** (RFC 8446; the 1.3 path — `tls13_handshake.c`, `tls13_keysched.c`, `tls13_record.c`, `tls13_certverify.c` — is offered with `TLS13=1` and is known-answer-proven against RFC 8448's test vectors). **Certificate trust is real**: `x509_verify_chain` (`x509_verify.c`) validates the chain to the bundled CA roots (`ca_bundle.c`). Key exchange is ECDHE over X25519 / P-256 (P-384 also implemented); CertificateVerify supports RSA-PSS and ECDSA. This is the transport under the from-scratch browser `userspace/apps/browser2` (its own DOM/HTML/CSS/layout + ES5 JS engine).

![browser2 over the hand-rolled TLS stack](../../screenshots/browser.png)

### IDE + on-device compiler, and the games

The **Semantic LEGO Map** IDE (`userspace/apps/ide`) presents code as a navigable blueprint map; **Ctrl+B / Ctrl+R** build and run on-device through the from-scratch C compiler `userspace/apps/cc`. `cc` compiles a careful **C subset** (single-file; no floats, no `switch`) — enough to self-host small programs, not full C11. The app suite also includes the standard desktop tools (calculator, paint, notes, `sheet`, taskman, photos, settings, filemanager, terminal) and ~18 games (snake, tetris, 2048, mines, breakout, pong, invaders, solitaire, chess, asteroids, sudoku, pacman, `zombietd`, `bubbletd` tower defenses, and `cube3d` / `ray` / `derby` on the from-scratch `g3d` software 3D renderer) — all proven to spawn and survive by the `GAMETEST=1` harness.

![The Semantic LEGO Map IDE](../../screenshots/ide.png)

---

## The T410 initrd-in-`.bss` rescue

This is the keystone fix that made real-hardware boot work, and it lives in **`kernel/arch/x86_64/boot.asm`** (the `[BITS 64] long_mode_start` block, label `.initrd_done`).

The problem: GRUB loads our initrd (Multiboot module 0) immediately after the kernel image. On the T410's fragmented E820 memory map that landing spot falls **inside** `[__bss_start, __bss_end]`. The kernel's `.bss` is a `NOBITS` section — it occupies no bytes in the ELF file, so GRUB sizes the post-kernel free space from the *file* sections and can't see it. The `rep stosq` that zeroes `.bss` during early boot would therefore wipe the compositor/init binary, and userspace spawn fails the instant the splash clears.

The fix: **before** clearing `.bss`, `boot.asm` reads module 0's `mod_start`/`mod_end` out of the Multiboot module struct, `rep movsb`-copies the initrd up to **16 MiB** (above `__bss_end`, which sits near ~4 MiB, and above the source), then rewrites `mod_start`/`mod_end` to point at the relocated copy. `kernel.c` then reads the safe copy transparently. It is a no-op in QEMU (where the module already lands outside `.bss`) and when no module is present.

The companion layout discipline lives in `kernel/linker.ld`: `.pagetables` is its own `NOBITS` section so the `.bss` clear can't destroy the early page tables, and large non-control-path buffers are emitted last (`.bss.deferred`) to keep critical kernel state packed below the `0x200000` userspace load base.

---

## What is actually compiled

The tree contains far more source than ships in the binary — staged drivers, alternative compositor milestones (`compositor_m2..m7`), SMP code, and experiments. **The build scripts are the ground truth for what is real.**

### Kernel — `scripts/quick_build.sh`

This script *is* the kernel source manifest. It assembles six `.asm` files (default build) and compiles the C kernel one file at a time, then links with `ld -T kernel/linker.ld` under a strict policy (undefined symbols are fatal). Compiled in by default:

- **Arch/boot**: `boot.asm`, `gdt.asm`, `interrupt.asm`, `syscall.asm`, `context_switch.asm`, `usermode.asm`; `gdt.c`, `idt.c`, `paging.c`, `syscall_init.c`, `madt.c` (read-only CPU enumerator), **`tlb_uni.c`** (the *single-CPU* TLB).
- **Memory**: `pmm`, `vmm`, `cow`, `slab`, `heap`, `vma`, `vma_region`, `vma_rbtree`.
- **Sched/IPC/signal**: `scheduler`, `process`, `context`, `waitqueue`, `nice`, `kill`; `clipboard`, `notify`, `ipc`, `shm`, `msgqueue`, `channel`.
- **Syscalls**: `handlers`, `syscall`, plus `futex`, `sendfile`, `epoll`, `poll`, `batch`, `vma_test`.
- **FS**: `vfs`, `vfs_dir`, `fs_registry`, `ext2`, `fat32`, `diskfs`, `page_cache`, `initrd`, `elf_loader`, `exec`.
- **Drivers**: `serial`, `pit`, `ps2`, `input/ps2mouse`, `rtc`, `rng`, `framebuffer`, `core/irq`, `pci`, `acpi/acpi`, `gpu/nvidia`, `storage/{block,ahci,ahci_block}`, `net/{e1000,rtl8139}`, **`hda` + `hda_stream` + `audio/{audio_core,audio_tone}`** (linked in; HDA *init* is still gated by `HDA_ENABLE`), `input/{input,evdev,dev_input}`, `pty/{pty,pty_dev}`, `core/bus`.
- **Net**: `net/{net,route,netif,netsyscall,socket,udp,tcp,net_testrig}` plus the **WiFi control plane** `net/{wlansyscall,wifidiag}` (always compiled — `SYS_WLAN_*` return ENOTSUP when no WiFi interface is registered).
- **Misc**: `lib/{string,printf,panic,perf}`, `stubs.c`, `core/procapi`, `core/usermode`, `security/namespace`.

**Conditionally compiled (env-gated, off by default):**

- `WIFI_SIM=1` → `net/wireless/sim/wifisim.c` (the simulated `wlan0`).
- `IWLWIFI=1` → the full `net/wireless/intel/iwlwifi/` driver set (`iwl-pci`, `iwl-fw`, `iwl-trans`, `iwl-hostcmd`, `iwl-fw-load`, `iwl-nvm`, `iwl-rxon`, `iwl-scan`, `iwl-ops`).
- `SMP=1` (and its sub-gates `SMP_SCHED`, `SMP_SCHED_DISPATCH`, `SMP_IPI`, `SMP_BKL`, …) → `lapic.c`, `ap_boot.c`, `ap_trampoline.asm`, `ipi.c`/`ipi_handlers.asm`, `bkl.c`, `kref.c`, `ownership.c`, `health_monitor.c`; output goes to `build/kernel-smp.elf`, leaving the default `kernel.elf` byte-for-byte unchanged.
- `PREEMPT=1` → `-DPREEMPTIVE` (gcc + nasm); output `build/kernel-preempt.elf`.
- `USB_UHCI=1` → the dormant `usb/{usb_core,uhci,hid}` (compiled, boot init still gated in `kernel.c`).

**Still explicitly NOT compiled** (per the comments in `quick_build.sh`): the staged NVMe driver and a duplicate `kernel/drivers/acpi/acpi.c` variant; the SMP `tlb.c` (the IPI TLB shootdown) is deferred in favor of `tlb_uni.c`. The HDA audio driver — once entirely compiled out for the same `.bss`/initrd-overlap reason — now links in by default; only its *boot init* is gated behind `HDA_ENABLE`.

### Userspace + ISO — `scripts/build_all.sh`

Builds the shared libs, the compositor (`compositor_m8.c` → `/sbin/compositor`), `init` (`userspace/init/main.c` → `/sbin/init`), and the full app/tool suite (each compiled freestanding, `-mstackrealign`, linked with `userspace/userspace.ld`). It then unpacks the existing initrd, copies the fresh ELFs into `/sbin` and `/bin`, runs a `fs:0x28` stack-canary check on every binary, re-tars `iso/boot/initrd.img`, copies `build/kernel.elf` into the ISO tree, and runs `grub-mkrescue` to emit `build/automationos.iso`.

The app suite spans the desktop apps and games, the from-scratch browser (`browser2`) and IDE, the on-device compiler (`cc`), the WiFi tools (`netman`, `wlanctl`, `wpasupp`, `iwlup`), the Sound Manager (`soundman`), the AI agent rail (`agentd` + the gated `tool_*` set + `cockpit` + `aibroker`), and a large battery of boot self-test probes (each prints a `<NAME>: PASS` marker the smoke test greps for). A few env flags shape the userspace side: `GAMETEST=1` runs the games/apps spawn-and-survive harness, `COCKPIT_PROOF=1` has `init` auto-launch the cockpit↔agentd seam proof headlessly, `SHOWCASE=1` stages a documentation scene, and `DESKTOP_MINIMAL`/`WIFI_DEMO` trim or pre-arm the desktop.

The boot contract that ties them: `grub.cfg` does `multiboot /boot/kernel.elf` + `module /boot/initrd.img`; `init` (PID 1) `SYS_SPAWN`s `sbin/compositor` first, then the terminal, file manager, and a long list of apps and self-test probes; if the compositor ever exits, `init` restarts it. The real iwlwifi radio is *not* in that list — it is brought up only when the operator runs `iwlup` from the desktop.

### Build-flag quick reference

| Flag | Effect | Default |
|------|--------|---------|
| `IWLWIFI=1` | Compile the real Intel iwlwifi DVM driver (bring-up still deferred to `iwlup`) | off |
| `WIFI_SIM=1` | Compile the simulated `wlan0` backend (scan/connect demo) | off |
| `HDA_ENABLE=1` | Enable Intel HD Audio init at boot | off |
| `TLS13=1` | Offer TLS 1.3 alongside TLS 1.2 | off |
| `SMP=1` | Multi-core build → `build/kernel-smp.elf` (sub-gates `SMP_SCHED`, …) | off |
| `PREEMPT=1` | Preemptive scheduler → `build/kernel-preempt.elf` | off |
| `IDE=1` | Stage the Semantic LEGO Map IDE | — |
| `GAMETEST=1` | Run the 25-game/app spawn-and-survive harness | off |
| `SHOWCASE=1` | Stage a documentation showcase scene | off |
| `DESKTOP_MINIMAL`, `WIFI_DEMO` | Trim / pre-arm the desktop | — |

(The default `build/kernel.elf` is byte-for-byte unchanged when every flag is unset.)

---

## Directory map

| Path | What lives here |
|------|-----------------|
| `kernel/` | The kernel. `kernel.c` (`kernel_main`), `linker.ld`, `stubs.c`. |
| `kernel/arch/x86_64/` | Long-mode entry (`boot.asm`), GDT/IDT, paging, syscall/context-switch/usermode asm+C, the single-CPU `tlb_uni.c` (and the un-compiled SMP files). |
| `kernel/core/` | `mem/` (pmm, vmm, heap, slab, cow, vma), `sched/` (scheduler, process, context), `syscall/` (dispatch + futex/epoll/sendfile/batch), `signal/`, `procapi/`. |
| `kernel/drivers/` | `serial`, `pit`, `ps2`, `rtc`, `rng`, `framebuffer`, `pci`, `acpi/`, `gpu/`, `storage/` (AHCI), `net/` (e1000/rtl8139 + `net/wireless/` = `sim/wifisim` + `intel/iwlwifi/` the real Intel WiFi driver), `hda`/`hda_stream` + `audio/` (Intel HD Audio), `input/`, `pty/`, `usb/` (gated). |
| `kernel/fs/` | VFS + `ext2`, `fat32`, `diskfs`, `page_cache`, ELF loader, `exec`. |
| `kernel/net/` | IPv4 stack: `net`, `route`, `netif`, `socket`, `udp`, `tcp`, `netsyscall`; WiFi control plane `wlansyscall` + `wifidiag` (ABI in `include/uapi/wlan.h`, seam in `include/wifi.h`). |
| `kernel/ipc/`, `kernel/security/`, `kernel/init/` | Shared memory / message queues / clipboard / notify; namespaces; `initrd.c`. |
| `kernel/include/` | Kernel headers (`kernel.h`, `sched.h`, `syscall.h`, `x86_64.h`, …). |
| `userspace/` | Userland. `init/` (PID 1), `compositor/` (`compositor_m8.c` is the live one), `crt0.asm`, `userspace.ld`. |
| `userspace/lib/` | Shared libs: `wl` (compositor client), `ui`, `font`/`bitfont`, `crypto` (incl. WPA2/WPA3-SAE primitives), `tls` (1.2 + 1.3 + X.509 chain verify), `net`, `js`, `dom`/`html`/`css`/`layout`, `imgcodec`, `game`, `g3d` (software 3D). |
| `userspace/apps/` | GUI apps + CLI tools: terminal, filemanager, editor, `browser2`, `ide`, `cc` (on-device C compiler); WiFi tools `netman`/`wlanctl`/`wpasupp`/`iwlup`; `soundman`; the AI agent rail `agentd` + `tool_*` + `cockpit` + `aibroker`; ~18 games (incl. `cube3d`/`ray`/`derby` on `g3d`); coreutils; and the boot self-test probes. |
| `boot/` | A separate experimental bootloader (`boot.asm`, `loader.c`, …). **Not** the active boot path — the shipped image boots via GRUB; the active long-mode entry is `kernel/arch/x86_64/boot.asm`. |
| `scripts/` | Build + run + test scripts. `quick_build.sh` (kernel), `build_all.sh` (userspace + ISO), `run-qemu.*`, `smoke*.sh`. |
| `docs/` | Documentation, including `docs/wiki/` (this wiki) and `docs/ROADMAP.md`. |
| `iso/` | The GRUB ISO staging tree: `iso/boot/grub/grub.cfg`, `kernel.elf`, `initrd.img`. |

---

## See also

- [Home](Home.md) — wiki landing page
- [Kernel Internals](Kernel-Internals.md) — memory, scheduler, syscalls, the cooperative model
- [Drivers & I/O](Drivers-and-IO.md) — PS/2, framebuffer, PCI, storage, networking
- [Desktop & Apps](Desktop-and-Apps.md) — the compositor, the SHM window protocol, the app suite
- [Building & Running](Building-and-Running.md) — `quick_build.sh`, `build_all.sh`, QEMU + T410
- [Roadmap](../ROADMAP.md) — what's done, in progress, and planned
