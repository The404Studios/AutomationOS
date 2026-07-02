#!/usr/bin/env bash
# Measure stack frames with the ACTUAL OS userspace build flags (from p384.h).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
C=userspace/lib/crypto
T=/tmp/p384suos
mkdir -p "$T"
FLAGS="-std=gnu11 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mstackrealign -O2 -w -fstack-usage"
gcc $FLAGS -c "$C/p384.c"   -o "$T/p384.o"   -I "$C"
gcc $FLAGS -c "$C/bignum.c" -o "$T/bignum.o" -I "$C"
echo "=== p384 frames (OS flags) ==="
cat "$T/p384.su" | sort -t$'\t' -k2 -n -r
echo "=== bignum frames (OS flags) ==="
cat "$T/bignum.su" | sort -t$'\t' -k2 -n -r | head -8
echo ""
echo "=== Deepest verify path (scalar-mul loop), OS flags ==="
awk -F'\t' '
  /p384_ecdsa_verify/{v=$2}
  /:jscalar/{js=$2}
  /:jadd/{ja=$2}
  /:jdouble/{jd=$2}
' "$T/p384.o" 2>/dev/null
python3 - "$T/p384.su" "$T/bignum.su" <<'PY'
import sys
def load(f):
    d={}
    for line in open(f):
        parts=line.rstrip("\n").split("\t")
        if len(parts)>=2:
            name=parts[0].split(":")[-1]
            try: d[name]=int(parts[1])
            except: pass
    return d
p=load(sys.argv[1]); b=load(sys.argv[2])
chain=[("p384_ecdsa_verify",p.get("p384_ecdsa_verify",0)),
       ("jscalar",p.get("jscalar",0)),
       ("jadd",p.get("jadd",0)),
       ("jdouble",p.get("jdouble",0)),
       ("bn_mod_mul",b.get("bn_mod_mul",0))]
tot=0
for n,s in chain:
    tot+=s; print(f"  {n:20s} {s:7d}  cum={tot}")
print(f"  DEEPEST (scalar-mul path) = {tot} bytes")
# finv path
chain2=[("p384_ecdsa_verify",p.get("p384_ecdsa_verify",0)),
        ("bn_mod_exp",b.get("bn_mod_exp",0)),
        ("mont_mul",b.get("mont_mul",0))]
tot2=sum(s for _,s in chain2)
print(f"  finv/bn_mod_exp path      = {tot2} bytes")
PY
