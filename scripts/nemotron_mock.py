#!/usr/bin/env python3
"""nemotron_mock.py -- a SCRIPTED stand-in for the Nemotron agent broker.

Proves the whole OS agentic loop end-to-end at ZERO cost (no NVIDIA key), the
same way oauth_mock.py proved the device flow. It speaks the multi-step agent
protocol the OS (sbin/agentd) drives over the QEMU slirp seam:

    OS  -> broker:  "GOAL <natural-language request>\n"
    broker -> OS:   "TOOL {\"tool\":\"<name>\",\"args\":\"<args>\"}\n"   (call a gated tool)
                    -- or --
                    "DONE <final answer>\n"                              (task complete)
    OS  -> broker:  "RESULT <tool stdout / exit>\n"                     (after running it)
    ... loop until DONE; one task per TCP connection.

Instead of asking a model, this mock runs a FIXED tool sequence so the loop +
the gated rail + the policy gate are provable deterministically. It uses only
the tools the OS already ships (read_file/list_dir/stat); once the Phase-2
write/compile/execute tools land, extend STEPS to drive a real code task.

Run:  python3 scripts/nemotron_mock.py        # listens 127.0.0.1:8433 (slirp 10.0.2.2:8433)
"""
import os, sys, json, base64, socket

HOST, PORT = "127.0.0.1", 8433

# A cc-subset-friendly C program (NO string literals / arrays / globals) that the
# agent will WRITE, COMPILE on-device, and RUN -- it prints "AGENTOK\n" to serial.
AGENT_C = (
    "int main(){\n"
    "  char c;\n"
    "  c=65; syscall(3,1,(long)&c,1);\n"   # A
    "  c=71; syscall(3,1,(long)&c,1);\n"   # G
    "  c=69; syscall(3,1,(long)&c,1);\n"   # E
    "  c=78; syscall(3,1,(long)&c,1);\n"   # N
    "  c=84; syscall(3,1,(long)&c,1);\n"   # T
    "  c=79; syscall(3,1,(long)&c,1);\n"   # O
    "  c=75; syscall(3,1,(long)&c,1);\n"   # K
    "  c=10; syscall(3,1,(long)&c,1);\n"   # \n
    "  return 0;\n"
    "}\n"
)
CODE_B64 = base64.b64encode(AGENT_C.encode()).decode()

# A deterministic ReAct-style plan: each entry is a (tool, args) to call; after
# the last tool's RESULT comes back, the mock emits DONE with a summary.
STEPS = [
    ("list_dir",  "/etc"),
    ("stat",      "/etc/toolset0.txt"),
    ("read_file", "/etc/toolset0.txt"),
]

# HOSTILE plan (NEMO_HOSTILE=1): the model is untrusted text, so the mock plays
# the attacker -- a non-whitelisted destructive tool, then a path-traversal -- and
# the OS gate MUST reject BOTH (agentd prints "AGENTD: DENY ...", returns a
# policy-denied RESULT) before the final legit read proves the loop still recovers.
HOSTILE_STEPS = [
    ("delete_everything", "/"),                    # unknown tool   -> whitelist DENY
    ("read_file",         "/etc/../boot/grub.cfg"),# ".." traversal -> path-policy DENY
    ("read_file",         "/etc/toolset0.txt"),    # legit          -> runs, recovers
]

# CODE-TASK plan (NEMO_CODETASK=1): the headline run-open-code workflow end-to-end --
# make a project dir, WRITE a C program (base64), COMPILE it on-device, EXECUTE it,
# snapshot processes, then attempt a DESTRUCTIVE system delete that the gate MUST deny.
CODETASK_STEPS = [
    ("mkdir",      "/tmp/agentproj"),
    ("write_file", "/tmp/agentproj/m.c\t" + CODE_B64),
    ("compile",    "/tmp/agentproj/m.c\t/tmp/m.elf"),  # flat out path (nested-dir exec is a sep. fs gap)
    ("execute",    "/tmp/m.elf"),
    ("ps",         ""),
    ("remove",     "/etc/passwd"),                 # destructive -> path-policy DENY
]
# GUI plan (NEMO_GUI=1): the headline SYNTHETIC-INPUT workflow -- the agent moves the
# mouse, clicks, and types into the focused window via the gated mouse/key tools. Those
# tools enqueue events into the compositor's well-known injection page, which the
# compositor drains each frame (pump_synth_input) and applies as real input. Proves
# "the agent can drive the GUI (mouse + keyboard) at the user's request".
GUI_STEPS = [
    ("mouse", "move\t180\t120"),    # move the cursor by +180,+120
    ("mouse", "click\tleft"),        # left button press + release (a click)
    ("key",   "type\tHELLO"),        # type 5 chars into the focused window
    ("mouse", "move\t-40\t30"),      # move again (proves repeated injection)
    ("badtool", "/etc/passwd"),      # bonus: an unknown tool must still DENY
]
# CONFIRM plan (NEMO_CONFIRM=1): exercises the human-in-the-loop CONFIRM gate end-to-end
# through a cockpit. The cockpit (--proof) auto-ALLOWS file ops and auto-DENIES process
# spawns. Proves: CONFIRM Allow (remove runs), CONFIRM Deny (spawn blocked), pre_snapshot +
# rollback (the removed file is restored from its snapshot and reads back as "v1").
SMALL_B64 = base64.b64encode(b"v1\n").decode()
CONFIRM_STEPS = [
    ("mkdir",      "/tmp/cptest"),
    ("write_file", "/tmp/cptest/a.txt\t" + SMALL_B64),   # snapshotted (cockpit attached)
    ("remove",     "/tmp/cptest/a.txt"),                 # CONFIRM -> ALLOW -> file removed
    ("spawn",      "sbin/tool_ps"),                       # CONFIRM -> DENY  -> not spawned
    ("rollback",   "/tmp/cptest/a.txt"),                 # CONFIRM -> ALLOW -> restored from snapshot
    ("read_file",  "/tmp/cptest/a.txt"),                 # read back -> "v1" proves rollback worked
]
if os.environ.get("NEMO_HOSTILE"):
    STEPS = HOSTILE_STEPS
elif os.environ.get("NEMO_CODETASK"):
    STEPS = CODETASK_STEPS
elif os.environ.get("NEMO_GUI"):
    STEPS = GUI_STEPS
elif os.environ.get("NEMO_CONFIRM"):
    STEPS = CONFIRM_STEPS


def log(m):
    sys.stderr.write("[nemo-mock] " + m + "\n"); sys.stderr.flush()


def recv_line(conn):
    buf = b""
    while not buf.endswith(b"\n"):
        b = conn.recv(4096)
        if not b:
            break
        buf += b
    return buf.decode("utf-8", "replace").rstrip("\r\n")


def handle(conn):
    goal = recv_line(conn)
    if not goal.startswith("GOAL"):
        log("expected GOAL, got: %r" % goal[:60]); return
    log("GOAL: " + goal[5:120])
    results = []
    for tool, args in STEPS:
        msg = json.dumps({"tool": tool, "args": args})
        conn.sendall(("TOOL " + msg + "\n").encode())
        log("-> TOOL %s %s" % (tool, args))
        r = recv_line(conn)               # "RESULT <text>"
        body = r[7:] if r.startswith("RESULT") else r
        results.append((tool, args, body))
        log("<- RESULT %s" % body[:80].replace("\n", " "))
    # Synthesize a final answer from the observations (no model).
    last = results[-1][2] if results else ""
    answer = "Listed /etc, stat'd + read /etc/toolset0.txt. Contents start: " + last[:60].replace("\n", " ")
    conn.sendall(("DONE " + answer + "\n").encode())
    log("-> DONE")


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT)); srv.listen(8)
    log("scripted agent broker on %s:%d  (steps=%d, zero cost)" % (HOST, PORT, len(STEPS)))
    while True:
        conn, _ = srv.accept()
        try:
            handle(conn)
        except Exception as e:
            log("error: %s" % e)
        finally:
            try: conn.close()
            except OSError: pass


if __name__ == "__main__":
    main()
