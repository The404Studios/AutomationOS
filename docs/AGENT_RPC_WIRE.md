# AGENT-RPC-0 wire schema (P6a)

> The typed-tool contract that rides on CHANNEL-0 `CH_MSG` packets. This is the
> **wire contract only** — type IDs, versioned payload structs, and validation
> rules. Dispatch (spawn a tool, pass its stdout channel back) is **P6b**, not
> covered here. Authoritative header: [`userspace/lib/agent_rpc.h`](../userspace/lib/agent_rpc.h).

## Why a locked schema first

The agent must never scrape the terminal. It sends a **`TOOL_RUN`** request and
receives a **`TOOL_RESULT`** — structured, versioned, with explicit error
semantics. Dispatch is where the risk starts (spawning, fd passing, channel
lifetime), so the wire format is frozen and self-tested *before* any runner is
built. A receiver that doesn't recognize the `version` MUST reject the packet —
that is what lets the format evolve across future updates without silent
misreads.

## Transport

A message rides one `CH_MSG` packet (CHANNEL-0 P5a/P5b):

```
msg_packet_t { type, flags, len, request_id }  +  payload[len]
```

- `type` selects the payload struct: `MSG_TOOL_RUN` (`0x0101`) or
  `MSG_TOOL_RESULT` (`0x0102`).
- `request_id` correlates a result to its request (set by the agent on the
  request, echoed by the runner on the result).
- `len` MUST equal `sizeof` the selected payload struct (fixed-size payloads).

## Payloads

### `TOOL_RUN` (`type = 0x0101`, payload = `tool_run_t`, 392 bytes)

| field      | type           | meaning                                            |
|------------|----------------|----------------------------------------------------|
| `version`  | u16            | `= AGENT_RPC_VERSION` (1)                           |
| `flags`    | u16            | `TOOL_F_*` (reserved; 0 in P6a)                     |
| `path_len` | u32            | used bytes of `path[]`, `1..TOOL_PATH_MAX-1`        |
| `args_len` | u32            | used bytes of `args[]`, `0..TOOL_ARGS_MAX-1`        |
| `reserved` | u32            | 0                                                  |
| `path`     | char[120]      | tool path, NUL-terminated within the buffer        |
| `args`     | char[256]      | space-separated args string, NUL-terminated        |

### `TOOL_RESULT` (`type = 0x0102`, payload = `tool_result_t`, 16 bytes)

| field           | type | meaning                                                |
|-----------------|------|--------------------------------------------------------|
| `version`       | u16  | `= AGENT_RPC_VERSION` (1)                               |
| `flags`         | u16  | `TOOL_F_*` (reserved; 0 in P6a)                         |
| `exit_code`     | i32  | the tool's exit status                                 |
| `stdout_token`  | u32  | opaque, checkpoint-defined stdout token — **not** a usable handle (see the semantics note below) |
| `reserved`      | u32  | 0                                                      |

### `stdout_token` semantics (READ THIS before claiming "the agent reads stdout")

The field was renamed `stdout_handle` → `stdout_token` so no reader assumes it is
a directly-usable process-local handle. Its meaning is checkpoint-defined — be
precise about what each level actually proves; do not let a summary overclaim:

- **P6a:** always `0`. Inert. No tool was run.
- **P6b:** a **runner-local handle token** — the handle, in the *runner's* handle
  table, of the `CH_BYTE` channel it bound to the tool's stdout and **drained
  itself**. Non-zero but **NOT dereferenceable by the agent** (CHANNEL-0 had no
  cross-process handle transfer yet). P6b's real invariant is *"the runner created
  a stdout channel, the tool wrote to it, and the runner drained N bytes"* — NOT
  *"the agent read stdout"*.
- **P6c (DONE):** a **one-shot grant id**. The runner calls `SYS_CH_GRANT(out_handle,
  agent_pid)` and returns the grant id here; the agent **MUST** call
  `SYS_CH_ACCEPT(stdout_token)` to convert it into a **read-only** local handle,
  then read the tool's stdout itself. The grant is `CH_BYTE`-only, `CH_END_MASTER`,
  `CH_R_READ`-only, bound to `agent_pid`, single-use (`grant_id = (gen<<16)|(slot+1)`,
  so a stale/reused id fails). Only now may anything claim *the agent reads the
  tool's stdout* — and only after a successful `ACCEPT`.

## Validation rules (enforced by `*_validate`)

A receiver rejects a payload unless **all** hold:

1. `payload_len == sizeof(struct)` → else `AR_E_LEN`.
2. `version == AGENT_RPC_VERSION` → else `AR_E_VERSION`.
3. (`TOOL_RUN`) `1 <= path_len < TOOL_PATH_MAX` and `args_len < TOOL_ARGS_MAX`
   → else `AR_E_FIELD`.
4. (`TOOL_RUN`) `path[path_len] == 0` and `args[args_len] == 0` (NUL-terminated)
   → else `AR_E_NUL`.

Encoding rejects an over-long `path`/`args` with `AR_E_TOOLONG`.

## Status

- **P6a — DONE:** schema + `tool_run_encode/validate`, `tool_result_encode/validate`,
  and an in-memory encode→validate→reject self-test (`sbin/rpctest`): serial
  `RPCTEST: PASS ...`. No spawn, no fd passing, no stdout channel.
- **P6b — DONE:** the path-only runner — recv one `TOOL_RUN` (args rejected:
  `args_len` must be 0), spawn the tool with its stdout bound to a `CH_BYTE`
  channel, the **runner drains the stdout itself**, send `TOOL_RESULT
  { exit_code, stdout_handle }`. `stdout_handle` is a **runner-local token**
  (see the semantics note above). Proof `sbin/toolrun`: serial `RUNNER: PASS
  ... stdout_bytes=183` + `TOOLRUN: PASS ...`.
- **P6c — DONE (capability only):** `stdout_token` is real. New syscalls
  `SYS_CH_GRANT(handle, to_pid)` / `SYS_CH_ACCEPT(grant_id)` — a one-shot,
  read-only, `CH_BYTE`-only, `MASTER`-end, target-pid-bound capability transfer.
  The runner grants the tool's stdout read end to the agent's pid; the agent
  accepts → read-only handle → reads the **exact** stdout bytes. Proof
  `sbin/toolrun` + `sbin/echoproof` (deterministic 17-byte stdout): serial
  `TOOLRUN: PASS … agent_read=17 exact=1 ro=1 dblaccept_deny=1 bogus_deny=1` +
  `RUNNER: PASS grant=1 ctrl_deny=1 inv_deny=1 norights_deny=1 wrongpid_deny=1
  enospc=1`. Grants from/to a dead process are swept (no leaked refs). NOT general
  fd passing.
- **P6d — NEXT:** argv — `args` as NUL-separated argv bytes, validated (reject
  empty `arg0`, missing final NUL, malformed empty entries). Still no shell. Kept
  separate from P6c so an ABI/parsing failure can't be confused with a capability
  failure.
