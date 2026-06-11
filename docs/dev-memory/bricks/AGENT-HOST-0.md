# brick record: AGENT-HOST-0

> Make a tiny agent actually RIDE the finished AGENT-RPC-0 rail. The first real
> consumer, not more plumbing: agent -> runner -> tool -> stdout capability ->
> agent DECISION. The first "AI-OS" milestone.

```yaml
brick: AGENT-HOST-0
status: complete
branch: brick/agent-host-0
base: brick/agent-rpc-0            # off the frozen typed-tool rail (9460446)
request: >
  The kernel/user IPC rail is done (CHANNEL-0 + AGENT-RPC-0). The open question is no longer "can the OS
  do more plumbing?" but "can an agent issue TOOL_RUN, receive TOOL_RESULT, accept the stdout_token, read
  the EXACT stdout, and make a DECISION?" Build one host loop that proves exactly that, plus a malformed
  TOOL_RUN is rejected. NO networking, NO model inference, NO async batching, NO tool registry.
checkpoints:
  - id: AH0
    title: one agent-host loop -- TOOL_RUN -> TOOL_RESULT -> accept token -> read exact stdout -> verdict
    commits: [98bd950]
    files:
      - userspace/lib/agent_rpc.h          # +TOOL_F_ERR (a TOOL_RESULT error flag, so the runner can reject cleanly)
      - userspace/apps/agenthost/agenthost.c  # NEW: the agent host + a clean production runner (self-spawn)
      - scripts/build_all.sh               # compile + stage sbin/agenthost
      - userspace/init/main.c              # init spawns sbin/agenthost
    tests: [build_test/agenthost_verify.sh]
    result: >
      build_all clean (kernel unchanged); serial 'AGENTHOST: PASS path_ok=1 argv_ok=1 stdout_exact=1
      exit=0 malformed_rejected=1' -- the agent issued TOOL_RUN{sbin/echoargs, ["hello world","a;b|c"]},
      got TOOL_RESULT, ACCEPTed the stdout_token, read the EXACT 32-byte stdout via the P6c read-only
      capability, decided the tool succeeded with the expected output, and refused a malformed call (argv
      with its final NUL clobbered -> TOOL_F_ERR). The whole rail (CHAN/RPCTEST/MSGTEST/TOOLRUN) still
      PASSES; ahcheck.png clean, 0 panic.
    design:
      - the first CONSUMER, not plumbing: agenthost self-spawns a clean runner over a CH_MSG ctrl. The
        runner handles exactly two requests (valid, then malformed); on valid it spawns the tool via
        SYS_SPAWN_EX_ARGV (P6d) with stdout bound to a CH_BYTE channel and GRANTs the read-only stdout
        capability (P6c); on malformed it replies TOOL_F_ERR with stdout_token=0 (no spawn).
      - the agent makes a DECISION from the structured result: path_ok (accepted+ran, no error flag,
        token present), argv_ok (the stdout echo contains the entries intact), stdout_exact (byte-match),
        exit (the tool's code). One ACK per request sequences the runner's lifetime past the agent's accept.
      - scope held: the ONLY schema addition is TOOL_F_ERR. No networking / model / async / registry. No
        kernel change -- it rides the finished rail with the existing syscalls.
    review:
      default_build_changed: false      # userspace-only; kernel byte-identical; the whole rail still passes
      all_waits_bounded: true           # bounded poll+yield loops (recv/waitpid/grant/read/ack)
      hardware_init_gated: n/a
      touches_userspace: true
      touches_kernel: false
      preserves_known_good_t410: true
      smoke_proves_claim: true          # AGENTHOST: PASS = the full agent decision loop + malformed reject
      raw_pointers_or_truncation: none  # typed recv buffers; bounded read; capability is read-only/one-shot
    verdict: pass
done: >
  AGENT-HOST-0 COMPLETE. AutomationOS now has a tiny agent that issues a typed tool call, reads the
  tool's exact output through a one-shot read-only capability, and makes a decision -- no shell, no
  terminal scraping, auditable trust boundaries. The substrate for the chainlayer2 host agent.
next:
  - a real tool set + a TOOL_RESULT that carries structured fields; OR an external-model host (llama.cpp/
    GGUF) driving this rail via CHANNEL-0 typed calls; OR P7/P8 (async batch / NIC channels) when needed.
```
