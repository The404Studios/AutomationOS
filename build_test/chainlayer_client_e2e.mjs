// chainlayer_client_e2e.mjs -- ChainLayerTwo's OWN MCP client driving AutomationOS through the bridge.
//
//   ChainLayerTwo (chainlayercli/src/mcp.ts: connectMcpServer, the exact code the CLI uses)
//        --Streamable-HTTP + Bearer-->  scripts/chainlayer_mcp_bridge.py
//        --TCP via QEMU slirp-->        sbin/agentd (guest, serve mode)  -->  gated tools  -->  KERNEL
//
// It does what "ChainLayerTwo creates software on the OS" means, in order, and asserts every step from the
// OUTSIDE (tool results), never from the bridge's own claims:
//   1. connect + discover tools            (ChainLayer's tool wrapper builds Tool objects from tools/list)
//   2. read kernel state                   (fw_status = the kernel packet filter)
//   3. CREATE A FILE                       (write_file, then read it back byte-for-byte)
//   4. CREATE SOFTWARE                     (write C source -> compile ON THE DEVICE -> execute -> exit code)
//   5. the OS-side gates still hold        (protected path / traversal refused by the OS, not by this script)
//
// Run from chainlayercli so `tsx` and the MCP SDK resolve (the harness build_test/chainlayer_e2e.sh does this):
//   BRIDGE_URL=http://127.0.0.1:8940/mcp BRIDGE_TOKEN=... CHAINLAYER_ROOT=<...>/chainlayercli \
//     node --import tsx build_test/chainlayer_client_e2e.mjs
//
// It never reads or writes ~/.chainlayer, never touches the EULA gate (that lives in the CLI entry point, not in the
// client library), and never spawns the MCP servers imported from other tools.
import { pathToFileURL } from "node:url";
import path from "node:path";

const URL_ = process.env.BRIDGE_URL;
const TOKEN = process.env.BRIDGE_TOKEN;
const ROOT = process.env.CHAINLAYER_ROOT;
if (!URL_ || !TOKEN || !ROOT) {
  console.error("need BRIDGE_URL, BRIDGE_TOKEN and CHAINLAYER_ROOT (path to chainlayercli)");
  process.exit(2);
}

const fails = [];
const check = (name, cond, detail = "") => {
  console.log((cond ? "  ok   " : "  FAIL ") + name + (!cond && detail ? " -- " + String(detail).slice(0, 220) : ""));
  if (!cond) fails.push(name);
};

const { connectMcpServer } = await import(pathToFileURL(path.join(ROOT, "src", "mcp.ts")).href);

console.log("ChainLayerTwo MCP client -> " + URL_);
const conn = await connectMcpServer({ name: "automationos", url: URL_, token: TOKEN, callTimeoutMs: 0 }, {});
const byName = new Map(conn.tools.map((t) => [t.name, t]));
const ctx = { cwd: process.cwd() };

async function call(name, args) {
  const t = byName.get(name);
  if (!t) return { ok: false, text: "tool not exposed: " + name };
  const r = await t.run(args, ctx);
  const text = typeof r.content === "string" ? r.content : JSON.stringify(r.content ?? r);
  return { ok: !!r.ok, text };
}

try {
  const names = [...byName.keys()];
  check("ChainLayerTwo discovered the OS tool rail", ["read_file", "write_file", "compile", "execute", "fw_status"].every((n) => byName.has(n)), names.join(","));
  check("ChainLayerTwo classifies every discovered OS tool as mutating (its default) -> its approval gate applies",
        conn.tools.every((t) => t.mutating === true && t.source === "mcp"));
  check("control-tier tools are NOT exposed by default (remove/spawn/kill/mouse/key/firewall)",
        !["remove", "spawn", "kill", "mouse", "key", "firewall"].some((n) => byName.has(n)) || process.env.ALLOW_CONTROL === "1", names.join(","));

  // wait for the guest's agentd to dial in (serve mode)
  let up = false;
  for (let i = 0; i < 60 && !up; i++) {
    const r = await call("ps", {});
    up = r.ok;
    if (!up) await new Promise((res) => setTimeout(res, 3000));
  }
  check("the guest OS is attached (agentd serve mode dialled the bridge)", up, "timed out waiting for the OS");
  if (!up) throw new Error("no OS session");

  // 2. kernel state
  let r = await call("fw_status", {});
  check("kernel packet filter readable through ChainLayerTwo", r.ok && /FW enabled=1/.test(r.text) && /policy_in=drop/.test(r.text), r.text);

  // 3. create a file, read it back
  const stamp = "chainlayer-wrote-this-" + Date.now() + "\n";
  r = await call("write_file", { path: "/tmp/cl_note.txt", content: stamp });
  check("ChainLayerTwo created /tmp/cl_note.txt on the OS", r.ok, r.text);
  r = await call("read_file", { path: "/tmp/cl_note.txt" });
  check("...and read it back byte-for-byte", r.ok && r.text.includes(stamp.trim()), r.text);
  r = await call("mkdir", { path: "/tmp/clproj" });
  check("ChainLayerTwo created a project directory", r.ok, r.text);

  // 4. create software: fib(10) = 55, returned as the process exit code so the result travels back through the rail.
  //    (the on-device compiler is a C subset: no string literals / arrays / globals -- so the answer is the exit code)
  const src = [
    "int main() {",
    "  long a; long b; long t; long i;",
    "  a = 0; b = 1; i = 0;",
    "  while (i < 10) { t = a + b; a = b; b = t; i = i + 1; }",
    "  return a;",
    "}",
    "",
  ].join("\n");
  r = await call("write_file", { path: "/tmp/clproj/fib.c", content: src });
  check("ChainLayerTwo wrote a C program (/tmp/clproj/fib.c)", r.ok, r.text);
  r = await call("compile", { source: "/tmp/clproj/fib.c", output: "/tmp/clproj/fib.elf" });
  check("ChainLayerTwo compiled it ON THE OS (the device's own cc)", r.ok, r.text);
  r = await call("execute", { path: "/tmp/clproj/fib.elf" });
  check("ChainLayerTwo ran the software it just made; fib(10) came back as exit=55", r.ok && /exit=55\b/.test(r.text), r.text);
  r = await call("list_dir", { path: "/tmp/clproj" });
  check("the project's files exist on the OS", r.ok && r.text.includes("fib.c") && r.text.includes("fib.elf"), r.text);

  // 5. the OS-side gates (these decisions are made by the guest, not by this script or the bridge)
  r = await call("write_file", { path: "/etc/cl_evil.txt", content: "x" });
  check("writing under protected /etc is refused by the OS", !r.ok || /DENY|denied/i.test(r.text), r.text);
  r = await call("read_file", { path: "/etc/../boot/grub.cfg" });
  check("path traversal is refused by the OS", !r.ok && /denied/i.test(r.text), r.text);
} catch (e) {
  check("no exception", false, e && e.message);
} finally {
  await conn.close().catch(() => {});
}

console.log("CHAINLAYER-E2E: " + (fails.length ? "FAIL (" + fails.length + ": " + fails.join("; ") + ")" : "PASS"));
process.exit(fails.length ? 1 : 0);
