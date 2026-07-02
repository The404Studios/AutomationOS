#!/bin/bash
# Re-prove after the demo-audit fixes (gated render counter + on-screen-only proof +
# static-facing auto-drive + in-bounds world_join spread). Run: wsl -d Arch bash build_test/wave_demo_audit_reprove.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
declare -A R
run(){ local k="$1"; shift; echo "############ $k ############"
  if bash "$@" >"/tmp/wda_$k.log" 2>&1; then R[$k]=PASS; echo "@@@ $k: PASS"; tail -3 "/tmp/wda_$k.log"
  else R[$k]=FAIL; echo "@@@ $k: FAIL"; tail -16 "/tmp/wda_$k.log"; fi; echo; }
run MPGUI   build_test/dz_mpgui_lean_check.sh
run MPLIVE  build_test/dz_mplive_smoke.sh
run MP2     build_test/dz_mp2_smoke.sh
run SRVTEST build_test/dz_srvtest_smoke.sh
run RAID    build_test/dz_raid_smoke.sh
echo "================= AUDIT-FIX REPROVE SUMMARY ================="
f=0; for k in MPGUI MPLIVE MP2 SRVTEST RAID; do printf "  %-8s %s\n" "$k" "${R[$k]:-MISSING}"; [ "${R[$k]}" = PASS ]||f=1; done
echo "============================================================"
[ "$f" = 0 ] && echo "ALL GREEN" || echo "SOME FAILED (see /tmp/wda_*.log)"
exit $f
