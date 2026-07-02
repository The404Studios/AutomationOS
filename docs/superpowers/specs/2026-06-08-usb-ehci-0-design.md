# USB-EHCI-0 Design — Boot-Protocol HID Mouse over EHCI on the T410

**Status:** approved (2026-06-08), routing-ledger edit incorporated.
**Branch:** `brick/usb-ehci-0` (isolated off `t410-recovery`; that branch is the known-good physical-hardware baseline and stays pristine).
**Goal:** Let one wired boot-protocol HID mouse move the same cursor as the PS/2 trackpad on the ThinkPad T410, whose Intel QM57 / Ibex Peak PCH is **EHCI-only** (no UHCI controller for the existing `uhci.c` to bind). Gated, polling, QEMU-first, T410-second.

---

## 0. Founding constraint — do NOT assume the routing

The T410's PCH integrates **EHCI controllers plus rate-matching hubs (RMH)**; the documented expectation is that a low/full-speed mouse appears behind an RMH's Transaction Translator (TT) and must be reached with EHCI **split transactions**. **This brick treats that as a hypothesis to be PROVEN by a routing ledger, never as a given.** UHCI already burned us by passing on QEMU and failing on hardware; we will not repeat that by assuming the EHCI/RMH path before the controller's own registers confirm it.

**Routing decision rule (the heart of the brick):**
- After BIOS handoff, **log and verify port routing** from real registers before any transfer work.
- If a port exposes an **RMH/TT path** (high-speed hub downstream of EHCI, owning the low/full-speed device) → enumerate it and use EHCI **split transactions**.
- If low/full-speed ports **route to UHCI companions** → **do not fight routing.** Let UHCI own that mouse path (the existing `uhci.c` is the correct driver there). EHCI does not force ownership.
- **`CONFIGFLAG=1` (route all ports to EHCI) is allowed ONLY after the routing decision is understood and bounded** — not as a blind bring-up step.

This keeps the brick honest: E3 (the ledger) tells us whether we are even building the correct path before we invest in split-transaction transfer code.

---

## 1. Scope (YAGNI)

**In:** exactly one wired **boot-protocol** HID mouse; **a single *selected* path after the routing decision** — the T410 may expose two EHCI controllers, so enumerating both is fine, but only one chosen mouse path proceeds past E3; **polling** (no IRQ); enumerate → poll → inject; reuse the shared input layer.

**Out (explicitly deferred):** hotplug, USB keyboards, hubs beyond the one RMH needed to reach the mouse, multiple simultaneous devices, IRQ-driven operation, isochronous/bulk, power management, runtime suspend. These are later bricks (USB-HID-1, etc.).

---

## 2. Architecture & components

**New file `kernel/drivers/usb/ehci.c`** — self-contained, mirroring `uhci.c`'s shape and the existing stack boundaries. `usb_core.c` remains an unused simulation stub (same as the UHCI path). `ehci.c` owns: PCI discovery, MMIO mapping, BIOS handoff, the routing ledger, schedule setup, (conditional) RMH enumeration, (conditional) split-transaction transfers, a boot-mouse decoder, and the poll loop.

**Reuse — the win.** The mouse report is injected through the **shared `input_report_rel` / `input_report_key` / `input_sync` layer** (`input.c`) that `ps2mouse.c` and `uhci.c` already use. A ~15-line boot-mouse decoder in `ehci.c` (mirroring `uhci.c`'s `process_mouse`: buttons byte → `BTN_LEFT/RIGHT/MIDDLE`, signed dx/dy → `REL_X/REL_Y`, wheel → `REL_WHEEL`) feeds it. The compositor and input core are untouched; no new input path.

**Wiring (gated).** `ehci_init()` from `kernel.c` after `ps2_init()`, `ehci_poll()` from the `pit` `timer_handler` (every 8th tick, same as UHCI), both `#ifdef EHCI_USB`.

### Component responsibilities
- **Discovery:** scan PCI for EHCI class `0x0C/0x03/progif 0x20`. The T410 has **two** EHCI controllers — enumerate both; the routing ledger decides which (if any) owns the mouse.
- **MMIO map/validate:** BAR0 is memory-mapped (unlike UHCI's I/O BAR). Validate it is an MMIO BAR, read `CAPLENGTH`/`HCIVERSION`/`HCSPARAMS`/`HCCPARAMS`, locate the operational register window at `base + CAPLENGTH`.
- **BIOS handoff:** follow `HCCPARAMS.EECP` to the USBLEGSUP capability; set OS-Owned, bounded-wait for BIOS-Owned to clear, clear SMI enables in USBLEGCTLSTS.
- **Reset:** `USBCMD.HCRESET`, bounded-wait for clear; `CTRLDSSEGMENT=0`.
- **Routing ledger (E3):** read every port's `PORTSC` (owner bit, connect, enabled, line state/speed); emit the ledger (§5); take the routing decision (§0).
- **Schedules (only if EHCI path chosen):** allocate/program `PERIODICLISTBASE` (4 KB-aligned, 1024-entry frame list) and `ASYNCLISTADDR` (a dummy async QH ring); enable async + periodic in `USBCMD`; set port power; `CONFIGFLAG=1` **only now**, post-decision.
- **RMH enumeration (if present):** standard device enumeration of the high-speed hub (GET_DESCRIPTOR device/config/hub, SET_ADDRESS, SET_CONFIGURATION), read TT info, walk downstream ports for the low/full-speed mouse.
- **Transfers (split):** control on the async schedule, interrupt-IN on a periodic QH at the mouse's `bInterval`; QH carries TT hub-addr/port + S-mask/C-mask; qTDs do start-split/complete-split. Standard requests: SET_ADDRESS, GET_DESCRIPTOR, SET_CONFIGURATION, **SET_PROTOCOL=boot**, SET_IDLE(0).
- **Poll:** `ehci_poll()` checks the periodic qTD status; on completion → decode → inject → re-arm. Bounded; returns quickly.

### Data structures (EHCI-spec, physically addressed, aligned)
- Periodic frame list: `1024 × uint32`, 4 KB-aligned.
- **QH** (Queue Head): horizontal link; endpoint characteristics (device addr, ep#, speed, max packet, data-toggle control); endpoint capabilities (**split:** hub addr, port number, S-mask, C-mask, mult); qTD overlay. 32-byte aligned.
- **qTD**: next/alt-next pointers, token (status, PID, total bytes, data toggle, IOC, C-Page, error counter), 5 × 4 KB buffer pointers. 32-byte aligned.
- All allocated from the kernel's physical allocator (the same path `uhci.c` uses for its frame list).

---

## 3. Data flow

1. `ehci_init()` (gated) → discover controllers → map+validate MMIO → BIOS handoff → HCRESET.
2. **Routing ledger** → decision: *RMH/split path* | *companion/UHCI path* | *no device*.
3. If *companion/UHCI*: log it, do nothing further (UHCI owns it); return cleanly.
4. If *RMH/split*: program schedules, `CONFIGFLAG=1`, enumerate the RMH, find + enumerate the mouse, SET_PROTOCOL=boot, arm a periodic split interrupt-IN QH.
5. `ehci_poll()` every 8th tick: qTD complete? → `process_mouse(buf, len)` → `input_report_*` + `input_sync` → re-arm.
6. Cursor moves through the existing input → compositor path; PS/2 trackpad unaffected (separate input source).

---

## 4. Error handling & safety — NON-NEGOTIABLE

These are mandatory and carry over verbatim from the project's hardware laws:

- **`EHCI_USB` default OFF.** The default build is **byte-for-byte unchanged** (gated compiles + gated init/poll). No default-T410-image contamination.
- **Every wait iteration-capped** — HCRESET, BIOS handoff, port reset, qTD completion, hub enumeration. No unbounded device-controlled spin (frozen-tick discipline: a stuck controller must time out, never hang the single core).
- **Tick-independent delay only** — bounded `inb(0x80)`/IF-independent udelay (the `uhci_udelay` pattern); no reliance on the timer tick (syscalls/IRQs run IF=0).
- **MMIO BAR validated** before any access; graceful **no-controller / no-device / wrong-routing** returns (no hang).
- **No hotplug. No keyboard. No multi-device.** One mouse, one path.
- **Test ISOs use FIXED userspace** (`automationos-t410-FIXED.iso`'s initrd) + the EHCI kernel built `T410_SAFE=1 SCHED_DEBUG=0 EHCI_USB=1`, so test images never carry the desktop regression or debug markers.
- **QEMU first, T410 second.**

---

## 5. Required boot logs — the routing ledger (E2/E3)

These lines are part of the deliverable; they exist so we never guess at routing:

```
[EHCI] controller bus:dev.fn vendor/device
[EHCI] BAR0 mmio base=... size=... valid=...
[EHCI] caplength=... hcsparams=... hccparams=... eecp=...
[EHCI] BIOS owned before=... after=...
[EHCI] OS owned before=... after=...
[EHCI] configflag before=... after=...
[EHCI] port N status=... owner=... enabled=... connect=... line=...   (per port)
[EHCI] decision: RMH/split path | companion/UHCI path | no device
```

---

## 6. Testing & the honest testability ledger

- **QEMU rig:** `q35` (ICH9 EHCI) and/or `-device usb-ehci` (+ a high-speed hub if QEMU supports a TT) with `-device usb-mouse`. QEMU's ICH9 model routes full/low-speed devices to **UHCI companions**, which may **not** exercise the EHCI split-transaction/TT path the T410 needs. **We document exactly what QEMU covers vs. what it cannot** (a written testability ledger in the brick) — no silent gaps. Minimum QEMU coverage targets: discovery, MMIO map, BIOS handoff, HCRESET, schedule run, port-status reads, and the routing ledger output itself.
- **Smoke harness:** mirror `build_test/usb_smoke.sh` — assert (a) no-device boots clean (no hang), (b) device-present boots clean, on `[EHCI]` markers + desktop-reached + no-panic.
- **T410 hardware (the real gate):** serial-confirm the ledger, then — only if the decision is RMH/split — confirm the mouse enumerates and the cursor moves, and the trackpad still works. If the decision is *companion/UHCI*, that is itself a finding that redirects the work.

---

## 7. Checkpoints (hard-gated, one commit each)

- **E1** — gate + skeleton (`EHCI_USB` in `quick_build.sh`; empty `ehci_init`/`ehci_poll`; wiring in `kernel.c`/`pit.c`; default build unchanged).
- **E2** — PCI discovery + MMIO map + BIOS handoff + bounded HCRESET (+ the §5 logs through `configflag`).
- **E3** — **port ownership / routing ledger + the routing decision** (the gate that proves the path; §0, §5).
- **E4** — async control transfer (e.g. GET_DESCRIPTOR) — only on the path E3 selected.
- **E5** — RMH hub enumerate (if present).
- **E6** — periodic split-transaction boot mouse → `input_report_*` injection.
- **E7** — smoke harness (`build_test/ehci_smoke.sh`).
- **E8** — T410 hardware (serial ledger → cursor move → trackpad still works).

Commit split mirrors the UHCI brick: `build(usb): gate ehci.c behind EHCI_USB` → `feat(usb): EHCI discovery, MMIO map, BIOS handoff, reset` → `feat(usb): port routing ledger + decision` → `feat(usb): async control transfers` → `feat(usb): RMH hub enumeration` → `feat(usb): periodic split-transaction boot mouse -> shared input` → `test(usb): EHCI no-device + mouse smoke`.

---

## 8. Parallel free experiment (not part of the brick)

On the next T410 reboot, enable **BIOS "USB Legacy Support."** If the BIOS SMM emulates the external USB mouse as a PS/2 device, the existing `ps2mouse` driver reads it with **zero new code** and EHCI becomes *optional for now*. We still ship this spec and (eventually) the driver, because real USB support is needed long-term — but a working legacy path would let us defer E4–E8.

---

## 9. Reuse / boundaries summary

- **Reused unchanged:** the `input_report_rel/key` + `input_sync` injection layer (`input.c`); the gating pattern + bounded-udelay discipline from `uhci.c`; the FIXED-userspace test-ISO method (`build_test/usb_clean_iso.sh`).
- **New & self-contained:** `kernel/drivers/usb/ehci.c` + minimal `usb.h` additions; `build_test/ehci_smoke.sh`.
- **Untouched:** `usb_core.c` (stub), `uhci.c`, compositor, input core, the default build.
