#!/usr/bin/env python3
"""res_corpus_report.py -- summarise the TSV produced by res_scan (see tests/win/res_corpus.sh).

usage: res_corpus_report.py corpus.tsv [--rejects]

Columns (res_scan): path size parse dll resdir enum_rc n_res man_rc well_formed elevation ui_access n_deps deps os_mask
dpi longpath codepage ver_rc file_version description man_size man_enc nelem
"""
import collections
import sys

EXEC = {0: "none/absent", 1: "asInvoker", 2: "requireAdministrator", 3: "highestAvailable", 4: "unknown"}
DPI = {0: "unspecified", 1: "unaware", 2: "system", 3: "per-monitor", 4: "per-monitor-v2"}
OS = [(1, "Vista"), (2, "Win7"), (4, "Win8"), (8, "Win8.1"), (16, "Win10/11")]


def main():
    path = sys.argv[1]
    show_rejects = "--rejects" in sys.argv
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        rows.append(f)
    total = len(rows)
    parse_fail = collections.Counter()
    ok = []
    for f in rows:
        if len(f) < 4 or f[2] != "ok":
            parse_fail[f[2] if len(f) > 2 else "short-line"] += 1
        else:
            ok.append(f)
    print("files scanned                 : %d" % total)
    print("rejected by pe_parse (not PE32+/x64, managed, odd layout): %d" % sum(parse_fail.values()))
    for k, n in parse_fail.most_common():
        print("    %4d  %s" % (n, k))
    print("parsed + mapped (analysed)    : %d" % len(ok))

    def col(f, i, default=""):
        return f[i] if i < len(f) else default

    nodir = [f for f in ok if col(f, 4) == "none"]
    baddir = [f for f in ok if col(f, 4) not in ("ok", "none")]
    enum_bad = [f for f in ok if col(f, 5) not in ("ok", "none")]
    print("no resource directory         : %d" % len(nodir))
    print("resource directory rejected   : %d" % len(baddir))
    print("resource enumeration errors   : %d" % len(enum_bad))
    tot_res = sum(int(col(f, 6, "0") or 0) for f in ok)
    print("resource leaves enumerated    : %d" % tot_res)

    man_none = [f for f in ok if col(f, 7) == "none"]
    man_ok = [f for f in ok if col(f, 7) == "ok"]
    man_err = [f for f in ok if col(f, 7) not in ("none", "ok")]
    print("manifest: none                : %d" % len(man_none))
    print("manifest: present             : %d" % (len(man_ok) + len(man_err)))
    print("manifest: parsed OK           : %d" % len(man_ok))
    print("manifest: well_formed == 1    : %d" % sum(1 for f in ok if col(f, 8) == "1"))
    print("manifest: rejected            : %d" % len(man_err))
    classes = collections.Counter(col(f, 7) for f in man_err)
    for k, n in classes.most_common():
        print("    %4d  %s" % (n, k))
    if show_rejects:
        for f in man_err:
            print("    REJECT %s  [%s] size=%s enc=%s nelem=%s" % (f[0], col(f, 7), col(f, 20), col(f, 21), col(f, 22)))

    lv = collections.Counter(EXEC.get(int(col(f, 9, "0") or 0), "?") for f in man_ok)
    print("requestedExecutionLevel (parsed manifests):")
    for k, n in lv.most_common():
        print("    %4d  %s" % (n, k))
    print("uiAccess=true                 : %d" % sum(1 for f in man_ok if col(f, 10) == "1"))
    nd = collections.Counter()
    deps = collections.Counter()
    for f in man_ok:
        nd[col(f, 11, "0")] += 1
        for d in col(f, 12).split(";"):
            if d:
                deps[d.split("@")[0]] += 1
    print("manifests with N dependentAssembly: " + ", ".join("%s:%d" % (k, n) for k, n in sorted(nd.items(), key=lambda x: int(x[0]))))
    print("dependentAssembly names:")
    for k, n in deps.most_common(20):
        print("    %4d  %s" % (n, k))
    osm = collections.Counter()
    for f in man_ok:
        m = int(col(f, 13, "0") or 0)
        for bit, name in OS:
            if m & bit:
                osm[name] += 1
    print("supportedOS (count of manifests listing it): " + ", ".join("%s:%d" % (k, n) for k, n in osm.most_common()))
    dp = collections.Counter(DPI.get(int(col(f, 14, "0") or 0), "?") for f in man_ok)
    print("DPI mode: " + ", ".join("%s:%d" % (k, n) for k, n in dp.most_common()))
    lp = collections.Counter(col(f, 15) for f in man_ok)
    print("longPathAware: " + ", ".join("%s:%d" % (k, n) for k, n in lp.most_common()))
    cp = collections.Counter(col(f, 16) for f in man_ok)
    print("activeCodePage: " + ", ".join("%s:%d" % (k, n) for k, n in cp.most_common()))

    ver_ok = [f for f in ok if col(f, 17) == "ok"]
    ver_none = [f for f in ok if col(f, 17) == "none"]
    ver_err = [f for f in ok if col(f, 17) not in ("ok", "none")]
    print("version info: ok %d, none %d, rejected %d" % (len(ver_ok), len(ver_none), len(ver_err)))
    for k, n in collections.Counter(col(f, 17) for f in ver_err).most_common():
        print("    %4d  %s" % (n, k))
    if show_rejects:
        for f in ver_err:
            print("    VER-REJECT %s [%s]" % (f[0], col(f, 17)))
        for f in baddir + enum_bad:
            print("    RES-REJECT %s dir=[%s] enum=[%s]" % (f[0], col(f, 4), col(f, 5)))
    print("with FileDescription          : %d" % sum(1 for f in ver_ok if col(f, 19) not in ("-", "")))


if __name__ == "__main__":
    main()
