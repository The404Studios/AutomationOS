#!/bin/bash
# MATH-0a verify (fpm fixed-point math library + mathtest boot KAT battery).
# Shape cloned from w1b_check.sh, but this script wires NO kernel/ISO build and
# NO QEMU boot -- the main session owns those (shared build dir). It gates:
#   (a) fpm.c compiles standalone with the repo userspace flags and its object
#       is integer-only: 0 xmm/float instructions, 0 stack canaries.
#   (b) the host KAT battery (tests/fpm_hosttest.c vs libm) is green.
#   (c) a PROVIDED serial log (arg $1) contains the [MATHTEST] boot markers.
#       The main session wires sbin/mathtest into build_all.sh + the boot, then
#       runs:  bash build_test/mathtest_check.sh <serial.log>
# Run: wsl -d Arch bash build_test/mathtest_check.sh [serial.log]
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

SER="${1:-build_test/mathtest_ser.log}"

# --- (a) freestanding object gate (repo userspace CF, flags passed DIRECTLY) ---
echo "[math0a] compiling fpm.c freestanding..."
gcc -std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 \
    -c userspace/lib/fpm/fpm.c -o /tmp/fpm_gate.o \
  || { echo "MATH-0a: FAIL (fpm.c does not compile freestanding)"; exit 1; }

XMM=$(objdump -d /tmp/fpm_gate.o 2>/dev/null | grep -ciE 'xmm|movss|movsd')
CANARY=$(objdump -d /tmp/fpm_gate.o 2>/dev/null | grep -c 'fs:0x28')
echo "fpm.o: xmm/float=$XMM canary=$CANARY (both must be 0)"

# --- (b) host KAT battery (dense sweeps vs libm double references) ---
echo "[math0a] host KAT battery..."
HOST=0
gcc -std=gnu11 -O2 -DFPM_HOSTTEST -I userspace/lib/fpm \
    tests/fpm_hosttest.c -o /tmp/fpm_hosttest -lm \
  && /tmp/fpm_hosttest > /tmp/fpm_hosttest.log 2>&1 \
  && grep -q 'FPM HOSTTEST: PASS' /tmp/fpm_hosttest.log && HOST=1
grep -E 'worst|FPM HOSTTEST' /tmp/fpm_hosttest.log | sed 's/^/  /'

# --- (c) boot markers from the provided serial log ---
BOOT=-1
if [ -f "$SER" ]; then
  echo "=== mathtest markers ($SER) ==="
  grep -aE '\[MATHTEST\] |MATHTEST: ' "$SER"
  FAM=$(grep -acE '\[MATHTEST\] .*: PASS' "$SER")
  BAD=$(grep -acE '\[MATHTEST\] .*: FAIL' "$SER")
  SUM=0; grep -qaE 'MATHTEST: PASS n=[0-9]+' "$SER" && SUM=1
  echo "families_pass=$FAM families_fail=$BAD summary=$SUM (need >=11 pass, 0 fail, summary 1)"
  if [ "$FAM" -ge 11 ] && [ "$BAD" = "0" ] && [ "$SUM" = "1" ]; then BOOT=1; else BOOT=0; fi
else
  echo "(no serial log at $SER -- boot markers unverified; pass the log path as \$1)"
fi

echo ""
echo "xmm=$XMM canary=$CANARY host_kat=$HOST boot=$BOOT"
if [ "$XMM" = "0" ] && [ "$CANARY" = "0" ] && [ "$HOST" = "1" ] && [ "$BOOT" = "1" ]; then
  echo "MATH-0a: PASS (fpm integer-only; host KATs green; boot battery green)"
  exit 0
elif [ "$XMM" = "0" ] && [ "$CANARY" = "0" ] && [ "$HOST" = "1" ] && [ "$BOOT" = "-1" ]; then
  echo "MATH-0a: PARTIAL (static + host gates green; no serial log yet -- boot unproven)"
  exit 2
else
  echo "MATH-0a: FAIL"
  [ -f "$SER" ] && { echo "--- tail ---"; tail -15 "$SER"; }
  exit 1
fi
