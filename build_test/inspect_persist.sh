#!/usr/bin/env bash
L=/tmp/smoke_persist_boot1.log
echo "=== boot1 lines: DISKFS ==="
grep -iF 'diskfs' "$L" | head -20
echo "=== boot1 lines: AHCI ==="
grep -iF 'ahci' "$L" | head -20
echo "=== boot1 lines: SATA / sata0 ==="
grep -iF 'sata' "$L" | head -10
echo "=== boot1: 'boot #' ==="
grep -iF 'boot #' "$L" | head
echo "=== boot1: present / detect ==="
grep -iF 'present' "$L" | head
echo "=== crash count ==="
crash=0
for p in 'PANIC' 'CPU EXCEPTION' 'page fault'; do crash=$((crash + $(grep -icF "$p" "$L"))); done
echo "crash=$crash  lines=$(wc -l < "$L")"
echo "=== last 6 ==="
tail -6 "$L"
