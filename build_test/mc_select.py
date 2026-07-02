#!/usr/bin/env python3
# Filter `git diff -- userspace/init/main.c` (CRLF) to the B8 or B10 spawn block.
# - DROP the FAIRTEST (#ifdef FAIRTEST) hunk.
# - In the spawn hunk, keep only the selected app's contiguous block (run-tracker
#   keyed on the `+// SIG-FULL-0 (B8)` / `+// POLL-SELECT-0 (B10)` opener; blank
#   `+` lines inherit the current block).
# Binary-safe, CRLF-preserving (splitlines keepends). Usage: mc_select.py B8|B10
import sys
sel = sys.argv[1]  # "B8" or "B10"
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
    if b"FAIRTEST" in text or b"pureburn" in text or b"fairwake" in text:   # FAIRTEST hunk -> drop
        return
    if b"SIG-FULL-0 (B8)" in text or b"POLL-SELECT-0 (B10)" in text:        # spawn hunk
        out.append(head)
        block = None
        for ln in h[1:]:
            if ln.startswith(b"+") and b"SIG-FULL-0 (B8)" in ln:
                block = "B8"
            elif ln.startswith(b"+") and b"POLL-SELECT-0 (B10)" in ln:
                block = "B10"
            if ln.startswith(b"+") and not ln.startswith(b"+++"):
                if block == sel:
                    out.append(ln)
            else:
                out.append(ln)                    # context / - lines always kept
        return
    return  # unknown hunk -> drop

for ln in lines:
    if ln.startswith(b"@@"):
        emit(hunk)
        hunk = [ln]
        header_done = True
    elif not header_done:
        out.append(ln)
    else:
        hunk.append(ln)
emit(hunk)
sys.stdout.buffer.write(b"".join(out))
