# T410 WiFi — Operator Guide (iwlwifi on the real ThinkPad T410)

> How to take WiFi off the simulator and onto the real Intel radio in a
> physical ThinkPad T410. This guide is grounded in the code as it exists
> today — every claim names the file/flag it comes from. It is honest about
> what is **DONE**, what is **WRITTEN-but-HELD**, and what is **NOT-YET-WRITTEN**.
>
> The one-line truth: the entire WPA2/WPA3 software stack is already done and
> QEMU-proven against a simulated radio. The only thing standing between you
> and real WiFi is the Intel radio bring-up — and that bring-up has **no
> emulator**, so it must be iterated on the physical T410 with a serial console.

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

**The whole software stack above the seam is already done and QEMU-proven:**

- The scan→connect→status→disconnect flow (`wifisim.c` walks a canned AP list
  of OPEN/WPA2/WPA3 networks and drives the state machine all the way to
  `WLAN_CONNECTED`).
- The WPA supplicant and crypto: `userspace/apps/wpasupp/`,
  `userspace/lib/wpa/wpa.c`, `userspace/lib/crypto/wpa_aad.c` — the 4-way
  handshake lands its pairwise key via `SYS_WLAN_SET_KEY` (`set_key`), exactly
  as `sim_set_key()` models (`pairwise && klen>0 → CONNECTED`).
- The control tool `userspace/apps/wlanctl/` and DHCP on the brought-up `wlan0`.

All of that runs end-to-end **today** in QEMU under `WIFI_SIM=1` with no radio.
The moment the real driver can *scan*, that same stack runs over the real air —
because it never knew it was talking to a simulator.

---

## 2. Status of the iwlwifi bricks

The real driver is being built brick-by-brick. Be precise about which state
each brick is in:

| Brick | File | State | What it does |
|-------|------|-------|--------------|
| **IWL-IDENT** | `iwl-pci.c`, `iwl-devices.h` | **DONE (QEMU-checkable)** | Detects the T410 card over the candidate PCI IDs, enables MMIO + bus-master, maps BAR0, reads `CSR_HW_REV` — one side-effect-free MMIO read — then **stops**. No APM, no firmware, no reset. |
| **IWL-FW** | `iwl-fw.c`, `iwl-fw-file.h` | **DONE (QEMU-checkable)** | Parses the modern TLV `.ucode` container (bounds-checked, hostile-input-safe), recording the INST/DATA/INIT/INIT_DATA sub-image sizes the loader will need. Proven by `iwl_fw_selftest()` against an embedded synthetic blob + truncation/short/bad-magic negative tests. |
| **IWL-TRANS** | `iwl-csr.h` (register map present) | **WRITTEN-but-HELD** | The transport register dictionary (CSR/PRPH/FH offsets, all cited verbatim from Linux v5.10 sources) is in the tree. APM power-up + command/RX DMA ring programming is the next step. **Held** — it has no emulator and is not wired to a trigger yet. |
| **IWL-LOAD** | — | **NOT-YET-WRITTEN** | uCode image load into SRAM/DRAM → kick the radio → wait for the **ALIVE** notification. |
| **IWL-OPS** | — | **NOT-YET-WRITTEN** | NVM/EEPROM read → RF config → a real scan → register a `netif` + `wifi_ops` behind the seam (the point at which the sim is finally replaced). |

So today, with `IWLWIFI=1`, the kernel does exactly two things on real hardware:
prints the detected card + `CSR_HW_REV` (IWL-IDENT), and — if you drop a
firmware file in the initrd — can parse it (IWL-FW). **The radio does not yet
come up.** Everything past `CSR_HW_REV` (IWL-TRANS/LOAD/OPS) is the hardware
tail.

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

Drop the file into the **initrd firmware directory** so `iwl_fw_load_from_initrd()`
can find it via `initrd_get_file()`. The initrd is staged at `/tmp/ird` during
`build_all.sh` (then re-tarred into `iso/boot/initrd.img`). Place the blob at a
path like `/tmp/ird/lib/firmware/iwlwifi-<family>-<api>.ucode` and add a `cp`
for it alongside the other initrd staging copies in `scripts/build_all.sh`
(the section that populates `/tmp/ird/...`). The loader is called with the path
you give it, so keep the two consistent.

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
Arrandale CPU needs it. You may add `WIFI_SIM=1` too: the sim and the real
driver coexist (the sim registers `wlan0`, the real driver only prints
detect/probe today), which lets you keep exercising the GUI while the radio
tail is still being written.

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

**Beyond this point is the hardware tail that is not wired yet.** Once IWL-TRANS
is enabled and triggered, the next markers will be the `IWLTRANS:` lines around
APM power-up and ring setup. The discipline is: **every risky MMIO touch is
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
| `IWL-FW: <path> is malformed (not a valid TLV .ucode)` | Wrong/corrupt blob, or a legacy v1 (non-TLV) image | Re-download the matching family file from linux-firmware; confirm it is a modern TLV image (the parser rejects v1 by design — `zero` field must be 0, `magic` must be `IWL\n`) |
| Stall at marker **X** (last serial line before a hang) | The MMIO touch *after* marker X wedged (most likely a real hardware bus stall on the T410) | Note marker X — it names the exact step. Compare that register access against the Linux `iwlwifi` source for this family; consider gating that step OFF and deferring, the same way the PCH NIC's risky half was deferred |
| **ALIVE timeout** (once IWL-LOAD exists) | uCode loaded but the radio never raised the ALIVE notification within `IWL_TRANS_POLL_MAX` | Verify the firmware family/API matches the card; verify INST/DATA were copied to the right SRAM/DRAM addresses; verify APM power-up + clocks completed (earlier markers); this is the core hardware-iteration step with no emulator |
| Scan returns nothing once OPS exists, but auth/DHCP code looks fine | Software above the seam is *not* the problem — it is QEMU-proven | The bug is in the radio path (RF config / scan command / RX ring), below the seam. Keep `WIFI_SIM=1` as the known-good A/B reference for everything above the seam |

---

## Quick reference

- **Build it:** `T410_SAFE=1 IWLWIFI=1 bash scripts/quick_build.sh && bash scripts/build_all.sh`
- **The seam:** `kernel/include/wifi.h` (`wifi_ops_t`) + `kernel/include/uapi/wlan.h` (`SYS_WLAN_*`)
- **The sim (known-good reference):** `kernel/drivers/net/wireless/sim/wifisim.c` (`WIFI_SIM=1`)
- **The real driver:** `kernel/drivers/net/wireless/intel/iwlwifi/` — `iwl-pci.c` (IDENT, done), `iwl-fw.c`+`iwl-fw-file.h` (FW parse, done), `iwl-csr.h` (TRANS register map, held), LOAD/OPS not yet written
- **Firmware:** `iwlwifi-<family>-<api>.ucode` from linux-firmware → `/tmp/ird/lib/firmware/` (non-secured / no RSA sig on 1000/5000/6000)
- **Diagnosis:** attach a serial console; the **last `IWL:`/`IWLTRANS:` marker before a hang** names the failing step
- **Golden rule:** nothing above the seam needs changing — when the radio can scan, the whole proven WPA2/WPA3 + DHCP stack just works over real air
