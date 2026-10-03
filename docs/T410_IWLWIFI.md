# T410 WiFi — Operator Guide (iwlwifi on the real ThinkPad T410)

> How to take WiFi off the simulator and onto the real Intel radio in a
> physical ThinkPad T410. This guide is grounded in the code as it exists
> today — every claim names the file/flag it comes from. It is honest about
> what is **DONE**, what is **WRITTEN-but-HELD**, and what is **NOT-YET-WRITTEN**.
>
> The one-line truth: the Network Manager and WLAN control ABI exist, and QEMU
> proves a simulated control flow, but real association, live EAPOL, DVM key
> installation, and the `wlan0` data path are not complete. Passive scan is the
> current physical-hardware milestone and must be iterated on a T410 over serial.

---

## 1. Architecture: the `wifi_ops` swap seam

There is exactly one contract that everything wireless talks through:

```
  GUI / Network Manager / wpasupp  ── SYS_WLAN_* syscalls ──┐
  (userspace)                                               │
                                                            ▼
                          kernel/include/uapi/wlan.h   (the ABI:
                          SCAN 113 / CONNECT 114 / STATUS 115 /
                          DISCONNECT 116 / SET_KEY 117)
                                                            │
                                                            ▼
                          kernel/include/wifi.h            (wifi_ops_t:
                          scan_start / scan_results / connect /
                          disconnect / set_key / get_status +
                          the RADIO SEAM tx_mgmt / rx_poll_mgmt)
                                                            │
                          ┌─────────────────────────────────┴─────────────────┐
                          ▼                                                     ▼
        kernel/drivers/net/wireless/sim/wifisim.c          kernel/drivers/net/wireless/intel/iwlwifi/
        (the SIMULATED backend — today)                    (the REAL Intel radio — the goal)
```

The contract is `wifi_ops_t`, defined in `kernel/include/wifi.h`. It hangs off a
`netif_t` (`w.wifi = &ops`). The control plane is the five `SYS_WLAN_*`
syscalls whose userspace ABI lives in `kernel/include/uapi/wlan.h` (each struct
carries an `ABI_SIZE` constant + a `_Static_assert`, so kernel/userspace drift
is a compile error).

**Why this matters for the operator:** `wifisim.c` and the real `iwlwifi`
driver implement the *same* `wifi_ops`. Swapping the simulator for the real
radio touches *only the driver below the seam* — nothing above it
(`SYS_WLAN_*`, the supplicant, the GUI) changes at all. The header says this
explicitly (`wifi.h` lines 5-8).

**What the QEMU simulator proves:**

- The scan→connect→status→disconnect flow (`wifisim.c` walks a canned AP list
  of OPEN/WPA2/WPA3 networks and drives the state machine all the way to
  `WLAN_CONNECTED`).
- WPA2 key-derivation and crypto known-answer paths in `userspace/lib/wpa/`.
- The control tool `userspace/apps/wlanctl/` and Network Manager GUI wiring.

This is not an RF or IP data-plane proof. `wpasupp` currently uses fixed demo
MACs/nonces and explicitly reports `no live EAPOL yet`; `wifisim` has no packet
TX/RX path, so DHCP and Internet traffic still use the emulated wired NIC.

---

## 2. Status of the iwlwifi bricks

The real driver is being built brick-by-brick. Be precise about which state
each brick is in:

| Brick | File | State | What it does |
|-------|------|-------|--------------|
| **IWL-IDENT** | `iwl-pci.c`, `iwl-devices.h` | **DONE (QEMU-checkable)** | Detects the T410 card over the candidate PCI IDs, enables MMIO + bus-master, maps BAR0, reads `CSR_HW_REV` — one side-effect-free MMIO read — then **stops**. No APM, no firmware, no reset. |
| **IWL-FW** | `iwl-fw.c`, `iwl-fw-file.h` | **DONE (QEMU-checkable)** | Bounds-checks both legacy v1/v2 and TLV containers. This includes the legacy `iwlwifi-6000-4.ucode` used by common T410 6200/6300 cards. The KAT verifies metadata, section pointers, and malformed/truncated rejection. |
| **IWL-TRANS** | `iwl-trans.c`, `iwl-hostcmd.c` | **WRITTEN-but-HARDWARE-UNVERIFIED** | Deferred APM, DMA rings, SCD command queue, bounded notifications, RF-kill, and scan-sized host commands. No emulator covers the device path. |
| **IWL-LOAD** | `iwl-fw-load.c` | **WRITTEN-but-HARDWARE-UNVERIFIED** | INIT/runtime section DMA, ALIVE waits, calibration capture/replay. The INIT-to-runtime stop/restart transition still needs an upstream-faithful implementation before this is considered complete. |
| **IWL-NVM/RXON/SCAN** | `iwl-nvm.c`, `iwl-rxon.c`, `iwl-scan.c` | **PARTIAL** | Pure builders/parsers have KATs. EEPROM/OTP family geometry, antenna/regulatory data, and physical scan require T410 validation. |
| **Association/WPA2/data** | `iwl-ops.c`, `wpasupp.c` | **SCAFFOLDED / NOT CONNECTIVITY** | `connect`, `set_key`, and radio TX/RX are placeholders. There is no live auth/association, EAPOL exchange, PTK/GTK firmware install, CCMP data path, or WLAN DHCP path yet. |

With `IWLWIFI=1`, boot performs safe identification and software KATs only.
Running `iwlup` after the desktop explicitly attempts the held hardware ladder.
Even a successful scan does not yet mean the machine can associate or carry IP
traffic over WiFi.

### Connectivity milestones

The following gates must pass in order before claiming T410 Internet access:

1. Firmware INIT/calibration/runtime ALIVE, valid NVM, and repeated passive scans.
2. Real 802.11 authentication and association with a controlled open AP.
3. Intel data TX/RX queues plus Ethernet/802.11 LLC-SNAP conversion; ARP and ping
   must work with the wired NIC disconnected.
4. Live WPA2-PSK EAPOL messages 1-4 with random SNonce, replay/MIC validation,
   GTK unwrap, PTK/GTK firmware installation, and CCMP replay protection.
5. Per-interface DHCP, ARP, routes, source MAC/IP selection, DNS, TCP 80/443, and
   trusted TLS through `wlan0`.
6. Network Manager failure states: wrong password, RF-kill, timeout, reconnect,
   disconnect, and lease renewal.

Google HTTPS is an acceptance test after gate 5, not a driver primitive. Full
YouTube playback is a separate browser/media project: the current browser has no
external-script pipeline, `<video>`, MSE, MP4/WebM demuxer, or video/audio codecs.
A realistic first media gate is a native player for one controlled local/direct
format, followed by browser media elements; loading the production YouTube web
application must not be represented as currently supported.

**Honesty about the radio bring-up:** QEMU does not emulate any iwlwifi card.
`iwl_init()` on QEMU prints `IWL: no Intel WiFi card found` and returns cleanly
— that graceful absence *is* the QEMU acceptance test (`iwl-pci.c` lines 50-54).
There is **no way to test APM/uCode/RF bring-up except on the physical T410**,
one flash→boot→read-serial cycle at a time. The register values in `iwl-csr.h`
are "correct-by-review against Linux," not "tested" — by design, because there
is nothing to test them against until hardware day.

---

## 3. Step-by-step: enable real WiFi on the T410

### a. Identify the card

Boot any build of AutomationOS on the T410 (or even a current build in QEMU to
learn the tool) and run the PCI lister:

```
lspci
```

(`userspace/apps/lspci/lspci.c`.) Find the line with vendor **`8086`** (Intel)
and a WiFi device ID. Match that device ID against the candidate table in
`kernel/drivers/net/wireless/intel/iwlwifi/iwl-devices.h`:

| PCI ID (8086:____) | Card | Firmware family |
|--------------------|------|-----------------|
| `4239` | Centrino Advanced-N 6200 | **6000** |
| `4238` / `422B` | Centrino Ultimate-N 6300 | **6000** |
| `0085` | Centrino Advanced-N 6205 | **6000g2a** |
| `0083` / `0084` | WiFi Link 1000 BGN | **1000** |
| `4232` / `4237` | WiFi Link 5100 AGN | **5000** |
| `4235` / `4236` | Ultimate-N 5300 AGN | **5000** |

Note the **firmware family** in the right column — that is the `<family>` you
need for the firmware file in the next step. (If `IWLWIFI=1` is already in your
build, the kernel also prints the matched name + family on the serial line:
`IWL: found <name> [8086:xxxx] ... (fw family iwlwifi-<family>)`.)

If your card's ID is not in the table, it is still almost certainly an iwlwifi
part (Lenovo's FRU whitelist on the T410 only allows a handful) — add the row
to `iwl-devices.h` with the right family before building.

### b. Get the matching firmware

Download the matching uCode blob from the kernel's **linux-firmware** tree:

```
https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/
```

The file naming is `iwlwifi-<family>-<api>.ucode`, e.g.:

- family **1000** → `iwlwifi-1000-5.ucode`
- family **5000** → `iwlwifi-5000-5.ucode`
- family **6000** → `iwlwifi-6000-4.ucode` (and `iwlwifi-6000g2a-6.ucode` for `0085`)

The `<api>` digit is the firmware API version; if several exist, the highest
one your driver advertises support for is the right one. (For the first
bring-up, any valid blob for the family will parse; the loader picks the API.)

**The one mercy of these old cards:** the 1000/5000/6000-family uCode is
**non-secured** — it carries **no RSA signature**, so the OS does not need a
signature-verification path to load it. (Newer Intel cards, 7000+, require a
signed image and a far more complex secure-boot flow; the T410 predates all of
that. This is a large reason the T410 was chosen as the WiFi target.)

Drop the exact versioned file into the repository's `firmware/` directory.
`scripts/build_all.sh` stages every `firmware/iwlwifi*.ucode` into the initrd at
`/lib/firmware/` under its real filename. It does not synthesize a generic alias;
the driver selects only family-specific candidates and validates the API.

> NOTE: today no firmware ships in the tree, and `iwl_fw_load_from_initrd()`
> prints a clean hint (`IWL-FW: no firmware ... in initrd`) and returns -1 when
> the file is absent — so a missing blob never crashes anything; it just means
> "no real WiFi yet."

### c. Build and flash

```
IWLWIFI=1 bash scripts/quick_build.sh     # kernel: real detect + safe probe + FW parser
bash scripts/build_all.sh                  # userspace + initrd + GRUB ISO
```

`quick_build.sh` turns `IWLWIFI=1` into `-DIWLWIFI` (lines 60-62), which
compiles `iwl-pci.c` + `iwl-fw.c` (lines 560-562) and arms the `#ifdef IWLWIFI`
block in `kernel.c` (lines 1078-1089) that calls `iwl_init()` then
`iwl_fw_selftest()`. `build_all.sh` writes the bootable ISO to
`build/automationos.iso`.

You will almost always want to combine flags for the T410:

```
T410_SAFE=1 IWLWIFI=1 bash scripts/quick_build.sh
```

`T410_SAFE=1` disables modern-CPU optimizations (Westmere-safe) — the T410's
Arrandale CPU needs it. Do not combine `WIFI_SIM=1` and `IWLWIFI=1`; both own
`wlan0`, and the kernel deliberately rejects that ambiguous profile.

Then flash `build/automationos.iso` to a USB stick (e.g. `dd` the ISO to the
raw USB device on a Linux box, or use Rufus in DD mode on Windows) and boot the
T410 from it. **Attach a serial console** — the entire bring-up diagnosis is
the serial marker ladder, and without it you are blind.

### d. What you'll see, and the iteration loop

On a real T410 with `IWLWIFI=1`, the serial log shows the IWL-IDENT ladder:

```
IWL: found Intel ... [8086:xxxx] at bb:dd.f (fw family iwlwifi-<family>)
IWL: CSR_HW_REV=0x........ (safe probe only)
IWL: IDENT ok -- APM/firmware/RF bring-up is the T410 hardware tail ...
```

If you staged a firmware file and the loader is invoked, IWL-FW then reports the
parsed sizes:

```
IWL-FW: loaded <path>: inst=.. data=.. init=.. init_data=.. ver=.. tlvs=..
```

(If the blob is absent or malformed you get the `no firmware`/`malformed` hint
instead — both are clean, non-fatal.)

**Beyond this point is the hardware-unverified tail.** Running `iwlup` produces
the `IWLTRANS:`/`IWLLOAD:`/`IWLNVM:`/`IWLSCAN:` marker ladder. The discipline is:
**every risky MMIO touch is
preceded by a serial marker**, so whichever marker is the *last line printed*
tells you exactly where the radio stalled. That last-line-wins ladder is how you
iterate: flash → boot → read the last marker → fix the step it names → reflash.
This is the identical method already proven on the e1000 PCH NIC bring-up
(`docs/dev-memory/bricks/NET-P1-0.md`, the `E1000PCH` marker ladder
PROBE→FWSM→SWFLAG→PHYID→ANEG→LINK).

---

## 4. The safety laws that make this safe to try

These are why you can attempt a radio bring-up on real hardware without
bricking your boot. They mirror the e1000 PCH NIC containment rules exactly.

1. **Gated, default-OFF.** `IWLWIFI` is opt-in (`quick_build.sh`). A normal
   build does not compile the driver at all; the default kernel is byte-for-byte
   unchanged. The `SYS_WLAN_*` syscalls are always present but return ENOTSUP
   when no wifi interface is registered.

2. **A serial marker before every risky MMIO touch.** IWL-IDENT already prints
   the card + `CSR_HW_REV` before the only MMIO it does. Every future
   APM/firmware/RF step prints its marker *before* the access, so a stall is
   localized to one named line — never a silent hang.

3. **All hardware polls are iteration-bounded, never tick-based.** `iwl-csr.h`
   defines `IWL_TRANS_POLL_MAX = 100000` and states the rule outright: a "20 ms
   timeout" in Linux becomes a bounded *iteration count* here, because the PIT
   can be frozen when the bring-up runs — **never wait on ticks.** (This is the
   same lesson the net stack learned: any wall-clock wait in a syscall needs an
   iteration cap.)

4. **Abort-clean-and-defer.** IWL-IDENT bails cleanly if BAR0 is unmapped
   (`IWL: BAR0 not mapped -- aborting safe probe`). IWL-FW returns -1 (not a
   crash) on a missing or malformed blob. A failed bring-up costs you a re-run,
   not a wedged boot — exactly like `nicup` for the PCH NIC: worst case is a
   reflash with a live serial line naming the exact wedging access.

The net effect: trying real WiFi on the T410 can never be worse than "it didn't
come up, and the serial log tells me which step to fix." It cannot cost you a
boot.

> Caveat consistent with the e1000 PCH experience: a true **hardware bus stall**
> (not a software spin) is unrecoverable in software — iteration caps cannot
> rescue an MMIO read that the bus never completes. The marker-before-touch
> ladder is what makes even that case *diagnosable*: you learn precisely which
> register access hung, and can gate that step OFF.

---

## 5. Troubleshooting

| Symptom (serial) | Likely cause | Next step |
|------------------|--------------|-----------|
| `IWL: no Intel WiFi card found` on the **T410** | Card ID not in `iwl-devices.h`; or WiFi disabled in BIOS / by the hardware RF-kill switch; or the half-mini card is unseated | Run `lspci`, find the real `8086:xxxx`, add the row (with the right family) to `iwl-devices.h`; check the BIOS WLAN toggle and the physical wireless switch; reseat the card |
| `IWL: BAR0 not mapped -- aborting safe probe` | PCI BAR0 not assigned (BIOS/PCI enumeration) | Confirm `lspci` shows a memory BAR for the device; check that `pci_enable_memory_space`/`bus_master` ran; this is a platform/PCI issue, not the radio |
| `CSR_HW_REV=0x00000000` or `0xffffffff` | MMIO not actually reaching the device (bad BAR map, card powered down, or a bus stall) | `0xffffffff` usually = no device responding (RF-kill / power); `0x0` = mapped-but-dead. Recheck RF-kill switch and BAR mapping before going further |
| `IWL-FW: no firmware <path> in initrd` | No `.ucode` staged, or the loader path ≠ the staged path | Drop `iwlwifi-<family>-<api>.ucode` into `/tmp/ird/lib/firmware/`, add the `cp` in `build_all.sh`, and make the `iwl_fw_load_from_initrd` path match exactly |
| `IWL-FW: <path> is malformed (not valid legacy/TLV .ucode)` | Wrong-family, corrupt, truncated, or structurally inconsistent blob | Re-download the exact family/API filename from linux-firmware; do not rename another card's firmware to a generic alias |
| Stall at marker **X** (last serial line before a hang) | The MMIO touch *after* marker X wedged (most likely a real hardware bus stall on the T410) | Note marker X — it names the exact step. Compare that register access against the Linux `iwlwifi` source for this family; consider gating that step OFF and deferring, the same way the PCH NIC's risky half was deferred |
| **ALIVE timeout** (once IWL-LOAD exists) | uCode loaded but the radio never raised the ALIVE notification within `IWL_TRANS_POLL_MAX` | Verify the firmware family/API matches the card; verify INST/DATA were copied to the right SRAM/DRAM addresses; verify APM power-up + clocks completed (earlier markers); this is the core hardware-iteration step with no emulator |
| Scan returns nothing once OPS exists, but auth/DHCP code looks fine | Software above the seam is *not* the problem — it is QEMU-proven | The bug is in the radio path (RF config / scan command / RX ring), below the seam. Keep `WIFI_SIM=1` as the known-good A/B reference for everything above the seam |

---

## Quick reference

- **Build it:** `T410_SAFE=1 IWLWIFI=1 bash scripts/quick_build.sh && bash scripts/build_all.sh`
- **The seam:** `kernel/include/wifi.h` (`wifi_ops_t`) + `kernel/include/uapi/wlan.h` (`SYS_WLAN_*`)
- **The sim (known-good reference):** `kernel/drivers/net/wireless/sim/wifisim.c` (`WIFI_SIM=1`)
- **The real driver:** `kernel/drivers/net/wireless/intel/iwlwifi/` — firmware parse and pure KATs are complete; transport/load/NVM/scan are hardware-unverified; association, EAPOL/key install, and data TX/RX remain incomplete
- **Firmware:** `iwlwifi-<family>-<api>.ucode` from linux-firmware → `/tmp/ird/lib/firmware/` (non-secured / no RSA sig on 1000/5000/6000)
- **Diagnosis:** attach a serial console; the **last `IWL:`/`IWLTRANS:` marker before a hang** names the failing step
- **Golden rule:** treat simulator state transitions as control-plane tests only; require packet captures and WLAN-only DHCP/TLS before claiming real WPA2 or Internet connectivity

---

## Addendum -- deep-research findings on WPA3 and card choice (2026-10-02; [V] primary source read, [S] secondary, [I] inference)

**Verdict for "connect to my WPA3 network on this laptop":** spec-compliant WPA3-Personal (SAE **plus** protected management
frames) is **not available on the Intel 6000-series cards the T410 shipped with**.

* [V] Linux `iwldvm` sets `MFP_CAPABLE` only if the firmware advertises `IWL_UCODE_TLV_FLAGS_MFP`. Parsed from the current
  linux-firmware blobs: `iwlwifi-6000g2a-6` / `6000g2b-6` have TLV flags 0xb (MFP bit clear); `iwlwifi-6000-4` (6200/6300) is the old
  non-TLV format (no flags at all); 1000-5 / 5000-5 / 6050-5 likewise. The DVM key ABI has only NO_ENC/WEP/CCMP/TKIP -- no BIP/IGTK
  key type. mac80211 therefore drops the BIP cipher suites for these cards.
* What works: **WPA2-PSK on a WPA2/WPA3 *transition-mode* SSID (PMF optional)**; SAE *without* PMF only against an AP that sets
  `sae_require_mfp=0` (non-compliant; field reports of it completing on a 6200/6235); it **fails** on WPA3-only SSIDs and on mixed
  SSIDs with `sae_require_mfp=1` (the current OpenWrt default). Host-software IGTK/BIP is plausible but unproven for SAE on this chip [I].
* **Recommended path:** (1) `lspci -nn` on the laptop to learn the real card; (2) put the router (or a second SSID) in
  WPA2/WPA3 transition mode with PMF *optional*; (3) finish the station-mode bring-up to WPA2-PSK + DHCP; (4) add SAE-without-PMF only
  as an optional extra. Wired Ethernet (`PCH_NIC=1`, see the hardware ladder) remains the dependable internet path meanwhile.
* **Which cards shipped** [V: Lenovo HMM 63y0535 Table 39]: Intel Wireless-N 1000, Realtek "Adapter II" (RTL8192SE 10ec:8172 or
  RTL8188CE 10ec:8176 [S]), Intel Advanced-N 6200, Ultimate-N 6300, Advanced-N + WiMAX 6250. **Not** a T410 option: 6205.
  Our `iwl-devices.h` has 4239, 4238, 422B, 0085; **missing**: 422C (6200), 0083/0084 (1000), 0087/0089 (6250/6050) [V: Linux
  device table]. Add them (and the matching firmware families `iwlwifi-1000-5`, `iwlwifi-6050-5`) in the next Wi-Fi brick, default-OFF.
* **BIOS whitelist** [V: HMM error 1802 "Unauthorized network card"; S: ThinkWiki]: swapping in a non-Lenovo card (e.g. Atheros
  AR9280, whose ath9k driver is fully ISC-licensed and needs no firmware blob) needs a patched BIOS or an EEPROM-ID rewrite -- brick
  risk, not recommended without a recovery plan. Atheros was never a T410 option.
* **License handling:** Linux `dvm/commands.h`, `agn.h`, `iwl-csr.h`, `iwl-prph.h`, `fw/file.h` are `GPL-2.0 OR BSD-3-Clause` (take the
  BSD-3 option and keep the notice); `dvm/rxon.c`, `main.c`, `mac80211.c`, `scan.c`, `tx.c`, `rx.c`, `sta.c` are GPL-only (behaviour
  only, never copy). OpenBSD/FreeBSD `iwn(4)` (ISC; keep every copyright holder's notice) supports the 6000 series and is the cleaner
  reference -- but implements **no** 802.11w. Intel firmware (`LICENCE.iwlwifi_firmware`): unmodified binary redistribution allowed
  with the licence text, no reverse engineering; the project still does not bundle blobs -- the owner stages them in `firmware/`.
* **Effort ladder** (OpenBSD `iwn` is 7.2k lines for every family; a minimal 6000-series station path is ~3-4.5k lines [I]):
  BAR0/APM/EEPROM (MAC matches the card label) -> DMA rings + interrupts -> firmware load (hardest, hardware-only) -> BT-coex/
  calibration/RXON -> scan -> open auth+assoc -> EAPOL + PTK/GTK via ADD_STA -> 802.11<->802.3 + DHCP. Every step needs the physical
  laptop; QEMU emulates no iwlwifi device. Hardware unknowns: firmware-alive failures, per-card EEPROM calibration, hardware rfkill,
  BT-coexistence stalls, ASPM/clock quirks on the Ibex Peak root port.
