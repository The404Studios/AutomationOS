#!/bin/bash
cd /mnt/c/Users/wilde/Desktop/Kernel
bash scripts/smoke_boot.sh --iso build/automationos.iso > /tmp/p1smoke.log 2>&1
echo "--- result ---"
grep -E "Passed:|Failed:|RESULT" /tmp/p1smoke.log | head -4
echo "--- compositor / desktop / fault markers (from boot log) ---"
for m in 'compositor' 'desktop' 'Created with PID' 'All services started' 'CPU EXCEPTION' 'Page Fault' 'PANIC' 'SHELL'; do
  printf '%-24s %s\n' "$m" "$(grep -ciF "$m" /tmp/smoke_boot.log 2>/dev/null)"
done
