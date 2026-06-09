#!/usr/bin/env python3
# model_server_stub.py -- MODEL-BRIDGE-0: the EXTERNAL model endpoint, first pass.
#
# This is the host-side stand-in for the chainlayer2 brain (llama.cpp/GGUF
# later). It speaks the MODEL-BRIDGE-0 wire protocol: one request per TCP
# connection, one text line in, one text line out, then close.
#
#   guest (sbin/modelbridge) --TCP 10.0.2.2:8431--> this server (host loopback)
#
# Requests (newline-terminated):
#   SELECT <prompt>        -> the model's tool-selection JSON (one line)
#   ANSWER <observation>   -> the model's final answer (one line)
#
# It is DETERMINISTIC and SCRIPTED: specific prompts return specific canned
# model text, including deliberately HOSTILE outputs (unparseable text, a
# non-whitelisted tool) so the in-OS host gate can be proven to reject them.
# The bridge must treat every byte from here as untrusted -- swapping in a
# real llama.cpp server later changes NOTHING on the OS side.
import socket, sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8431

CANON_PROMPT = "What is inside /etc/toolset0.txt?"

def respond(line: str) -> str:
    if line == "SELECT " + CANON_PROMPT:
        # the "model" picks the right tool for the canonical prompt
        return '{"tool":"read_file","path":"/etc/toolset0.txt"}'
    if line == "SELECT __malformed__":
        # hostile: chatty, unparseable model output -> must die at the parser
        return 'Sure! I will read it: {"tool":"read_file","path":'
    if line == "SELECT __badtool__":
        # hostile: valid shape, non-whitelisted tool -> must die at the whitelist
        return '{"tool":"delete_file","path":"/etc/toolset0.txt"}'
    if line == "ANSWER TOOLSET-0-FILE":
        # the "model" turns the observation into the final answer
        return "TOOLSET-0-FILE"
    return "ERR unknown_request"

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(8)
print(f"[model-stub] listening on 127.0.0.1:{PORT}", flush=True)
while True:
    conn, _ = srv.accept()
    try:
        conn.settimeout(10.0)
        data = b""
        while b"\n" not in data and len(data) < 512:
            chunk = conn.recv(256)
            if not chunk:
                break
            data += chunk
        line = data.split(b"\n", 1)[0].decode("ascii", "replace").rstrip("\r")
        reply = respond(line)
        print(f"[model-stub] {line!r} -> {reply!r}", flush=True)
        conn.sendall(reply.encode("ascii") + b"\n")
    except Exception as e:
        print(f"[model-stub] error: {e}", flush=True)
    finally:
        conn.close()
