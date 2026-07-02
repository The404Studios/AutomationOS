#!/bin/bash
# Build userspace+ISO, then headless-screenshot the desktop terminal.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
OUT="${1:-t2check}"
echo "[t2] building..."
bash scripts/build_all.sh > /tmp/t2_build.log 2>&1
echo "[t2] build rc=$? ; tail:"; tail -3 /tmp/t2_build.log
# fail-fast on hard errors
if grep -nE "error:|undefined reference" /tmp/t2_build.log; then
  echo "[t2] BUILD HAD ERRORS"; exit 1
fi
echo "[t2] shooting..."
bash build_test/shot.sh build/automationos.iso "$OUT"
