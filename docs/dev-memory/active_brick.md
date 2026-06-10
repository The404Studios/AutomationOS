# active_brick — what's in flight right now

> Warm memory. Refresh per checkpoint. One active brick at a time.

## NET-P1-0 — LANDED (6 commits local on `brick/net-p1-0`, QEMU green; T410 ladder pending) — track 1 of the roadmap
- **the roadmap:** user-approved 3-track plan (networking → SMP two-core → GT218 native modesetting)
  + permissive-only porting; plan file + [`bricks/NET-P1-0.md`](bricks/NET-P1-0.md) carry the details.
- **landed:** A0 rig (`1e226c6`, inject+capture, NETRIG PASS) → A SYN side-table (`3965242`, SYNs stop
  burning sockets) → B 4-slot OOO heap-backed (`24100a6`, reassembled=5840) → C/D/E (`30136ae`,
  persist probe + the TWO rig-caught bugs: dead zero-window clamp, cap-races-clock; UDP 16,
  SOCK_MAX 32) → E1000-PCH-0A/0B/0C (`14a0bdb`, deferred bring-up + nicup trigger + never-CTRL_RST-
  without-SWFLAG).
- **T410 hardware ladder (next hardware day):** flash `automationos-t410-net.iso`, travel-router
  bridge, run `nicup` in the terminal, read the `E1000PCH:` serial ladder, then dhcpc → ping → httpd.
  Aborts safe; nicup re-runnable.
- **known/noted:** smoke_boot.sh has 5 PRE-EXISTING profile-mismatch FAILs (BOOT_QUIET-suppressed +
  SMP-only markers) — follow-up task open; gateway-ARP pre-resolve marker is boot-flaky (gate on
  `[NET] up:`).
- **then (roadmap order):** GPU tier 0 + NV-REF-0 (same hardware day as the NIC ladder if convenient)
  → SMP-R0 (cherry-pick `faf7444`) → the 9-brick two-core path.

## SELFHEAL-FIX-0 — FROZEN / COMPLETE (pushed `0023943`, ls-remote verified; user: "ok perfect!") — recovery now restores the desktop
- **user report:** wave-2 works on the T410 BUT "the self healing is not working" → "run 7 agents and fix the self heal".
- **record:** [`bricks/SELFHEAL-FIX-0.md`](bricks/SELFHEAL-FIX-0.md) · **branch:** `brick/selfheal-fix-0` (off `brick/ide-xfile-0` `4766b1d`).
- **7-agent verdict:** the recovery CHAIN was sound (detect/overlay/kill/respawn/re-arm all clean) — the hole
  was the OUTCOME: the respawned compositor zeroes `g_windows` and no client re-register protocol existed →
  every open window vanished → empty desktop = "not working". The original smoke asserted markers only,
  never desktop CONTENTS.
- **fix (H0 `2a071f3`):** SELFHEAL v2 — a 16-entry window registry in the spare bytes of the SAME init-owned
  heartbeat page; compositor mirrors it (create/destroy + per-second); a respawn rebuilds windows by
  re-attaching client buffers via shm_id (dead client ⇒ dead segment ⇒ failed shmat = the liveness test),
  ORIGINAL win_ids preserved so client handles stay valid. Zero client changes.
- **tuning (H1 `00354ef`):** RESUME_TIMEOUT 5→10 s, ATTACH ~10→20 s, BREAKER window 60→90 s (single-core T410).
- **proof:** `build_test/selfheal_t410_check.sh` (exact T410 profile + forced blocking freeze) failing-then-
  passing: baseline `restored_windows=0` + bare-desktop screenshot (the user's symptom, `shfix_base_recovered.png`)
  → fixed `SELFHEAL-FIX: PASS recovery=1 restored_windows=4 no_storm=1 no_fault=1` + all four windows back
  (`shfix_recovered.png`); + 100 s shipping-build no-false-trip soak.
- **flash:** `automationos-t410-selfheal.iso` (T410_SAFE kernel + DESKTOP_MINIMAL+SELFHEAL userspace, no freeze hook).
- **known limits (documented, parked):** cooperative kernel can't detect a ring-3 tight-loop freeze
  (FREEZE_MODE=1 = SKIP; PREEMPT hard-spin gap tracked separately) · kernel IF=0 spin kills everything
  (iteration-cap law) · a non-compositor spinner gets the wrong remedy (DESKTOP_MINIMAL removed that storm).

## IDE-WAVE-2 — FROZEN / COMPLETE (all four branches PUSHED, T410-CONFIRMED "it works") (all four bricks committed local, awaiting review/push) — four parallel designs, serial implementation
- **user verdict on IDE-SYNC-0:** "perfect its working good" (T410 hands-on) → pushed + frozen.
- **record:** [`bricks/IDE-WAVE-2.md`](bricks/IDE-WAVE-2.md) · **branch chain (off `brick/ide-sync-0`
  `0f321fe`):** `brick/ide-context-0` → `brick/map-stable-0` → `brick/viz1-parity-0` →
  `brick/ide-xfile-0` (each off the previous HEAD; all LOCAL, push on the user's word).
- **IDE-CONTEXT-0** (`06f7787` + `9502f2d`): ONE breadcrumb spine ("project > file > FN Ln l,c")
  in BOTH workspaces' status bars + the edits-since-save what-changed star (choke-point wiring).
  EN ROUTE: the BLANK-IDE bug = scan_dir's 18 KB-per-level stack dirent buffer crossing the ~64 KB
  mapped user stack → sys_readdir copy_to_user EFAULT read as end-of-dir; fix = static buffer.
  RULE: no large stack locals in recursive userspace code (2nd hit of this family).
- **MAP-STABLE-0** (`c68c310`): layout was already deterministic; the real bug was shared pan
  (map_ox/oy) surviving focus changes → reset in ide_set_focus + order-contract comments.
- **VIZ1-PARITY-0** (`5859843`): 2-line satellite cards (Type:/Ports(N)/(extern)), connector
  studs, CLICK-TO-CLOSE hint, legend chips — `viz1_lego.png` matches the user's mockup.
- **IDE-XFILE-0** (0a `334fb52` + 0b `7dbd819`): model_parse split (reset+append) →
  ide_parse_project_model parses every sibling .c then the OPEN file LAST; editor-coupled views
  filter to cur_file; map resolves siblings + shows INCOMING cross-file callers; 0b
  `ide_sel_jump_xfile` (copy-inputs-then-reopen — pointers into the model DIE at reopen) makes a
  cross-file follow OPEN the sibling and land the caret. PROOF `xfile0b2_xjump.png`: follow
  game_main() from tower_tick's map → breadcrumb flips to "src > main.c > game_main Ln 12,1",
  every pane rebuilt in sync. HARNESS FINDING: the old right+ret jump step had always no-oped
  (g_enemies READ, no producer); added the left+ret xjump step that exercises the real follow.
- **deferred (XFILE design):** globals-file mislabel (extern-first dedupe) · static-name
  collisions · recursive subdirs · .asm siblings.
- **next:** T410 hands-on (mouse paths) → push all four branches → net-stack Phase 1 → E1000-PCH-0.

## IDE-SYNC-0 — FROZEN / COMPLETE (pushed `16b2a01`, T410-CONFIRMED "perfect its working good") — the core prosthetic loop: ONE selection model, three panes
- **branch:** `brick/ide-sync-0` (off the frozen `brick/ide-repair-0` HEAD `90e6da4`) · **record:**
  `bricks/IDE-SYNC-0.md` (to write)
- **why (user):** "Until that loop is tight, the map is only a picture. Once it syncs both ways, it
  becomes external working memory." editor caret → map node → inspector table → explanation panel →
  back to editor.
- **DESIGN LAW (the big rule):** ONE active selection model — `active_file / active_line /
  active_symbol_id / active_node_id / active_panel`. Editor, map, and inspector do NOT invent their
  own selection state; they all read/write the same model.
- **checkpoints (user-set):** S0 editor caret → active symbol/node · S1 active symbol → inspector
  detail (+ map highlight) · S2 map click → editor jump · S3 inspector row click → editor jump ·
  S4 selection highlight consistent across all three panes, sync survives typing/redraw.
- **HARD NO's:** no stable-layout work yet, no map redesign, no new visual styles, no new parser
  unless the existing symbol extraction is unusable.
- **acceptance:** click in a function → map node highlights + inspector shows it + breadcrumb
  updates; map node click → editor jumps + inspector updates; inspector row click → editor jumps +
  map highlights; type/edit → sync survives; no overlap regression; build_all clean; 0 panic.
- **commits (user-suggested):** S0 `feat(ide): IDE-SYNC-0 S0 track editor caret symbol` · S1
  `feat(ide): IDE-SYNC-0 S1 sync map and inspector selection` · S2/S3 `feat(ide): IDE-SYNC-0 S2
  jump editor from map and inspector` · docs record.
- **after (user order):** IDE-CONTEXT-0 → MAP-STABLE-0 → VIZ1-PARITY-0.
- **status:** **LANDED — all checkpoints committed local, awaiting review/push.** S0 `337258d`
  (IdeSel + ide_sel_from_caret hooked after every caret move in both workspaces + the FN/Ln
  breadcrumb) · S1 `0cc10ff` (caret crossings refocus; ide_set_focus tail-writes THE model —
  **proven interactively** via QMP key injection: caret into tower_init in the editor → LEGO shows
  the tower_init central card + INSPECTOR — tower_init + FN tower_init Ln 15,1, `s1b_lego.png`) ·
  S2/S3/S4 `073898d` (ide_sel_jump: map follows + inspector CONN/PORTS rows land the editor caret
  on the symbol; newline edits shift the recorded line ranges so sync survives typing;
  `s2_jump.png` = the full coherence frame, near-mockup). Harness:
  `build_test/ide_sync_check.sh` (QMP KEYBOARD injection works; mouse rel does NOT reach the
  compositor — T410 hands-on covers the mouse paths). record: `bricks/IDE-SYNC-0.md`.

## IDE-APHANTASIA (NORTH STAR, user-set 2026-06-09) — who this IDE is FOR
- **the user:** "we are building this ide for people who have aphantasia." The Semantic LEGO map /
  inspector tables / runtime flow ARE the mental image aphantasic programmers cannot form internally
  — a prosthetic working memory, not decoration. EVERY IDE brick filters through:
  nothing lives only in the user's head · always-visible context (where am I, what's connected,
  what changed) · verbal/symbolic representations in sync with the spatial map (tables + EXPLAIN
  narratives matter as much as boxes-and-edges) · STABLE map layout (same function = same place) ·
  tight code↔map↔inspector synchronization.
- **candidate next bricks (user picks):** IDE-SYNC-0 (bidirectional caret↔map↔inspector follow) ·
  IDE-CONTEXT-0 (persistent breadcrumb/where-am-I strip) · map layout pinning/stability ·
  VIZ-1 mockup parity (port-labeled edges, dashed-red ABSENT gate cards) · richer EXPLAIN.

## IDE-REPAIR-0 — FROZEN / COMPLETE (pushed, T410-CONFIRMED "it works") — four checkpointed fixes, no rewrite
- **branch:** `brick/ide-repair-0` (off `brick/desktop-toast-0`, which carries the toast removal
  `438e0c6`) · **record:** `bricks/IDE-REPAIR-0.md` (to write)
- **why (user, with T410 photos):** the IDE is "extremely bugged": UX/UI overlapping panels, the
  ability to code broken (autocomplete + line rendering), structure doesn't match the design mockup;
  plus the desktop-wide "weird text" (`sbin/compositor` as the panel title) and the debug box.
- **checkpoints (user-set, ONE COMMIT + CLEAN BUILD + SCREENSHOT EACH, in order):**
  - **I0** debug/title cleanup: no startup toast (already removed on the base branch) · no compositor
    debug overlay by default · no raw `sbin/compositor` path as the panel/titlebar text — real app
    names · all debug text behind a compile flag · ASCII-only visible strings (the 8x16 font has no
    UTF-8). Commit: `fix(desktop): hide debug overlays and normalize app titles`.
  - **I1** editor text rendering: hard clip rect; each row stops at ITS line end (the diagonal
    suffix-bleed is THE "can't code" bug); consistent line height; caret->pixel exact; scroll
    clamped. Acceptance: 10 clean lines typed, Backspace/Enter sane, save/build callable.
  - **I2** autocomplete boundaries: popup = a LAYER (clipped, never overwrites text); buffer gets
    typed chars; Tab/Enter accept = insert exactly ONCE; Esc closes; arrows only navigate when open.
  - **I3** panel layout vs the mockup: LIB list only in the right sidebar; runtime/coherence/
    warnings NOT duplicated (compact persistent strip + detail only inside VIZ-3); every section
    clipped to its rect.
- **HARD NO's:** no full IDE rewrite; no compiler-semantics changes unless save/build breaks; every
  draw path gets a clip rect; every visible debug string gated or removed.
- **diagnosis:** 5 read-only agents fanned out (editor render / autocomplete / inspector layout /
  runtime duplication / title text) — fixes applied serially I0→I3 from their findings.
- **status:** **LANDED — all four checkpoints committed local, awaiting review/push.**
  I0 `7991d5b` (THE sbin/compositor text + flashing square = the SCHED_DEBUG timer-ISR liveness
  heartbeat, default-ON in quick_build → now DEFAULT OFF, opt-in; panel/titlebar path-guards;
  "Semantic IDE") · I1 `1a17c1d` (one-glyph-per-cell: the editor+codeview passed interior pointers
  into the un-NUL'd source buffer to gfx_text_clip → every row drew the whole buffer suffix — THE
  can't-code bug, render-only, buffer was always correct) · I2 `20da381` (suppress completion while
  snippet ${N} fields are live so Tab navigates fields) · I3 `4009e8e` (LIB palette sidebar-only
  via insp_tab save/restore + mirrored click router; panel_runtime branches on rect height: compact
  one-line strip vs detailed VIZ-3 view; rt_hits no longer clobbered). Per-checkpoint clean builds +
  screenshots (i0check/i2check/i3check.png); editor proven pixel-clean in QEMU; LEGO-mode visuals +
  typing/snippet flows = the T410 hands-on list. record: `bricks/IDE-REPAIR-0.md`.

## T410-RETEST (PARTIAL — heap fixes verified, desktop artifacts root-caused) — both heap/aliasing fixes on the real 2010 hardware
- **what:** flash + boot `automationos-t410-bothfixes.iso` (T410_SAFE=1 SCHED_DEBUG=0 kernel +
  DESKTOP_MINIMAL=1 SELFHEAL=1 userspace — the proven T410 profile) carrying BOTH suspects' fixes:
  the malloc tcache three-state fix (`8a0aafc`) and the initrd direct-map fix (`9dad3ac`).
- **hypothesis to test:** the stray color-shifting window + erratic titlebars
  (DESKTOP-PROJECT-REGRESSION-0) were cross-linked-heap / aliased-memory symptoms. If the desktop
  comes up clean, the real cause is found and that brick closes.
- **result so far:** the T410 BOOTS to desktop with both fixes; the remaining top-right "flashing
  square + text" was root-caused as the compositor's never-expiring WELCOME TOAST (expiry frame is
  never presented on an idle desktop; per-second clock repaints jitter its fade alpha; the em-dash
  renders as UTF-8 garbage through the ASCII font) — REMOVED in `438e0c6` on `brick/desktop-toast-0`;
  `automationos-t410-bothfixes.iso` rebuilt clean. The `sbin/compositor` panel-title text + the IDE's
  brokenness moved into IDE-REPAIR-0 (above).
- **then:** NET-STACK-PHASE-1 (SOCK_MAX 16→64 · TCP SYN queue · UDP queue 8→32 · TCP OOO 1→4 slots,
  per the audit's quick-win tier) → E1000-PCH-0.
- **parked (user-approved, later):** **KERNEL-DIRECTMAP-AUDIT-0** — find any kernel subsystem still
  reading long-lived physical/kernel buffers through low identity VAs under arbitrary process CR3;
  move those to direct-map/kernel-only mappings. (The residual exposure class from INITRD-ALIAS-0.)

## INITRD-ALIAS-0 — FROZEN / COMPLETE (pushed to origin `d9a0e2e`) — kill the kernel file-read alias bug before building on it
- **branch:** `brick/initrd-alias-0` (off the frozen `brick/browser2-img-0` HEAD) · **record:**
  `bricks/INITRD-ALIAS-0.md` (to write)
- **why (user's call, ahead of net Phase 1):** kernel CORRECTNESS bug — "initrd file content is
  untrustworthy inside mmap-heavy processes" poisons every higher-level browser/network/tool test
  that reads initrd files. More fundamental than TCP queue depth or browser polish.
- **the pinned failure:** VFS reads of initrd-backed (zero-copy) files return the EXACT byte count
  but ALL-ZERO data when the reader is mmap-heavy (browser2: wl SHM + JS arenas). Same-4KiB-page
  discriminator: tool_read reads toolset0.txt fine from the SAME initrd page that browser2 reads as
  zeros; destination irrelevant (stack-bounce + pre-touch both still zero) → per-process
  address-space aliasing over the kernel/identity initrd mapping (family of the fixed
  higher-half/identity PD alias bug).
- **scope (user-set, narrow):** prove WHY → fix the address-space aliasing / kernel initrd mapping
  access → regression test with an mmap-heavy reader + browser2 initrd-backed image. **HARD NO's:**
  no browser layout, no network, no image decode changes.
- **constraints:** single-core runtime; T410 (2010 hardware) must stay safe — pure memory-management
  fix, no new hardware init, default boot preserved.
- **acceptance:** same initrd file, same byte count, same expected bytes from BOTH a pristine reader
  (tool_read) and an mmap-heavy reader; browser2 initrd-backed image loads real bytes; no all-zero
  buffer; desktop 0 panic. Proof: `INITRD-ALIAS: PASS pristine_read=1 mmapheavy_read=1 same_bytes=1
  browser_file_img=1 zero_bug_gone=1`.
- **after this (user-set order):** net-stack Phase 1 → E1000-PCH-0; ALSO retest the T410 desktop
  regression with the malloc fix (prime suspect for the stray color-shifting window).
- **status:** **LANDED (fix `9dad3ac` + test `1588c2a`, committed local, awaiting review/push).**
  ROOT CAUSE (refined): not mmap at all — **BIG ELF IMAGES**. User images load at VA 0x800000 inside
  the identity region as private per-process pages; browser2's image grew to memsz ~35 MB (spans VA
  8..44 MiB), REPLACING the identity PTEs over the initrd's VA 16..21.3 MiB in its own page tables —
  kernel reads of inode->data on that CR3 returned browser2's OWN pages (first zero BSS, later live
  data: the pre-fix probe read literal big.png pixels, hdr=180,110,60,255 — the alias photographed).
  Anon mmaps land at 4 GiB (harmless). FIX = one seam: `initrd_init` converts the physical address
  to its DIRECT-MAP alias (`PHYS_TO_DIRECT`, PML4[256], supervisor-only, never-split, shared into
  every CR3); pmm keeps reserving the raw phys range. VERIFIED FAILING-THEN-PASSING: with the fix
  stashed the new regression pair FAILs (`INITRD-ALIAS: FAIL ... mmapheavy_read=0 same_bytes=0
  zero_bug_gone=0`, `BROWSER2-IMG-FILE: FAIL`); with it, boot prints `Initrd phys 0x1000000 ->
  direct-map 0xffff800001000000` and the composite acceptance hits exactly: `INITRD-ALIAS: PASS
  pristine_read=1 mmapheavy_read=1 same_bytes=1 browser_file_img=1 zero_bug_gone=1`. Pair =
  `sbin/initrdp` (tiny control) + `sbin/initrdalias` (16 MiB VOLATILE pad — dead-store-elimination
  ate the first, non-volatile pad: memsz=0x100, false PASS, caught by objdump) + browser2's 6th
  `<img src="/etc/imgtest/t.png">` on its own `BROWSER2-IMG-FILE` line (frozen flags untouched).
  GOTCHA RECORDED: kernel bricks need `quick_build.sh` BEFORE `build_all.sh` (build_all only
  packages the prebuilt `build/kernel.elf` — the first "post-fix" boot ran the OLD kernel).
  Residual exposure class documented in the record (other low-identity-VA reads inside a big
  image's span; initrd was the only zero-copy long-lived consumer found). Rail + browser pipeline
  green both runs; `iacheck2.png` clean; 0 panic. record: `bricks/INITRD-ALIAS-0.md`.
- **user verdict on freeze:** approved — "not optional polish — it killed a real kernel
  address-space correctness bug... the failing-then-passing verifier discipline is exactly what
  makes it trustworthy"; the direct map is "the right seam"; keep all three commits unsquashed
  (fix → proof → memory = the forensic chain). Pushed (`d9a0e2e`, ls-remote verified).
  KERNEL-DIRECTMAP-AUDIT-0 queued for later, NOT now.

## BROWSER2-IMG-0 — FROZEN / COMPLETE (pushed to origin `a5a9267`) — `<img>` rendering in browser2 from code already in-tree
- **branch:** `brick/browser2-img-0` (off the frozen `brick/model-bridge-0` HEAD) · **record:**
  `bricks/BROWSER2-IMG-0.md` (to write)
- **why (user's call):** highest visible return for the least architectural risk. The 5-agent audit
  found `<img>` never renders even though `userspace/lib/imgcodec/{png,gif,bmp}.c` decoders, the
  HTTP/TLS fetch, and the layout/paint pipeline all already exist in-tree. "AutomationOS can browse
  real pages better" — without touching dangerous hardware init.
- **scope (user-set, narrow):** HTML `<img>` support ONLY — fetch the image resource, sniff the type,
  decode via imgcodec, create an `LB_IMAGE` layout box, paint the bitmap during the render pass,
  fallback placeholder on failure. **HARD NO's:** no CSS overhaul, no JS, no forms, no JPEG (unless
  already trivial), no network-stack rewrite, no engine replacement.
- **acceptance:** local test page with PNG/GIF/BMP `<img>` → browser2 renders text + images; missing
  image → placeholder/clean failure; large image bounded/clipped, no panic; desktop clean; 0 panic.
  Proof: `BROWSER2-IMG: PASS png=1 gif=1 bmp=1 missing_safe=1 bounded=1`.
- **after this (user-set order):** net-stack Phase 1 → E1000-PCH-0 → TOOL-AUTH-0 / TOOL-RESULT-0 →
  real llama.cpp model bridge. Native Wi-Fi stays PARKED (travel-router client-bridge + hardened
  wired NIC is the route).
- **status:** **LANDED (fix `8a0aafc` + feat `f597d3b`, committed local, awaiting review/push).**
  `BROWSER2-IMG: PASS png=1 gif=1 bmp=1 missing_safe=1 bounded=1` + `imgcheck7.png` (all five cases
  visible). LB_IMAGE = atomic inline box via a dims-provider seam (`layout_set_img_dims_provider`);
  clamped box + clipped blit = the bounded guarantee; bordered placeholder on failure. TWO BUGS
  SURFACED EN ROUTE: **(1) FIXED — the c70ee87 malloc tcache regression** (free=1 before caching →
  the arena walker double-allocated tcache-parked blocks → heap corruption in every malloc-heavy
  app; browser2 had been CRASHING at boot since June 8 and HTMLTEST/CSSTEST/WEBTEST FAILing,
  masked because smokes grep only rail markers; three-state flag fix resurrected the whole browser
  pipeline; likely also DESKTOP-PROJECT-REGRESSION-0's stray window). **(2) OPEN — INITRD-ALIAS-0**
  (initrd-backed VFS reads return exact counts but ALL-ZERO data inside mmap-heavy processes;
  proven not-the-bytes / not-the-destination via a same-4KiB-page working/broken file pair; needs
  its own kernel brick) — so about:imgtest sources EMBEDDED generated fixtures (`fixture:` scheme);
  the file/network loaders are in place for real pages. record: `bricks/BROWSER2-IMG-0.md`.
- **user verdict on freeze:** approved — "the fix(malloc) commit belongs in this branch... it was a
  prerequisite fix that resurrected the browser pipeline." Pushed (`a5a9267`, ls-remote verified).
  Next = INITRD-ALIAS-0 BEFORE net Phase 1 (kernel correctness first).

## MODEL-BRIDGE-0 — FROZEN / COMPLETE (pushed to origin `ceeb886`) — the seam fed by an external model transport
- **branch:** `brick/model-bridge-0` (off the frozen `brick/chainlayer-host-0` HEAD `7553849`) · **record:**
  `bricks/MODEL-BRIDGE-0.md` (to write)
- **why:** CHAINLAYER-HOST-0 proved the model↔host↔tool↔model plumbing with a deterministic stub at the
  seam. The next proof: the SAME seam fed by an EXTERNAL model endpoint — the model as hostile/untrusted
  text from day one, replacing the fake brain without rewriting the OS-side trust surface.
- **scope (user-set):** host sends the prompt to an external model endpoint → receives text →
  strict-parses `{"tool":"read_file","path":"/etc/toolset0.txt"}` → rejects malformed model output →
  dispatches through the EXISTING policy (same parser, same TOOLSET-0 whitelist, same read-only tools) →
  sends the observation back → receives the final answer. One prompt, one tool, one answer. For the first
  pass the "external model" is a host-side bridge/server STUB returning deterministic model text over an
  existing transport; llama.cpp swaps in later. **HARD NO's:** no loops, no tool expansion, no write
  tools, no shell.
- **acceptance:** `MODELBRIDGE: PASS select_parse=1 policy_ok=1 read_exact=1 answer_exact=1
  malformed_model_rejected=1 bad_tool_rejected=1`; rail still green; desktop 0 panic.
- **status:** **LANDED.** Transport = TCP over the existing socket syscalls (slirp guest→10.0.2.2:8431;
  serial rejected — kernel-debug write-only, no userspace read path; TCP = zero kernel changes and the
  same transport llama.cpp uses later). `sbin/modelbridge`: the ONLY changed seam bodies — model_select/
  model_answer became `model_exchange()` (one request per connection, `"SELECT <prompt>\n"` /
  `"ANSWER <observation>\n"`, one line back, bounded everything); parser/whitelist/path-policy/runner
  byte-for-byte CHAINLAYER-HOST-0. `scripts/model_server_stub.py` = the external endpoint, SCRIPTED TO
  ATTACK (chatty unparseable reply + valid-shape `delete_file`) so the gate is proven against real
  remote bytes. Bounded net/endpoint probes → `SKIP` exit 0 keeps the default boot clean. Serial
  `MODELBRIDGE: PASS select_parse=1 policy_ok=1 read_exact=1 answer_exact=1 malformed_model_rejected=1
  bad_tool_rejected=1` + the stub's log shows all four exchanges crossed the wire; whole rail green
  (CHAINHOST/TOOLSET/AGENTHOST/TOOLRUN/RPCTEST/[CHAN]); kernel unchanged; `mbcheck.png` clean, 0 panic.
  Verify: `build_test/modelbridge_verify.sh`. record: `bricks/MODEL-BRIDGE-0.md`. Pushed (`ceeb886`,
  ls-remote verified).
- **user verdict on freeze:** approved; the verify script staying IN the feat commit is deliberate —
  "this brick's proof depends on a host-side TCP stub, so the harness is part of the milestone, not
  disposable scratch." Later (after BROWSER2-IMG-0 + net Phase 1 + E1000-PCH-0): TOOL-AUTH-0 /
  TOOL-RESULT-0 · the real llama.cpp/GGUF bridge behind the same port · MODEL-LOOP-0.

## CHAINLAYER-HOST-0 — FROZEN / COMPLETE (pushed to origin `7553849`) — the first full chainlayer host milestone
- **branch:** `brick/chainlayer-host-0` (off the frozen `brick/toolset-0` HEAD `6ff7be0`) · **record:**
  [`bricks/CHAINLAYER-HOST-0.md`](bricks/CHAINLAYER-HOST-0.md)
- **why:** all the OS-side foundation is done (CHANNEL-0 → TERMINAL-0 → AGENT-RPC-0 → AGENT-HOST-0 →
  TOOLSET-0). The next proof is the full agent step: **a model chooses a tool → host validates → tool
  runs safely → model sees the result → model makes the next decision.**
- **scope (tiny):** one model backend, one prompt, one tool-selection JSON shape, one or two tools, one
  final decision. **HARD NO's:** no autonomous loops, no write/delete tools, no networking, no recursive
  planning, no self-modifying code, no tool-registry explosion.
- **KEY DESIGN NOTE:** there is no LLM inside this freestanding OS; per the chainlayer2 strategy the brain
  is EXTERNAL (llama.cpp/GGUF). So the in-OS brick's "model" is a **deterministic stub** standing in at the
  same interface — it proves the model↔host↔tool↔model PLUMBING (tool-selection JSON → host policy →
  dispatch → exact result → answer), NOT the intelligence. The real model plugs into the same seam later.
- **acceptance (deterministic prompt "What is inside /etc/toolset0.txt?"):** model selects
  `read_file("/etc/toolset0.txt")` → host validates name+path → `tool_read` runs → host reads the
  `stdout_token` → model answers `TOOLSET-0-FILE`. Proof: `CHAINHOST: PASS selected_tool=read_file
  policy_ok=1 read_exact=1 model_answer_exact=1 rejected_bad_tool=1`; desktop 0 panic.
- **status:** **LANDED.** `sbin/chainhost`: the seam = `model_select(prompt)`→JSON / `model_answer(obs)`
  →text (deterministic stubs; the external llama.cpp brain replaces ONLY those two bodies later). Host
  gate = strict one-shape JSON parse (exact keys, no escapes, no trailing bytes) → TOOLSET-0 whitelist →
  path policy, all BEFORE dispatch; tool runs via the self-spawn runner; stdout read exactly via the P6c
  token. Serial `CHAINHOST: PASS selected_tool=read_file policy_ok=1 read_exact=1 model_answer_exact=1
  rejected_bad_tool=1` FIRST TRY (rejected_bad_tool = unknown tool AND traversal AND shape-violation
  with a trailing shell suffix, all dead at the gate). Whole rail green, kernel unchanged, desktop clean
  0 panic. record: [`bricks/CHAINLAYER-HOST-0.md`](bricks/CHAINLAYER-HOST-0.md). Pushed (`7553849`).
- **user verdict on freeze:** "the right milestone boundary because it proves the whole rail with a
  deterministic stub, while being honest that it does not yet prove intelligence... the model is treated
  correctly: untrusted text, never authority." → next = MODEL-BRIDGE-0, not autonomy.

## TOOLSET-0 — FROZEN / COMPLETE (pushed to origin `6ff7be0`) — safe structured tool surface
- **branch:** `brick/toolset-0` (off the frozen `brick/agent-host-0` HEAD `19e96c3`) · **record:**
  [`bricks/TOOLSET-0.md`](bricks/TOOLSET-0.md)
- **why:** before plugging a real model into the rail, harden a tiny tool SURFACE with stable schemas.
  The model should drive safe typed tools, not arbitrary process execution.
- **scope:** `tool.run` (path+argv, exists) · `tool.echoargs` (proven) · **`tool.read_file`** (bounded
  read, explicit path, size cap) · **`tool.list_dir`** (bounded entries, NO recursion) · **`tool.stat`**
  (file metadata). Enforce: a tool whitelist (named tools, not arbitrary paths), an explicit path-traversal
  policy (reject `..`/escapes), and size/entry caps. **HARD NO's:** no shell, no networking, no model
  inference, no recursive fs walk, no write/delete tools.
- **acceptance:** agenthost lists a dir · stats a file · reads a small file EXACTLY · runs echoargs ·
  malformed tool request rejected · oversize read rejected · path-traversal rejected (explicit policy) ·
  desktop 0 panic.
- **status:** **LANDED (`bb9bbdf`).** Tools = small sandboxed programs over the rail: `sbin/tool_read`
  (stat-first; `>256 B` → reject, else exact bytes), `sbin/tool_ls` (opendir/readdir≤32, no recursion),
  `sbin/tool_stat` (`size=<n> type=<f|d>`), + `echoargs`/run. Trust surface = `sbin/toolset_host`:
  name-whitelist (`resolve_tool`) + path policy (`bad_path`: reject empty/`..`) → dispatch only a known
  program; stdout returns via the P6c capability, read exactly. Serial `TOOLSET: PASS ls=1 stat=1
  read_exact=1 run=1 unknown_rejected=1 malformed_rejected=1 oversize_rejected=1 traversal_rejected=1`
  (fixture `/etc/toolset0.txt`, 15 B). `SYS_READDIR` returns 0=entry (found+fixed). No kernel change;
  whole rail green; `tsfinal.png` clean, 0 panic. Policy = traversal-denial NOT a jail (→ TOOL-AUTH-0);
  plain-text results (→ TOOL-RESULT-0). record: `bricks/TOOLSET-0.md`. Pushed (`6ff7be0`).
- **then:** **CHAINLAYER-HOST-0** — a local/API model chooses among these typed tools (the chainlayer2
  host agent). Later: TOOL-AUTH-0 (root allowlists / per-tool authority) · TOOL-RESULT-0 (typed results).

## AGENT-HOST-0 — FROZEN / COMPLETE (pushed to origin `19e96c3`) — the first agent riding the rail
- **branch:** `brick/agent-host-0` (off the frozen `brick/agent-rpc-0` HEAD `9460446`) · **record:**
  [`bricks/AGENT-HOST-0.md`](bricks/AGENT-HOST-0.md)
- **why:** the kernel/user IPC rail is finished (CHANNEL-0 + AGENT-RPC-0). The open question is no longer
  "can the OS do more plumbing?" but "can an agent issue `TOOL_RUN`, get `TOOL_RESULT`, accept the
  `stdout_token`, read the EXACT stdout, and make a DECISION?" — the first real "AI-OS" milestone.
- **scope (one host loop):** a userspace `agent_host` drives one full round trip — send `TOOL_RUN{path,
  argv}` → receive `TOOL_RESULT` → accept `stdout_token` → read stdout → render a STRUCTURED verdict
  (`path_ok` / `argv_ok` / `stdout_exact` / `exit`) → plus a malformed `TOOL_RUN` is rejected. **HARD
  NO's:** no networking, no model inference, no async batching, no complex tool registry.
- **acceptance:** `AGENTHOST: PASS path_ok=1 argv_ok=1 stdout_exact=1 exit=0` (runs `echoargs` with an
  argv vector, reads exact stdout via the P6c capability) + `malformed_rejected=1`; desktop 0 panic.
- **status:** **LANDED (`98bd950`).** `sbin/agenthost` self-spawns a clean runner; one host loop drives a
  VALID call (`TOOL_RUN{sbin/echoargs, ["hello world","a;b|c"]}` → spawn via `SYS_SPAWN_EX_ARGV` + grant
  → accept token → read the **exact 32-byte stdout** → decide) and a MALFORMED call (clobbered final NUL
  → `argv_validate` fails → runner replies `TOOL_F_ERR`, no spawn). Serial `AGENTHOST: PASS path_ok=1
  argv_ok=1 stdout_exact=1 exit=0 malformed_rejected=1`. Only schema add = `TOOL_F_ERR`; no kernel change;
  whole rail still green; `ahcheck.png` clean, 0 panic. **The first AI-OS milestone is real.** record:
  `docs/dev-memory/bricks/AGENT-HOST-0.md`. NOT pushed (new brick, awaiting review). **Next (user's
  call):** a real tool set + structured `TOOL_RESULT` fields · an external-model host (llama.cpp/GGUF)
  driving this rail · or P7/P8 (async batch / NIC channels).

## AGENT-RPC-0 — FROZEN / COMPLETE (P6a–P6d, pushed to origin `9460446`) — the typed-tool rail
- **branch:** `brick/agent-rpc-0` (off the published `brick/terminal-0` HEAD `47a2bc0`) · **spec (P5/P6):**
  `docs/superpowers/specs/2026-06-08-channel-0-design.md` · **record:** [`bricks/AGENT-RPC-0.md`](bricks/AGENT-RPC-0.md)
- **why:** now that the console is human-stable (TERMINAL-0), give the agent a TYPED rail — framed
  `CH_MSG` packets → `TOOL_RUN`/`TOOL_RESULT` — so it never scrapes terminal text.
- **checkpoints (one commit each):** **P5a** kernel `CH_MSG` framing + boot selftest ← done ·
  P5b syscall surface (frame-aware `sys_ch_write/read` or `SYS_CH_SENDMSG/RECVMSG`) + userspace wrapper ·
  P6 `TOOL_RUN`/`TOOL_RESULT` + the agent runtime. Substrate-before-surface; additive (CH_BYTE untouched).
- **status:** **P5a LANDED (substrate).** `msg_packet_t {type,flags,len,request_id}` framed on the
  existing SPSC rings; `channel_write_msg`/`channel_read_msg` are **message-atomic** (whole frame or
  nothing; one packet per read; header peeked so an undersized buffer gets `EMSGSIZE` with the message
  intact). Hard boundary: header+len > ring cap → **`EMSGSIZE` (-90, new)**, NOT EAGAIN. Proven by
  `channel_selftest_p5()` at boot: serial `[CHAN] p5 msg selftest PASS (w=30 r=14 rid_ok=1
  empty=EAGAIN:1 oversize=EMSGSIZE:1 payload='/bin/cc main.c')`; P1/p2 selftests still PASS (default
  CH_BYTE path unchanged); ISO byte-identical to TERMINAL-0; `p5final.png` == `t4final.png` (0 panic).
  **P5b (`2a06157`): the syscall surface.** Dedicated **`SYS_CH_SENDMSG` (102) / `SYS_CH_RECVMSG`
  (103)** — byte channels keep `sys_ch_write/read` (pure stream semantics) forever; CH_MSG gets
  explicit packet syscalls (auditable). Handlers copy header+payload across the user boundary,
  cap at `CH_MAX_IO` (64 KiB→`EMSGSIZE`), propagate `EAGAIN`/`EMSGSIZE`. Userspace proof = a real
  self-spawning program `sbin/msgtest` (parent holds master; child fd0(READ)+fd1(WRITE) = slave end;
  child silent since its fd1 IS the channel): serial `MSGTEST: PASS eagain=1 emsgsize=1 send=1
  roundtrip=1 reply='PONG' rid=0x1234abcd` — a genuine cross-process userspace round-trip + EAGAIN +
  EMSGSIZE. CH_BYTE unchanged (P1/p2/p5 selftests pass, terminal renders, `p5bcheck.png` clean, 0
  panic). `gitignore` anchored (`decf0f9`). **P5a+P5b PUSHED** (`2fe28f4`, ls-remote verified).
  **P6a (`9178d89`): the wire schema (schema-only).** `userspace/lib/agent_rpc.h` — type IDs
  `MSG_TOOL_RUN` (0x0101)/`MSG_TOOL_RESULT` (0x0102), `AGENT_RPC_VERSION`=1, fixed-size packed
  `tool_run_t` (392 B)/`tool_result_t` (16 B; `stdout_handle`=0 until P6b), encode/validate with
  explicit `AR_E_*` codes; doc `docs/AGENT_RPC_WIRE.md`. The CH_MSG payload IS one struct; the kernel
  ring stays opaque (schema = userspace policy). Proven by `sbin/rpctest` (encode→validate + every
  rejection): serial `RPCTEST: PASS v=1 run_sz=392 res_sz=16 rej(len=1,ver=1,fld=1,enc=1,resver=1)`;
  no kernel change; MSGTEST + selftests still green; `p6afinal.png` clean. **P6a PUSHED** (`52da0c8`,
  ls-remote verified). **P6b (`f12cecc`): path-only TOOL_RUN runner — DISPATCH.** Self-spawning
  `sbin/toolrun`: agent sends a path-only `TOOL_RUN {/bin/free, args_len=0}` over a `CH_MSG` ctrl →
  runner validates (rejects args!=0/reserved!=0) + spawns `/bin/free` with stdout bound to a `CH_BYTE`
  channel → runner **drains 183 B of stdout** (the "stdout read") → `TOOL_RESULT {exit_code=0,
  stdout_handle}` returned + validated. Serial `RUNNER: PASS ... stdout_bytes=183` + `TOOLRUN: PASS
  sent=1 result=1 type=1 rid=1 valid=1 handle_nz=1`. All hard-no's held (no shell/parser/PATH/env/
  stdin/stderr/concurrency). **TRUST-BOUNDARY NOTE:** no up-transfer in CHANNEL-0, so the RUNNER reads
  stdout; `stdout_handle` is the runner's token; agent-side cross-process stdout-read is P6c. No kernel
  change; RPCTEST/MSGTEST/[CHAN] green; `p6bcheck.png` clean, 0 panic. **P6b PUSHED** (`694185e`) +
  token-semantics doc (`de30b37`). **P6c (`f2d3882`): `stdout_token` is REAL — capability grant/accept.**
  Two syscalls `SYS_CH_GRANT(handle,to_pid)` / `SYS_CH_ACCEPT(grant_id)`: a one-shot, **CH_BYTE-only,
  MASTER-end, CH_R_READ-forced, to_pid-bound** transfer (NOT general fd passing). `grant_id=(gen<<16)|
  (slot+1)`; bounded table→`ENOSPC`; grants swept on either side's death. The runner GRANTs the tool's
  stdout (no drain); the agent ACCEPTs → read-only handle → reads the **exact 17 bytes** (`exact=1`,
  `ro=1`); ACK handshake so the runner outlives the accept. Renamed `stdout_handle→stdout_token`. Serial
  `TOOLRUN: PASS … agent_read=17 exact=1 ro=1 dblaccept_deny=1 bogus_deny=1` + `RUNNER: PASS grant=1
  ctrl_deny=1 inv_deny=1 norights_deny=1 wrongpid_deny=1 enospc=1`. All prior selftests green;
  `p6cfinal.png` clean, 0 panic. **P6c PUSHED** (`47ee247`, ls-remote verified) and **FROZEN as the
  capability/security checkpoint** — the grant/accept primitive (CH_BYTE-only, READ-only, MASTER-end,
  to_pid-bound, one-shot, bounded, death-swept) is locked; nothing downstream should loosen it.
  **P6d (`dd0213d`): argv as a VECTOR.** Dedicated **`SYS_SPAWN_EX_ARGV(path, argv_buf, argv_len, …)`**
  (NOT an a6 overload — the vector ABI is loud in the table). `path`=argv[0]; `argv_buf`=NUL-separated
  `argv[1..]` ONLY; `exec.c` splits on NUL ONLY → entries **intact** (no whitespace split, no shell, no
  PATH). `SYS_SPAWN`/`SYS_SPAWN_EX` byte-for-byte unchanged; staged vector cleared post-spawn.
  `argv_validate` matrix (path-only / cap / final-NUL / empty-entry / too-many; metachars literal).
  Proof: `TOOL_RUN{sbin/echoargs, "hello world\0a;b|c\0"}` → `TOOLRUN: PASS … agent_read=32 exact=1
  **vector=1** …` (multi-word ONE arg, `;|` literal, read via the P6c capability) + `RPCTEST … argv(
  zero=1,ok=1,nonul=1,empty=1,many=1,cap=1)`. `p6dfinal.png` clean, 0 panic. **AGENT-RPC-0 core arc
  COMPLETE: schema (P6a) → runner (P6b) → capability (P6c) → argv (P6d).** P6d committed local, awaiting
  review/push. **Next (user's call):** P7 async batch · P8 NIC channels · or the typed agent runtime /
  chainlayer2 host integration.

## TERMINAL-0 — FROZEN / COMPLETE (T0–T4, pushed to origin `935f54f`) — CHANNEL-0 console output human-stable
- **branch:** `brick/terminal-0` (off `brick/channel-0`), **PUSHED** `git push origin brick/terminal-0`
  verified via `ls-remote` (remote == local HEAD `935f54f`) · **spec:** `docs/superpowers/specs/2026-06-09-terminal-0-design.md`
- **why:** CHANNEL-0 P4 made external output appear, but messy (after-prompt, no scrollback, half-built
  line editing, escapes-as-boxes). Make it human-stable BEFORE the typed agent rail (AGENT-RPC-0).
- **checkpoints (one commit each):** **T0** output-before-next-prompt (waitpid-lite: defer the prompt
  until the bound child exits, poll `SYS_WAITPID` WNOHANG each frame, bounded await) ← do first ·
  T1 scrollback ring · T2 line-editing cleanup (cursor_pos + Left/Right/Home/End/Del, kill `[TERM] key`
  spam) · T3 minimal ANSI/VT (SGR color first — `grid_color[][]` substrate exists) · T4 delete ~3000
  lines dead terminal code. ALL userspace (`terminal_m3.c`); does NOT touch the kernel primitive.
- **status:** **T0 + T1 + T2 + T3 LANDED.** T0 (`3eac1bb`): defer the prompt for a bound child + waitpid-lite
  (`SYS_WAITPID` WNOHANG=1, non-blocking) → prompt prints AFTER the output. T1: **scrollback ring**
  (256-line `sb[][]` ring; `cur_row` → `sb_head`/`sb_view`/`sb_follow` viewport; `grid_putchar` is the
  ONE append path; PageUp/PageDown scroll, PageDown re-follows). SCREENSHOT-verified (`t0demo.png`,
  `t1demo.png` — a scrolled-back viewport holds lines the old discard grid lost); build_all clean;
  desktop. **T2 (`97faa0c`): clean line editing** — `line_cursor` + redraw-current-line (`redraw_input_line`);
  Left/Right/Home/End/Backspace/Delete/insert all cursor-aware; `[TERM] key` spam gated behind
  `TERM_DEBUG_KEYS` (default off). Screenshot-verified (`t2check.png`: a multi-line self-check runs
  the three acceptance cases through the SAME edit primitives → PASS lines in scrollback; `t2final.png`
  = clean build with the temp self-check removed). **T3 (`a6b1efc`): minimal ANSI/VT SGR colour** —
  a 3-state parser (`ANSI_TEXT`/`ESC`/`CSI`) sits BEFORE `grid_putchar` (ANSI = state mutation, not
  output); colour ONLY (`ESC[0m` reset, `30-37`/`90-97` fg, `39` default, `1` bold retro-brightens so
  `31;1`==`1;31`); 32-byte CSI buffer, overflow→TEXT; child drain routes through `term_write()` +
  `ansi_reset()` on child-exit; builtins/prompt keep explicit colour (no ANSI). Screenshot-verified
  (`t3check.png`: red/green/bright-red render, `ESC[999m` sane in default, NO escape junk; `t3final.png`
  clean). **T4 (`6aa9ce2` + `f3c8958`): deletion surgery** — removed the superseded OLD terminal app
  island (terminal.c[main] + renderer/buffer/tabs/pty_impl/vt_parser/profiles/utils + terminal.h + the
  dead dir Makefiles/README) and the `grid_backspace` orphan: **5,241 lines deleted** with a
  BYTE-IDENTICAL ISO (38281216) + identical screenshot (`t4check.png`) proving nothing live was removed.
  Kept: sh_git.c (live shell), font_integration.c/.h (verify-script refs). record:
  `docs/dev-memory/bricks/TERMINAL-0.md`. **TERMINAL-0 FROZEN + PUSHED** — the rail is human-stable end
  to end (output-before-prompt → scrollback → line editing → ANSI colour), and `brick/terminal-0` is
  published on origin. **Next brick (chosen): AGENT-RPC-0 / CHANNEL-0 P5** — the human console can now
  display long output, colour, errors, and logs cleanly, so it's ready to host the typed agent rail
  (the chainlayer2 north star). Parked alternatives: `brick/usb-ehci-0` E3, latency/perf track.

## CHANNEL-0 — FROZEN / COMPLETE (P0–P4, pushed to origin)
- **branch:** `brick/channel-0` (PUSHED, commit `1dd5107`) · **spec:** `docs/superpowers/specs/2026-06-08-channel-0-design.md`
- **record:** [`bricks/CHANNEL-0.md`](bricks/CHANNEL-0.md)
- **goal:** one kernel primitive (handle → shared rings) = our StdIO model + the structured-tool rail.
- **build order:** P0 handle table · P1 ring + `ch_*` syscalls + self-test · P2 spawn binds child
  stdio · P3 sys_write/read route fd0/1/2 · P4 terminal holds master, externals' output in the grid.
- **status:** **P0–P4 LANDED — the visible win works.** P0/P1 (`41a1c0a`) primitive + syscalls;
  P2 (`2d745b9`) `SYS_SPAWN_EX` binds child stdio; P3 (`3d46209`) `sys_write`/`sys_read` route the
  bound stdio; **P4: the terminal `ch_create`s a channel, spawns externals via `spawn_ex` bound to
  it, and drains the master into the grid (bounded ≤4 KB/frame).** SCREENSHOT-VERIFIED: `/bin/free`'s
  output appears in the terminal grid (child → write(1) → channel → drain → grid) instead of vanishing
  to serial; selftests PASS, desktop, 0 panic, default build unchanged. The kernel substrate (P0–P3)
  + the terminal holder (P4) = CHANNEL-0's core goal done. userspace wrapper: `userspace/lib/channel.h`.
- **next (CHANNEL-0 follow-ons, separate bricks):** P3.5 blocking `ch_wait` (wait_object) · P5 typed
  `CH_MSG`/`msg_packet_t` message channels · P6 AGENT-RPC-0 (`TOOL_RUN`/`TOOL_RESULT`) · TERMINAL-0
  polish (output-before-prompt, mid-line editing, scrollback, VT/ANSI parser, dead-code deletion).

## Parked / adjacent
- `brick/usb-ehci-0` — EHCI driver, **E1/E2 landed** (routing ledger); E3 (routing decision) next.
  The T410 is EHCI-only; UHCI brick works on QEMU only.
- `brick/terminal-0` (not created) — TERMINAL-0 quick-wins + the ~3,000-line dead-terminal-code
  deletion, after CHANNEL-0 P4.
- `docs/repo-presentation` — the README/docs overhaul, **committed local-only, awaiting push OK**.
- AI track north star: chainlayer2 (host agent) + external llama.cpp/GGUF backend + this dev-memory
  as training data. CHANNEL-0's `CH_MSG`/AGENT-RPC-0 = the typed-tool substrate.
