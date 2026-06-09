# brick record: TOOLSET-0

> Give the agent a small, safe, structured tool SURFACE on top of the AGENT-RPC-0
> rail, BEFORE plugging in a model. The model should drive hardened typed tools,
> not arbitrary process execution.

```yaml
brick: TOOLSET-0
status: complete
branch: brick/toolset-0
base: brick/agent-host-0          # off the frozen first-agent milestone (19e96c3)
request: >
  A small safe tool set with stable schemas: read_file (bounded, size cap), list_dir (bounded, no
  recursion), stat (metadata), run (path+argv), echoargs (proven). A host-side trust surface: tool-name
  whitelist + a conservative path policy + caps. NO shell, NO networking, NO model, NO recursive fs walk,
  NO write/delete tools, NO kernel change, NO typed result schema yet.
decisions:
  - path policy = reject empty + reject any "..". Conservative TRAVERSAL DENIAL, NOT a full jail. Root
    allowlists / per-tool authority scopes deferred -> TOOL-AUTH-0.
  - result format = plain text stdout. Typed result envelopes deferred -> TOOL-RESULT-0.
  - tools are small sandboxed PROGRAMS over the rail, not kernel handlers, not a giant registry.
checkpoints:
  - id: TS0
    title: read_file/list_dir/stat tools + a whitelisting/path-policy host + the 8-point proof
    commits: [bb9bbdf]
    files:
      - userspace/apps/tool_read/tool_read.c   # SYS_STAT first; size>256 -> reject(exit 3); else read exact
      - userspace/apps/tool_ls/tool_ls.c       # opendir/readdir<=32 (readdir returns 0=entry!), skip ./.., reject>32
      - userspace/apps/tool_stat/tool_stat.c   # "size=<n> type=<f|d>" (type via OPENDIR probe)
      - userspace/apps/toolset_host/toolset_host.c  # the TRUST SURFACE: whitelist + bad_path + dispatch + 8 tests
      - scripts/build_all.sh                   # compile/stage the 4 progs + the /etc/toolset0.txt fixture (15 B)
      - userspace/init/main.c                  # init spawns sbin/toolset_host
    tests: [build_test/toolset_verify.sh]
    result: >
      build_all clean (kernel unchanged); serial 'TOOLSET: PASS ls=1 stat=1 read_exact=1 run=1
      unknown_rejected=1 malformed_rejected=1 oversize_rejected=1 traversal_rejected=1' -- the host lists
      /etc, stats + reads /etc/toolset0.txt EXACTLY (15 B via the P6c capability), runs echoargs, and every
      guard fires (unknown name -> host whitelist; malformed argv -> runner; >256 B read -> tool; ".." ->
      host policy). Whole rail (CHAN/RPCTEST/TOOLRUN/AGENTHOST) still PASSES; tsfinal.png clean, 0 panic.
    design:
      - the host is the trust surface: tool request -> validate NAME (resolve_tool whitelist) + PATH
        (bad_path: reject empty/".."): unknown name and traversal are refused BEFORE any dispatch. Only a
        known program path is ever spawned (via SYS_SPAWN_EX_ARGV), and its stdout returns through the
        one-shot read-only P6c capability, which the host reads exactly.
      - each tool is one bounded op with caps in BOTH host and tool (defense in depth); errors go to fd2
        (serial), never to fd1 (the channel). oversize read is statted-first and rejected with a nonzero
        exit, never truncated; too-many dir entries are a visible reject, not a partial list.
      - GOTCHA (found+fixed in-loop): SYS_READDIR returns 0 on success (entry valid), nonzero to stop --
        my first tool_ls used `>0` so it emitted nothing (ls=0). Matched find.c's `if(r!=0) break`. The
        other 7 fields passed first try, proving the rail/VFS/policy were already right.
    review:
      default_build_changed: false      # userspace-only; kernel byte-identical; the whole rail still passes
      all_waits_bounded: true           # bounded poll/yield loops; bounded read(256)/entries(32)
      hardware_init_gated: n/a
      touches_userspace: true
      touches_kernel: false
      preserves_known_good_t410: true
      smoke_proves_claim: true          # TOOLSET: PASS = 4 tool ops + 4 rejects, all green
      raw_pointers_or_truncation: none  # READ_CAP/LS_MAX bounded; stat-first oversize reject; no truncation
    verdict: pass
done: >
  TOOLSET-0 COMPLETE. The agent now has a hardened tool surface (read_file/list_dir/stat/run) gated by a
  name whitelist + conservative path policy + caps -- safe typed tools, not arbitrary exec. Ready for a
  model to choose among them.
next:
  - CHAINLAYER-HOST-0: a local/API model chooses among these typed tools (the chainlayer2 host agent).
  - later: TOOL-AUTH-0 (root allowlists / per-tool authority) · TOOL-RESULT-0 (typed result envelopes).
```
