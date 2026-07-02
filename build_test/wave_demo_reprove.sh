#!/bin/bash
# Re-prove the co-op tests after the world_join spawn-spread + deadzone render-proof
# changes: dz_mp2 (live 2-client) + dz_srvtest (server selftest/phantom) + RAID
# (deadzone selftests) + smoke_boot (default kernel). Run: wsl -d Arch bash build_test/wave_demo_reprove.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
declare -A R
run(){ local k="$1"; shift; echo "############ $k ############"
  if bash "$@" >"/tmp/wdr_$k.log" 2>&1; then R[$k]=PASS; echo "@@@ $k: PASS"; tail -3 "/tmp/wdr_$k.log"
  else R[$k]=FAIL; echo "@@@ $k: FAIL"; tail -16 "/tmp/wdr_$k.log"; fi; echo; }
run MP2     build_test/dz_mp2_smoke.sh
run SRVTEST build_test/dz_srvtest_smoke.sh
run RAID    build_test/dz_raid_smoke.sh
run SMOKE   scripts/smoke_boot.sh
echo "================= DEMO-REPROVE SUMMARY ================="
f=0; for k in MP2 SRVTEST RAID SMOKE; do printf "  %-8s %s\n" "$k" "${R[$k]:-MISSING}"; [ "${R[$k]}" = PASS ]||f=1; done
echo "======================================================="
[ "$f" = 0 ] && echo "ALL GREEN" || echo "SOME FAILED (see /tmp/wdr_*.log)"
exit $f
