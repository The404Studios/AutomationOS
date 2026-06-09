# Brick: USB-EHCI-0

> **Bricks** are small, isolated, hard-gated milestones — each on its own branch with a scoped
> design, explicit risks, acceptance tests, and merge criteria.

**Branch:** `brick/usb-ehci-0` (off `t410-recovery`, the known-good hardware baseline)
**Goal:** drive one wired boot-protocol HID mouse on the T410's EHCI-only PCH, injecting into the
same input layer as the PS/2 trackpad. Gated (`EHCI_USB=1`, default OFF), polling, no hotplug.
**Full design spec:** [`docs/superpowers/specs/2026-06-08-usb-ehci-0-design.md`](../superpowers/specs/2026-06-08-usb-ehci-0-design.md)

## Why

The T410's Ibex Peak PCH is **EHCI-only** — no UHCI controller for the existing `uhci.c` to bind.
The UHCI brick works in QEMU (emulated PIIX3 has UHCI) but finds nothing on the real laptop. A USB
mouse there needs an EHCI host-controller driver.

## The core principle — prove the routing first

Do **not** assume the mouse routes through an EHCI rate-matching-hub Transaction Translator (split
transactions). Checkpoint **E3** is a routing-ledger gate that reads real `PORTSC`/ownership
registers and decides: **RMH/split path · companion/UHCI path · no device**. `CONFIGFLAG=1` is set
only after that decision is understood and bounded; if low/full-speed routes to a UHCI companion,
EHCI does not fight it. No split-transaction code until E3 proves it is needed.

## Risks

- EHCI-for-a-mouse mandatorily needs split transactions + hub enumeration (heavier than UHCI).
- QEMU may not faithfully emulate the T410's rate-matching-hub path — the split-transaction path may
  be verifiable only on real hardware. The brick keeps an honest testability ledger.

## Safety gates (non-negotiable)

`EHCI_USB` default OFF · default build byte-for-byte unchanged · all waits iteration-capped ·
tick-independent delay only · no hotplug/keyboard/multi-device · FIXED-userspace test ISOs ·
QEMU first, T410 second.

## Checkpoints

| # | Scope | Status |
|---|---|---|
| E1 | gate + inert skeleton (no hardware access) | ✅ done |
| E2 | PCI discovery + MMIO map + BIOS handoff + bounded reset | ✅ done |
| E3 | port ownership / routing **ledger + decision** | ⏭ next |
| E4 | async control transfer | pending |
| E5 | RMH hub enumeration (if present) | pending |
| E6 | periodic split-transaction boot mouse → `input_report_*` | pending |
| E7 | smoke harness (no-device + mouse) | pending |
| E8 | T410 hardware (cursor moves, trackpad still works) | pending |

**E2 ledger (verified in QEMU):** discovers the controller, maps BAR0, takes ownership from the
BIOS (`USBLEGSUP`), HCRESETs, and logs `n_ports`/`n_cc` — `n_cc>0` + companion-owned port ⇒ UHCI
path; `n_cc=0` + EHCI-owned ⇒ RMH path.

## Acceptance

Builds with `EHCI_USB=1`; boots to desktop with and without a USB mouse; E3 decision is logged;
if RMH/split, the mouse moves the cursor; the PS/2 trackpad still works; no-device case never hangs.
