#!/usr/bin/env bash
# Web-stack fuzz smoke (B11 / WEB-FUZZ-0)
#
# Host-builds the REAL HTML parser under AddressSanitizer and replays a corpus
# of malformed + adversarial inputs to catch memory-safety regressions and
# unbounded-growth DoS. No kernel/QEMU -> runs on a plain CI runner.
#
# Run: wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && bash build_test/web_fuzz_smoke.sh'
#
# For coverage-guided fuzzing (local), build with clang instead:
#   clang -O1 -fsanitize=fuzzer,address -DUSE_LIBFUZZER -Iuserspace/lib/html \
#     build_test/web_fuzz_harness.c userspace/lib/html/html_parse.c \
#     userspace/lib/dom/dom.c -o /tmp/webfuzz_cov && /tmp/webfuzz_cov tests/fuzz/corpus/html
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
BIN="/tmp/webfuzz_$$"
CORPUS="tests/fuzz/corpus/html"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"

gcc -w -g -fsanitize=address -Iuserspace/lib/html -Iuserspace/lib/dom \
    build_test/web_fuzz_harness.c userspace/lib/html/html_parse.c \
    userspace/lib/dom/dom.c -o "$BIN" || { echo "WEBFUZZ: FAIL build=1"; exit 1; }

# Seed an adversarial corpus the first time.
mkdir -p "$CORPUS"
if [ -z "$(ls -A "$CORPUS" 2>/dev/null)" ]; then
  python3 - "$CORPUS" <<'PY'
import sys, os
d = sys.argv[1]
cases = {
  "unclosed.html":       b"<div><p>unclosed<span>text",
  "bad_attrs.html":      b"<a href=x attr attr2= =val 'q>t</a></b></c><img src=1 />",
  "unterm_comment.html": b"<!-- never ends <div><script>x<",
  "entities.html":       b"&#xZZ; &#999999999; &amp &unknown; <p>& &#; &#x;",
  "tag_soup.html":       b"</></p<<>><a<b>></a b=>><=>",
  "script_style.html":   b"<script><div></script><style>a{b<<!--",
  "deep_nest.html":      b"<div>" * 50000,
  "wide.html":           b"<p>" * 20000 + b"x",
  "attr_flood.html":     b"<x " + b"a=1 " * 20000 + b">",
}
for n, b in cases.items():
    open(os.path.join(d, n), "wb").write(b)
print("seeded", len(cases), "cases ->", d)
PY
fi

n=0
for f in "$CORPUS"/*; do [ -f "$f" ] && n=$((n + 1)); done
echo "== replaying $n corpus inputs through html_parse() under ASan =="
timeout 60 "$BIN" "$CORPUS"/*
rc=$?
rm -f "$BIN"

if [ "$rc" -eq 0 ]; then
  echo "WEBFUZZ: PASS inputs=$n asan_clean=1"
elif [ "$rc" -eq 124 ]; then
  echo "WEBFUZZ: FAIL timeout=1 (possible unbounded-growth DoS)"; exit 1
else
  echo "WEBFUZZ: FAIL asan_fault=1 rc=$rc"; exit 1
fi
