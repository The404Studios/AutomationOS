#!/usr/bin/env python3
# claude_broker.py -- host-side Claude broker for AutomationOS (CLAUDE-API-0).
# ============================================================================
# The OS (guest) reaches this over the proven MODEL-BRIDGE slirp seam:
#
#   guest sbin/claudehost  --TCP 10.0.2.2:PORT-->  this broker (host loopback)
#                                              --HTTPS--> api.anthropic.com
#
# The Anthropic API key lives ONLY here, from the host env ANTHROPIC_API_KEY.
# It NEVER enters the OS image, the initrd, or the repo. The broker also lets
# us prove the FULL OS<->broker plumbing with NO key + NO cost: with the key
# unset it returns a clear "no key" line that the OS prints, so the only thing
# separating the plumbing test from a real Claude reply is `export ...KEY=...`.
#
# Wire protocol (one request per TCP connection, matches modelbridge's seam):
#   <-  one line: the prompt text (optional "CLAUDE " verb prefix), newline-terminated
#   ->  Claude's reply text (may be multi-line), then close.
#
# Run on the host (WSL):
#   export ANTHROPIC_API_KEY=sk-ant-...        # only for a real (billed) call
#   python3 scripts/claude_broker.py           # listens on 127.0.0.1:8432
# Tunables (env): CLAUDE_BROKER_PORT, CLAUDE_MODEL, CLAUDE_MAX_TOKENS.
import socket, sys, os, json, http.client

PORT   = int(os.environ.get("CLAUDE_BROKER_PORT", sys.argv[1] if len(sys.argv) > 1 else 8432))
MODEL  = os.environ.get("CLAUDE_MODEL", "claude-haiku-4-5-20251001")  # fast + cheap for the demo
MAXTOK = int(os.environ.get("CLAUDE_MAX_TOKENS", "512"))
KEY    = os.environ.get("ANTHROPIC_API_KEY", "")


def ask_claude(prompt: str) -> str:
    if not KEY:
        return ("[broker] no ANTHROPIC_API_KEY set on the host -- plumbing OK, "
                "but cannot call Claude. Run: export ANTHROPIC_API_KEY=sk-ant-... "
                "then restart this broker.")
    body = json.dumps({
        "model": MODEL,
        "max_tokens": MAXTOK,
        "messages": [{"role": "user", "content": prompt}],
    })
    try:
        conn = http.client.HTTPSConnection("api.anthropic.com", 443, timeout=60)
        conn.request("POST", "/v1/messages", body, {
            "x-api-key": KEY,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        })
        r = conn.getresponse()
        raw = r.read().decode("utf-8", "replace")
        conn.close()
    except Exception as e:
        return f"[broker] transport error to api.anthropic.com: {e}"
    if r.status != 200:
        return f"[broker] Anthropic HTTP {r.status}: {raw[:500]}"
    try:
        data = json.loads(raw)
        text = "".join(b.get("text", "") for b in data.get("content", [])
                        if b.get("type") == "text").strip()
        usage = data.get("usage", {})
        return text + (f"\n[broker] model={MODEL} in={usage.get('input_tokens','?')} "
                       f"out={usage.get('output_tokens','?')} tokens") if text else "[broker] empty reply"
    except Exception as e:
        return f"[broker] response parse error: {e}: {raw[:300]}"


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(8)
    print(f"[claude-broker] listening 127.0.0.1:{PORT} model={MODEL} "
          f"key={'SET' if KEY else 'MISSING'}", flush=True)
    while True:
        conn, _ = srv.accept()
        try:
            conn.settimeout(70.0)
            data = b""
            while b"\n" not in data and len(data) < 16384:
                chunk = conn.recv(2048)
                if not chunk:
                    break
                data += chunk
            prompt = data.split(b"\n", 1)[0].decode("utf-8", "replace").strip()
            if prompt.startswith("CLAUDE "):
                prompt = prompt[7:]
            print(f"[claude-broker] prompt={prompt!r}", flush=True)
            reply = ask_claude(prompt)
            print(f"[claude-broker] reply[{len(reply)}]={reply[:140]!r}", flush=True)
            conn.sendall(reply.encode("utf-8"))
        except Exception as e:
            try:
                conn.sendall(f"[broker] error: {e}".encode("utf-8"))
            except Exception:
                pass
            print(f"[claude-broker] error: {e}", flush=True)
        finally:
            conn.close()


if __name__ == "__main__":
    main()
