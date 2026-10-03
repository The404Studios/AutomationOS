"""Assertion engine: every assertion returns (ok, evidence). Evidence is the line / count / value that decided it."""
import os
import re
import subprocess

from . import pcap as pcapmod
from . import png as pngmod

OPS = {">=": lambda a, b: a >= b, ">": lambda a, b: a > b, "==": lambda a, b: a == b,
       "<=": lambda a, b: a <= b, "<": lambda a, b: a < b, "!=": lambda a, b: a != b}


def _lines(path):
    try:
        return open(path, "rb").read().decode("latin1").splitlines()
    except OSError:
        return None


def _rx(spec, key="pattern"):
    p = spec[key]
    return re.compile(p if spec.get("regex") else re.escape(p))


def _count_ok(n, spec, default_min=1):
    lo, hi = spec.get("min", default_min), spec.get("max")
    return n >= lo and (hi is None or n <= hi), "count=%d (want >=%s%s)" % (n, lo, "" if hi is None else ", <=%s" % hi)


class Ctx:
    """What assertions can see. vm label -> artifacts.

    TRUST MODEL: scenario files are repo-authored build/test recipes, exactly like a Makefile -- they are
    reviewed code, never generated from or interpolated with external input. `cmd` assertions therefore run
    through a shell on purpose (env prefixes, pipelines). Do not feed a scenario from untrusted data."""

    def __init__(self, art, root="."):
        self.art = art
        self.root = root
        self.vms = {}        # label -> {"serial": path, "pcap": path, "shots": {name: path}}
        self.actions = {}    # name -> {"exit": int, "log": path}
        self.companions = {}  # name -> log path
        self.default_vm = None

    def vm(self, spec):
        label = spec.get("vm") or self.default_vm
        return self.vms.get(label)


def kernel_fault(lines):
    """smoke_boot.sh's judgement: PANIC/triple fault, or an exception with no 'Terminating faulting process'."""
    exc = sum(1 for l in lines if "CPU EXCEPTION" in l)
    con = sum(1 for l in lines if "Terminating faulting process" in l)
    # A *report* of zero panics ("Total panics detected: 0", printed by the SMP harness) is not a panic.
    bad = [l for l in lines if re.search(r"PANIC|TRIPLE FAULT", l, re.I) and not re.search(r"panics?\s+(detected|count|seen)", l, re.I)]
    return bool(bad) or exc > con, exc, con, bad[:1]


# ---------------------------------------------------------------------------------------------- serial
def a_serial_contains(ctx, s):
    v = ctx.vm(s)
    ls = _lines(v["serial"]) if v else None
    if ls is None:
        return False, "no serial log"
    rx = _rx(s)
    hits = [l for l in ls if rx.search(l)]
    ok, why = _count_ok(len(hits), s)
    return ok, (hits[0].strip()[:160] if hits else "") + ("  [" + why + "]")


def a_serial_absent(ctx, s):
    v = ctx.vm(s)
    ls = _lines(v["serial"]) if v else None
    if ls is None:
        return False, "no serial log"
    rx = _rx(s)
    hits = [l for l in ls if rx.search(l)]
    return (not hits), ("none" if not hits else "%d match(es), first: %s" % (len(hits), hits[0].strip()[:150]))


def a_serial_order(ctx, s):
    v = ctx.vm(s)
    ls = _lines(v["serial"]) if v else None
    if ls is None:
        return False, "no serial log"
    i = 0
    for n, pat in enumerate(s["patterns"]):
        rx = re.compile(pat if s.get("regex") else re.escape(pat))
        while i < len(ls) and not rx.search(ls[i]):
            i += 1
        if i >= len(ls):
            return False, "pattern #%d never appeared after #%d: %r" % (n + 1, n, pat)
        i += 1
    return True, "all %d in order" % len(s["patterns"])


def a_serial_value(ctx, s):
    v = ctx.vm(s)
    ls = _lines(v["serial"]) if v else None
    if ls is None:
        return False, "no serial log"
    rx = re.compile(s["pattern"])
    vals = []
    for l in ls:
        m = rx.search(l)
        if m:
            try:
                vals.append(float(m.group("v")))
            except (IndexError, ValueError):
                pass
    if not vals:
        return False, "pattern never matched (need a (?P<v>NUMBER) group)"
    pick = {"first": vals[0], "last": vals[-1], "max": max(vals), "min": min(vals)}[s.get("which", "last")]
    ok = OPS[s["op"]](pick, s["value"])
    return ok, "%s=%g (want %s %g)" % (s.get("which", "last"), pick, s["op"], s["value"])


def a_no_kernel_fault(ctx, s):
    v = ctx.vm(s)
    ls = _lines(v["serial"]) if v else None
    if ls is None:
        return False, "no serial log"
    bad, exc, con, first = kernel_fault(ls)
    note = "exceptions=%d contained-ring3=%d" % (exc, con)
    return (not bad), (note + ("  first: " + first[0].strip()[:100] if first else ""))


# ---------------------------------------------------------------------------------------------- pcap
def _pcap(ctx, s):
    v = ctx.vm(s)
    return pcapmod.read(v["pcap"]) if v and v.get("pcap") else []


def a_pcap_has(ctx, s):
    recs = _pcap(ctx, s)
    if not recs:
        return False, "no packets captured (pcap missing/empty)"
    hits = [r for r in recs if pcapmod.match(r, s)]
    ok, why = _count_ok(len(hits), s)
    return ok, why + ("  e.g. %s" % _fmt(hits[0]) if hits else "")


def a_pcap_absent(ctx, s):
    recs = _pcap(ctx, s)
    hits = [r for r in recs if pcapmod.match(r, s)]
    return (not hits), ("none of %d packets match" % len(recs) if not hits else "%d match, first %s" % (len(hits), _fmt(hits[0])))


def a_pcap_absent_after(ctx, s):
    """No packet matching `s` may appear AFTER the first packet matching s["after"] ("after X, never Y")."""
    recs = _pcap(ctx, s)
    if not recs:
        return False, "no packets captured"
    anchor = next((i for i, r in enumerate(recs) if pcapmod.match(r, s["after"])), None)
    if anchor is None:
        return False, "anchor never seen: %s" % s["after"]
    spec = {k: v for k, v in s.items() if k not in ("after",)}
    late = [r for r in recs[anchor + 1:] if pcapmod.match(r, spec)]
    return (not late), ("anchor #%d %s; none after it" % (recs[anchor]["n"], _fmt(recs[anchor])) if not late
                        else "%d after anchor #%d, first %s" % (len(late), recs[anchor]["n"], _fmt(late[0])))


def a_pcap_order(ctx, s):
    recs = _pcap(ctx, s)
    if not recs:
        return False, "no packets captured"
    i = 0
    for n, spec in enumerate(s["steps"]):
        while i < len(recs) and not pcapmod.match(recs[i], spec):
            i += 1
        if i >= len(recs):
            return False, "step #%d never seen after #%d: %s" % (n + 1, n, spec)
        i += 1
    return True, "all %d steps in order" % len(s["steps"])


def _fmt(r):
    k = r.get("kind")
    if k in ("udp", "tcp", "dhcp", "dns"):
        return "%s %s:%s>%s:%s%s" % (k, r["src"], r.get("sport"), r["dst"], r.get("dport"),
                                      (" " + r["dhcp"]) if "dhcp" in r else "")
    if k == "arp":
        return "arp %s %s>%s" % (r["op"], r["spa"], r["tpa"])
    return str(r)


# ---------------------------------------------------------------------------------------------- screenshots
def a_screenshot(ctx, s):
    v = ctx.vm(s)
    path = v["shots"].get(s["name"]) if v else None
    if not path or not os.path.exists(path):
        return False, "screenshot %r was not captured" % s["name"]
    try:
        st = pngmod.stats(path)
    except Exception as e:  # noqa: BLE001
        return False, "unreadable PNG: %s" % e
    notes, ok = ["%dx%d distinct=%d luma=%.0f" % (st["width"], st["height"], st["distinct"], st["mean_luma"])], True
    if s.get("not_blank") and st["distinct"] < s.get("min_colors", 16):
        ok = False
        notes.append("BLANK (<%d colours)" % s.get("min_colors", 16))
    if "color" in s:
        f = st["fraction"](s["color"], s.get("tol", 24))
        want = s.get("min_fraction", 0.0005)
        good = f >= want and f <= s.get("max_fraction", 1.0)
        ok = ok and good
        notes.append("colour %s +-%d covers %.3f%% (want %.3f%%..%.1f%%)" % (s["color"], s.get("tol", 24), f * 100,
                                                                              want * 100, s.get("max_fraction", 1.0) * 100))
    if "golden" in s:
        g = os.path.join(ctx.art, "..", "..", "..", s["golden"]) if not os.path.isabs(s["golden"]) else s["golden"]
        if os.path.exists(g):
            diff = pngmod.compare(path, g)
            good = diff <= s.get("max_diff", 12.0)
            ok = ok and good
            notes.append("golden diff=%.1f (<=%.1f)" % (diff, s.get("max_diff", 12.0)))
        else:
            notes.append("golden missing (%s) -- not compared" % s["golden"])
    return ok, "; ".join(notes)


# ---------------------------------------------------------------------------------------------- host side
def a_cmd(ctx, s):
    try:
        r = subprocess.run(s["cmd"], shell=True, capture_output=True, text=True, timeout=s.get("timeout", 120),
                           cwd=ctx.root)
    except subprocess.TimeoutExpired:
        return False, "timed out"
    out = (r.stdout + r.stderr).strip()
    ok = r.returncode == s.get("expect_exit", 0)
    if ok and "stdout_matches" in s:
        ok = re.search(s["stdout_matches"], out) is not None
    return ok, "exit=%d %s" % (r.returncode, out[-140:].replace("\n", " | "))


def a_file(ctx, s):
    p = s["path"] if os.path.isabs(s["path"]) else os.path.join(ctx.root, s["path"])
    if not os.path.exists(p):
        return False, "missing: " + p
    sz = os.path.getsize(p)
    return sz >= s.get("min_size", 1), "size=%d" % sz


def a_action_exit(ctx, s):
    a = ctx.actions.get(s["name"])
    if a is None:
        return False, "action %r never ran" % s["name"]
    ok = a["exit"] == s.get("expect_exit", 0)
    tail = ""
    try:
        tail = open(a["log"], "rb").read().decode("latin1").strip().splitlines()[-1][:120]
    except (OSError, IndexError):
        pass
    return ok, "exit=%s  %s" % (a["exit"], tail)


def a_action_log(ctx, s):
    a = ctx.actions.get(s["name"])
    if a is None:
        return False, "action %r never ran" % s["name"]
    ls = _lines(a["log"]) or []
    rx = _rx(s)
    hits = [l for l in ls if rx.search(l)]
    ok, why = _count_ok(len(hits), s)
    return ok, (hits[0].strip()[:150] if hits else "") + "  [" + why + "]"


def a_build_step(ctx, s):  # filled by the runner (implicit); here only for completeness
    return True, ""


TYPES = {
    "serial_contains": a_serial_contains, "serial_absent": a_serial_absent, "serial_order": a_serial_order,
    "serial_value": a_serial_value, "no_kernel_fault": a_no_kernel_fault,
    "pcap_has": a_pcap_has, "pcap_absent": a_pcap_absent, "pcap_order": a_pcap_order,
    "pcap_absent_after": a_pcap_absent_after,
    "screenshot": a_screenshot, "cmd": a_cmd, "file": a_file, "action_exit": a_action_exit, "action_log": a_action_log,
}
