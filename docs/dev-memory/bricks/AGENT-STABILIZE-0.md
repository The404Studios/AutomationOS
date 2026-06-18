# AGENT-STABILIZE-0 — lock down the proven Nemotron agent foundation

Status: **LANDED (local)** on `brick/agent-stabilize-0` (off `brick/smp-matmul-batch-0` @ 46ffcc3).
Not yet pushed (user pushes from Windows PowerShell).

## Why
A 10-agent sweep found the Nemotron OS-automation agent was already built and proven
(`agentd` + 13 gated tools + run-open-code + brokers, serial `AGENTD: PASS`) but the **entire
agent subsystem was untracked** — 402 untracked entries, one `git clean -fxd` from deletion — and
the kernel diffs it depends on were uncommitted. This brick commits the proven foundation in clean
bricks, deletes dev scratch, and pins a green build+smoke baseline before any new feature work.

## What landed (7 commits)
1. `feat(exec/sched)` — kernel process-model: `g_exec_spawn_argv[4096]` (agent-rpc v2 base64),
   `sys_spawn_ex_argv` bound 4095, `copy_user_string` sys_spawn path fix, PREEMPT-WAITSAFE-0
   (`WAIT_RECHECK_MS=25` self-healing block in waitpid/thread_join).
2. `feat(agent-rpc)` — `agent_rpc.h` v2 (`TOOL_ARGS_MAX` 256→3072, packet < CH_PAGE).
3. `feat(net)` — `http_request`/`http_post`/`https_post` (POST verb, body, headers; hdr buf 4096).
4. `feat(ide/cc)` — global-initializer/array/multi-declarator/struct-init codegen + the 9-case
   host regression suite. **CCREGRESSION: PASS tests=9 failures=0**.
5. `feat(agent)` — the gated rail userspace: `agentd`, `codeagent`, `tool_*`, `claudehost`,
   `gsignin` + host brokers (`nemotron_mock.py`, `nemotron_broker.js`, `claude_broker.py`).
6. `feat(desktop)` — UI-CRISP + DOCK-DND + browser/compositor consolidation + wire all new apps
   into `build_all.sh`/`init/main.c`; delete m2–m7/minimal/simple + the legacy text browser.
7. `chore(repo)` + this CI/gate commit — `.gitattributes` (LF), extended `.gitignore`, deleted 194
   scratch files; smoke-gate fixes + CI jobs (below).

## Green baseline (this toolchain: WSL-Arch, gcc 15.2.1)
- **Build:** kernel `quick_build.sh` 93/0, userspace `build_all.sh` BA_EXIT=0, ISO 127 sbin
  entries, fs:0x28 canary all 0.
- **Byte-identity:** default kernel md5 = `29b434764dbde177036dad4469d96c17`, reproducible across
  two clean builds. Anchor re-pinned in `scripts/threadinherit_smoke.sh` (was the stale
  `6f99ed9f…`).
- **smoke_boot.sh: 43/43 PASS** (was 35/43). Gate corrections (kernel HEALTH, not "no exceptions"):
  - `check_no_cpu_exception` / `check_no_page_fault`: tolerate ring-3 faults the kernel CONTAINED
    (terminated + survived) — e.g. sigtest [5]'s deliberate bad handler at RIP 0x4000. Uncontained
    (kernel-mode/fatal) still fails.
  - `check_kernel_started`: accept `[VMM]`/`[HEAP]`/`[RTC]` (the `[KERNEL]` tag is BOOT_QUIET-
    suppressed).
  - `check_webstack`: browser2 marker (the legacy text browser was removed; browser2 is gated by
    `check_browser_wave`).
  - RQLOCK/AFFINITY/heap/slab self-tests: report **N/A** when compiled out (SCHED_DEBUG off /
    BOOT_QUIET), but still FAIL on an actual `FAIL` marker; the runtime `[SCHED_INVARIANT]` guard
    stays a hard fail.
- **sig_smoke.sh: SIGFULL: PASS** (all 7 checks, contained 0x4000 fault, kernel survives).
- **CI (`.github/workflows/test.yml`):** + compiler-regression (`cc_regression_smoke.sh`) and
  web-fuzz (`web_fuzz_smoke.sh`, ASan over the HTML parser, **WEBFUZZ: PASS inputs=9**).

## Stale assumptions the sweep corrected
- TLS "now-arg cert bug" is **already fixed** (`x509_verify.c` threads `now_yyyymmddhhmmss`) —
  NET-TLS-TRUST is not a blocker.
- The agent broker port is **8433** (not 8431).
- playful-boole "Phase 0" (temp applauncher spawn) was already done — `init/main.c` is clean.

## Phase C progress — the gate is solid; agentd wired live
- **Gap closed:** the committed agent system was DORMANT — `init/main.c` never spawned `agentd`,
  so the feature wasn't live (the proof scripts boot the default ISO expecting it). Added the spawn
  next to `claudehost`: net-gated and self-skipping (`AGENTD: SKIP no_net/no_broker`), so a normal
  boot is unaffected; it comes alive when a broker is up on `10.0.2.2:8433`.
- **C6 adversarial proof PASSES** (`run_agentd_hostile.sh`, hostile mock plays the attacker):
  - `AGENTD: DENY tool=delete_everything` — unknown destructive tool → whitelist DENY.
  - `AGENTD: DENY tool=read_file` (`/etc/../boot/grub.cfg`) — `..` traversal → path-policy DENY.
  - `AGENTD: TOOL read_file /etc/toolset0.txt` → `AGENTD: PASS loop_completed steps=3` — legit allowed.
- **Positive end-to-end proof PASSES** (`run_agentd_codetask.sh`): the agent runs the full
  run-open-code pipeline — `mkdir /tmp/agentproj` → `write_file m.c` (base64 over the v2 rail) →
  `compile` (ON-DEVICE /bin/cc) → `execute /tmp/m.elf` → `ps` → `remove /etc/passwd` **DENIED** →
  `AGENTD: PASS loop_completed steps=6`. This is the on-device validation of the cc commit that the
  host-only `cc_regression_smoke.sh` could not give, plus the agent-rpc v2 base64 rail end-to-end.
- **Rail assessment:** the live gate is already well-built — whitelist + `bad_path` run BEFORE any
  dispatch, tool stdout is newline-collapsed (closes the multi-line broker-desync vector), stdout
  flows over one-shot P6c capabilities. Remaining C items are ENHANCEMENTS, not open holes:
  externalized `/etc/ai/policy.json`, `O_NOFOLLOW` symlink defense (needs kernel support; low
  exploitability today — no symlink-creating tool is whitelisted), tool-level rollback, ledger
  integrity hash-chain.

## Phase C/D push (10-agent) — synthetic input LIVE + a real gate bypass closed
- **CRITICAL gate bypass FOUND + CLOSED:** `shell` was on agentd's `resolve_tool` whitelist;
  `tool_shell` forwards its argv[2..] UN-GATED to `/bin` coreutils (cc/touch/tee/head/...) which
  carry no path policy, and `bad_path` only checks av[0] (the command NAME). So
  `{"tool":"shell","args":"touch\t/etc/evil"}` defeated the whole write allowlist. FIX: removed
  `shell` from the rail (agentd.c `resolve_tool`); re-enabling requires gating every `/bin` path arg.
- **SYNTHETIC INPUT (D1) BUILT + PROVEN** — the agent can now drive mouse+keyboard:
  - `userspace/include/synthinput.h` (SHM ring, mirrors dockdnd), `tool_mouse` + `tool_key`
    (freestanding, attach lookup-only, enqueue), wired into `resolve_tool` (mouse/key replace shell;
    CONFIRM-class) + build_all.
  - `compositor_m8.c`: `pump_synth_input()` drains the ring each frame BEFORE the real PS/2 pump,
    reusing send_pointer_to_focus / wm_handle_key / send_key_to_focus; compositor creates+owns the
    0600 page (active=1).
  - PROOF (`run_agentd_gui.sh`, NEMO_GUI=1): `SYNTHINPUT: injection page ready` → `AGENTD: TOOL
    mouse move` → `SYNTHINPUT: input applied (agent is driving)` → click/key/move → `DENY badtool`
    → `AGENTD: PASS loop_completed steps=5`. The compositor CONSUMES the agent's injected events;
    the gate still denies unknown tools in GUI mode.
- **C1 policy.json LANDED:** `etc/ai/policy.json` (canonical, auditable gate policy) seeded into the
  initrd (`/etc/ai/policy.json`) so aibroker loads it instead of built-in defaults. NOTE: aibroker's
  parser is a dumb token scanner — the words allow/require_approval/deny may appear ONLY as the 3
  array keys (the file is crafted around this). agentd's gate stays compiled-in (resolve_tool).
- **Committed-foundation reviews:** kernel process-model brick (d837409) reviewed SOLID (no bugs;
  BSS buffer copied under caller CR3, PREEMPT-WAITSAFE-0 recovers lost wakes correctly). Gate review
  found ONLY the shell bypass (now closed); traversal/arg-injection/JSON-escape/spawn/kill all sound.
- **DESIGNS READY (not yet applied):** O_NOFOLLOW symlink defense (C2: O_NOFOLLOW=0x100, final-
  component-only check in vfs.c, tool opens) — precise patch in hand; tool-rollback + ledger
  hash-chain (C3/C4: agentd pre_snapshot + FNV1a chain + verify util) — ramfs = in-session only;
  cockpit GUI (D2: new `cockpit.c` extending the anthropic/ui.h pattern, spawns agentd + parses its
  serial, Allow/Deny/grant-full/STOP). Apply these next.

## Cockpit (D2) + CONFIRM gate (C5) — the human-facing surface, PROVEN
- **Ring-desync BUGFIX (a29a373 follow-up):** the synthetic-input SPSC ring desynced — producers
  kept `head` in `[0,QMAX)` (`% QMAX`) but `pump_synth_input` advanced `tail` UNBOUNDED, so after
  ~64 events `tail!=head` stayed true forever → stale-event replay (cursor jitter/stuck keys). Fixed:
  consumer now masks `tail = (tail+1) & (QMAX-1)`. (Found by the adversarial review of a29a373.)
- **Cockpit seam (`agentcockpit.h`, KEY 0x41434B50):** the cockpit OWNS a 0600 SHM page (control:
  goal/goal_seq/stop/grant_full/confirm; status: state/step/tool/args/last). agentd attaches it
  LOOKUP-ONLY: **absent → g_cp NULL → byte-identical to today** (every existing proof still passes —
  verified: hostile DENY/DENY/TOOL/PASS steps=3 unchanged).
- **`cockpit.c`** (new UI app, crt0-linked like filemanager for `--proof` argv): goal textbox,
  RUN/STOP, scroll step-log, Allow/Deny + grant-full checkbox; RUN posts goal+spawns agentd; per-frame
  tick reads status + streams steps; `--proof` auto-posts a goal + auto-RUNs headlessly.
- **agentd integration:** cp_attach + per-step status writes + STOP check + a CONFIRM gate on the
  CONFIRM-class tools (remove/spawn/kill/mouse/key — wait for Allow/Deny unless grant_full, bounded-
  poll, fail-safe deny on timeout/STOP) + pre_snapshot (write_file/remove → /var/snapshots/<base>.<seq>,
  ramfs = in-session). All g_cp-guarded.
- **C3 `tool_rollback`** (new, on the rail): restores a file from its latest snapshot.
- **Build/boot wiring:** cockpit + tool_rollback compiled (canary 0), staged, canary-listed; a
  `COCKPIT_PROOF` build flag gates an init `spawn_args("sbin/cockpit","--proof")` for the headless test
  (default boot never auto-runs it). `build_test/run_cockpit.sh`.
- **PROVEN (`run_cockpit.sh`, COCKPIT_PROOF=1):** `[INIT] COCKPIT_PROOF: launching sbin/cockpit
  --proof` → `[COCKPIT] proof: posted goal seq=1` → `AGENTD: GOAL sent` → `TOOL list_dir` → `AGENTD:
  PASS loop_completed` (+ `[COCKPIT] step N: <tool>` proves STATUS-back). smoke_boot 43/43, no
  regression. **`COCKPIT: PASS` — cockpit↔agentd seam proven headlessly (no human).**
- **C4 ledger hash-chain DONE + PROVEN:** aibroker's `ledger_record` now appends `seq=<n> hash=<16hex>`
  where hash=FNV1a(prev_hex || fields || seq); new freestanding `sbin/ledgerver` re-reads
  /var/log/ai/actions.log and recomputes the chain. Boot proof: **`LEDGER: VERIFIED records=10`**
  (init spawns ledgerver after "All services started"); smoke_boot 43/43. /var is ramfs => in-session
  integrity only.
- **C2 O_NOFOLLOW DEFERRED (low priority):** exact patch in hand (O_NOFOLLOW=0x100, final-component-only
  in vfs.c, tool opens), BUT the adversarial review confirmed the symlink hole is currently
  UNREACHABLE (no whitelisted tool can create a symlink), so it defends a theoretical attack at HIGH
  kernel-regression risk. Note: the round-1 patch is incomplete (threads `flags` through the resolver
  but never wires vfs_open's open-flags into it) -- finish that before applying. Park until a
  symlink-creating capability lands.

## 10-agent skeptical AUDIT + the hardening pass (the build had outrun the proof)
After the fast build, a 10-agent adversarial audit found the architecture sound + the broader OS
GREEN (no regressions; kernel/net/SMP untouched; smoke 43/43 honest; git clean, 15 commits intact
but UNPUSHED) -- but the **human-in-the-loop safety model was asserted, not demonstrated**, plus real
bugs hid behind mock proofs:
- **The CONFIRM Allow/Deny gate had NEVER fired in any test** (the cockpit proof ran read-only tools;
  the dangerous-tool proofs ran with NO cockpit -> auto-allow). Same for rollback/pre_snapshot/STOP/
  grant_full.
- **Cockpit goal truncated to its first word** -- it spawned agentd via space-split argv; the mock
  ignores the goal so the proof passed while the feature was broken.
- `rollback`/`move` mutate files but were NOT CONFIRM-class (vs policy.json). `confirm_gate` cleared
  the decision AFTER announcing CONFIRM (lost-Allow race). `tool_key` ring index wasn't masked like
  tool_mouse (latent OOB).

**FIXES (this pass):** rollback+move -> is_confirm_tool; confirm_gate clears decision BEFORE announce;
tool_key masks head/tail; agentd reads the FULL goal from `g_cp->goal` (untruncated) when a cockpit is
attached; added `AGENTD: CONFIRM-WAIT/CONFIRM-ALLOW` markers.

**KEYSTONE PROOF (`run_cockpit_confirm.sh`, NEMO_CONFIRM):** the cockpit (--proof) auto-ALLOWs file
ops + auto-DENIES spawns. `COCKPIT-CONFIRM: PASS` --
`CONFIRM-WAIT remove -> auto-confirm ALLOW -> CONFIRM-ALLOW remove`,
`CONFIRM-WAIT spawn -> auto-confirm DENY -> CONFIRM-DENY spawn`,
`rollback ALLOW -> TOOL_ROLLBACK: OK a.txt.3 -> read-back = v1` (proves CONFIRM both ways + rollback
+ pre_snapshot end-to-end). The audit's #1 gap is CLOSED.

**HONEST deferred / known-limits (recorded, NOT overclaimed):**
- **No-cockpit posture = autonomous mode**: with no cockpit attached, CONFIRM-class tools auto-run
  (path+whitelist gated only). This is BY DESIGN (the user delegates by starting the agent unattended);
  supervised mode = open the cockpit. Now explicit.
- **SHM 0600 is uid-0 theater**: all agent procs run as uid 0, so the "owner-only" guard confers no
  isolation -- a future tool that does raw shmget+write could set grant_full itself. Needs non-root
  agents or a non-DAC guard. The 0600 comments are aspirational on this kernel.
- **C4 ledger audits the LEGACY aibroker self-test, not the live agentd path** (agentd writes no
  ledger). It's a public-seed checksum chain (not a keyed MAC) and unsanitized args allow newline/
  field injection. Relabel its scope; don't claim it covers the live agent.
- **policy.json is decorative for agentd** (gate is hardcoded resolve_tool/is_confirm_tool; editing
  the file changes nothing live -- only aibroker loads it). Real next step: load policy in agentd.
- **rollback basename-collision**: snapshots key on basename only -> `/a/x` and `/b/x` alias. Low
  severity (only agent-written files, writable tree). Fix = full-path key.
- Synthetic input >64 events + visible cursor/text effect still unproven (serial marker only).

## "Get everything working" pass (10-agent designed, integrated + verified)
Closed the audit's remaining STRUCTURAL gaps so the safety model is configurable + audited for the
LIVE agent (not just the supervised cockpit case). All built clean (canary 0, smoke 43/43):
- **agentd is now POLICY-DRIVEN (tighten-only):** `policy_load()` reads `/etc/ai/policy.json` and
  overlays a deny-set + confirm-set on the hardcoded floor. It can only TIGHTEN (add DENY/CONFIRM),
  never loosen -- the `allow` array is IGNORED, `resolve_tool` stays the sole whitelist, exact-name
  match, empty tokens dropped, load-once, fail-safe (absent/bad file => floor). Adversarially vetted:
  GO. policy.json fixed (`move` -> require_approval). PROOF: `run_policy_deny.sh` adds read_file to
  deny -> `AGENTD: DENY tool=read_file` (live gate changed by editing the file).
- **agentd SELF-AUDITS:** writes its own FNV-1a hash-chained ledger to `/var/log/ai/agent.log`
  (separate from aibroker's actions.log), one record per gated decision (ALLOW/DENY/CONFIRM-*/STOP),
  args SANITIZED against injection. `ledgerver <path>` verifies it; init re-verifies after agentd's
  reap. PROOF: `run_agent_ledger.sh` -> **LEDGER: VERIFIED records=6** (the live agent's decisions,
  tamper-evident). Closes the C4 "legacy only" overclaim.
- **rollback FULL-PATH keyed:** pre_snapshot + tool_rollback encode the full path (`/`->`_s`,
  `_`->`_u`) so `/tmp/a/x` and `/home/x` never collide. CONFIRM proof still restores (read=v1).
- **>64 synthetic-input proof:** `tool_mouse moven <dx> <dy> <count>` injects many events from one
  gated step (yields every 32 to let the consumer drain); compositor prints `drained N events` once
  past QMAX. `run_synth_stress.sh` proves the masked-tail wrap with no stale-replay.
- HONEST: SHM 0600=uid-0 theater + persistent (non-ramfs) ledger/snapshots across reboot remain
  architectural; live model run needs the user's key (Puter isn't keyless from headless Node).

## Next (this branch, in order)
- **Phase C** — harden the gate: `/etc/ai/policy.json`, O_NOFOLLOW symlink defense, rollback
  ownership, ledger integrity, CONFIRM + grant-full, multi-line-result hardening, adversarial verify.
- **Phase D** — synthetic input (`synthinput.h` SHM mirror of dockdnd + `tool_mouse`/`tool_key`),
  cockpit GUI, end-to-end mock proof, then the live NVIDIA run.
