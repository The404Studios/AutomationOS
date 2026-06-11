#!/usr/bin/env bash
L=/tmp/smoke_persist_boot1.log
echo "=== DISKFS lines (boot1) ==="
grep -iF 'diskfs' "$L" | head -25
echo "=== AHCI sector0 ==="
grep -iF 'sector0' "$L" | head
echo "=== diskfs SELFTEST result ==="
grep -iF 'SELFTEST' "$L" | grep -iF 'diskfs'
