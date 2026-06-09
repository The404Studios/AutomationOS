# Hardware

AutomationOS is developed in QEMU but validated on real, decade-old hardware. The real-hardware
target is the strongest credibility claim this project makes.

## Reference machine — Lenovo ThinkPad T410 (2010)

| Component | Detail |
|---|---|
| CPU | Intel Core i5-M520 ("Westmere", 32 nm) |
| Chipset / PCH | Intel QM57 (Ibex Peak-M) |
| Graphics | NVIDIA NVS 3100M (linear framebuffer via GRUB VBE) |
| Input | PS/2 keyboard + Synaptics trackpad |
| Boot | Legacy BIOS / CSM, from USB |

## What works on the T410

| Capability | State |
|---|---|
| Boot from USB → compositor desktop | ✅ working |
| PS/2 keyboard + trackpad | ✅ working |
| Framebuffer graphics | ✅ working |
| External USB mouse | 🚧 in progress (see below) |
| Ethernet (Intel 82577LM) | ⚙️ gated off by default |

## Known hardware issues

- **USB is EHCI-only.** The Ibex Peak PCH has no UHCI/OHCI companion controllers — full/low-speed
  devices ride EHCI rate-matching hubs. The existing UHCI driver therefore finds no controller on
  the T410 (it works in QEMU, whose emulated PIIX3 *does* have UHCI). An external USB mouse needs
  an EHCI driver: the [`USB-EHCI-0`](bricks/USB-EHCI-0.md) brick. A cheap interim option is the
  BIOS **"USB Legacy Support"** setting, which can SMM-emulate a USB mouse as PS/2.
- **Ethernet (82577LM)** stalls on an MMIO access during bring-up; it is gated off (`E1000_PCH_NIC=1`
  to opt in) so the default image always reaches the desktop.
- **Cooperative single-core** means there is no second core to recover a hung syscall, so every
  device wait must be iteration-capped (the "frozen-tick" discipline). See
  [known-limitations.md](known-limitations.md).

## Build profile for the T410

The T410-safe profile disables modern-CPU fast paths (the ERMS REP-string copy) and the on-screen
scheduler debug markers, and keeps boot output quiet:

```bash
# kernel, T410-safe profile
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh
# stage + package the ISO (build_all builds userspace; then re-stage the T410 kernel)
bash scripts/build_all.sh
T410_SAFE=1 SCHED_DEBUG=0 bash scripts/quick_build.sh   # ensure the staged kernel is T410-safe
# write build/automationos.iso to a USB stick and boot in legacy BIOS / CSM mode
```

| Flag | Effect |
|---|---|
| `T410_SAFE=1` | disable ERMS REP-string fast path (Westmere-safe word copy) |
| `SCHED_DEBUG=0` | suppress the yellow on-screen scheduler markers (cleaner desktop) |
| `E1000_PCH_NIC=1` | opt in to the 82577LM NIC (otherwise gated off) |
| `EHCI_USB=1` | compile the in-progress EHCI USB driver |

Tip: capture a known-good baseline ISO before changing the build, and tag it (see the release
discipline in [the contributing guide](../CONTRIBUTING.md)).
