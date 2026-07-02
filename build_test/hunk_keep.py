#!/usr/bin/env python3
# Keep the diff file-header + ONLY the hunks whose body contains <marker>.
# Binary-safe, line-ending-preserving (splitlines keepends). Used to peel a
# specific audit hunk out of handlers.c (which also carries SMP hunks).
# Usage:  git diff -- file | hunk_keep.py <marker>  > out.patch
import sys
marker = sys.argv[1].encode()
data = sys.stdin.buffer.read()
lines = data.splitlines(keepends=True)
out = []
header_done = False
hunk = []

def flush(h):
    if not h:
        return
    if marker in b"".join(h[1:]):
        out.extend(h)

for ln in lines:
    if ln.startswith(b"@@"):
        flush(hunk); hunk = [ln]; header_done = True
    elif not header_done:
        out.append(ln)          # diff --git / index / --- / +++
    else:
        hunk.append(ln)
flush(hunk)
sys.stdout.buffer.write(b"".join(out))
