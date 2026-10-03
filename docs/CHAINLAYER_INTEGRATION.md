# ChainLayerTwo <-> AutomationOS

Status 2026-10-02. This is the honest map of how ChainLayerTwo (the Node/TypeScript platform that hosts Claude and
other models) works with this OS, what is proven, what is not, and what cannot be done at this tier.

## Architecture (the OS is the *execution target*; the brain and the Node tooling stay on the host)

```
 ChainLayerTwo (host, Node)                       AutomationOS (QEMU guest or the T410)
 chainlayercli / chainlayer-api                   sbin/agentd  (AGENTD_SERVE=1, /etc/agentd.conf)
   |  its MCP client (Streamable-HTTP, Bearer)       ^  dials OUT (kernel firewall: outbound ACCEPT)
   v                                                 |  TCP  broker=<host>:8433
 scripts/chainlayer_mcp_bridge.py  ------------------+
   tiers: observe | mutate | control                 gated tools  ->  KERNEL
   token, tool schema, arg sanitising                (path policy, CONFIRM gate, hash-chained ledger, rollback,
                                                      PCAP-0 privilege drop, FW-0 packet filter)
```

Why this shape: the guest *dials out*, so the OS needs **no inbound port** for ChainLayerTwo (its firewall default is
inbound DROP), and the same design works over QEMU's NAT and over a real LAN. ChainLayerTwo's CLI has no remote
execution backend, so an MCP server is the supported seam; the bridge is that server.

## What ChainLayerTwo can do on the OS today

| capability | tool(s) | tier | enforced by |
|---|---|---|---|
| read files, list/stat, processes, kernel firewall state | `read_file list_dir stat ps fw_status` | observe | OS path policy |
| **create files and directories** | `write_file mkdir move` | mutate | OS path policy (only `/tmp`, `/home`, `/usr/src`; the old contents are snapshotted) |
| **create software**: write C -> compile on the device -> run | `write_file` -> `compile` -> `execute` | mutate | on-device `cc`; the program runs with **all kernel privileges dropped** |
| undo | `rollback` | mutate | pre-mutation snapshot |
| start/stop programs, delete, synthetic mouse/keyboard, **change the kernel firewall** | `spawn kill remove mouse key firewall` | control (off unless `--allow-control`) | the **device** still asks its operator (CONFIRM gate) unless `grant_full` is set |

Proven (not just claimed):
* `build_test/mcp_bridge_e2e.sh` -- a generic MCP client drives all of the above against a booted guest, including
  *packet-level* proof that a firewall rule written through MCP changes real traffic.
* `build_test/chainlayer_e2e.sh` -- **ChainLayerTwo's own client code** (`chainlayercli/src/mcp.ts`) runs the
  create-file / create-software / run / read-back loop (see the Status section at the bottom for the last result).

### "Full automation of the OS"

The default is deliberately *not* unrestricted: an agent that can delete files, drive the mouse and rewrite the
firewall should be something the owner switches on. To grant it:
1. start the bridge with `--allow-control` (exposes the control tier);
2. on the device, the operator either answers each CONFIRM prompt, or sets the cockpit `grant_full` toggle for the
   session (it is operator-only and resets each boot).
Three firewall actions stay refused even then, by design: opening the whole host (`policy in accept`), severing the
agent's own link (`policy out drop`), and blanket port ranges. `flush` is human-only.

## Network map

Host services of ChainLayerTwo (read from its source; **not** exercised end to end here):

| service | default port | bind | notes |
|---|---|---|---|
| OmniRoute (LLM gateway, OpenAI-compatible `/v1`) | 20128 | `.env` sets 127.0.0.1 | `Authorization: Bearer sk-...`; `REQUIRE_API_KEY=true` locally; health `GET /api/health/ping` |
| chainlayer2-mcp (tool gateway) | 8930 | 127.0.0.1 | Streamable-HTTP `/mcp`, Bearer; `/health` open. Running on this machine |
| chainlayer-api (control plane, `/v1/runs`) | 8785 | **all interfaces** | Bearer `cl_live_...` |
| chainlayer-relay (`/remote`) | 8944 | 127.0.0.1 | its token is shell-equivalent |
| chainlayer-stats / authserver | 8945 / 8080 (8090 prod) | 127.0.0.1 | |

Ports this repo adds:

| port | who listens | who connects | purpose |
|---|---|---|---|
| 8940 | the bridge (host) | ChainLayerTwo | MCP over HTTP, Bearer token |
| 8433 | the bridge (host) | the OS's `agentd` | OS dial-in; QEMU slirp maps guest `10.0.2.2:8433` to host `127.0.0.1:8433` |
| 10.0.2.2:PORT | the host | the guest | how the guest reaches *any* host service above under QEMU (fails with `restrict=on`) |

Nothing listens on the OS for ChainLayerTwo. From the guest, OmniRoute is `http://10.0.2.2:20128/v1`.

## Running it

QEMU (what the e2e scripts do):
```
AGENTD_SERVE=1 DESKTOP_MINIMAL=1 bash scripts/build_all.sh          # /etc/agentd.conf: serve=1 broker=10.0.2.2:8433
CL_BRIDGE_TOKEN=<>=16 random chars> python3 scripts/chainlayer_mcp_bridge.py [--allow-control]
```
Register with ChainLayerTwo (**its EULA must be accepted on your machine first: `chainlayertwo eula accept` -- that is
your decision, nothing here does it for you**):
```
chainlayertwo eula accept
chainlayertwo mcp add automationos --url http://127.0.0.1:8940/mcp --token <the token>
chainlayertwo -y --deny-tools spawn,kill,remove "build a program on the OS that prints fib(10)"   # headless needs -y or an allowlist
```
Notes from reading ChainLayerTwo: every MCP tool is treated as *mutating* (its approval gate applies); its
`--no-shell` does not cover an MCP `execute`/`spawn`, so use `--deny-tools` for those; MCP tool names must not collide
with its built-ins (ours do not).

Real hardware (the T410) -- **not yet exercised on hardware**:
1. `AGENTD_SERVE=1 AGENTD_BROKER=<host LAN IP>:8433 bash scripts/build_all.sh`
2. `python3 scripts/chainlayer_mcp_bridge.py --os-host <host LAN IP> --os-allow-ip <the T410's IP>` -- the bridge now
   **refuses** a non-loopback OS listener without `--os-allow-ip`, because otherwise the first machine on the LAN to
   say `GOAL` would become "the OS": it would receive every tool call and file ChainLayerTwo writes and could answer with
   forged results. Also restrict the host firewall for 8433 to the T410's address.
3. The T410 needs a working NIC. The onboard 82577LM bring-up is **gated OFF by default** (it can hardware-stall the
   machine; `PCH_NIC=1` opts in -- validate with a serial console attached). A supported PCI/USB NIC, or Wi-Fi
   (`IWLWIFI=1` + firmware), are the alternatives.

## What cannot be done on the OS (so it is not claimed)

* **Node.js, git, Blender, Unreal, Unity, Steam do not run inside AutomationOS.** They need a Linux/Windows userland
  and syscall surface (threads, mmap/mprotect, a dynamic loader, GPU APIs) far beyond what exists
  (`docs/COMPAT_FEASIBILITY_2026-10-02.md`). Windows `.exe` support is at tier 1 (`docs/WIN_MIN.md`).
  The supported split is: **Node/ChainLayerTwo, git and the heavy tools run on the host; the OS is the machine
  ChainLayerTwo builds for and drives.** Version control of what ChainLayerTwo makes is done host-side (read the files
  back over `read_file`, commit there; ChainLayerTwo's own `/git` and worktree tooling already work on host paths).
* The on-device compiler is a **C subset** (no string literals, arrays or globals), so "software ChainLayerTwo
  creates on the OS" is currently that subset; results travel back as exit codes / files.
* Writes are limited to `/tmp`, `/home`, `/usr/src` by the OS path policy; `/etc` is not writable by any tool.

## Security notes (found while mapping)

* `chainlayer-api` binds **all interfaces** by default (`app.listen(port)` with no host); keep it behind a firewall.
* OmniRoute's runbook shows the dashboard password `CHANGEME` -- change it before exposing anything.
* The relay token is equivalent to shell access on the host CLI.
* The bridge requires a >=16-character Bearer token, listens on loopback by default, and sanitises tool arguments
  (control characters / oversize paths rejected before the OS sees them).

## Status

See `CHANGELOG.md` and the last `build_test/chainlayer_e2e.sh` run for the current results.
