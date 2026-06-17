#!/usr/bin/env node
/*
 * nemotron_broker.js -- the FREE Nemotron agent brain for AutomationOS, via Puter.
 * ==============================================================================
 *
 * Puter (https://developer.puter.com) gives keyless, free, "unlimited" access to
 * NVIDIA Nemotron models from JavaScript -- no API key, your Puter session pays.
 * This host-side broker bridges that to the OS's agentic rail over the QEMU
 * slirp seam, exactly like scripts/claude_broker.py bridges Claude. YOU run it
 * on your machine (the OS connects out to 10.0.2.2:8433):
 *
 *     npm i @heyputer/puter.js
 *     node scripts/nemotron_broker.js
 *
 * Puter's chat is text-in/text-out with NO native tool-calling, so the broker
 * drives a PROMPTED strict-JSON ReAct loop -- which is exactly what the OS rail
 * (modelbridge / sbin/agentd) already expects. The multi-step wire protocol the
 * OS drives over one persistent TCP connection:
 *
 *     OS  -> broker:  "GOAL <natural-language request>\n"
 *     broker -> OS:   "TOOL {\"tool\":\"<name>\",\"args\":\"<args>\"}\n"
 *                     -- or -- "DONE <final answer>\n"
 *     OS  -> broker:  "RESULT <tool stdout / exit>\n"
 *     ... loop until DONE.
 *
 * Every byte the model emits is HOSTILE TEXT -- the OS gates every TOOL through
 * its policy engine (allow/confirm/deny + audit + rollback) before executing.
 * This broker only PLANS; the OS decides what actually runs.
 *
 * Env:
 *   NEMO_MODEL  Nemotron model id (default: nvidia/nemotron-3-ultra-550b-a55b).
 *               Free ids end in ":free" (e.g. nvidia/nemotron-nano-9b-v2:free,
 *               nvidia/nemotron-3-super-120b-a12b:free). The ultra reasoning
 *               model is the strongest planner.
 *   NEMO_PORT   listen port (default 8433; slirp maps guest 10.0.2.2:8433 here).
 */
const net = require("net");

const PORT  = parseInt(process.env.NEMO_PORT || "8433", 10);
const MODEL = process.env.NEMO_MODEL || "nvidia/nemotron-3-ultra-550b-a55b";
const MAX_STEPS = parseInt(process.env.NEMO_MAX_STEPS || "16", 10);

// The toolset advertised to the model. The OS gates + may reject any of these;
// an unsupported/denied tool comes back as a RESULT the model can react to.
const TOOLS = [
  ["read_file", "<path>",                 "read a text file"],
  ["list_dir",  "<path>",                 "list a directory"],
  ["stat",      "<path>",                 "file metadata"],
  ["write_file","<path>\\t<base64 content>", "write/overwrite a file (gated)"],
  ["compile",   "<src.c>\\t<out.elf>",    "compile C with the on-device cc"],
  ["execute",   "<path>",                 "run a program (gated)"],
  ["shell",     "<command line>",         "run a /bin shell command (gated)"],
  ["spawn",     "<sbin/app>",             "launch a desktop app"],
  ["mkdir",     "<path>",                 "make a directory (gated)"],
  ["move",      "<src>\\t<dst>",          "move/rename (gated)"],
  ["remove",    "<path>",                 "delete (gated, snapshotted)"],
  ["mouse",     "<x> <y> [click]",        "move/click the cursor (gated)"],
  ["key",       "<text>",                 "type text into the focused window (gated)"],
];

function systemPrompt() {
  const list = TOOLS.map(t => `  - ${t[0]} ${t[1]}  : ${t[2]}`).join("\n");
  return [
    "You are the automation agent for AutomationOS, a from-scratch operating system.",
    "You accomplish the user's GOAL by calling ONE tool at a time and observing the RESULT.",
    "The OS gates every tool through a safety policy (it may deny or require user confirm).",
    "",
    "Tools:",
    list,
    "",
    "Respond with EXACTLY ONE JSON object and NOTHING else, on a single line:",
    '  to call a tool:  {"tool":"<name>","args":"<args>"}',
    '  when finished:   {"done":"<a short final answer for the user>"}',
    "Use \\t to separate multiple args (e.g. write_file path\\tBASE64). Keep going",
    "until the goal is met, then return done. Never invent a RESULT; wait for it.",
  ].join("\n");
}

// --- Puter model call (lazy-loaded so the file parses even without the dep) ---
let puter = null;
async function chat(prompt) {
  if (!puter) {
    try { puter = require("@heyputer/puter.js"); }
    catch (e) {
      throw new Error("@heyputer/puter.js not installed -- run: npm i @heyputer/puter.js");
    }
  }
  const r = await puter.ai.chat(prompt, { model: MODEL });
  // Puter returns either a string or an object; extract the text robustly.
  if (typeof r === "string") return r;
  if (r && r.message && typeof r.message.content === "string") return r.message.content;
  if (r && typeof r.text === "string") return r.text;
  if (r && r.message && Array.isArray(r.message.content))
    return r.message.content.map(c => c.text || "").join("");
  return JSON.stringify(r);
}

// Extract the first {...} JSON object from a model reply (tolerates stray prose).
function parseAction(text) {
  const s = text.indexOf("{");
  if (s < 0) return null;
  let depth = 0;
  for (let i = s; i < text.length; i++) {
    if (text[i] === "{") depth++;
    else if (text[i] === "}") { depth--; if (depth === 0) {
      try { return JSON.parse(text.slice(s, i + 1)); } catch (_) { return null; }
    } }
  }
  return null;
}

function recvLine(sock, cb) {
  let buf = "";
  const onData = d => {
    buf += d.toString("utf8");
    const nl = buf.indexOf("\n");
    if (nl >= 0) { sock.removeListener("data", onData); cb(buf.slice(0, nl).replace(/\r$/, "")); }
  };
  sock.on("data", onData);
}

async function runSession(sock) {
  recvLine(sock, async (first) => {
    if (!first.startsWith("GOAL")) { sock.end(); return; }
    const goal = first.slice(5);
    log("GOAL: " + goal.slice(0, 120));
    // The running transcript re-sent to the (stateless) model each turn.
    let transcript = systemPrompt() + "\n\nGOAL: " + goal + "\n";
    for (let step = 0; step < MAX_STEPS; step++) {
      let reply;
      try { reply = await chat(transcript + "\nYour single-line JSON action:"); }
      catch (e) { sock.write("DONE [broker error] " + e.message + "\n"); sock.end(); return; }
      const act = parseAction(reply);
      if (!act) { transcript += "\n(your last reply was not valid JSON; reply with one JSON object)\n"; continue; }
      if (act.done !== undefined) {
        log("DONE after " + step + " steps");
        sock.write("DONE " + String(act.done).replace(/\n/g, " ") + "\n"); sock.end(); return;
      }
      const tool = String(act.tool || ""), args = String(act.args || "");
      transcript += `\nACTION {"tool":"${tool}","args":"${args}"}\n`;
      log(`-> TOOL ${tool} ${args.slice(0, 80)}`);
      sock.write("TOOL " + JSON.stringify({ tool, args }) + "\n");
      // wait for the OS's RESULT, then loop
      const result = await new Promise(res => recvLine(sock, res));
      const body = result.startsWith("RESULT") ? result.slice(7) : result;
      transcript += "RESULT " + body + "\n";
      log("<- RESULT " + body.slice(0, 80).replace(/\n/g, " "));
    }
    sock.write("DONE [reached step limit]\n"); sock.end();
  });
}

function log(m) { process.stderr.write("[nemo] " + m + "\n"); }

const srv = net.createServer(sock => {
  runSession(sock).catch(e => { try { sock.end(); } catch (_) {} log("session error: " + e); });
});
srv.listen(PORT, "127.0.0.1", () =>
  log(`Nemotron agent broker (Puter, model=${MODEL}) on 127.0.0.1:${PORT} -- slirp 10.0.2.2:${PORT}`));
