# known_good_images — which ISOs/tags are safe, which are contaminated

> Operational memory. The single most expensive mistake is flashing a contaminated image to the
> T410 and mistaking a userspace regression for a kernel/feature break.

## Known-GOOD (safe to flash / fall back to)
- **`build/automationos-t410-FIXED.iso`** (built 6/7) — the true known-good T410 desktop. Predates
  all of 6/8's IDE/desktop commits. Clean window titles, no debug markers. **Use this as the
  baseline + the userspace source for single-variable hardware test ISOs.**
- **tag `stable-t410-recovery`** — pushed known-good branch point.
- **tag `stable-smp-offload-nxe-pagingalias`** — SMP offload path validated (NXE fix).

## CONTAMINATED / do-not-trust
- `build/automationos-t410.iso` (rebuilt 6/8 17:15) — carries the 6/8 desktop regression
  (raw-path window titles + a stray window). NOT a safe fallback despite the name.
- `build/automationos-t410-usb.iso` (first cut) — contaminated userspace + a `SCHED_DEBUG=1`
  non-T410_SAFE kernel ("debug mode" yellow markers). Superseded.

## Clean single-variable test images (FIXED userspace + feature kernel)
- **`build/automationos-t410-usb-clean.iso`** — FIXED initrd + `T410_SAFE=1 SCHED_DEBUG=0 USB_UHCI=1`
  kernel. Both QEMU boots pass. (USB mouse still won't work on the T410 — EHCI-only chipset.)
- Build recipe: `build_test/usb_clean_iso.sh` (extract FIXED `initrd.img` via `xorriso -osirrox`,
  restage grub + the feature kernel, `grub-mkrescue`, re-run the no-device + device-present asserts).

## How to make a clean hardware test ISO
1. Build the kernel with the feature gate + `T410_SAFE=1 SCHED_DEBUG=0`.
2. Take the **FIXED** userspace (`iso/boot/initrd.img` extracted from `automationos-t410-FIXED.iso`),
   not the current `iso/`.
3. `grub-mkrescue` → a SEPARATE `*-clean.iso`; never overwrite a known-good image.
4. QEMU-prove no-device + device-present before flashing.
