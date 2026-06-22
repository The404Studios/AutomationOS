# Originality + 5x Charter (AutomationOS vs BoredOS)

> Produced by a 20-agent role team (5 system developers, 10 software engineers,
> 3 project leads, 2 project managers — kernel-expert depth throughout),
> grounded in the actual repo. Source workflow: `wf_1e865950-d2d`.
> This charter governs every brick on the net/sound/system/driver roadmap
> (`~/.claude/plans/ultrathink-run-20-agents-squishy-conway.md`). It answers the
> directive: **make our work demonstrably ORIGINAL vs BoredOS and 5x its capability.**

## 1. Originality verdict

**Independent across all five subsystems** (anti-clone lead review avg **8.3/10**).
BoredOS's defining choices are *reuse* (lwIP for net, Nova `/tmp/nova.sock` protocol,
LibWidget toolkit, TCC+Lua). Ours are *from-scratch* with our own data structures,
naming, and proof discipline — no lwIP `pbuf`/`tcp_pcb`/`mem_pool`, no Nova wire format,
no LibWidget.

| Subsystem | Verdict |
|---|---|
| Net data plane + NIC drivers | independent |
| Net protocols + sockets + userspace | independent |
| Sound / audio | independent |
| System: services + init + PTY + terminal + customization | independent |
| Driver framework + USB + storage + power | independent |

**The ONE real clone exposure:** `kernel/drivers/net/wireless/intel/iwlwifi/iwl-dvm-commands.h`
is admitted "copied verbatim from Linux v5.10" — a GPL/derivative-work *and* originality
problem. **Mandate:** treat the iwl command/struct ABI as a documented hardware-compat
surface (with provenance note) **or** regenerate from public Intel firmware-API docs; ship
the genuinely-ours **WPA3-SAE (H2E/SSWU on our `p256.c`/`hkdf.c`/`sha256.c`)** and the
`net_send`/`net_recv` de-hardcode as a **separate** brick so original work is not gated
behind clone-exposed code.

## 2. Originality Charter (enforceable checklist — CI-grep'able)

1. **NO lwIP, ever** — not even as a "reference." SACK/cwnd/wscale/RTT/2MSL are
   re-implemented from the RFCs (793/5681/2018/6298/7323) in OUR `sock_t`-table model
   (`tcp_ooo[][]`, `TCP_SYNQ` side-table, `tcp_rt_rto_ms[]`).
   **CHECK:** `grep -ri 'pbuf\|tcp_pcb\|mem_malloc\|linkoutput\|tcpip_input\|lwip\|etharp'`
   over `kernel/net/` + `userspace/lib/net/` returns **zero**.
2. **`netif_t.{tx,rx_poll,get_mac}` + embedded `wifi_ops*`** is OUR generic OS abstraction
   (cf. Linux `net_device`, BSD `ifnet`) and predates the bricks. Do-not-reshape toward lwIP.
   New drivers (virtio, iwl) register via `netif_register`.
3. **NO Nova wire protocol, NO bare `/tmp/*.sock` daemons.** IPC is the capability-channel
   rail (`SYS_CH_*` 96–105) + SysV SHM + the `wl_client` compositor protocol. **DELETE**
   (not `#ifdef`-dormant) the endemic Nova-echo stubs: `userspace/bin/audiomixerd.c`
   (`/tmp/audiomixer.sock`), `userspace/system/services/servicectl.c` (AF_UNIX
   `/run/services/control.sock`). **CHECK:** no `AF_UNIX` / `/tmp/*.sock` / `/run/*.sock`
   in any shipped service/audio/terminal TU.
4. **NO LibWidget, NO terminfo/libvterm.** UI uses OUR `ui_widget`/`ui_app`/`wl_client`;
   the VT engine is a from-scratch ECMA-48 state machine (`vt_parse.c`).
5. **NO Linux device-model revival.** MSI goes on OUR `pci_device_t` (NOT the orphaned
   `device_t` placeholder at `irq.c:517/526`); IOAPIC keeps a flat `g_ioapic_chip`
   {enable,disable,eoi,set_affinity} on the existing `irq_desc_t[256]` — not an irqdomain.
6. **Open-standard ≠ clone.** Matching VirtIO ring layout, `virtio_net_hdr`, EHCI qTD/QH,
   ECMA-48 escapes, SCSI CBW/CSW, RFC wire formats byte-for-byte is *required conformance*.
   The originality test is whether the **wiring, state model, data structures, and proof
   rig** are ours, mirroring OUR `e1000.c`/`uhci.c` conventions (static driver struct,
   `desc_wmb` sfence, bounded `*_TIMEOUT_SPINS`, budget=64 drain) — NOT Linux
   `vring_*`/`virtnet_*`/`usbcore`. **CHECK:** no `vring_`/`virtnet_`/`usbcore` symbols.
7. **Wireless provenance guard** — see §1 (the one exposure).
8. **Automation-native is the headline, not an afterthought.** Every brick exposes its
   capability to the gated agent tool rail under the audit ledger: per-owner-pid net
   accounting, agent-introspectable config (`cfg_list`), gated `svc` tool, agent-attachable
   PTY master, agent audio duck/route via per-stream gain, `SYS_DEVTREE`. This is the axis
   where "ours" is structurally unassailable vs BoredOS/lwIP/Nova/LibWidget. A brick that
   omits its automation surface is **incomplete**.
9. **Proof-rig house style is mandatory** and is itself an original convention: every brick
   ships a `-D`-gated, hardware-free self-test emitting deterministic serial PASS markers
   grep'd by `scripts/smoke_boot.sh` (modeled on `net_testrig.c`). A KAT that exercises a
   *mock* proves the mock — any capability that can only be mocked (audio capture, WiFi DVM
   data, virtio/USB interrupt delivery) is labeled **HELD-FOR-HARDWARE** and its 5x headline
   re-scoped to the provable artifact.

## 3. Measurable 5x acceptance (per subsystem)

| Subsystem | 5x target (BoredOS → ours) | How measured |
|---|---|---|
| **Net data plane** | NICs carrying real IP data 2→3 (add virtio-net); interrupt-driven NICs 0→≥1; idle CPU at line-idle ~100%→<2% | `NETV0`/`NETV1` rig markers (ring round-trip + feature negotiation, hardware-free); live bonus: `virtio-net-pci` ARP/ICMP + `rx_intr_count>0` |
| **Net protocols/sockets** | TCP RFC features 0/5→4/5 (MSS/RTO/fast-rexmit/2MSL + wscale parse); loss recovery ≥1000ms→≤3·RTT; RX checksum verified both ingress points; DHCP T1/T2; DNS ≥3 servers+AAAA; live per-iface/per-pid counters; `netstat`; TLS 1.2+1.3 vs 0 | `NETP1I..O` markers + `DHCPRENEW`/`DNSMULTI` offline selftests; one RTO authority; dedicated wired-regression gate (ARP+DHCP+ICMP) |
| **Sound** (BoredOS=0) | concurrent streams 1→8; rates 2→≥6; formats 1→4 (u8/s16/s24/s32); audio syscalls 5→9; gapless via wired BCIS refill | `AMIX1-6` + `AFMT_*`/`ACAP` markers, hardware-free; capture = HELD-FOR-HARDWARE (LPIB/format KAT) |
| **System** | persisted knobs ~6→≥30 across ≥4 namespaces, reboot durability 0%→100% (disk); declarative units 0→≥15 (topo DAG + cycle detect + restart-window reset); VT categories 1→≥5; PTY pairs 0→≥8; tty signals 0→3 | `CFGRIG`/`CFGP1A-D` + `smoke_persist`; `SVCD`/`SVCCTL` markers; `VTTEST`/`PTYP1A-H` |
| **Drivers/USB/power** | USB gens 1→2 (UHCI+EHCI); classes 1→2 (HID+mass-storage); IOAPIC GSIs 0→≥24; MSI vectors 0→≥8; interrupt-driven controllers 0→≥1 | `DRVRIG-A..E` + `USBRIG-F/G/H` on simulated config space; live EHCI/IOAPIC = T410-only |

Every 5x number maps to **either a CI marker or an explicit HELD-FOR-HARDWARE label** — no
unfalsifiable claims.

## 4. Wave 0 — foundation blockers (land BEFORE any feature brick)

The team verified three hard, in-repo blockers:

1. **SYSCALL-LEDGER.** `syscall.h` tops at `SYS_SHUTDOWN=127`; slot 128 was **quadruple-booked**
   and CONFIG-STORE wrongly claimed **124/125 which are LIVE** (`SYS_WLAN_DIAG=124`,
   `SYS_SETSOCKOPT=125` — the latter shipped in brick A4). This is the exact `d5c9e05`
   last-writer-wins bug. **Fix:** publish a fixed disjoint range **128–140** before any build
   (audio `STREAM_WRITE=128`/`GAIN=129`/`RECORD=130`, `SOCK_LIST=131`, `PTY_SPAWN=132`,
   `CFG_GET=133`/`CFG_SET=134`, `DEVTREE=135`; 136–140 reserved). No brick self-allocates.
2. **PCI-CAP-FOUNDATION.** No `pci_find_capability`, no `ioapic.c`; `msi_enable`/`msix_enable`
   are `kprintf` placeholders on the orphaned `device_t*` (`irq.c:517/526`). VIRTIO-NET and
   DRIVER-FW-USB both independently reinvent this. **Fix:** ONE additive sub-brick — cap-walk
   (status bit4 → 0x34 chain, bounded 48 iters) + PCI bridge secondary-bus recursion +
   `pci_msi_enable(pci_device_t*)` + a **standalone `lapic_eoi`/`lapic_id` shim** (since
   `lapic.c` compiles only under `-DSMP_FOUNDATION`). Both consumers call it; neither reinvents.
3. **NET-INGRESS-SEAM owner.** `net_send`/`net_recv` still hard-code `g_nic` (`net.c:33/615/620`)
   and `net_recv` does inline demux at `net.c:623`; the K1 TX seam is landed at `socket.c:234`.
   FOUR bricks edit this hot path. **Fix:** TCP-ROBUST is the **single seam owner**; others
   (NET-RESILIENCE counters, WIFI de-hardcode, VIRTIO dispatch) **append** to settled code.
   Add a dedicated wired-regression gate (ARP+DHCP+ICMP) — aggregate 43/43 is insufficient.

## 5. Revised delivery sequence

- **Wave 0 — Foundation:** SYSCALL-LEDGER · PCI-CAP-FOUNDATION.
- **Wave 1 — Fastest proven 5x (hardware-free):** TCP-ROBUST (seam owner) · AUDIO-FMT converter.
- **Wave 2 — Keystone + observability + mixer:** CONFIG-STORE (keystone) · NET-RESILIENCE-OBS · AUDIO-MIXER-0.
- **Wave 3 — VM fast path + WiFi provable half:** VIRTIO-NET (polled half CI; MSI-X half gated) · WIFI-REAL provable half (net de-hardcode + WPA3-SAE H2E).
- **Wave 4 — Boot-fragile, gated, legacy-fallback mandatory:** SERVICE-MANAGER (`-DSVCD`) · TERMINAL (`-DPTY_SELFTEST`) · DRIVER-FW-USB (decomposed ladder, HELD-FOR-HARDWARE tail).

**DRIVER-FW-USB was REJECTED as a monolith** → 6-step ladder: (1) PCI-cap-walk+bridge →
(2) MSI/MSI-X+`lapic_eoi` shim → (3) IOAPIC routing (default-OFF, 8259 fallback) →
(4) EHCI (poll first) → (5) USB mass-storage BOT → (6) hub class. Each keeps smoke 43/43 with
its `-D` flag off.

## 6. Premises the review corrected (do not design against these)

- **HDA IRQ is LIVE-wired** (`hda.c:224` registers the handler, `hda_stream.c:600` bumps
  `g_hda_bcis` in the ISR). The gapless `on_bcis` refill **completes our own seam**; PIT-poll
  is a probe-decided fallback, not the primary path.
- **TCP RTO already backs off** (`tcp_rt_rto_ms[]`) — the "flat 1000 ms" framing is wrong.
  Pick ONE RTO authority (adaptive `cb->rto_ms`); do not double-backoff.
- **`netif_t` counters are hollow** (read at `netsyscall.c:93`, incremented nowhere) — wire them.
- **LAW 19 byte-identity trap:** append new fields/defines at end-of-struct/end-of-range; gate
  new TUs to compile empty without their `-D`; make **per-`.o` md5** a PR gate on shared files
  (`tcp.c`, `net.c`, `syscall.h`, `socket.h`, `iwl-*.c`).

## 7. Status

- **A4 (SOCKET-PARITY-0)** already landed/proven/committed (`99a37bb`) — uses 125/126/127
  correctly (consistent with the ledger above).
- Next per this charter: **Wave 0 (SYSCALL-LEDGER + PCI-CAP-FOUNDATION)**, then **Wave 1
  (TCP-ROBUST seam owner + AUDIO-FMT converter)**.
