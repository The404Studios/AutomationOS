# brick record: CHAINLAYER-HOST-0

> The first model-in-the-loop tool decision: a model chooses a tool, the host
> validates it, the tool runs safely over the rail, the model sees the result,
> the model makes the final decision. The moment the OS-side rail meets the
> chainlayer2 brain -- via a SEAM, not an embedded LLM.

```yaml
brick: CHAINLAYER-HOST-0
status: complete
branch: brick/chainlayer-host-0
base: brick/toolset-0               # off the frozen tool-surface milestone (6ff7be0)
request: >
  One model backend, one prompt, one tool-selection JSON shape, one or two tools, one final decision.
  Acceptance: prompt "What is inside /etc/toolset0.txt?" -> model chooses read_file -> host validates ->
  tool_read runs -> host reads stdout_token -> model answers TOOLSET-0-FILE. NO autonomous loops, NO
  write/delete tools, NO networking, NO recursive planning, NO self-modifying code, NO registry explosion.
decisions:
  - the in-OS "model" is a DETERMINISTIC STUB at a loud seam (model_select/model_answer) -- AutomationOS is
    freestanding; the real chainlayer2 brain is an EXTERNAL llama.cpp/GGUF host that plugs into the SAME
    two functions later (same JSON in, same observation out). This brick proves the model<->host<->tool<->
    model PLUMBING, not the intelligence.
  - model output is UNTRUSTED TEXT: the host strictly parses the ONE selection shape
    {"tool":"<t>","path":"<p>"} (exact key order, no escapes, no trailing bytes) and then re-validates
    name+path against the TOOLSET-0 trust surface (whitelist + traversal denial) BEFORE any dispatch.
checkpoints:
  - id: CH0
    title: stub model -> strict JSON parse -> host policy -> rail dispatch -> exact read -> final answer
    commits: [459ef6e]
    files:
      - userspace/apps/chainhost/chainhost.c   # the seam + parser + host gate + runner (self-spawn)
      - scripts/build_all.sh                   # compile/stage sbin/chainhost (98th sbin entry)
      - userspace/init/main.c                  # init spawns sbin/chainhost after toolset_host
    tests: [build_test/chainhost_verify.sh]
    result: >
      serial 'CHAINHOST: PASS selected_tool=read_file policy_ok=1 read_exact=1 model_answer_exact=1
      rejected_bad_tool=1' FIRST TRY -- the stub selects read_file as JSON, the host parses + whitelists +
      path-checks it, tool_read runs over the rail, the host reads the exact 15 B via the P6c stdout_token,
      the stub answers 'TOOLSET-0-FILE' (checked exactly), and ALL THREE bad selections die at the gate
      before any dispatch: unknown tool (delete_file), traversal path (../etc/...), and a shape violation
      (trailing '} rm -rf'). Whole rail still green (CHAN p1/p2/p5, RPCTEST, TOOLRUN, AGENTHOST, TOOLSET);
      kernel unchanged; chcheck.png desktop clean, 0 panic.
    design:
      - THE SEAM (the whole point): model_select(prompt)->JSON text and model_answer(observation)->text are
        the ONLY two places a model touches the program. Replace their bodies with an external-model bridge
        and nothing else changes -- the host around them never trusts either direction.
      - the host gate is parse -> whitelist -> path policy, in that order, all before dispatch. The parser
        accepts exactly one shape; any deviation (escapes, reordered keys, trailing bytes) is a reject, so
        prompt-injection-style suffixes ('} rm -rf') die at the parse step, not at exec.
      - rejected_bad_tool=1 requires ALL THREE reject classes (unknown tool AND traversal AND shape) --
        one flag, three gates proven.
      - reuses the proven self-spawn runner + ACK pattern and the TOOLSET-0 whitelist (read_file/list_dir/
        stat -> sbin/tool_*); echoargs/run intentionally NOT whitelisted here -- the model surface is
        read-only (no write tools per the hard no's; run stays a TOOLSET-0/host capability).
    review:
      default_build_changed: false      # userspace-only; kernel byte-identical; whole rail re-proven
      all_waits_bounded: true           # bounded recv/read/wait loops (same caps as toolset_host)
      hardware_init_gated: n/a
      touches_userspace: true
      touches_kernel: false
      preserves_known_good_t410: true
      smoke_proves_claim: true          # the PASS line = selection + policy + exact read + answer + reject
      raw_pointers_or_truncation: none  # bounded parse (32/120), bounded read (256), no truncation
    verdict: pass
done: >
  CHAINLAYER-HOST-0 COMPLETE. The full agent step exists end-to-end on AutomationOS: model decision ->
  host validation -> safe tool run -> exact observation -> model answer, with every boundary checked and
  bad selections rejected before dispatch. The external chainlayer2 brain now has a single, proven seam.
next:
  - MODEL-BRIDGE-0 (name TBD): replace the stub seam with the EXTERNAL llama.cpp/GGUF chainlayer2 host
    (per the prove-tiny-first strategy) -- same JSON contract, same host gate.
  - later: TOOL-AUTH-0 (root allowlists / per-tool authority) · TOOL-RESULT-0 (typed result envelopes).
```
