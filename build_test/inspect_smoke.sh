#!/usr/bin/env bash
L=/tmp/smoke_boot.log
echo "=== total lines: $(wc -l < "$L") ==="
echo "=== lines mentioning CPU EXCEPTION (with 6 lines after each) ==="
grep -n -A6 'CPU EXCEPTION' "$L" | head -60
echo
echo "=== Page Fault blocks (8 lines after) ==="
grep -n -A8 'Page Fault' "$L" | head -50
echo
echo "=== did SLAB / HEAP selftests appear at all? ==="
grep -niE 'SLAB|HEAPEXT|heap_selftest|heap self' "$L" | head
echo "=== last spawn/created before the first CPU EXCEPTION ==="
firstexc=$(grep -n 'CPU EXCEPTION' "$L" | head -1 | cut -d: -f1)
echo "first CPU EXCEPTION at line $firstexc"
if [ -n "$firstexc" ]; then sed -n "$((firstexc>25?firstexc-25:1)),$((firstexc+2))p" "$L"; fi
