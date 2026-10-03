# AutomationOS gap register -- 2026-10-02

Sources: a six-way read-only audit (YouTube/media, sound, GPU, scheduler/locks, ChainLayer
protocols, OS-side client readiness) plus hands-on verification in QEMU. Every item below is
tagged **FIXED** (landed in the working tree, proven), **OPEN-SW** (software only, can be done
and proven in QEMU), or **OPEN-HW** (needs the physical T410 and/or the user).

Nothing here is committed. Hardware behaviour is inferred from code unless stated.

---------------------------------------------------------------------------------------------

## 0. FIXED this session

| id | what | proof |
|----|------|-------|
| TOOLCHAIN-0 | WSL Arch is now GCC 16.1.1. It lowers aggregate copies/inits to `memcpy`/`memset` calls even under `-ffreestanding -fno-builtin`; crt0-only apps (awk, rpctest, chess ...) failed to link, and `build_all.sh` (`set -e`) stopped at the first one, so **no ISO could be built from the tree**. Fix: `userspace/lib/c/fsmem.c` -- weak `memcpy/memset/memmove/memcmp` (rep movsb/stosb, never a C loop that could recurse) linked into every ELF via `$LD`. Any app/lib with its own strong copy still wins. | full `FULL=1` build + 50/50 smoke |
| PT-DIRECTMAP-0 | **Core kernel bug.** `paging.c` walked page tables through the low identity alias (phys==virt). Under a process CR3 that range holds the process's own user pages: `browser2` (33 MB BSS from 0x882000) shadowed its own PML4 frame at 0x24e8000, so the kernel read/wrote user data instead of page tables. The stack demand-fault "resolved" without installing a PTE, the instruction re-faulted forever, each retry leaked a frame -> **all physical RAM consumed at boot, at any RAM size (512 MB / 1 GB / 2 GB identical)**. Fix: every table dereference (152 sites) goes through the dedicated direct map via `PTV()`; boot-time identity construction is unchanged (flag set only once the direct map is live). Boot also got ~1 s faster. | gdb trace + page-table walk (root cause); T410-profile proof; 50/50 smoke |
| SELFHEAL-3 | The uncommitted rewrite (PID 1 supervises the compositor: monotonic time, bounded backoff, circuit breaker, no external cwatchdog) works. Forced freeze -> stall detected at 2500 ms -> recovery overlay -> backoff 250 ms -> respawn -> **4 windows restored** -> healthy after 2 heartbeats. `SELFHEAL-FIX: PASS recovery=1 restored_windows=4 no_storm=1 no_fault=1`. | `build_test/selfheal_t410_check.sh` |
| FW-0 | **There was no firewall.** Now: stateful IPv4 filter at the two chokepoints (`net_recv` -> `fw_ingress`, `net_send` -> `fw_egress`), default IN=DROP / OUT=ACCEPT, connection tracking (return traffic admitted automatically), built-in DHCP/ICMP handling, ingress sanity (spoof, NULL/SYN+FIN/SYN+RST scans, L4 fragments), `SYS_FW_CTL`, `fwctl` tool, boot self-test over crafted frames. See section 3. | `FW-SELFTEST`, `FWTEST` |
| PCAP-0 | **The kernel had no privilege boundary** -- `capability.h`/`mac.h`/`seccomp.h` exist but are not compiled or wired to any syscall, so any ring-3 process could send/sniff raw Ethernet frames, reprogram IP/gateway/DNS and (with FW-0) the firewall. Now: a monotonic per-process `cap_denied` mask, inherited by spawn/fork/thread, enforced on `SYS_NET_SEND/RECV` (PCAP_NET_RAW) and `SYS_NET_CONFIG` + firewall writes (PCAP_NET_ADMIN). Zero by default -> no behaviour change until something drops. | `FWTEST` |

| TOOLSET-FW-0 + AGENTD-SERVE-0 | Kernel-surface tools for the agent rail: `fw_status` (read-only, auto) and `firewall` (operator-confirmed). The agent can `allow`/`block` a port (range <= 16), `del`, `policy out accept`/`policy in drop`, `ping on/off`; it **cannot** `policy in accept`, `policy out drop`, flush/reset/disable (human-only via `fwctl`). `tool_exec`/`tool_spawn` drop all PCAP bits before launching anything, so launched code is kernel-confined. `agentd` can run as a long-lived tool host (`/etc/agentd.conf`: `serve=1`, `broker=A.B.C.D:PORT`; build with `AGENTD_SERVE=1 [AGENTD_BROKER=...]`), reconnects forever, `PING` keepalive. | `MCP-E2E: PASS` (25 checks) |
| MCP bridge | `scripts/chainlayer_mcp_bridge.py` exposes that rail to ChainLayerTwo / any MCP client (Streamable HTTP, Bearer token, loopback by default, tiers observe/mutate/control, arguments validated, refusals surface as `isError`). | self-test 18/18; `build_test/mcp_bridge_e2e.sh`: MCP client -> bridge -> guest agent over slirp -> kernel: file ops, OS path-gate refusals, **a kernel firewall rule changes whether a live guest service answers a real HTTP request (before: silent, after: HTTP, after delete: silent)**, agent-compiled code runs with every PCAP bit dropped and is refused raw frames |

**Lesson recorded (FW-0):** the first FW-0 build passed 52/52 smoke and `FW-SELFTEST` yet was broken -- the stack's TCP/UDP transmit path (`ip_send_fragment`) calls `nif->tx()` directly and never reaches `net_send()`, so outbound flows were never tracked and every reply was dropped (an agent connection to the host failed with `SKIP no_broker`). Crafted-frame self-tests and the smoke suite cannot see this class of bug because nothing real answers on TCP there; only a real bidirectional TCP harness did (`build_test/run_agentd_hostile.sh`, failing-first then passing). Fixed by hooking the netif branch too. **Any new transmit path must call `fw_egress`/`fw_ingress`.**

---------------------------------------------------------------------------------------------

## 1. Self-heal -- what is still missing

* OPEN-SW  Tight-loop (ring-3 hard spin) freeze is not recoverable on the cooperative kernel (watchdog never gets a slice); PREEMPT does not reliably preempt it either. Needs a kernel timer-ISR stall detector or the PREEMPT hard-spin fix.
* OPEN-SW  Desktop-only: a non-compositor spinner starving the desktop gets the wrong remedy; no per-app supervision.
* OPEN-SW  Kernel IF=0 spin kills everything including PID 1's supervisor (mitigated only by the iteration-cap law).
* OPEN-SW  `selfheal_t410_check.sh` / `selfheal_smoke.sh` grep `CPU EXCEPTION` as "kernel fault" -- it also matches ring-3 faults (that is how the PT-DIRECTMAP bug surfaced as a false "kernel fault"). Should key on `Privilege level: Kernel`, and report user faults separately.
* OPEN-SW  In the T410 profile `browser2` now starts and no longer crashes, but opens no desktop window (taskbar shows Terminal/Files/Network/Sound only). Needs a look -- may be the profile, may be the window handshake (it attaches then immediately detaches its 1.9 MB buffer).
* OPEN-SW  `malloc.c`/`clib.c` arena growth: `clib.c` `arena_new` rounds with `~4095u` (32-bit mask) on a `size_t` -> clears the high 32 bits for >=4 GB totals.

## 2. Internet + WiFi on the T410

Wired first -- it is the realistic path to "on the internet" and the only one with a code path complete enough to try.

| gap | state |
|-----|-------|
| OPEN-HW  82577LM (PCH) wired NIC | default-OFF (`E1000_PCH_NIC`). Ladder exists (`nicup` -> `dhcpc` -> `ping`), **never run on hardware**. Flash a `T410_SAFE=1 PCH_NIC=1` ISO, run `nicup`, read the `E1000PCH:` serial markers. |
| OPEN-SW  DNS | resolver is hard-wired to 10.0.2.3 (`dns.c:79,130`); **no caller ever calls `dns_set_server`**, although `SYS_NET_INFO` already returns the DHCP-learned `dns`. On any real LAN every lookup goes to a dead address. One-line wiring + test. |
| OPEN-SW  DHCP | no lease renew; routing decoupled from DHCP. |
| OPEN-SW  RTC fallback | TLS cert validity uses the RTC; if the year is < 2020 it silently uses 2026-01-01. No NTP. |
| OPEN-HW  WiFi firmware | **there is no `firmware/` directory** -> no `.ucode`; `iwl_fw_load_from_initrd` finds nothing. Need `iwlwifi-<family>-<api>.ucode` (1000/5000/6000/6000g2a) from linux-firmware, and `lspci` from the T410 to know the family. |
| OPEN-HW  iwlwifi | transport/load/NVM/passive-scan written but hardware-unverified. `iwl-ops.c` `connect`, `set_key`, TX, RX are `TODO(HARDWARE)` scaffolds -- **no association, EAPOL, key install or data path exists**. |
| OPEN-HW  WPA3 | **compliant WPA3 (SAE + PMF) is impossible on the shipped Intel 6000-series cards** (firmware has no MFP; see `docs/T410_IWLWIFI.md` addendum). Realistic: WPA2-PSK on a transition-mode SSID; SAE-without-PMF only on lenient APs; wired Ethernet meanwhile. |
| OPEN-SW  K1 | `net_send/net_recv` hard-code `g_nic`; the `netif_t.tx/rx_poll` pointers are never used on the data path, so `wlan0` cannot carry a packet even if it associated. Must land together with the iwl data queues (and call `fw_egress/fw_ingress`). |
| OPEN-SW  TLS | 11 roots; **ISRG Root X2 missing** (Let's Encrypt ECDSA chains E5/E6 won't anchor); default build TLS 1.2 only (`TLS13=1` for 1.3, no HelloRetryRequest); failed cert verification still completes the handshake with `trusted=0` and `http_request` sends the Bearer token anyway; fixed 8 s request timeout; 256 KiB static response buffer silently truncates; no incremental reads/SSE in `http.c` (use `netconn_*`); no JSON builder. |

## 3. Firewall + privilege (FW-0 / PCAP-0)

Model: two chains, first-match-wins, default policies, conntrack, loopback trusted. Built-ins (survive `flush`): DHCP server->client, ICMP echo (toggle, rate limited) and ICMP errors (rate limited). Management: `fwctl status|list|allow|block|add|del|policy|ping|flush|reset|enable|disable`.

* Raw-frame syscalls are now capability-gated; **any future `netif->tx` path (WiFi K1) must call `fw_egress/fw_ingress` or it becomes a bypass.**
* Ingress is filtered *before* ARP/ICMP/TCP/UDP see a frame, so the stack never RSTs an unsolicited SYN (stealth).
* Egress drops report success (silent drop) -- an error return tore TCP connections down.
* `FW_OPEN=1 bash scripts/quick_build.sh` builds with inbound default ACCEPT, for harnesses that must reach a guest service from the host.
* OPEN-SW  not yet: IPv6, ICMP-error RELATED matching (errors are rate-limited, not tied to a flow), per-interface rules, persisted ruleset (`/etc/fw.rules` applied by init), egress source-address validation, SYN-flood limits in the filter (the stack has its own SYN side-table).
* OPEN-SW  `capability.h`/`mac.h`/`seccomp.h` remain uncompiled; PCAP-0 is a deliberately tiny enforced core (4 bits) so it can be audited. Reserved bits `PCAP_SYS_ADMIN`/`PCAP_PROC_CTL` are not yet enforced anywhere.

## 4. ChainLayerTwo on the T410 ("automate every request down to the kernel")

ChainLayer is a Node/TypeScript platform; Node cannot run on AutomationOS. The OS is therefore a **tool host / body**. Findings from the protocol audit:

* No ChainLayer component today lets a device execute tools for the CLI. The embodiment "device-link" is in-memory only.
* **Best first path (no ChainLayer changes): MCP in reverse.** The CLI already connects to any Streamable-HTTP MCP URL with a Bearer token; remote MCP tools are forced to "mutating" (approval required, blocked under `--sandbox`). A host-side shim exposes the OS's typed tools (agentd's whitelist: read/list/stat/ps/write/compile/execute/mkdir/move/remove/spawn/kill/mouse/key, plus the new kernel-surface tools below) and relays `tools/call` to the OS over the existing agentd line protocol.
* Second path: T410 as relay *guest* (terminal/approval front-end, `chainlayer-relay`: plain HTTP/1.1 + SSE + JSON, Bearer token = shell-equivalent on the host).
* Third: T410 as a completions-only client (`/v1/chat/completions`, device-code login) with the agent loop on-device.
* OS-side prerequisites (from the readiness audit): `https_post` exists; need `http_request_ex` (timeout, require-trusted-before-Bearer, chunk end detection, stale-keep-alive retry), ISRG X2, JSON builder + tool_calls parser, token storage (`/etc/clt.conf`; persistence is OFF by default), DNS wiring, SSE via `netconn`. Real-network use needs sections 2's wired bring-up first.

**Kernel-level automation, safely.** "Every request down to the kernel" is implemented as *tiers enforced by the kernel*, not a flat allow-all:

| tier | examples | gate |
|------|----------|------|
| observe | read_file, list_dir, stat, ps, **fw_status** | auto |
| mutate sandboxed | write_file, compile, mkdir, move (under /tmp /home /usr/src) | auto + path gate + snapshot/rollback |
| control | spawn, kill, remove, mouse, key, **firewall** (allow/block/del/policy/ping) | operator CONFIRM in the cockpit |
| kernel-admin | flush/reset/disable firewall, raw frames, IP/gateway/DNS | **not reachable from the agent**; needs PCAP_NET_ADMIN/RAW, held only by human-launched tools |
| launched code | whatever `execute`/`spawn` start | `tool_exec`/`tool_spawn` drop all PCAP_* before launching, so the kernel confines it regardless of what that code tries |

Every step lands in the hash-chained ledger (`LEDGER: VERIFIED`).

## 5. YouTube

* Not close: **no media code of any kind** (no demux/decoders), ES5-subset JS tree-walker (no Promise/async/class/Map/regexp), external `<script src>` never loaded, no `<video>`/canvas/MSE, no flex/grid/float, 800x600 fixed viewport. The real YouTube web app is infeasible (also needs HTTP/2-only paths, cookies, anti-bot tokens).
* Minimum viable path: **host-side transcoder** (yt-dlp + ffmpeg -> ~480x270 MJPEG @ 15-24 fps + s16le 48 kHz over plain TCP/HTTP to 10.0.2.2), plus a native player app (SHM window, `stb_image` JPEG decode, audio via `SYS_AUDIO_STREAM_WRITE`=128). Est. 1-2 weeks incl. QEMU proof. Direct MP4/H.264 on-device is 2-4 months and ~50-100 % of a core at 360p30.
* Audio on real hardware is the other blocker (below).

## 6. Sound (HDA)

Gated by `HDA_ENABLE` (default OFF); no `audioup` trigger exists (copy `nicup`/`iwlup`). Driver gaps (all OPEN-HW to validate): no `bar0==0` check; no PM D0 / TCSEL / snoop / MSI; **raw `interrupt_line` registered unchecked** (a line of 0/1/12 overwrites the PIT/keyboard/mouse handler); pin selection = "first output-capable pin" (pin default config read but never used); mixers/selectors never traversed; amps set without reading caps; EAPD set without checking caps; no jack detect/auto-mute; no Conexant (CX20585) quirk; serial NID prints use `'0'+nid` (garbage above 9); stream setup leaks 64 KB+4 KB per tone; no stop/pause/drain; IRQ handler clears status only while `running`; 44.1 kHz only via the stream path's fixed 48 kHz. QEMU proves register path/DMA/IRQ ring only (`-audiodev none`, audibility never tested).

## 7. Graphics

* Today: GRUB/VBE linear framebuffer + software rendering. **No GPU driver exists** (`nvidia.c` is detect-only and compiled out; no Intel gfx code). The code is inconsistent about which GPU it assumes (NVS 3100M vs Ironlake) -- run `lspci` on the T410 first.
* Highest value, lowest risk: native-resolution framebuffer + verified write-combining + faster present: boot-time FB write-vs-read probe, PAT-WC path, `gfxmode=auto`/1440x900 in grub.cfg, `present_diff` scans ~1M px every frame ignoring damage, per-second clock pulse forces a full composite, `draw_cursor` reads the hardware FB, `simd_blit.c` is not built, mouse hard-clamped to 800x600 (`ps2.c:742`).
* Backlight: only stores a number; needs EC/ACPI or panel PWM. Native modeset/accel (GT218 or Ironlake KMS) is XL/high-risk -- only if it is actually needed.

## 8. Scheduler / spinlocks / SMP

* There is **no "staged" spinlock** -- `spinlock_t` is an xchg test-and-set with PAUSE. BKL is TAS+PAUSE+rdtsc with a 2 s watchdog covering ~59 syscalls (fs/net have no other locks).
* Scheduler: O(1) 140-level MLFQ, per-CPU rq lock, **no balancing/stealing**, `choose_cpu` returns CPU0, idle is `sti; hlt`, eager fxsave.
* Candidate wins (each default-OFF, branch-isolated): kernel appears built with **no `-O` flag** (verify what produced the shipped ELF) -- biggest single lever, high risk; `cpu_id()` is a LAPIC MMIO read repeated 8+ times per enqueue (use RDTSCP/TSC_AUX with fallback); pmm free path takes the global lock every time; slab double-free walk is O(n); TTAS + bounded PAUSE backoff + lockstat; pad the syscall.asm per-CPU slots; MWAIT idle needs your amendment of hardware law 12.
* **Correctness bugs found (separate from tuning):** `g_sig_frame` is one global written by both CPUs' syscall entry (CPU1 can clobber CPU0's sigreturn frame) -> make per-CPU; `sleep_list` guarded by cli only (uniprocessor assumption) with ungated `this_cpu()` wake enqueue; `yield_requeue` unlock/relock gap can double-enqueue; (unverified) x2APIC mode set globally but AP only runs `lapic_enable`; plain locks assume IF=0 while `process_unref` can run in IRQ context; `epoll.c:92` initialises a 1-field `spinlock_t` with 3 fields.

---------------------------------------------------------------------------------------------

## 9. What I need from you (hardware / decisions)

1. On the T410: `lspci` output (WiFi card ID, GPU ID, audio controller) -- one line each.
2. Flash the T410 wired ISO and report the `E1000PCH:` serial ladder (or tell me you have no serial and I will add on-screen markers).
3. Drop the matching `iwlwifi-*.ucode` files into `firmware/` (I will not download firmware blobs on your behalf without you saying so).
4. ChainLayer: which transport first (MCP bridge recommended), the host/port the OS should reach, and where the device token should live. I will not handle live tokens.
5. Commit policy: I have committed nothing.
