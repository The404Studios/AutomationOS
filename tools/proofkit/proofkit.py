#!/usr/bin/env python3
"""proofkit -- run declarative proof scenarios.

  python3 tools/proofkit/proofkit.py list
  python3 tools/proofkit/proofkit.py run  dns_lease                      # build + boot + assert
  python3 tools/proofkit/proofkit.py run  dns_lease --no-build           # reuse the images
  python3 tools/proofkit/proofkit.py eval dns_lease                      # re-judge saved artifacts (no boot)
  python3 tools/proofkit/proofkit.py run  dns_lease --expect-fail dns.follows_lease
        # red-before-green: exit 0 ONLY if that assertion fails (and everything else passes)

Exit status: 0 = the scenario's claims hold (or, with --expect-fail, the targeted ones are provably red).
Run it inside WSL (Arch): it drives qemu-system-x86_64, bash and python3 only.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pk import engine  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SCEN = os.path.join(HERE, "scenarios")
ICON = {"PASS": "PASS", "FAIL": "FAIL", "WARN": "WARN", "SKIP": "skip"}


def find(name):
    for cand in (name, os.path.join(SCEN, name), os.path.join(SCEN, name + ".json")):
        if os.path.isfile(cand):
            return cand
    sys.exit("no such scenario: %s (try: proofkit.py list)" % name)


def report(run, expect_fail):
    res, w = run.results, 0
    w = max([len(r["id"]) for r in res] + [8])
    print("\n=== proofkit: %s -- %s" % (run.name, run.sc.get("description", "")))
    stage = None
    for r in res:
        if r["stage"] != stage:
            stage = r["stage"]
            print("\n  [%s]" % (stage or "-"))
        flag = " (EXPECTED red)" if r["id"] in expect_fail and r["status"] == "FAIL" else ""
        print("   %-4s  %-*s  %s%s" % (ICON[r["status"]], w, r["id"], r["evidence"], flag))
        if r["status"] in ("FAIL", "WARN") and r.get("why"):
            print("         %-*s  why it matters: %s" % (w, "", r["why"]))
    first = next((r for r in res if r["status"] == "FAIL" and r["id"] not in expect_fail), None)
    npass = sum(r["status"] == "PASS" for r in res)
    nfail = sum(r["status"] == "FAIL" for r in res)
    nskip = sum(r["status"] == "SKIP" for r in res)
    nwarn = sum(r["status"] == "WARN" for r in res)
    print("\n  passed=%d failed=%d warned=%d skipped=%d   artifacts: %s" % (npass, nfail, nwarn, nskip, run.art))
    if first:
        print("  FIRST FAILING STAGE (root cause candidate): %s -- %s" % (first["id"], first["evidence"]))
    json.dump({"scenario": run.name, "results": res}, open(os.path.join(run.art, "report.json"), "w"), indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    for c in ("run", "eval"):
        p = sub.add_parser(c)
        p.add_argument("scenario")
        p.add_argument("--no-build", action="store_true")
        p.add_argument("--no-vm", action="store_true")
        p.add_argument("--expect-fail", action="append", default=[], metavar="ID")
    a = ap.parse_args()
    if a.cmd == "list":
        for f in sorted(os.listdir(SCEN)):
            if f.endswith(".json"):
                d = json.load(open(os.path.join(SCEN, f)))
                print("%-22s %s" % (f[:-5], d.get("description", "")))
        return 0
    run = engine.run_scenario(find(a.scenario), ROOT, {"no_build": a.no_build, "no_vm": a.no_vm, "eval_only": a.cmd == "eval"})
    report(run, set(a.expect_fail))
    st = {r["id"]: r["status"] for r in run.results}
    if a.expect_fail:
        ok = all(st.get(i) == "FAIL" for i in a.expect_fail) and \
            all(s in ("PASS", "WARN", "SKIP") for i, s in st.items() if i not in a.expect_fail and not i.startswith("build."))
        print("  --expect-fail: %s" % ("red as expected -- the test CAN catch the bug" if ok else "NOT as expected"))
        return 0 if ok else 1
    bad = [i for i, s in st.items() if s == "FAIL"]
    print("  PROOF: %s" % ("PASS" if not bad else "FAIL (%s)" % ", ".join(bad)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
