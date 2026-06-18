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
- **Rail assessment:** the live gate is already well-built — whitelist + `bad_path` run BEFORE any
  dispatch, tool stdout is newline-collapsed (closes the multi-line broker-desync vector), stdout
  flows over one-shot P6c capabilities. Remaining C items are ENHANCEMENTS, not open holes:
  externalized `/etc/ai/policy.json`, `O_NOFOLLOW` symlink defense (needs kernel support; low
  exploitability today — no symlink-creating tool is whitelisted), tool-level rollback, ledger
  integrity hash-chain.

## Next (this branch, in order)
- **Phase C** — harden the gate: `/etc/ai/policy.json`, O_NOFOLLOW symlink defense, rollback
  ownership, ledger integrity, CONFIRM + grant-full, multi-line-result hardening, adversarial verify.
- **Phase D** — synthetic input (`synthinput.h` SHM mirror of dockdnd + `tool_mouse`/`tool_key`),
  cockpit GUI, end-to-end mock proof, then the live NVIDIA run.
