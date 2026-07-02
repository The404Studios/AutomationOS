#!/usr/bin/env python3
# Filter `git diff -- userspace/init/main.c` (CRLF) to the forkreg or forkfd spawn
# block. Keep only the selected block (a leading blank + line attaches to the block
# of the comment that follows it). Drops the other block + any FAIRTEST hunk.
# Binary-safe, CRLF-preserving. Usage: fr_mc_select.py reg|fd
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
    if b"FAIRTEST" in text or b"pureburn" in text or b"fairwake" in text:
        return
    if b"fork-fd-table inheritance" in text or b"FORK-REGS-INHERIT-0 probe" in text:
        out.append(head)
        block = None
        pending_blank = None
        for ln in h[1:]:
            is_add = ln.startswith(b"+") and not ln.startswith(b"+++")
            if not is_add:
                pending_blank = None
                out.append(ln)
                continue
            if ln.strip(b"+\r\n ") == b"":          # blank + line: hold it
                pending_blank = ln
                continue
            if b"fork-fd-table inheritance" in ln:  block = "fd"
            elif b"FORK-REGS-INHERIT-0 probe" in ln: block = "reg"
            if block == mode:
                if pending_blank is not None:
                    out.append(pending_blank)
                out.append(ln)
            pending_blank = None
        return
    return

for ln in lines:
    if ln.startswith(b"@@"):
        emit(hunk); hunk = [ln]; header_done = True
    elif not header_done: out.append(ln)
    else: hunk.append(ln)
emit(hunk)
sys.stdout.buffer.write(b"".join(out))
