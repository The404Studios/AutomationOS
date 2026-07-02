# Getting It All Working — Batched Execution Plan (2026-06-21)

A sequenced, batch-by-batch plan to drive AutomationOS to "everything working," grounded in this
session's verified findings (40/100/46/50/30-agent audits + empirical QEMU tests). Companion to
`docs/ROADMAP_TO_DONE.md` (the master plan); this file is the **execution sequencing**.

## Where we are (verified this session)
- Default build **boots 43/43** + sig 8/8 + pollselect 6/6 + fork 5/5 in QEMU. Zero regressions.
- **Empirically loads a real webpage**: `DHCP→DNS→ARP→TCP→TLS1.2→HTTP/1.1 200 OK` from `example.com` live (via slirp).
- **~15 commits landed** (all 43/43-gated, UNPUSHED): nvme, tls SHA-384/512, vmm PHYS_TO_DIRECT, diskfs, DeepSeek broker, signal atomics, socket port-collision + CLOSE_WAIT, poll/epoll slice **and** deadline floor, innerHTML markup, gsignin poll, netif_get_by_ip, http UA/Accept.
- **Held (uncommitted):** cc `switch` codegen (needs a runtime test before commit).

## Method — applies to every batch (the discipline that's worked all session)
1. Run a focused agent batch (from the 100-pool) to author patches → **adversarial QA**.
2. I **re-verify each survivor against HEAD myself** (the QA "approved" set still carries ~FPs).
3. Apply only the clean ones → `quick_build`/`build_all` → **43/43 smoke + targeted smoke (+ live test where relevant)**.
4. **Atomic per-subsystem commit.** Defer anything that fails verification. No big-bang batches.

---

## Batches (recommended order)

### Batch 1 — Real-web authentication + data plane  *(highest value)*  ~15 agents
- **K2: TLS cert trust in the live path** — pages load but are UNAUTHENTICATED ("no CA roots"); make `x509_verify_chain` consult the CA bundle so `cert=trusted`. *(security-critical → careful)*
- **K1: per-interface TX** — `net_send`/`ip_tx` → `netif->tx` via the new `netif_get_by_ip` (unblocks WiFi data plane).
- Verify SNI is sent (likely already; example.com connected).
- **Proof:** live fetch reports `cert=trusted`; wifisim sends a real frame; 43/43.

### Batch 2 — Filesystem correctness + DoS-safety  ~12 agents
- **ext2 `file_ops` at open** (P0 — ext2 reads currently silently fail).
- **FS loop-bounds DoS:** FAT32 cluster-cycle detection + ext2 dir `rec_len` bound (malicious-image hangs).
- nvme_free_queue 4-page free; AHCI init-failure DMA cleanup (off-path hardening).
- **Proof:** mount + read an ext2 image; crafted cyclic image terminates; 43/43.

### Batch 3 — Browser real-web functionality  ~14 agents
- Fetch **retry + on-page error** (no silent about:home); external **`<script src>`** loading.
- CSS: inline-`!important` order, **`%` widths in layout**, `@media` (fixed-viewport) handling.
- **Proof:** browser wave green + render a real fetched page; 43/43.

### Batch 4 — cc / toolchain depth  ~12 agents
- **cc `switch` runtime test** → release the held commit; then **floats (xmm)**, **multi-file linker**, recursive aggregate const-fold.
- **Proof:** compile + run `switch` / float / 2-file programs *through the on-device cc*.

### Batch 5 — Net throughput + robustness  ~14 agents
- **e1000 IRQ-driven RX + minimal softirq** (fixes the ARP-timeout symptom; unblocks sustained loads).
- **TCP OOO range-dedup** (P0 double-delivery); UDP RX checksum; **DNS source-validation** (+ live DNS re-test — DNS works, don't break it).
- **Proof:** sustained fetch with zero drop-counter; live DNS resolves; 43/43.

### Batch 6 — AI hardening + go live  ~12 agents
- **Ledger snapshot-path P0** (rollback currently broken); **synth-input STOP-revoke** (STOP must halt injection).
- **Capability/seccomp** fail-closed wiring at the syscall boundary.
- **LIVE model run** — needs your `DEEPSEEK_API_KEY` (or local Ollama) + `run_agent_live.sh`. *(no code gap; user-gated)*
- **Proof:** one live agent step; cockpit STOP halts injection; rollback restores a file; 43/43.

### Batch 7 — libc / POSIX depth  ~10 agents
- **Wire `signal.c` into the build** (currently dead-code) + link to signal-using apps; add **`SYS_LSEEK`+`lseek`**, **`scanf`** family, `exit()` stdio flush.
- **Proof:** port/run a real C program that seeks + scanf; sig/poll smokes.

### Batch 8 — Integrate & release  *(meta — mostly me + your push)*
- Merge the brick branch → `main`; **single app-list source of truth** in `build_all.sh`; wire `smoke_boot.sh` into **CI**; push from Windows git.
- **Proof:** empty `main..HEAD`; CI green on a clean clone.

---

## Big features — each is its own multi-turn program (NOT a single batch)
- **B9 Persistence-into-VFS** — diskfs→VFS adapter at `/persist`, route `/home`, `DISK_PERSIST` default-on validated → durable projects survive reboot.
- **B10 TLS 1.3** — ~3-4 months (supported_versions + key_share, HKDF schedule, new handshake state machine ~1500 LOC). Unlocks TLS-1.3-only modern sites.
- **B11 SMP-by-default / preempt-default** — real `cpu_id()` on AP, per-CPU TSS, drop global lock, ship the preempt fairness handoff.

## Hardware tier — needs YOUR T410 (not QEMU-validatable)
- 82577LM Ethernet PHY bring-up; real iwlwifi RF/uCode; HDA on metal; GPU modesetting (3-6 mo). I write/refine the code behind flags; you flash + serial-validate.

## Honest non-goals (so "everything working" stays honest)
- **YouTube / heavy SPA sites: architecturally infeasible** from-scratch — require HTTP/2, TLS 1.3, ES2020 JIT JS, and MSE + VP9/AV1 video codecs. Realistic browser target = **simple/static HTTPS pages** (which work today).

---

## Suggested cadence
One batch ≈ one focused turn (agents → verify → build → 43/43 → commit). Batches 1–7 are software-
completable from this machine; 8 needs your push; B9–B11 are multi-turn; the hardware tier needs the T410.
Recommended start: **Batch 1 (K2 cert-trust)** — the single biggest real-web win.
