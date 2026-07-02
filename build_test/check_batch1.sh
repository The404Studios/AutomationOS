#!/usr/bin/env bash
L=/tmp/smoke_boot.log
echo "=== does the log contain the [KERNEL] tag? ==="
grep -c '\[KERNEL\]' "$L"
echo "=== first 3 lines of the boot log ==="
head -3 "$L"
echo "=== kernel boot markers present? ==="
for m in 'KERNEL' 'RTC' 'All services started' 'PAGINGALIAS' 'slab' 'SELFHEAL'; do
  printf '%-22s %s\n' "$m" "$(grep -cF "$m" "$L")"
done
echo "=== log line count ==="
wc -l < "$L"
