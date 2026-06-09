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
| `stdout_handle` | u32  | a byte-channel handle for the tool's output (see the semantics note below) |
| `reserved`      | u32  | 0                                                      |

### `stdout_handle` semantics (READ THIS before claiming "the agent reads stdout")

The meaning of `stdout_handle` has changed per checkpoint. Be precise about what
each level actually proves — do not let a summary overclaim:

- **P6a:** always `0`. Inert. No tool was run.
- **P6b:** a **runner-local handle token** — the handle, in the *runner's* handle
  table, of the `CH_BYTE` channel it bound to the tool's stdout and **drained
  itself**. It is **non-zero but NOT dereferenceable by the agent**: CHANNEL-0
  has no cross-process handle transfer, so the number is meaningless in the
  agent's table. P6b's real invariant is *"the runner created a stdout channel,
  the tool wrote to it, and the runner drained N bytes"* — NOT *"the agent read
  stdout"*. The token is returned honestly so the contract is ready for P6c.
- **P6c+:** `stdout_handle` becomes **agent-readable** only after an explicit,
  one-shot, read-only **handle-transfer/capability primitive** moves the stdout
  *read* end from the runner to the requesting agent. Only then may anything
  claim the agent reads the tool's stdout.

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
- **P6c — NEXT (capability only):** make `stdout_handle` real — a one-shot,
  read-only transfer of the *tool's stdout read end* from the runner to the
  requesting agent, so the agent can read the exact stdout bytes. No argv. This
  is the capability/security change, kept isolated from parsing risk.
- **P6d — after P6c:** argv — `args` as NUL-separated argv bytes, validated
  (reject empty `arg0`, missing final NUL, malformed empty entries). Still no
  shell. Kept separate from P6c so an ABI/parsing failure can't be confused with
  a capability failure.
