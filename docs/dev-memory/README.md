# dev-memory — the AutomationOS development process as structured data

> **Why this exists.** The biggest lever for an AI coder specialized in *this* OS isn't a bigger
> model or fancier quantization — it's **turning the development process into structured data**.
> Every brick spec, hardware law, QEMU smoke, real-hardware failure, and successful commit becomes
> (a) chainlayer2's working **memory** (retrieve only the few chunks a task needs) and (b) the
> **local-student training set** (distill *our* workflow, not generic coding). This directory is
> that structured data, versioned alongside the code it describes.

## The working-memory hierarchy

Retrieval/context discipline beats dumping the whole repo into the model. Three tiers:

| Tier | File(s) | Refresh |
|---|---|---|
| **hot** | the current files + the current error (not stored here — it's the live task) | per task |
| **warm** | [`active_brick.md`](active_brick.md) — the brick in flight, its checkpoint, its spec | per checkpoint |
| **cold** | [`repo_map.md`](repo_map.md), [`hardware_laws.md`](hardware_laws.md), [`known_good_images.md`](known_good_images.md), [`recent_failures.md`](recent_failures.md), [`bricks/`](bricks/) | per milestone |

A coding agent should ask memory — *what defines `sys_write`/stdout? what laws apply to T410 hardware
init? what branch/spec is active? what tests prove this brick? what failed here before?* — and pull
only the 5–20 relevant chunks. The static prefix (laws + repo map + style + active brick spec) is
cache-friendly; the dynamic suffix is the current files + error + request.

## The machine-readable brick record (the dataset schema)

Each completed (or in-flight) brick gets one record under [`bricks/`](bricks/). It is the unit of
the patch-replay dataset — a successful engineering trajectory the local student learns from.

```yaml
brick: CHANNEL-0
status: in-progress            # planned | in-progress | landed | parked
branch: brick/channel-0
base: t410-recovery
spec: docs/superpowers/specs/2026-06-08-channel-0-design.md
request: >                     # the user intent that started it
gates: [additive]              # build flags / safety gates (EHCI_USB, DISK_PERSIST, "additive", ...)
checkpoints:                   # each = one commit, bisectable
  - id: P0/P1
    commit: 41a1c0a
    files: [kernel/ipc/channel.c, kernel/include/channel.h, ...]
    tests: [build_test/channel_p1.sh]
    result: "[CHAN] selftest PASS; 91 compiled; desktop reached; 0 panic"
review:                        # the reviewer pass (see hardware_laws.md checklist)
  default_build_changed: false
  all_waits_bounded: true
  hardware_init_gated: n/a
  preserves_known_good_t410: true
  smoke_proves_claim: true
verdict: pass
```

## The reviewer checklist (derived from hardware_laws.md)

A separate, stricter reviewer role should be able to say **no**. For AutomationOS, every patch is
checked against: *does the default build change? are all waits bounded? is hardware init gated?
does it touch userspace accidentally? does it preserve the known-good T410 image? does the smoke
test actually prove the claim? any raw pointers / path truncations?* See
[`hardware_laws.md`](hardware_laws.md).

## Structured tools, not terminal scraping

The in-OS counterpart of this discipline is **CHANNEL-0** (`CH_MSG` / `msg_packet_t` / AGENT-RPC-0):
the agent drives the machine through **typed tool calls** (`TOOL_RUN`/`TOOL_RESULT`), never by
reading a terminal grid. This memory feeds the model's *context*; the channel feeds its *actions*.
