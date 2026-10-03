# Real GPU drivers -- how, what's realistic, and the licensing rules

Date: 2026-10-02. Written from public documentation and project pages. **Nothing here copies code from
any other project.** Implementation will be written from hardware documentation and specifications
(clean-room); the ledger section says exactly what may be referenced.

## "Look at BoredOS" -- what it actually does

There are several unrelated projects called BoredOS. None ships a real GPU driver:

* `BoredOS/BoredOS` (GPLv3): boots via Limine, has a "Nova" compositor and TinyGL (software OpenGL).
  Its `drivers/` holds ACPI and I2C; `dev/` holds ac97, ahci, pci, ps2, rtc, serial; NICs are
  e1000/RTL/VirtIO. Graphics = a bootloader framebuffer plus software rendering.
* `zerfithel/BoredOS`, `TheArchitectEngineer/BoredOS`: forks of the above.
* `BoringOS` (MIT, a different project): its README says "there is no native AMD/NVIDIA/Intel modesetting
  or acceleration driver yet".

So "the way BoredOS does it" is the bootloader framebuffer + software compositor -- which is **already what
AutomationOS does**. We also must not copy from it (GPLv3 would pull its license into this tree).

## How other independent OSes get GPU support (architecture + license only)

| OS | GPUs | level | approach | upstream license |
|---|---|---|---|---|
| SerenityOS | Bochs, VirtIO-GPU, VMware SVGA, Intel (Gen4 only) | modeset + flush | native kernel drivers | BSD-2 |
| Haiku | Intel (i845..SandyBridge), Radeon HD | modeset (Ironlake: no HW cursor/2D/3D) | native add-on drivers | MIT |
| Redox | VESA/GOP, virtio-gpu, new Intel modeset (Kaby/Tiger Lake) | modeset; GL via Mesa llvmpipe | native | MIT |
| Managarm | NVIDIA via nvidia-open (Turing+) | modeset | ported open kernel modules behind own DRM | MIT/GPL |
| Genode | Intel Gen8/9 multiplexer | accelerated via Mesa iris | native + DDE | AGPLv3 |
| FreeBSD | i915/amdgpu/radeon | accelerated | LinuxKPI shim running Linux DRM | BSD/MIT/GPLv2 |
| Fuchsia | Intel Skylake/Kaby/Tiger Lake | display | native C++ | BSD-style |
| ReactOS | loads closed Windows drivers | -- | -- | GPL |

Takeaway: **nobody hobby-scale has accelerated 3D without either a Linux-DRM compatibility layer or a
multi-year effort.** Modeset-only native drivers are the common, achievable tier.

## This hardware (ThinkPad T410)

The GPU is one of: Intel Arrandale integrated ("Ironlake", Gen5), NVIDIA NVS 3100M (GT218, nouveau "NVA8",
NV50/Tesla family, PCI 10de:0a6c), or both with Optimus/switchable graphics depending on the SKU and the
BIOS setting (Config > Display > Graphics Device). **Which one drives the panel is not knowable from
here -- the T410's `lspci` output decides everything.**

* **Intel Ironlake display engine** -- Intel's public PRMs (Vol3 Part2 CPU display, Part3 PCH display) document
  pipes/planes (CPU) and FDI/ports/panel power (PCH). Minimal path: *take over the BIOS-configured mode* and
  flip surfaces: write `DSPASURF` (plane surface offset in the graphics aperture, mapped via the global
  GTT) for a double-buffered flip at vblank; the BIOS already did panel power, FDI, PLLs and timings.
  Backlight = PWM registers. Needs a second GTT-mapped surface; GTT/stolen-memory details to be taken from
  the PRM and verified on hardware.
* **NVIDIA GT218** -- register knowledge lives in the envytools `rnndb` XML and nouveau (MIT). Modeset needs
  the EVO core-channel push buffer, VBIOS/DCB parsing and PLL programming. **No firmware blob** is needed
  for modeset/2D/3D on this generation (PGRAPH context programs are generated in-kernel); only video decode
  needs extracted blobs. Reclocking is incomplete, so it runs at boot clocks.
* If the panel is wired to only one GPU (discrete-only units), taking over the *other* one yields a blank
  screen -- hence "identify first".

## Acceleration options, honestly

1. **virtio-gpu 2D** (QEMU-provable): GET_DISPLAY_INFO, RESOURCE_CREATE_2D, ATTACH_BACKING, SET_SCANOUT,
   TRANSFER_TO_HOST_2D, RESOURCE_FLUSH. A real GPU-style driver (command queues, resources, scanout) we can
   write from the OASIS virtio spec and prove end to end.
2. **Ironlake plane takeover + vsync flip** (hardware-validated by you).
3. **SSE-optimised software compositor** -- the actual biggest win on this hardware today (see below).
4. Optional later: software OpenGL (Mesa llvmpipe/softpipe) once a POSIX libc + threads exist.
5. Native 3D on the T410 (Mesa crocus for Ironlake -> GL 2.1; nouveau nv50 -> GL 3.3) needs a DRM/GEM/
   command-submission kernel layer: multi-year.

## What is wrong with the display *today* (from the audit, not research)

Native resolution is not requested (`gfxpayload` has no 1440x900/`auto`), write-combining is MTRR-only and
unverified, the compositor's `present_diff` scans all ~1M pixels per frame ignoring damage, the clock tick
forces a full composite, `draw_cursor` reads the hardware framebuffer, and `simd_blit.c` is not built.
Fixing those is low-risk and gives the T410 a big, immediate improvement independent of any GPU driver.

## Plan (ordered)

1. `GPU-ABSTRACT-0`: a `gpu_ops` interface (detect, set_mode, get_scanout, flip, wait_vblank, set_brightness)
   behind which the firmware framebuffer is the first implementation. Default behaviour unchanged.
2. `VIRTIO-GPU-0`: virtio-gpu 2D driver proven in QEMU (modeset to non-default resolutions, flush).
3. Compositor present path: damage-bounded scan, SSE2/`movntdq` blits, native resolution request.
4. `INTEL-GEN5-0`: Ironlake plane takeover + flip + backlight, **gated default-OFF** (project law) behind a
   runtime trigger with a serial/on-screen marker ladder, written clean-room from the PRM.
5. `NV50-0`: GT218 detect -> VBIOS shadow -> EVO modeset, same gating, only if the T410 turns out to be
   discrete-only.

## Licensing ledger -- what may be referenced

* **Intel PRMs** (IHD_OS_V3Pt2/V3Pt3, (c) Intel 2010; CC attribution / no-derivatives, no IP license):
  *reference only*; implement the registers; do not redistribute or paste tables. Cite by document ID.
* **Haiku `intel_extreme`** (MIT), **nouveau nvkm** and **envytools rnndb** (MIT), **Mesa** (MIT): may be
  *read as reference*; if any code is ever adapted, keep the copyright + permission notice and list it in
  `PORTED_CODE.md`.
* **virtio spec v1.2** (OASIS): implement from the spec.
* **GPL/AGPL sources (BoredOS, Linux DRM/simpledrm, Genode, ReactOS): not used.** Do not open them while
  writing the corresponding driver.
* Not legal advice.

---

## Addendum -- deep-research findings (2026-10-02, web research; [P] primary source, [S] secondary, [I] inference)

**What the "NVIDIA driver" request really is.** Whatever GPU the BIOS enables, GRUB's VBE linear framebuffer already gives
panel-native output. The measured, cheap, large win for a *software compositor* is **memory type**, not a GPU driver.

* T410 variants [P: Lenovo HMM 63y0535, https://download.lenovo.com/ibmdl/pub/pc/pccbbs/mobiles_pdf/63y0535.pdf]: Intel-only
  boards (no NVIDIA chip on the board), "discrete 256 MB" boards (NVS 3100M = GT218M, 10de:0a6c), and Optimus models (BIOS 1.32+
  [S]). BIOS menu Config > Display > Graphics Device: Integrated / Discrete / Optimus [S]. Optimus is muxless: the Intel IGP drives
  the internal panel [I, from kernel.org vga-switcheroo]. Which GPU drives the rear VGA/DisplayPort in each mode: **unknown, log it
  on the machine**. VBE per BIOS mode: no primary source; [I] the active boot-VGA device's VBIOS serves int10h, so Discrete and
  Optimus are different test cases -- test both. Intel IGP = Ironlake 8086:0046.
* Write-combining [P: kernel.org mtrr.html "2.5x or more"; Intel WC app note 24442201: UC 8 MB/s vs WC 100+ MB/s on the PCI-era
  platform; S: FreeBSD forum GT210 flood fill 202.7 MB/s UC vs 1200 MB/s WC]. PAT can turn a BIOS-UC framebuffer into WC
  (SDM Table 11-7); an overlapping UC MTRR wins if only MTRRs are used [I]. Estimate [I]: a 1440x900x4 frame is 5.2 MB = ~26 ms at
  200 MB/s (UC) vs ~4 ms at 1.2 GB/s (WC). Ranking: (1) UC->WC, (2) never read/blend in the LFB, keep the back buffer in system
  RAM, (3) dirty rects, (4) copy instruction (second-order; Arrandale has SSE4.2, no ERMSB).
* GT218 programming [P unless noted]: modesetting and 2D need **no firmware blob** (nouveau generates the PGRAPH ctxprog itself;
  PCOPY falcon microcode is in-tree). NVIDIA open-gpu-doc (MIT) has **no Tesla register manual** -- only class headers
  (`twod/cl502d.h`, `display/cl857d.h` EVO core channel, `dma-copy/cl85b5.h`, DCB 4.x spec); envytools rnndb (MIT) has the
  register database but its PDISPLAY prose is ".. todo". VP4 video decode needs NVIDIA-extracted firmware (not redistributable;
  irrelevant to desktop graphics). nouveau reclocking is partial, so the GPU would sit at boot clocks [I].
* Intel Ironlake [P]: PRMs (Vol 3 Part 2 CPU display, Part 3 PCH display) are CC BY-ND -- share with attribution, do not
  redistribute altered text. Native modeset is FDI training + PCH DPLL + transcoder (not a few writes). Backlight registers:
  CPU `BLC_PWM_CTL2` 0x48250 / `BLC_PWM_CTL` 0x48254; PCH 0xC8250 (enable bit 31) / 0xC8254 (frequency bits 31:16). In Discrete
  mode the backlight is driven by the NVIDIA PWM [S, ThinkWiki/Debian: `EnableBrightnessControl=1`]; whether the T410 backlight is
  EC- or GPU-PWM is **unverified** (the DSDT `_BCM/_BQC` methods would settle it).
* Reference licensing: Haiku `intel_extreme` (MIT, verified headers) and Intel's PRMs are the cleaner Intel references;
  envytools + open-gpu-doc (MIT) are the cleaner NVIDIA ones. nouveau and OpenBSD drm files carry MIT headers **inside a GPL-2 /
  mixed tree**: whether that satisfies "don't copy anyone / no GPL" is the owner's decision -- until decided, only
  envytools/open-gpu-doc/Haiku/PRMs are used as references. coreboot libgfxinit and Linux i915 are GPL: facts only, no code.

**Recommended ladder** (smallest safe step first; all hardware-affecting steps default-OFF):

| step | provable in QEMU | needs the T410 | effort |
|---|---|---|---|
| (i) framebuffer write-combining via PAT (+ non-temporal copies, dirty rects, back buffer in RAM) | MSR/page-table bits, copy correctness, byte-exact output (not the speed) | measure UC vs WC MB/s in each BIOS mode | days |
| (ii) read-only hardware report (PCI ids/BARs, VBE modes + LFB base, MTRR/PAT dump, CPUID, VBIOS signature) | PCI + VBE parsing vs QEMU's Bochs VGA | real values; check memory-enable before touching dGPU MMIO | days-2 weeks |
| (iii) backlight (read the BIOS-set PWM first; then, gated, write duty cycle with a minimum clamp; different path in Discrete) | no | yes | 1-3 weeks |
| (iv) native modeset (Intel FDI/DPLL or NVIDIA EVO + VBIOS DCB) | no | yes, blank-screen risk | multi-month each; not recommended |
| (v) 2D acceleration (PFIFO/VM/PGRAPH or Intel blitter) | no | yes | multi-month, little payoff for a software compositor |
