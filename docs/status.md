# Status

Honest stable-vs-experimental breakdown. **Stable** = relied on every boot. **Working** = does
its job but still maturing. **Experimental** = exists and runs, gated or rough. **Planned** =
scoped, not built.

## Matrix

| Area | Status | Build flag | Notes |
|---|---|---|---|
| Boot | Stable | — | GRUB Multiboot, legacy BIOS / CSM (not UEFI) |
| Memory | Stable | — | PMM, 4-level paging, slab + heap, on-demand growth |
| Scheduler (single-core) | Stable | default | cooperative `SYS_YIELD`, fork + copy-on-write, execve ELF |
| Preemption | Experimental | `PREEMPT=1` | timer-driven preemption; opt-in build |
| SMP | Experimental | `SMP=1` | AP startup, per-CPU data, LAPIC/IPIs, TLB shootdown, a stress-validated compute-offload path; **not** the default, not production-ready |
| PS/2 input | Stable | — | keyboard + trackpad; validated on the T410 |
| Compositor desktop | Stable | — | windows, edge-snap, hover-magnify dock, eased animations |
| IDE + on-device `cc` | Working | — | single-file C/ASM → ELF, Ctrl+B build, run as a process |
| ramfs root | Stable | — | system is RAM-rooted from initrd |
| ext2 / FAT32 | Partial | — | **read** support via the VFS registry |
| diskfs (durable) | Partial | `DISK_PERSIST=1` | flat durable blob over AHCI; survives reboot |
| Networking (QEMU) | Working | — | e1000 + ARP/IPv4/ICMP/UDP/TCP + DNS + BSD sockets |
| Networking (T410) | Gated off | `E1000_PCH_NIC=1` | Intel 82577LM PCH MMIO stall during bring-up; default off |
| TLS | Partial | — | **TLS 1.2 client** works (HTTPS); 1.3 primitives exist; 1.3 handshake planned |
| Browser | Experimental | — | own DOM / HTML / CSS / layout + ES5-subset JS |
| USB mouse (UHCI) | Working (QEMU) | `USB_UHCI=1` | enumerates a boot mouse on UHCI hardware (QEMU PIIX3); the T410 has no UHCI |
| USB mouse (EHCI) | In progress | `EHCI_USB=1` | `USB-EHCI-0` brick for the T410's EHCI-only PCH; routing ledger landed (E1/E2) |
| Durable projects | Planned | — | `persistfs` over `diskfs`, T410-safe gated |

## Gating philosophy

New or risky hardware support is **default-OFF behind a build flag** until it is proven on real
hardware. A default build is byte-for-byte the validated configuration. This is why the matrix
lists explicit flags: `PREEMPT`, `SMP`, `DISK_PERSIST`, `E1000_PCH_NIC`, `USB_UHCI`, `EHCI_USB`.

See [known-limitations.md](known-limitations.md) for the implications (notably: the cooperative
single-core default means any unbounded wait in a syscall freezes the machine — so every device
wait is iteration-capped).
