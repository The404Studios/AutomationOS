#!/usr/bin/env python3
"""mcp_bridge_e2e.py -- an MCP client driving AutomationOS through the bridge, end to end:

    this client --HTTP/MCP--> scripts/chainlayer_mcp_bridge.py --TCP (slirp)--> sbin/agentd in the guest
                                                                 --> gated tool --> KERNEL

Run by build_test/mcp_bridge_e2e.sh (which boots the OS and starts the bridge). Exit 0 = all pass.
Every check is a REAL effect observed from outside: file contents, kernel firewall status, and the
packet-level behaviour of a forwarded TCP port before/after the kernel rule changes.
"""
import json, os, socket, sys, time, urllib.request

BASE = os.environ.get("MCP_URL", "http://127.0.0.1:8940/mcp")
TOKEN = os.environ.get("CL_BRIDGE_TOKEN", "")
PROBE_PORT = int(os.environ.get("PROBE_PORT", "18080"))      # host port forwarded to guest :8080
SERIAL = os.environ.get("SERIAL_LOG", "")

_id = [0]
def rpc(method, params=None):
    _id[0] += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _id[0], "method": method, "params": params or {}}).encode()
    req = urllib.request.Request(BASE, body, {"Content-Type": "application/json", "Authorization": "Bearer " + TOKEN})
    with urllib.request.urlopen(req, timeout=180) as r:
        return json.loads(r.read())

def call(name, **args):
    r = rpc("tools/call", {"name": name, "arguments": args})
    if "error" in r:
        return True, "RPC-ERROR " + json.dumps(r["error"])
    res = r["result"]
    return res["isError"], res["content"][0]["text"]

fails = []
def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + ((" -- " + detail) if (detail and not cond) else ""))
    if not cond: fails.append(name)

def probe_port():
    """Send a real HTTP request to the forwarded port (host :18080 -> guest :8080) and see whether
    the guest SERVICE answers. slirp completes the host-side handshake itself, so what matters is
    what the guest KERNEL does with the SYN it forwards:
       dropped by the kernel firewall  -> nothing ever answers          -> 'silent'
       admitted, httpd listening       -> 'HTTP/1.x ...' comes back      -> 'http'"""
    s = socket.socket()
    s.settimeout(8.0)
    try:
        s.connect(("127.0.0.1", PROBE_PORT))
        s.sendall(b"GET / HTTP/1.0\r\nHost: guest\r\n\r\n")
    except OSError:
        return "refused"
    try:
        d = s.recv(64)
        return "http" if d.startswith(b"HTTP/") else ("closed" if d == b"" else "data")
    except socket.timeout:
        return "silent"
    except OSError:
        return "closed"
    finally:
        s.close()

def main():
    print("MCP e2e against", BASE)
    r = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "e2e", "version": "0"}})
    check("initialize", r["result"]["serverInfo"]["name"] == "automationos-bridge")
    tools = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
    check("tools/list exposes the rail incl. kernel tools", all(t in tools for t in ("read_file", "write_file", "execute", "firewall", "fw_status")), str(tools))

    # --- wait for the OS (agentd serve mode dials the bridge after boot) ---
    t0 = time.time(); up = False
    while time.time() - t0 < 150:
        err, txt = call("ps")
        if not err: up = True; break
        time.sleep(3)
    check("AutomationOS session attached (agentd serve mode dialled in)", up, "timed out waiting for the OS")
    if not up:
        return 1

    err, txt = call("list_dir", path="/etc")
    check("list_dir /etc", (not err) and "toolset0.txt" in txt, txt[:100])
    err, txt = call("read_file", path="/etc/toolset0.txt")
    check("read_file /etc/toolset0.txt", (not err) and "TOOLSET-0-FILE" in txt, txt[:100])

    # --- the OS-side gate, not the bridge, refuses these ---
    err, txt = call("read_file", path="/etc/../boot/grub.cfg")
    check("OS path gate refuses traversal", err and "denied" in txt.lower(), txt[:100])

    err, txt = call("write_file", path="/tmp/e2e.txt", content="hello from the bridge\n")
    check("write_file /tmp/e2e.txt", not err, txt[:100])
    err, txt = call("read_file", path="/tmp/e2e.txt")
    check("read it back", (not err) and "hello from the bridge" in txt, txt[:100])
    err, txt = call("write_file", path="/etc/evil.txt", content="x")
    check("write to protected /etc is refused by the OS", err or "DENY" in txt or "denied" in txt.lower(), txt[:100])

    # --- kernel level: the packet filter ---
    err, txt = call("fw_status")
    check("fw_status reads kernel firewall state", (not err) and "FW enabled=1" in txt and "policy_in=drop" in txt, txt[:140])
    # A real guest service (httpd on :8080), started through the confirm-gated spawn tool. The agent's
    # spawn tool drops all kernel privileges before launching it, so it runs unprivileged.
    err, txt = call("spawn", path="sbin/httpd")
    check("spawn sbin/httpd (a live guest service on :8080)", (not err) and "SPAWN" in txt, txt[:120])
    time.sleep(3.0)
    before = probe_port()
    check("service is up but UNREACHABLE: kernel drops the inbound SYN (default policy)", before == "silent", "got " + before)

    err, txt = call("firewall", verb="allow", args=["tcp", "8080"])
    check("firewall allow tcp 8080 (CONFIRM-class tool, kernel NET_ADMIN)", (not err) and "ALLOW rule" in txt, txt[:120])
    err, txt = call("fw_status", detail="list")
    check("kernel rule table shows the rule", (not err) and "accept tcp" in txt and "dport=8080" in txt, txt[:140])
    after = probe_port()
    check("same request now reaches the service: the rule changed real packet behaviour", after == "http", "got " + after)

    err, txt = call("firewall", verb="policy", args=["in", "accept"])
    check("agent cannot open the whole host (policy in accept refused)", err and "DENY" in txt, txt[:120])
    err, txt = call("firewall", verb="policy", args=["out", "drop"])
    check("agent cannot sever its own link (policy out drop refused)", err and "DENY" in txt, txt[:120])
    err, txt = call("firewall", verb="flush")
    check("flush is not expressible through the bridge", err)
    err, txt = call("firewall", verb="allow", args=["tcp", "1-60000"])
    check("blanket port range refused", err and "DENY" in txt, txt[:120])

    err, txt = call("firewall", verb="del", args=["0"])
    check("firewall del 0", (not err) and "DELETED" in txt, txt[:100])
    final = probe_port()
    check("after deleting the rule the service is unreachable again", final == "silent", "got " + final)

    # --- code the agent launches runs with NO kernel privileges ---
    prog = ("int main(){\n  char c;\n  long m;\n  long r;\n"
            "  m = syscall(138,0,0,0);\n"
            "  c=67; syscall(3,1,(long)&c,1);\n  c=65; syscall(3,1,(long)&c,1);\n  c=80; syscall(3,1,(long)&c,1);\n"
            "  c=68; syscall(3,1,(long)&c,1);\n  c=61; syscall(3,1,(long)&c,1);\n"
            "  c=78; if (m == 15) { c=89; }\n  syscall(3,1,(long)&c,1);\n"
            "  c=32; syscall(3,1,(long)&c,1);\n"
            "  r = syscall(68,(long)&c,60,0);\n"
            "  c=82; syscall(3,1,(long)&c,1);\n  c=61; syscall(3,1,(long)&c,1);\n"
            "  c=79; if (r == -1) { c=68; }\n  syscall(3,1,(long)&c,1);\n"
            "  c=10; syscall(3,1,(long)&c,1);\n  return 0;\n}\n")
    err, txt = call("write_file", path="/tmp/cap.c", content=prog)
    check("write cap probe source", not err, txt[:100])
    err, txt = call("compile", source="/tmp/cap.c", output="/tmp/cap.elf")
    check("compile on the device", not err, txt[:160])
    err, txt = call("execute", path="/tmp/cap.elf")
    check("execute the compiled probe", (not err) and "exit=0" in txt, txt[:160])
    time.sleep(1.0)
    if SERIAL and os.path.exists(SERIAL):
        ser = open(SERIAL, "rb").read().decode("latin1")
        check("launched code ran with ALL kernel privileges dropped (CAPD=Y)", "CAPD=Y" in ser)
        check("launched code was refused raw network access by the kernel (R=D)", "R=D" in ser)
    else:
        print("  (no serial log supplied; skipping the serial-side privilege proof)")

    print("MCP-E2E: %s" % ("PASS" if not fails else "FAIL (%d: %s)" % (len(fails), "; ".join(fails))))
    return 0 if not fails else 1

if __name__ == "__main__":
    sys.exit(main())
