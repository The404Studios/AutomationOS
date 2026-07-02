#!/usr/bin/env python3
# Filter `git diff -- scripts/build_all.sh` (LF) to the B8 or B10 wiring only.
# - DROP the FAIRTEST-flag hunk and the B1-TLS hunk.
# - In the compile-block hunk, keep only the selected app's per-app block
#   (run-tracker keyed on the `+# <app>:` opener comment).
# - On each `+for e in` sbin-list line, strip the non-selected app words.
# Binary-safe, LF-preserving (splitlines keepends). Usage: ba_select.py B8|B10
import sys
mode = sys.argv[1]
sel = mode  # "B8" or "B10"
strip = {"B8": b" pollselftest pureburn fairwake",
         "B10": b" pureburn fairwake"}[mode]
data = sys.stdin.buffer.read()
lines = data.splitlines(keepends=True)
out = []
header_done = False
hunk = []

def emit(h):
    if not h:
        return
    head = h[0]
    text = b"".join(h[1:])
    if b"for e in" in text:                       # sbin-list hunk: rewrite +line, keep rest
        for ln in h:
            out.append(ln.replace(strip, b"") if ln.startswith(b"+for e in") else ln)
        return
    if b"x509_verify_chain" in text or b"TLSREQUIRE" in text:   # B1 TLS hunk -> drop
        return
    if b"INIT_EXTRA" in text or b"DFAIRTEST" in text or b"pure burners" in text:  # FAIRTEST flag -> drop
        return
    if b"# sigtest:" in text or b"# pollselftest:" in text or b"# pureburn" in text:  # compile blocks
        out.append(head)
        block = None
        for ln in h[1:]:
            if ln.startswith(b"+# sigtest:"):
                block = "B8"
            elif ln.startswith(b"+# pollselftest:"):
                block = "B10"
            elif ln.startswith(b"+# pureburn"):
                block = "FAIR"
            if ln.startswith(b"+") and not ln.startswith(b"+++"):
                if block == sel:
                    out.append(ln)
            else:
                out.append(ln)                    # context / - lines always kept
        return
    return  # unknown hunk -> drop (not B8/B10)

for ln in lines:
    if ln.startswith(b"@@"):
        emit(hunk)
        hunk = [ln]
        header_done = True
    elif not header_done:
        out.append(ln)                            # diff --git / index / --- / +++
    else:
        hunk.append(ln)
emit(hunk)
sys.stdout.buffer.write(b"".join(out))
