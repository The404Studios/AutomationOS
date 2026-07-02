#!/usr/bin/env python3
# Filter `git diff -- scripts/build_all.sh` (LF) to forkreg or forkfd wiring only.
#   reg: keep the forkregtest compile block; for-e line keeps forkregtest, strips
#        forkfdtest + the pre-existing pureburn/fairwake.
#   fd : keep the forkfdtest compile block; for-e line keeps forkfdtest (forkregtest
#        already committed first), strips pureburn/fairwake.
# Binary-safe, LF-preserving. Usage: fr_ba_select.py reg|fd
import sys
mode = sys.argv[1]
data = sys.stdin.buffer.read()
lines = data.splitlines(keepends=True)
out = []
header_done = False
hunk = []

def emit(h):
    if not h: return
    head = h[0]; text = b"".join(h[1:])
    if b"for e in" in text:
        for ln in h:
            if ln.startswith(b"+for e in"):
                x = ln.replace(b" pureburn fairwake", b"")
                if mode == "reg":
                    x = x.replace(b" forkfdtest", b"")
                out.append(x)
            else:
                out.append(ln)
        return
    if b"INIT_EXTRA" in text or b"DFAIRTEST" in text:        # FAIRTEST flag -> drop
        return
    if b"# forkfdtest:" in text or b"# forkregtest:" in text or b"# pureburn" in text:
        out.append(head)
        block = None
        for ln in h[1:]:
            if ln.startswith(b"+# forkfdtest:"):   block = "fd"
            elif ln.startswith(b"+# forkregtest:"): block = "reg"
            elif ln.startswith(b"+# pureburn"):     block = "fair"
            if ln.startswith(b"+") and not ln.startswith(b"+++"):
                if block == mode: out.append(ln)
            else:
                out.append(ln)                       # context / - lines always kept
        return
    return  # drop any other (pre-existing) hunk

for ln in lines:
    if ln.startswith(b"@@"):
        emit(hunk); hunk = [ln]; header_done = True
    elif not header_done: out.append(ln)
    else: hunk.append(ln)
emit(hunk)
sys.stdout.buffer.write(b"".join(out))
