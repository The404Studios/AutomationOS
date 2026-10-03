# T410 compatibility research (2026-10-02)

Web research for making AutomationOS work on the real ThinkPad T410 (Arrandale i5-520M-class, Ibex Peak QM57 PCH, 82577LM,
CX20585 audio, optional NVS 3100M). Tags: **[V]** vendor datasheet/spec or vendor-authored BSD source read; **[O]** open-source
firmware/driver/forum evidence (Linux and coreboot are GPL -- behaviour only, no code copied); **[I]** inference / unverified.
Nothing here has been run on the machine. Ordered by likelihood x impact. Each item ends with the next brick; QEMU can prove only the
part marked "QEMU".

Companion docs: `T410_IWLWIFI.md` (Wi-Fi/WPA3 addendum), `GPU_ROADMAP_2026-10-02.md` (graphics addendum),
`T410_HW_VALIDATION_2026-06-21.md` (flash ladder), `dev-memory/hardware_laws.md`.

## 1. Wired Ethernet (82577LM) -- the biggest hang risk; keep `PCH_NIC` gated

* [V] The 82577 is a *separate PHY chip*; the PCH has only the GbE MAC. MAC<->PHY is a PCIe-signalled SerDes (S0) plus SMBus (Sx).
  PHY registers are reached over MDIO and are shared with the Management Engine: hardware/software/firmware must use the
  ownership flags (`EXTCNF_CTRL` 0xF00 bit 5 SWFLAG) before MDIC. `CTRL.LCD_RST` is gated by `FWSM.RSPCIPHY`. After a PHY reset wait
  **10 ms** before MDIO. [Intel 5-series datasheet, 82577 datasheet: intel.com/.../5-chipset-3400-chipset-datasheet.pdf,
  .../82577-gbe-phy-datasheet.pdf]
* [V] FreeBSD `e1000_ich8lan.c` / OpenBSD `if_em_hw.c` (BSD-licensed) agree on the sequence: disable PCIe master (`GIO_MASTER_DISABLE`,
  poll `GIO_MASTER_ENABLE`); mask interrupts, `RCTL`=0, `TCTL`=PSP, wait 10 ms; check `FWSM.RSPCIPHY` (a clear bit = the ME blocks
  PHY reset); acquire SWFLAG; **do a PCI *config-space* read of the vendor ID and write it to STRAP before and after the reset**; write
  `CTRL.RST`, then **20 ms with no MMIO and no read-back flush**; wait `STATUS.LAN_INIT_DONE`; clear `PHYRA`; 10 ms; set MDIO slow mode
  (PHY page 769 reg 16 bit 10); apply the K1/gigabit-stall workaround; push the OEM bits to PHY 768.25 (LPLU off in D0a); LANPHYPC toggle
  only when `FWSM.FW_VALID` is clear. Both drivers comment that a read of `EXTCNF_CTRL` during global reset, or a flush after the
  `CTRL.RST` write, **hangs the hardware**.
* [I] Our `kernel/drivers/net/e1000.c` `e1000_pch_mac_reset()` polls `CTRL` over MMIO a few ms after the reset write -- inside that
  20 ms window; it also lacks the config-space STRAP trick, the master-disable/RCTL/TCTL quiesce, the RSPCIPHY check, the LAN_INIT_DONE
  wait, the post-PHY-reset 10 ms wait, MDIO slow mode, and the OEM-bits push. Law 11 (SWFLAG before reset) is necessary, not sufficient.
  Also [O]: a T410 negotiated only 10 Mb/s on battery until PCI runtime-PM was forced "on".
* **Next brick `E1000-PCH-RESET-0`**: rewrite the reset to the sequence above, still `PCH_NIC`-gated and default OFF; prove it with a
  **host-side mock-MMIO trace test** that asserts the ordering invariants (no MMIO in the 20 ms window, STRAP writes, SWFLAG
  bracketing). QEMU cannot model the PCH PHY/ME (its e1000 is an 82540EM). On the laptop go in stages: PCI config dump -> read-only
  FWSM/STATUS -> SWFLAG acquire+release without reset -> PHY ID with slow mode -> full reset.

## 2. SMP on Arrandale (the multi-core ISO is hardware-UNPROVEN)

* [V: Intel SDM 11.4] INIT, 10 ms, SIPI, 200 us, SIPI. APs must use the **same MTRR mapping as the BSP**; IA32_PAT must also be
  programmed identically on each AP (else a WC mapping via PAT can behave as UC there [I]).
* [I] With Hyper-Threading on the T410 has **4 logical CPUs**. ACPI says firmware *should* list each core's first logical CPU before any
  siblings, but a BIOS may not. Our `ap_boot.c` starts exactly one AP, `madt_get_apic_id(1)`: on a non-conforming BIOS that is the SMT
  sibling of core 0, not the second core. Choose the AP by CPUID topology (leaf 0xB) as "another core, same package"; never use a
  broadcast SIPI shorthand; accept non-contiguous APIC IDs (law 14 already maps to a dense index); check `IA32_APIC_BASE` for x2APIC
  (a BIOS may pre-enable it; xAPIC-style `id<<24` then hangs).
* [V: Arrandale spec update] LAPIC-timer errata: AAT90 (count can read 0 prematurely across a frequency/C-state change -- do not use
  "count==0" as completion), AAT28, AAT81 (reprogram the divider only while disarmed), AAT31, AAT39 (logical-cluster broadcast IPIs may
  not wake sleeping cores). Calibrate against HPET/PIT/TSC (invariant TSC on i5-520M [O]).
* **Next brick `SMP-T410-0`**: topology-based AP choice + MTRR/PAT parity + timer calibration; QEMU:
  `-smp 4,cores=2,threads=2 -cpu Westmere` exercises MADT/CPUID.0B parsing (cannot reproduce errata, SMIs, real IPI timing, ID gaps).
  On the laptop print MADT, per-CPU topology, TSC deltas, MTRR/PAT per CPU, then start one AP. The single-core ISO stays the baseline.

## 3. USB (Ibex Peak has *no* UHCI/OHCI)

* [V: PCH datasheet] Two EHCI controllers (D29:F0, D26:F0) and two **Rate Matching Hubs**; `N_CC`=0, no port-ownership hand-off. Every
  low/full-speed device sits behind an RMH (a discrete-hub look-alike with one Transaction Translator, VID 8087 PID 0020, on EHCI
  port 0). So USB keyboards/mice need **hub enumeration, port power/reset and split transactions**; our UHCI driver is irrelevant here
  and `ehci.c`'s open question "E3: routing" is answered by the datasheet. BIOS hand-off: `LEG_EXT_CAP` (config 0x68, OS-owned bit 24,
  BIOS-owned bit 16) and `LEG_EXT_CS` 0x6C, done before touching BAR/PCICMD (already in `ehci.c`).
* The internal keyboard/TrackPoint are PS/2 (booted before), so USB matters for external devices only.
* **Next brick `EHCI-RMH-0`**: hub-class enumeration + split transactions. QEMU `usb-ehci` works standalone but has no RMH/TT model
  [I], so split transactions are validated on the laptop only (read-only EHCI/0x68/0x6C dump first).

## 4. Display / boot

* [V] Use the Multiboot-reported `framebuffer_pitch` (bytes), not `width*bpp/8`; request the mode in the Multiboot header with a
  fallback; test 24 and 32 bpp. grub-mkrescue output can be written to a USB stick (isohybrid); pick the BIOS "USB HDD" entry [I].
  WC for the LFB via PAT (and PAT on the APs) -- see the GPU addendum. Which VBE modes the T410's Ironlake VBIOS exposes: **unknown**.
  In Discrete mode the Intel IGD may be hidden from PCI [O: coreboot]. Record the BIOS graphics setting in a hardware report.

## 5. Audio

* [O: coreboot T410 verb table] Codecs: Conexant CX20585 (14f1:5069, subsystem 17aa:214c) and the Intel Ibex Peak HDMI codec
  (8086:2804) on SDI3; controller 8086:3b56. OpenBSD's T410 dock quirk attaches to 14f1:506e, so **read the codec vendor ID at runtime**.
  FreeBSD clears TCSEL (config 0x44), holds `GCTL.CRST` 100 us and waits >= 521 us for codecs. [V] **ThinkPad firmware owns the final
  amp and mute**: expect silence until the EC mute (EC 0x3A bit 0 [O]) is cleared (a FreeBSD T410 user needed a volume-key press).
* **Next brick `HDA-T410-0`**: read-only inventory first (STATESTS: codecs on SDI0 and SDI3; pin defaults via verb F1C vs the coreboot
  table); no controller reset in the inventory phase. QEMU `intel-hda -device hda-duplex` validates reset/CORB/RIRB/widget parsing only.

## 6. PS/2, EC, power (read-only until validated)

* PS/2: keep the bounded init; log the mouse-ID reply; do not parse Synaptics-specific data yet (pass-through vs active-mux topology on
  the T410 is unverified; Linux's i8042 quirk table has no T410 entry [O]). Do not touch PMH7 (0x15E0).
* EC: ACPI EC interface at 0x62/0x66 [V: ACPI 6.5 ch.12]; a second non-standard interface at 0x1600 [O]. Safe read-only registers [O,
  coreboot map; the stock DSDT may differ]: 0x78/0x79 temperatures (0x80 = invalid), 0x3A bit 0 audio mute, 0x48 headphone, 0x38
  battery state. **Never write EC 0x2F (fan)**, any 0x1600-range or PMH7 register, port 0xB2 (SMI) or SMI_EN. Battery %: ACPI `_BST`
  (needs an AML interpreter, e.g. MIT uACPI) or EC pages after verifying them against the real DSDT.
* Backlight: Ironlake PCH 0xC8250/0xC8254 and CPU 0x48250/0x48254 [V: PRM]; in Discrete mode the NVIDIA PWM is used instead.

## Bottom line
1. **NIC first** (it is the dependable route to real internet): fix the reset sequence, prove it host-side, then stage it on the laptop.
2. **Multi-core on hardware**: topology-based AP choice + MTRR/PAT parity before trusting the multicore ISO there.
3. **USB** needs hub + split transactions; **audio** will likely be silent at first (firmware mute, two codecs); **EC** stays read-only.
4. Unverified: x2APIC support/ID layout on Arrandale, T410 VBE mode list, stock DSDT vs coreboot EC map, pointing-device topology, UEFI mode.
