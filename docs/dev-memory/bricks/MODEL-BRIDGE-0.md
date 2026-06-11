# brick record: MODEL-BRIDGE-0

> The model seam fed by an EXTERNAL endpoint: the same model_select/model_answer
> seam CHAINLAYER-HOST-0 proved with a stub is now a real TCP exchange with a
> host-side model server -- and every byte off that socket is HOSTILE TEXT,
> day one. Swapping in llama.cpp later changes NOTHING on the OS side.

```yaml
brick: MODEL-BRIDGE-0
status: complete
branch: brick/model-bridge-0
base: brick/chainlayer-host-0          # off the frozen first chainlayer host milestone (7553849)
request: >
  Replace the deterministic stub at the seam with an external model transport: host sends the prompt to
  an external model endpoint -> receives text -> strict-parses {"tool":"read_file","path":"/etc/
  toolset0.txt"} -> rejects malformed model output -> dispatches through the EXISTING policy -> sends the
  observation back -> receives the final answer. Same seam, same strict JSON parser, same TOOLSET-0
  whitelist, same read-only tools. One prompt, one tool, one answer. First pass: the "external model" is
  a host-side deterministic bridge/server stub; llama.cpp swaps in later. HARD NO's: no loops, no tool
  expansion, no write tools, no shell.
decisions:
  - transport = TCP over the existing socket syscalls (SYS_SOCKET/CONNECT/SEND/RECV, QEMU slirp guest ->
    10.0.2.2:8431 -> host loopback). Serial was rejected: serial is kernel-debug write-only, no userspace
    read path -- TCP needs ZERO kernel changes and is the same transport a llama.cpp server uses later.
  - wire protocol = one request per connection, one line in, one line out, close:
    "SELECT <prompt>\n" -> the model's tool-selection JSON; "ANSWER <observation>\n" -> the final answer.
    Single-line observation framing for this brick; multi-line framing is a later brick.
  - the endpoint is SCRIPTED TO ATTACK: scripts/model_server_stub.py returns canned model text including
    deliberately hostile outputs (chatty unparseable text for __malformed__, a valid-shape non-whitelisted
    delete_file for __badtool__) so the in-OS gate is proven against a real remote adversary, not a local
    string literal.
  - default boot stays clean: bounded net_ready() probe (DHCP wait) then one connect probe -- no net or
    no endpoint -> "MODELBRIDGE: SKIP ..." exit 0. The verdict only runs when the stub is up.
checkpoints:
  - id: MB0
    title: external endpoint -> hostile text -> same gate -> rail dispatch -> observation back -> answer
    commits: [7aa25c1]
    files:
      - userspace/apps/modelbridge/modelbridge.c   # the seam = model_exchange() TCP; parser/whitelist/
                                                   # policy/runner byte-for-byte CHAINLAYER-HOST-0
      - scripts/model_server_stub.py               # the host-side external model endpoint (first pass)
      - scripts/build_all.sh                       # compile/stage sbin/modelbridge (99th sbin entry)
      - userspace/init/main.c                      # init spawns sbin/modelbridge after sockettest
    tests: [build_test/modelbridge_verify.sh]
    result: >
      serial 'MODELBRIDGE: PASS select_parse=1 policy_ok=1 read_exact=1 answer_exact=1
      malformed_model_rejected=1 bad_tool_rejected=1' -- the guest connects out through slirp, the stub's
      log proves all four exchanges crossed the wire ('SELECT <prompt>' -> the selection JSON, 'ANSWER
      TOOLSET-0-FILE' -> the answer, plus both scripted attacks), the strict parser kills the chatty
      unparseable reply, the whitelist kills delete_file, tool_read runs over the rail and the exact 15 B
      observation comes back via the P6c stdout_token. Whole rail still green (CHAN p1/p2/p5, RPCTEST,
      TOOLRUN, AGENTHOST, CHAINHOST, TOOLSET); kernel unchanged; mbcheck.png desktop clean, 0 panic.
    design:
      - the ONLY changed seam bodies: model_select/model_answer became model_exchange() -- a bounded TCP
        round trip (bounded DHCP wait, kernel-bounded connect, bounded send/recv loops, close per
        request). The host gate around the seam is UNCHANGED from CHAINLAYER-HOST-0: strict one-shape
        parse -> TOOLSET-0 whitelist -> path policy, all before any dispatch.
      - the model is hostile text from day one: the rejects are now proven against REAL remote bytes
        (the stub serves the attacks over TCP), upgrading CHAINLAYER-HOST-0's local-string rejection
        proof to a remote-adversary rejection proof.
      - policy_ok additionally pins tool==read_file for the canonical prompt, so a "wrong but
        whitelisted" selection (list_dir) cannot false-green the run.
    review:
      default_build_changed: false      # userspace-only; kernel byte-identical; whole rail re-proven
      all_waits_bounded: true           # net_ready cap, kernel-bounded connect, RECV_MAX, send guard
      hardware_init_gated: n/a          # no new hardware touched; e1000+slirp path is the proven one
      touches_userspace: true
      touches_kernel: false
      preserves_known_good_t410: true   # no net/endpoint -> SKIP exit 0; default boot unchanged
      smoke_proves_claim: true          # PASS line + the stub's own request log = both ends witnessed
      raw_pointers_or_truncation: none  # RESP_CAP-bounded reads, NUL-terminated, newline stripped
    verdict: pass
done: >
  MODEL-BRIDGE-0 COMPLETE. The fake brain is gone: the seam is fed by an external endpoint over TCP and
  the OS-side trust surface did not move -- hostile model output dies at the same parse/whitelist/path
  gate, now proven against real remote bytes. The llama.cpp/GGUF chainlayer2 brain now drops in by
  pointing 10.0.2.2:8431 at a real server; the OS side is already done.
next:
  - MODEL-LOOP-0 (user gate required): the first bounded multi-step loop (select -> observe -> select),
    still read-only tools, still one strict shape per step.
  - swap the stub for a real llama.cpp/GGUF server behind the same port + a prompt template that yields
    the one-shape JSON.
  - multi-line observation framing (length-prefixed) when a tool result stops being one line.
```
