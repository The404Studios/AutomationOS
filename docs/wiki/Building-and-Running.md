# Building and Running

AutomationOS builds under WSL Arch Linux (or any Arch/Linux with the toolchain). This page covers the toolchain, building the kernel and bootable ISO, running in QEMU, and flashing a USB stick to boot on a real ThinkPad T410.

## Toolchain

The build needs `gcc` (the host x86_64 gcc, used freestanding), `nasm`, `ld`, `grub-mkrescue` (plus `xorriso`/`mtools`), and `qemu-system-x86_64`. On Arch:

```sh
sudo pacman -S base-devel nasm grub xorriso mtools qemu-system-x86
```

The kernel and userspace are freestanding — no system libc. Userspace programs use the in-tree `userspace/libc` and `userspace/lib`. The build flags (see the `cc()` helper in `scripts/build_all.sh`) are:

```text
-ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2
```

## Building

Two scripts drive the build — one for the kernel, one for everything else.

```sh
bash scripts/quick_build.sh   # kernel only -> build/kernel.elf
bash scripts/build_all.sh     # userspace apps + initrd + the GRUB ISO
```

- `quick_build.sh` compiles the kernel from a hardcoded source list and links it with `kernel/linker.ld` into `build/kernel.elf` (strict — undefined symbols are fatal).
- `build_all.sh` compiles every userspace app, packs them into the initrd, installs the freshly-built kernel into the ISO tree, and runs `grub-mkrescue` to produce `build/automationos.iso`. It does not rebuild the kernel — run `quick_build.sh` first if you changed kernel code.

### Build-flag matrix (environment variables)

Optional subsystems are gated behind environment variables so the default `build/kernel.elf` stays byte-for-byte stable and risky or hardware-specific code only compiles in when you ask for it. Set the variable to `1` in front of the build command. Kernel-side flags go to `quick_build.sh`; init and userspace flags go to `build_all.sh`.

| Flag | Script | Effect |
|------|--------|--------|
| `IWLWIFI=1` | `quick_build.sh` | Compile the from-scratch Intel iwlwifi DVM driver (`kernel/drivers/net/wireless/intel/iwlwifi/`). Real radio, triggered post-desktop by `sbin/iwlup`, never at boot. T410 hardware. |
| `WIFI_SIM=1` | `quick_build.sh` | Compile the simulated WiFi backend (`kernel/drivers/net/wireless/sim/wifisim.c`) so a fake `wlan0` does scan/connect/DHCP in QEMU. |
| `HDA_ENABLE=1` | `quick_build.sh` | Compile in Intel HD Audio init (codec via the Immediate Command Interface, DMA stream playback). QEMU-safe, T410-unsafe. Required by the Sound Manager and `SYS_AUDIO_*`. |
| `SMP=1` | `quick_build.sh` | Gated multi-core build (`-DSMP_FOUNDATION`, BSP Local APIC + AP trampoline) -> `build/kernel-smp.elf`. Sub-gates (`SMP_SCHED`, `SMP_SCHED_DISPATCH`, `SMP_IPI`, `SMP_BKL`, `SMP_BATCH`, `SMP_RUNMASK`, `SMP_DSPLIT`, `SMP_THREAD_INHERIT`) layer on per-CPU scheduling. |
| `PREEMPT=1` | `quick_build.sh` | Gated preemptive scheduler (`-DPREEMPTIVE`) -> `build/kernel-preempt.elf`. The default scheduler is cooperative. |
| `IDE=1` | `build_all.sh` | init auto-opens the Semantic LEGO Map IDE (`-DIDE_AUTOSTART`). |
| `GAMETEST=1` | `build_all.sh` | init spawns `sbin/gametest` (`-DGAMETEST_RUN`), the 25-app spawn-and-survive harness (see the smoke section). |
| `SHOWCASE=1` | `build_all.sh` | init auto-opens a curated set of headline GUI apps (Sound Manager, AI cockpit, a game, the IDE) for documentation screenshots. |
| `TLS13=1` | `build_all.sh` | Build the TLS-1.3-capable client (`-DTLS13_CLIENT`): the stack offers and drives TLS 1.3 (RFC 8446) in addition to TLS 1.2. |
| `DESKTOP_MINIMAL=1` | `build_all.sh` | init spawns only the persistent desktop apps and skips the self-test storm — a faster, quieter boot. |
| `WIFI_DEMO=1` | `build_all.sh` | init headlessly auto-connects `wlan0` to the simulated `HomeNet` (WPA2) — needs a `WIFI_SIM` kernel. (`WIFI_DEMO_WPA3=1` targets the WPA3/SAE `SecureMesh`; `WIFI_DEMO_FAIL=1` proves the `WLAN_FAILED` path.) |

For example, to build a documentation image with simulated WiFi, sound, and the showcase apps:

```sh
WIFI_SIM=1 HDA_ENABLE=1 bash scripts/quick_build.sh
WIFI_DEMO=1 SHOWCASE=1 TLS13=1 bash scripts/build_all.sh
```

## Running in QEMU

Boot the ISO from the CD-ROM drive with an emulated NIC attached:

```sh
qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0
```

QEMU's emulated NIC is the Intel 82540EM (`-device e1000`), which the driver fully supports — ARP, DNS, TCP, and HTTPS all work in QEMU. Add `-smp 2` when running an `SMP=1` kernel. QEMU has no WiFi radio, so the real iwlwifi (`IWLWIFI=1`) RF tail can only be exercised on the T410; use the simulated backend (`WIFI_SIM=1`) to drive the WiFi control plane and Network Manager in QEMU.

### The smoke test

The smoke test boots the ISO headless and asserts a fixed set of checks.

```sh
bash scripts/smoke_boot.sh    # boots the ISO headless, asserts 43 checks
```

It boots diskless with the serial console logged to a file, then verifies that the kernel reaches the desktop, spawns processes, brings up networking, renders the browser, runs the on-device compiler, and exercises the crypto/TLS/web stack. A green run is 43/43 — the project's gate.

### The game/app harness (`GAMETEST=1`)

A separate harness proves the bundled apps actually launch and stay alive. Build the image with `GAMETEST=1` (see the flag matrix); init then spawns `sbin/gametest`, which spawns each of the 25 bundled games and apps in turn, waits, and confirms it is still running.

```sh
GAMETEST=1 bash scripts/build_all.sh
# boot the resulting ISO; on the serial log:
#   GAMETEST: <name> PASS alive pid=...   (per app)
#   GAMETEST: done 25/25 survived
#   GAMETEST: PASS
```

A green run is 25/25 (snake, tetris, game2048, mines, breakout, pong, invaders, solitaire, chess, asteroids, sudoku, pacman, zombietd, cube3d, ray, calculator, clockapp, settings, paint, notes, sheet, taskman, filemanager, bubbletd, photos).

## Flashing the ThinkPad T410 (real hardware)

Follow these steps to boot on the physical T410.

1. Build the ISO (above). It is a hybrid ISO, so it is USB-bootable as-is.
2. Write it to a USB stick with Rufus in DD mode (Windows) or `dd` (Linux):

   ```sh
   sudo dd if=build/automationos.iso of=/dev/sdX bs=4M status=progress conv=fsync
   ```

   Replace `/dev/sdX` with your USB device, and double-check it — `dd` is unforgiving.
3. Boot the T410 from USB (legacy/BIOS boot). It is RAM-rooted, so no internal disk is needed.

The T410 has no serial console, so on-screen green `[NN]` boot markers trace each subsystem as it loads; if it ever hangs, the last marker tells you exactly where. See [Architecture](Architecture.md) for the boot sequence and the initrd-rescue fix that made real-hardware boot reliable.

### T410 WiFi bring-up (real radio)

The real Intel iwlwifi radio has no emulator — its RF bring-up is iterated directly on the physical T410. Build with `IWLWIFI=1`, boot to the desktop, then run `sbin/iwlup` (it never runs at boot) to attach and start the card; the Network Manager then shows a live `Radio:` diagnostics line (`SYS_WLAN_DIAG`). The boot KATs (IWL-RXON / IWL-SCAN / IWL-FWSEL / IWL-FW) pass in QEMU, but association and the data plane are the next milestones on hardware. The full hardware loop — firmware staging, RF-kill, the diagnostic line, and what works versus what is still being iterated — is documented in [docs/T410_IWLWIFI.md](../T410_IWLWIFI.md).

## Toolchain gotchas

A couple of host-toolchain quirks are worth knowing before you debug a build.

- This WSL Arch `gcc` re-injects a stack canary (`fs:0x28`) into some shared-library objects even with `-fno-stack-protector`. The OS tolerates it (`ld -static` resolves the reference and it never trips), so a *linked* ELF showing `fs:0x28` is not a bug — verify the app's *own* object is canary-free.
- AutomationOS syscall numbers are not Linux's. Notably `SYS_WRITE = 3`, and syscall `1` is `SYS_FORK` — so an inline `write` using Linux's `rax=1` forks instead of writing. See [Kernel Internals](Kernel-Internals.md).

## See also

[Home](Home.md) · [Architecture](Architecture.md) · [Kernel Internals](Kernel-Internals.md) · [Drivers & I/O](Drivers-and-IO.md) · [Desktop & Apps](Desktop-and-Apps.md) · [Roadmap](../ROADMAP.md)
