/*
 * EHCI Host Controller Driver (USB 2.0, Enhanced Host Controller Interface)
 * =========================================================================
 *
 * USB-EHCI-0 brick: drive ONE wired boot-protocol HID mouse on the T410's
 * Intel QM57 / Ibex Peak PCH, which is EHCI-ONLY -- it has no UHCI controller
 * for uhci.c to bind, so a real USB mouse there needs this driver.
 *
 * GATED behind -DEHCI_USB (EHCI_USB=1 build). The default build never compiles
 * or calls this, so it is byte-for-byte unchanged and never touches USB at boot.
 * Poll-driven (no IRQ): ehci_poll() runs from the pit timer tick, same cadence
 * and bounded discipline as the UHCI poll.
 *
 * Reuses the shared input_report_* and input_sync injection layer (input.c) --
 * the SAME path ps2mouse.c and uhci.c use, so there is no second mouse system.
 *
 * CHECKPOINT STATUS: E1 (gate + skeleton). ehci_init()/ehci_poll() exist and
 * are wired, but DO NOT touch any USB/MMIO hardware yet -- no controller
 * discovery, no register access. The real work lands later and in order:
 *   E2  PCI discovery + MMIO map + BIOS handoff + bounded reset
 *   E3  port ownership / routing ledger + the routing DECISION
 *          -> RMH/split path | companion/UHCI path | no device
 *   E4+ async control transfers / RMH hub enumeration / periodic split-
 *       transaction boot mouse -- pursued ONLY if E3 proves the mouse actually
 *       routes through an EHCI rate-matching-hub TT (never assumed).
 *
 * Scope: kernel/drivers/usb/* and kernel/include/usb.h only.
 */

#include "../../include/kernel.h"
#include "../../include/types.h"

/*
 * ehci_init -- E1 skeleton: gated, wired, and intentionally inert.
 * Returns 0. Performs NO hardware access (no PCI scan, no MMIO). Exists so the
 * gate + wiring + build can be proven before any controller code is written.
 */
int ehci_init(void) {
    kprintf("[EHCI] skeleton (E1): EHCI_USB gated build active; controller "
            "bring-up + routing ledger deferred to E2/E3 (no MMIO access yet)\n");
    return 0;
}

/*
 * ehci_poll -- E1: nothing to poll (no controller selected). Bounded no-op;
 * runs in IRQ context from timer_handler(), so it must stay trivial.
 */
void ehci_poll(void) {
    /* intentionally empty until E2/E3 select a mouse path */
}
