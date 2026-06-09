# Known limitations

This is a hobby OS, built in the open and honest about its edges.

- **Cooperative single-core by default.** The scheduler is cooperative (`SYS_YIELD`). Preemption
  (`PREEMPT=1`) and SMP (`SMP=1`) exist but are gated, experimental builds. Consequence: there is
  no second core to recover a hung syscall, so a tight ring-3 spin can monopolize the CPU and any
  unbounded wait in a syscall would freeze the machine — hence the iteration-cap discipline on all
  device waits.
- **SMP is not production-ready.** AP bring-up, per-CPU data, LAPIC/IPIs and a stress-validated
  compute-offload path exist, but full multi-core scheduling is experimental and off by default.
- **Legacy BIOS / CSM only.** Boot is GRUB Multiboot; there is no UEFI path.
- **T410 networking is gated off.** The Intel 82577LM PCH NIC stalls on an MMIO access during
  bring-up; opt in with `E1000_PCH_NIC=1`. QEMU's e1000 works fully.
- **USB on the T410 needs EHCI.** The Ibex Peak PCH is EHCI-only; the UHCI driver (which works in
  QEMU) finds no controller there. The [`USB-EHCI-0`](bricks/USB-EHCI-0.md) brick is in progress.
- **TLS is 1.2 only.** The client speaks TLS 1.2; the crypto library has TLS 1.3 primitives but no
  full 1.3 handshake yet.
- **Filesystems are limited.** ramfs root; ext2/FAT32 are **read-only**; durable storage is a flat
  `diskfs` blob (no general writable on-disk tree yet).
- **The browser is experimental.** Its DOM/HTML/CSS/layout/JS engines are from scratch and handle a
  useful subset, not the full web platform.
- **Some apps are experiments** at varying maturity; the core desktop, terminal, file manager, IDE,
  compiler and networking are the focus.

None of these are hidden in the smoke output — see [testing.md](testing.md).
