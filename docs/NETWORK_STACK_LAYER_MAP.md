# AutomationOS — Network Stack Layer Map & Gap Analysis

**Method:** a 6-agent adversarial layer audit (2026-06-18), one agent per layer band, each
reading the real source and reporting components, the down-interface, the up-interface, proven
functionality, and gaps with severity + cross-layer impact. This document is the synthesis: a
formal layer-by-layer map, two end-to-end vertical traces (wired vs wifi), a consolidated gap
matrix, and the prioritized closure plan.

**Headline:** the **wired vertical is complete and real** (browser → HTTP → TLS → TCP → IP → ARP →
e1000 → live Internet over slirp). The **wifi vertical is connected through the control plane and
then breaks at the data plane.** Two architectural keystones explain almost every wifi/Internet
gap: (K1) the data path bypasses the `netif` abstraction, and (K2) TLS never authenticates certs.

```
            APPLICATION        browser2 · netman · wpasupp · dhcpc · net-tools          [REAL above the seam]
            ───────────────────────────────────────────────────────────────────
  L "beyond"  SYSCALL SEAM      SYS_SOCKET/CONNECT/SEND/RECV · SYS_WLAN_* · SYS_NET_* · SYS_*TABLE
            ───────────────────────────────────────────────────────────────────
  L5-7      APP SERVICES        DHCP · DNS · TLS 1.2(+x509/CA) · HTTP/1.1            [TLS trust = K2 gap]
            ───────────────────────────────────────────────────────────────────
  L4        TRANSPORT           TCP (tcp.c) · UDP (udp.c) · sockets (socket.c)
            ───────────────────────────────────────────────────────────────────
  L3        NETWORK             IPv4 · ICMP · routing (route.c)        [split across net.c + socket.c]
            ───────────────────────────────────────────────────────────────────
  L2        LINK                Ethernet · ARP · netif · 802.11 MLME (seam)         [802.11 data = K1 gap]
            ───────────────────────────────────────────────────────────────────
  L1        DEVICE/PHY          e1000 · rtl8139 · wifisim · iwlwifi · (ath9k=dead)  [net_send hard-codes g_nic = K1]
```

---

## The two keystones (root causes)

**K1 — the data path bypasses the `netif` abstraction.** `net_send`/`net_recv` (net.c:613-626)
are hard-wired: `(g_nic == NIC_RTL8139) ? rtl8139_tx : e1000_tx`. The per-interface
`netif_t.tx`/`rx_poll` function pointers are *stored at registration but never invoked on the data
path* (only the byte/packet counters are read, netsyscall.c:93). Consequences, confirmed
independently by the L1, L2, L3, and seam audits:
- wlan0 cannot carry a single IP packet even when "up + CONNECTED" — the bytes always exit the wired NIC;
- `route_lookup` returns an egress `iface` that `ip_tx` **ignores** (socket.c:233) — routing can't choose eth0 vs wlan0;
- there is no 802.11 data framing path at all; the radio is control-plane-only.
The driver authors document this explicitly (iwl-ops.c:144-149, wifisim.c:11-13): the data path
"must be made wlan0-aware AT THE SAME TIME" the real RXON/ADD_STA land, or traffic goes into a dead link.

**K2 — TLS never authenticates the server.** `tls_cert_trusted()` returns 0 for *every* HTTPS
connection because of two independent bugs in `userspace/lib/tls/tls.c`:
1. it declares a local **4-arg** prototype of `x509_verify_chain` and never includes the real header,
   but the implementation takes **5 args** (the 5th is `now`) → `now` is garbage → time-format check
   fails → `cert_trusted` never set (tls.c:71-74, 1449). Compiles clean *because* of the bad prototype.
2. `parse_certificate` keeps only the **leaf** cert → `ncerts=1` always → real leaf→intermediate→root
   chains can't validate even with bug 1 fixed (tls.c:1074, 1448).
The 7-root CA bundle and the otherwise-correct `x509_verify.c` are wasted. Every HTTPS fetch is
**encrypted-but-unauthenticated** and proceeds silently (no enforcement) — a MITM is undetected.

---

## L1 — Device / PHY

| Driver | Serves | tx | rx_poll | get_mac | link-state | Registered | Proven |
|---|---|---|---|---|---|---|---|
| **e1000** | 82540/82574/PCH | ✅ | ✅ | ✅ | ✅ live STATUS.LU | eth0 | **QEMU-proven (the load-bearing NIC)** |
| **rtl8139** | RTL8139C | ✅ | ✅ | ✅ | ❌ no MII read | eth0 | code-complete fallback |
| **wifisim** | none (sim) | ⛔ NULL | ⛔ NULL | ⛔ NULL | sim state | wlan0 | control-plane proven |
| **iwlwifi** | real T410 radio | 🟡 stub→-1 | 🟡 stub→0 | ✅ NVM MAC | scaffolded | wlan0 (HELD) | correct-by-review (no emulator) |
| **ath9k** | Atheros | ❌ wrong seam | ❌ | ❌ | ❌ | **none** | **VESTIGIAL — not compiled, no caller** |

- **DOWN (to HW):** e1000 + rtl8139 complete (MMIO/DMA/`sfence`/bounded spins, 32-bit DMA guards). iwlwifi DOWN exists but correct-by-review only.
- **UP (to L2):** THREE contracts coexist — `netif_t` fn-ptrs (wired/sim), `wifi_ops` (radio control), and `ieee80211_ops` (ath9k, **unbridged**).
- **Gaps:** **K1** (net_send hard-code); **ath9k vestigial** (CRIT — wrong seam, no netif, not in `quick_build.sh`, no caller); rtl8139 no link-state (MED); single-global `g_nic` → no multi-NIC coexistence (LOW).

## L2 — Link (Ethernet / ARP / netif / 802.11)

- **Ethernet II only** (net.c) — ethertypes ARP + IPv4; no VLAN/IPv6. **No 802.11 data framing anywhere.**
- **ARP** (net.c:124-228): 16-entry cache, request/reply/learn, **bounded** resolve (the historical IF=0 freeze is fixed — three 200k-iteration caps). Gaps: slot-0 eviction (no LRU), **no aging/timeout**, no gratuitous/DAD.
- **netif** (netif.c): registry of 4, default = first `up`, wifi-seam attach clean. Coexistence broken at the data plane (K1) — one global `net.{ip,gateway}`, `netif_sync_globals` mirrors only the default.
- **802.11 MLME:** `mac80211_core.c` is a **stub** (all TODO); the real "MLME" is the `wlan_state_t` enum driven through `wifi_ops` — backends short-circuit states, no auth/assoc frame exchange. **No SAE/WPA3 negotiation** (enum only); RSN-IE→WPA2 only.
- **DOWN/UP verdict:** bridges L1↔L3 **fully for wired**; for **wireless it does not bridge** — no data framing, no decap (mac80211 stub), K1 routing bypass.

## L3 — Network (IPv4 / ICMP / Routing)

**Structural CRIT (G1): L3 is split across two files with two independent IPv4 parsers + two TX
builders + two checksum functions.** net.c owns ARP/ICMP RX + reassembly + config; socket.c owns the
real `ip_tx`/`ip_send_fragment` + the TCP/UDP RX demux (`ipv4_demux`). **Every received IP frame is
parsed twice.** No unified `ip_input`/`ip_output`.

- **Routing** (route.c): a *real* 16-entry longest-prefix table — but barely used. `ip_tx` **ignores** the returned egress `iface` (K1); source IP is always the single global; **DHCP/`sys_net_config` never update the route table** (G3) so the default route + /24 prefix go stale after any reconfig.
- **ICMP:** echo request/reply **proven** (ping both ways). **Error generation is built but DEAD** (G2) — dest-unreach/port-unreach/TTL-exceeded/frag-timeout all have zero callers; `net_icmp_port_unreachable` is declared-never-defined.
- **Fragmentation:** TX split + RX reassembly both implemented (reassembly uses a ~2MB `.bss` per-byte hole map — DoS surface, LOW-MED). **No inbound TTL check / no forwarding.**
- **Gaps:** G1 split-IP (CRIT structural), G2 dead ICMP errors (MED), G3 route↔config decoupling (MED), G4 no multi-interface egress (MED, K1 tie-in), silent ARP-fail drop (MED), reassembly DoS (LOW-MED), no loopback/broadcast/IPv6 (LOW), shared static TX scratch not SMP-safe (LOW).

## L4 — Transport (TCP / UDP / sockets)

- **TCP** (tcp.c): full 3-way handshake (active + passive via SYN side-table), dynamic receive window + zero-window persist, seq-wraparound-safe, MSS segmentation of large sends (**the historical >1KB truncation bug is FIXED**), 4-deep OOO reassembly, FIN→CLOSE_WAIT/TIME_WAIT, RST. **Limits:** `SOCK_MAX=32`, OOO depth 4, **single retransmit slot** (only the most-recent outstanding segment is retransmittable — CRIT under loss), **no SACK / no fast-retransmit / no congestion control**, **no 2MSL TIME_WAIT**, OOO FIN dropped, no FIN_WAIT_2/CLOSING/LAST_ACK.
- **UDP** (udp.c): TX+RX complete for DNS/DHCP; **RX checksum unverified** (LOW); no ICMP port-unreachable (LOW).
- **Sockets** (socket.c): socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/close + `sock_poll`. **No `setsockopt`/`getsockopt`**; accept/recv **non-blocking only** (apps must spin-poll).
- **CRIT cross-layer (L3↔L4): inbound IP fragments never reach L4** — `ipv4_demux` (socket.c:328) bypasses net.c's reassembler (which serves ICMP only), so a fragmented inbound TCP/UDP segment is truncated/corrupt.
- **Verdict:** fine for the happy path (few connections, no fragmentation, low loss = QEMU/LAN); **not robust for a real multi-connection lossy Internet session** (single-slot retransmit + no congestion control + the fragment bypass).

## L5-7 — Application services (DHCP / DNS / TLS / HTTP)

- **DHCP** (dhcp.c): **full DORA** + option parse + lease, applies via SYS_NET_CONFIG. Gaps: **no RENEW/REBIND** (T1/T2 timers absent, lease never renewed — MED); single-interface (MED); first-router/first-DNS only (LOW).
- **DNS** (dns.c): A-record resolve + 64-entry TTL/LRU cache + EDNS0 + PTR. Gaps: no CNAME-chasing query (LOW), single server / no fallback (LOW-MED), no AAAA/IPv6.
- **TLS** (tls.c + x509): TLS **1.2** client, modern ECDHE+AEAD suites (GCM/ChaCha20) + SNI + SKE signature verify; x509_verify.c chain logic is competent; **7 real CA roots embedded.** **CRIT: trust is dead (K2)** — the 4-vs-5-arg `now` bug + leaf-only chain → `cert_trusted` always 0; verify failures are **silent** (handshake completes anyway, http.c never checks `trusted`). No TLS 1.3, no resumption.
- **HTTP** (http.c): **more complete than expected** — GET/POST/PUT/range, chunked, content-length, redirects (5-hop), **gzip/deflate**, keep-alive, bounded timeout. Gaps: no HTTP/2 (LOW).
- **Verdict:** the browser **can fetch real HTTPS pages as bytes**; it **cannot authenticate** them (K2).

## Syscall seam + apps ("beyond")

- **Seam:** SYS_SOCKET/CONNECT/SEND/RECV/SENDTO/RECVFROM/SOCK_POLL/BIND/LISTEN/ACCEPT (51-78), SYS_NET_INFO/CONFIG/ROUTE_TABLE/ARP_TABLE (59/89/90/91), SYS_WLAN_SCAN/CONNECT/STATUS/DISCONNECT/SET_KEY (113-117). UAPI structs all `_Static_assert`-sized (8/8 verified). Control-plane surface essentially complete.
- **Seam gaps:** **no EAPOL/mgmt-frame syscall** (CRIT) → the supplicant *cannot* run a real 4-way handshake with an AP (`tx_mgmt`/`rx_poll_mgmt` exist in-kernel but aren't exposed) — so wpasupp is a self-play key-derivation loop with demo nonces; **no IRQ-driven RX** (RX only advances on explicit `SYS_SOCK_POLL` — a non-polling backgrounded server goes deaf); `sys_wlan_status` leaves bssid/ssid zero (MED, latent).
- **Apps are REAL above the seam:** browser2 (real DNS→TLS→TCP), http GET/POST, netman (real SYS_WLAN_*), dhcpc/autodhcp (real DORA). The only mock is `wifisim` at the very bottom, behind the swap-seam.

---

## Vertical trace 1 — WIRED (browser → https://example.com): ✅ fully connected
```
GUI click → http.c https_get → DNS (real UDP) → TLS 1.2 handshake (real, on wire)
   → TCP active-open (socket.c) → ip_tx (socket.c) → resolve_mac/ARP → net_send → e1000_tx → WIRE
```
Every hop real and proven to a live external HTTPS server over slirp. **Only break: K2** (the page is
fetched + decrypted but not *authenticated* — the lock can never be green).

## Vertical trace 2 — WIFI (same, over wlan0): ⛔ breaks at the data plane
```
scan/connect/status/set_key  → SYS_WLAN_* → wifi_ops → wifisim/iwlwifi   [control plane: REAL]
                                                                           ───── break ─────
IP data → net_send → (K1) hard-coded e1000_tx, NOT wlan0->tx → exits the WIRED NIC, never the radio
4-way handshake → (no EAPOL syscall) → wpasupp invents nonces → key install discarded by driver
```
The wifi control plane reaches CONNECTED, but **no IP packet ever rides the radio.** Closing it needs:
(a) route the data path through `netif.tx/rx_poll` (K1), (b) an EAPOL/mgmt-frame transport, (c) the
real iwlwifi data queues (hardware phase).

---

## Consolidated gap matrix (severity-sorted)

| # | Gap | Layer(s) | Sev | Cross-layer impact | Fix |
|---|---|---|---|---|---|
| K1 | data path hard-codes `g_nic`, bypasses `netif.tx`/`rx_poll` | L1-L3 | **CRIT** | no wifi data path; routing can't pick egress; netif abstraction unused | route `net_send`/`net_recv` through the routing-selected netif's tx/rx_poll (wired fallback preserved) |
| K2 | TLS cert trust dead: 4-vs-5-arg `now` + leaf-only chain | L5-7 | **CRIT** | every HTTPS unauthenticated; MITM undetected | include x509_verify.h, pass real RTC `now`, collect full cert chain *(fixer dispatched)* |
| G1 | split IP layer (2 parsers/builders/checksums) | L3 | **CRIT (struct)** | every IP fix must be made twice; maintenance landmine | unify into one `ip_input`/`ip_output` |
| F1 | inbound IP fragments never reach L4 (`ipv4_demux` bypasses reassembler) | L3↔L4 | **CRIT** | fragmented inbound TCP/UDP truncated/corrupt | demux through net.c's reassembled buffer |
| T1 | single retransmit slot, no SACK/fast-rt/congestion control | L4 | **CRIT (under loss)** | large transfers stall/thrash on real lossy paths | per-segment retransmit queue + dup-ACK fast-retransmit |
| E1 | no EAPOL/mgmt-frame syscall | seam/L2 | **CRIT (wifi)** | no real 4-way handshake with an AP | add a mgmt-frame transport syscall over `tx_mgmt`/`rx_poll_mgmt` |
| A1 | ath9k vestigial (wrong seam, uncompiled, no caller) | L1 | MED | dead code / false capability | delete or port to the `wifi_ops` seam |
| M1 | mac80211_core.c is a stub (no 802.11 RX/decap) | L2 | MED | no 802.11→Ethernet path even with a radio | implement decap/defrag or rely on firmware-offload (DVM does) |
| G2 | ICMP error generation dead (built, no callers) | L3 | MED | no traceroute/PMTUD/fast-fail | wire dest-unreach/port-unreach/TTL-exceeded |
| G3 | routing decoupled from IP config (DHCP doesn't update routes) | L3 | MED | stale default route on real LANs | `netif_sync_globals`/`sys_net_config` → `route_*` |
| RW | DHCP no RENEW/REBIND (T1/T2) | L5 | MED | lease expiry on long uptime, no recovery | add T1/T2 renew loop |
| RX | no IRQ-driven NIC RX (poll-only) | seam | MED | backgrounded non-polling server goes deaf | NIC RX IRQ → wake pollers (or a background pump) |
| TW | no 2MSL TIME_WAIT | L4 | MED | port/seq reuse hazard under churn | add TIME_WAIT timer |
| SO | no setsockopt; accept/recv non-blocking only | L4 | MED | apps can't tune / must spin | add setsockopt + blocking variants |
| WS | `sys_wlan_status` leaves bssid/ssid empty | seam | MED (latent) | future consumer breaks | fill the UAPI fields |
| — | rtl8139 no link-state; ARP no aging; UDP RX checksum unchecked; reassembly DoS; no IPv6/VLAN/loopback; DNS single-server; no SAE negotiation; RSSI placeholder; stale docs (ca_bundle "empty", dhcp.h "no bind", dhcpc "no apply") | various | LOW | — | as scoped |

**What is solid (do not "fix"):** the wired Ethernet/ARP/ICMP/IP path (bounded, QEMU-proven); TCP
handshake/window/persist/segmentation/OOO/wraparound; UDP for DNS/DHCP; the full HTTP/1.1 client
(chunked/redirect/gzip/keep-alive); TLS 1.2 handshake + suites + SNI; the x509 chain logic + CA bundle;
the clean UAPI ABI; the `wifi_ops` swap-seam; the real apps above the seam.

---

## Prioritized closure plan

1. **K2 — TLS trust** *(in progress, fixer dispatched)*: include the header, pass a real RTC `now`,
   collect the full cert chain. Makes real HTTPS *authentication* work. Userspace, low-risk, high-value.
2. **K1 — `netif.tx` routing**: make `net_send`/`net_recv` dispatch through the routing-selected
   interface's `tx`/`rx_poll`, with the wired NIC as the behavior-preserving fallback. Unifies the
   stack and unblocks the wifi data path. *Must land together with the real iwlwifi data queues (or it
   routes into a dead radio).* Core data-path change — its own brick, byte-identical wired verification.
3. **F1 — fragment demux**: route `ipv4_demux` through net.c's reassembled buffer. Correctness for
   fragmented inbound traffic.
4. **G3 — route↔config coupling**: update the route table on DHCP/`sys_net_config`. Correct routing on
   real LANs.
5. **G1 — unify the IP layer**: one `ip_input`/`ip_output`. Removes the double-parse maintenance trap.
6. **E1 + the wifi data tail** (hardware phase): EAPOL transport + real iwlwifi RXON/ADD_STA/data queues
   + K1 — the three that together make wifi carry real traffic.
7. **T1 / TW / RW / G2 / RX / SO** — robustness for the real Internet (loss recovery, TIME_WAIT, DHCP
   renew, ICMP errors, IRQ RX, setsockopt), as the use-cases demand.

**Bottom line:** there is no gap in the *wired* vertical's functionality. The gaps are concentrated in
(a) the *wireless data plane* (K1 + E1 + the hardware data tail), (b) *HTTPS authentication* (K2), and
(c) *real-Internet robustness* (fragments, loss recovery, route freshness). K1 and K2 are the two
keystones — closing them converts "encrypted bytes over the wired NIC" into "an authenticated stack
that can route over the chosen interface."
