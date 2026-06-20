#!/usr/bin/env python3
# deepseek_broker.py -- host-side LIVE model brain for AutomationOS.
# ============================================================================
# Provider-agnostic: speaks any OpenAI-compatible /chat/completions endpoint.
# Default = DeepSeek (cheap, fast); set MODEL_* to use a LOCAL keyless server
# (Ollama/llama.cpp/vLLM -- truly free, no key, no login) or OpenRouter, etc.
#
# Drop-in live replacement for scripts/nemotron_mock.py: it speaks the EXACT
# same agentic wire protocol that sbin/agentd drives over the QEMU slirp seam,
# but instead of a scripted plan it asks a real model what to do next.
#
#   guest sbin/agentd  --TCP 10.0.2.2:8433-->  this broker (host loopback)
#                                          --HTTPS--> api.deepseek.com
#
# The DeepSeek key lives ONLY here, read from the host env DEEPSEEK_API_KEY. It
# NEVER enters the OS image, the initrd, or the repo. With the key unset the
# broker still proves the full OS<->broker plumbing (it returns a clear no-key
# DONE), so the only thing between the plumbing test and a real run is
# `export DEEPSEEK_API_KEY=...`.
#
# Multi-step protocol over ONE persistent TCP connection (matches agentd.c):
#     OS  -> broker:  "GOAL <natural-language request>\n"
#     broker -> OS:   "TOOL {\"tool\":\"<name>\",\"args\":\"<args>\"}\n"   (call a gated tool)
#                     -- or -- "DONE <final answer>\n"                     (task complete)
#     OS  -> broker:  "RESULT <tool stdout / status>\n"                    (after running it)
#     ... loop until DONE; one task per TCP connection.
#
# EVERY byte the model emits is HOSTILE TEXT. The broker only PLANS; the OS gates
# every TOOL through its policy engine (allow/confirm/deny + audit + rollback)
# before anything runs. A denied/unknown tool comes back as a RESULT the model
# reacts to.
#
# If the first line is NOT a GOAL it is treated as a single prompt and answered
# with one reply (so this also serves the simpler claudehost/modelbridge seam).
#
# Run on the host (WSL). One process can back agentd (8433) + the in-OS Claude
# apps (8432) + modelbridge (8431) at once via DEEPSEEK_BROKER_PORTS:
#   # DeepSeek (cheap):
#   export DEEPSEEK_API_KEY=sk-...
#   DEEPSEEK_BROKER_PORTS=8433,8432,8431 python3 scripts/deepseek_broker.py
#   # Local Ollama (FREE, no key, no login):
#   MODEL_SCHEME=http MODEL_BASE=localhost:11434 MODEL_PATH=/v1/chat/completions \
#     MODEL_NAME=llama3.2 DEEPSEEK_BROKER_PORTS=8433,8432,8431 python3 scripts/deepseek_broker.py
# Tunables (env, MODEL_* preferred; DEEPSEEK_* still honored): MODEL_NAME,
#   MODEL_BASE, MODEL_SCHEME(https|http), MODEL_PATH, MODEL_API_KEY, MODEL_NO_AUTH,
#   MODEL_MAX_STEPS/MAX_TOKENS/TEMPERATURE, DEEPSEEK_BROKER_PORT(S).
import socket, sys, os, json, http.client, threading

PORT    = int(os.environ.get("DEEPSEEK_BROKER_PORT", sys.argv[1] if len(sys.argv) > 1 else 8433))
# Serve several seams from one process: 8433 = agentd (agentic GOAL/TOOL loop),
# 8432 = the in-OS Claude apps (claudehost/claudechat/anthropic, single prompt),
# 8431 = modelbridge. handle() auto-detects GOAL vs plain, so one broker covers all.
PORTS   = [int(p) for p in os.environ.get("DEEPSEEK_BROKER_PORTS", str(PORT)).replace(" ", "").split(",") if p]
def _env(*names, default=""):
    for n in names:
        v = os.environ.get(n)
        if v:
            return v
    return default

# Provider-agnostic OpenAI-compatible endpoint. Defaults = DeepSeek; override the
# MODEL_* vars to point at anything that speaks /chat/completions:
#   DeepSeek (default): nothing to set but DEEPSEEK_API_KEY.
#   Local Ollama (FREE, no key, no login):
#     MODEL_SCHEME=http MODEL_BASE=localhost:11434 MODEL_PATH=/v1/chat/completions MODEL_NAME=llama3.2
#   OpenRouter: MODEL_BASE=openrouter.ai MODEL_PATH=/api/v1/chat/completions MODEL_API_KEY=...
MODEL   = _env("MODEL_NAME", "DEEPSEEK_MODEL", default="deepseek-chat")  # deepseek-chat fast; deepseek-reasoner stronger/slower
MAXTOK  = int(_env("MODEL_MAX_TOKENS", "DEEPSEEK_MAX_TOKENS", default="1024"))
MAXSTEP = int(_env("MODEL_MAX_STEPS", "DEEPSEEK_MAX_STEPS", default="16"))
TEMP    = float(_env("MODEL_TEMPERATURE", "DEEPSEEK_TEMPERATURE", default="0.2"))  # low temp = stable tool planning
BASE    = _env("MODEL_BASE", "DEEPSEEK_BASE", default="api.deepseek.com")          # host or host:port
SCHEME  = _env("MODEL_SCHEME", "DEEPSEEK_SCHEME", default="https").lower()         # https | http (http for local)
APIPATH = _env("MODEL_PATH", "DEEPSEEK_PATH", default="/chat/completions")
KEY     = _env("MODEL_API_KEY", "DEEPSEEK_API_KEY", default="")
# Local/keyless servers (Ollama, llama.cpp, vLLM) need no key -- auto-detect http/local, or force MODEL_NO_AUTH=1.
KEYLESS_OK = (SCHEME == "http") or ("localhost" in BASE) or ("127.0.0.1" in BASE) or (_env("MODEL_NO_AUTH") == "1")
# json_object response mode is a robustness nudge supported on deepseek-chat; the parser
# tolerates either way, so it auto-off for other models/providers.
JSON_MODE = _env("MODEL_JSON_MODE", "DEEPSEEK_JSON_MODE", default="1") != "0" and MODEL == "deepseek-chat"

# The toolset advertised to the model -- mirrors scripts/nemotron_broker.js so the
# plan matches exactly what the OS gated rail (sbin/agentd resolve_tool) supports.
# The OS may still deny/confirm any of these; that returns a RESULT to react to.
TOOLS = [
    ("read_file",  "<path>",                    "read a text file"),
    ("list_dir",   "<path>",                    "list a directory"),
    ("stat",       "<path>",                    "file metadata"),
    ("write_file", "<path>\\t<base64 content>", "write/overwrite a file (gated)"),
    ("compile",    "<src.c>\\t<out.elf>",       "compile C with the on-device cc"),
    ("execute",    "<path>",                    "run a program (gated)"),
    ("spawn",      "<sbin/app>",                "launch a desktop app"),
    ("mkdir",      "<path>",                    "make a directory (gated)"),
    ("move",       "<src>\\t<dst>",             "move/rename (gated)"),
    ("remove",     "<path>",                    "delete (gated, snapshotted)"),
    ("rollback",   "<path>",                    "restore a file from its pre-change snapshot (gated)"),
    ("kill",       "<pid>",                     "kill a process by pid (gated)"),
    ("ps",         "",                          "list running processes"),
    ("mouse",      "move <dx> <dy> | click <btn>", "move/click the cursor (gated)"),
    ("key",        "type <text>",               "type text into the focused window (gated)"),
]


def system_prompt() -> str:
    tlist = "\n".join("  - %s %s  : %s" % (t[0], t[1], t[2]) for t in TOOLS)
    return "\n".join([
        "You are the automation agent for AutomationOS, a from-scratch operating system.",
        "Accomplish the user's GOAL by calling ONE tool at a time and observing each RESULT.",
        "The OS gates every tool through a safety policy and may deny or require user confirm;",
        "react to whatever RESULT comes back. Never invent a RESULT -- wait for it.",
        "",
        "Tools:",
        tlist,
        "",
        "Reply with EXACTLY ONE valid json object and nothing else:",
        '  to call a tool:  {"tool":"<name>","args":"<args>"}',
        '  when finished:   {"done":"<a short final answer for the user>"}',
        "Use a literal \\t to separate multiple args (e.g. write_file path\\tBASE64).",
        "Keep going until the goal is met, then return the done object.",
    ])


def deepseek_chat(messages, force_json: bool):
    """Call an OpenAI-compatible /chat/completions endpoint. Returns (text, err)."""
    if not KEY and not KEYLESS_OK:
        return (None, "no MODEL_API_KEY/DEEPSEEK_API_KEY set on the host -- plumbing OK, but "
                      "cannot call the model. Set a key, or target a local keyless server "
                      "(MODEL_SCHEME=http MODEL_BASE=localhost:11434 MODEL_PATH=/v1/chat/completions).")
    payload = {
        "model": MODEL,
        "messages": messages,
        "max_tokens": MAXTOK,
        "temperature": TEMP,
        "stream": False,
    }
    if force_json and JSON_MODE:
        payload["response_format"] = {"type": "json_object"}
    body = json.dumps(payload)
    headers = {"Content-Type": "application/json"}
    if KEY:
        headers["Authorization"] = "Bearer " + KEY
    try:
        conn = (http.client.HTTPConnection(BASE, timeout=120) if SCHEME == "http"
                else http.client.HTTPSConnection(BASE, timeout=120))
        conn.request("POST", APIPATH, body, headers)
        r = conn.getresponse()
        raw = r.read().decode("utf-8", "replace")
        conn.close()
    except Exception as e:
        return (None, "transport error to %s://%s%s: %s" % (SCHEME, BASE, APIPATH, e))
    if r.status != 200:
        return (None, "model HTTP %d: %s" % (r.status, raw[:400]))
    try:
        data = json.loads(raw)
        text = data["choices"][0]["message"]["content"]
        u = data.get("usage", {})
        log("model=%s in=%s out=%s tok" % (MODEL, u.get("prompt_tokens", "?"),
                                           u.get("completion_tokens", "?")))
        return (text.strip() if text else "", None)
    except Exception as e:
        return (None, "response parse error: %s: %s" % (e, raw[:300]))


def parse_action(text):
    """Extract the first {...} JSON object from a model reply (tolerates fences/prose)."""
    if not text:
        return None
    try:
        return json.loads(text)
    except Exception:
        pass
    s = text.find("{")
    if s < 0:
        return None
    depth = 0
    for i in range(s, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                try:
                    return json.loads(text[s:i + 1])
                except Exception:
                    return None
    return None


def recv_line(conn):
    buf = b""
    while not buf.endswith(b"\n"):
        b = conn.recv(4096)
        if not b:
            break
        buf += b
    return buf.decode("utf-8", "replace").rstrip("\r\n")


def run_agent_session(conn, goal):
    log("GOAL: " + goal[:120])
    messages = [
        {"role": "system", "content": system_prompt()},
        {"role": "user",   "content": "GOAL: " + goal},
    ]
    for step in range(MAXSTEP):
        text, err = deepseek_chat(messages, force_json=True)
        if err:
            conn.sendall(("DONE [broker] " + err + "\n").encode()); return
        act = parse_action(text)
        if act is None:
            messages.append({"role": "assistant", "content": text or ""})
            messages.append({"role": "user",
                             "content": "Your last reply was not a single JSON object. "
                                        "Reply with exactly one JSON action object."})
            continue
        if "done" in act:
            ans = str(act.get("done", "")).replace("\n", " ")
            log("DONE after %d step(s)" % step)
            conn.sendall(("DONE " + ans + "\n").encode()); return
        tool = str(act.get("tool", "")); args = str(act.get("args", ""))
        messages.append({"role": "assistant",
                         "content": json.dumps({"tool": tool, "args": args})})
        log("-> TOOL %s %s" % (tool, args[:80]))
        conn.sendall(("TOOL " + json.dumps({"tool": tool, "args": args}) + "\n").encode())
        result = recv_line(conn)            # "RESULT <text>" (one line; agentd collapses newlines)
        body = result[7:] if result.startswith("RESULT") else result
        log("<- RESULT %s" % body[:80].replace("\n", " "))
        messages.append({"role": "user", "content": "RESULT " + body})
    conn.sendall("DONE [reached step limit]\n".encode())


def run_single_prompt(conn, prompt):
    """Fallback for the simpler claudehost/modelbridge seam: one prompt -> one reply."""
    for pfx in ("CLAUDE ", "MODEL ", "DEEPSEEK "):
        if prompt.startswith(pfx):
            prompt = prompt[len(pfx):]
    log("single-prompt: %r" % prompt[:120])
    text, err = deepseek_chat(
        [{"role": "user", "content": prompt}], force_json=False)
    conn.sendall((("[broker] " + err) if err else (text or "[broker] empty reply")).encode())


def handle(conn):
    first = recv_line(conn)
    if first.startswith("GOAL"):
        run_agent_session(conn, first[5:] if len(first) > 4 else "")
    elif first:
        run_single_prompt(conn, first)


def log(m):
    sys.stderr.write("[deepseek-broker] " + m + "\n"); sys.stderr.flush()


def serve_port(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port)); srv.listen(8)
    log("listening 127.0.0.1:%d  (slirp 10.0.2.2:%d)" % (port, port))
    while True:
        conn, _ = srv.accept()
        try:
            conn.settimeout(180.0)
            handle(conn)
        except Exception as e:
            try:
                conn.sendall(("DONE [broker] error: %s\n" % e).encode())
            except Exception:
                pass
            log("error: %s" % e)
        finally:
            try:
                conn.close()
            except OSError:
                pass


def main():
    log("LIVE model broker  endpoint=%s://%s%s  model=%s  auth=%s  json_mode=%s  ports=%s"
        % (SCHEME, BASE, APIPATH, MODEL,
           "KEY" if KEY else ("KEYLESS" if KEYLESS_OK else "MISSING"),
           JSON_MODE, ",".join(str(p) for p in PORTS)))
    # One accept-loop thread per port; serve from this process so a single broker
    # backs agentd (8433) and the in-OS Claude apps (8432/8431) at once.
    threads = [threading.Thread(target=serve_port, args=(p,), daemon=True) for p in PORTS]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


if __name__ == "__main__":
    main()
