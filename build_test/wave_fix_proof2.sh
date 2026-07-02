#!/bin/bash
# WAVE-FIX-0 round-2 proof: re-verify the 4 self-audit fixes (compositor EX_ARGV
# ABI #1, proof-gap #2, PLAYER_SPEED #3, RAID-HUD gate #4) + regression-check the
# harnesses they touch. Sequential (shared build/). Run: wsl -d Arch bash build_test/wave_fix_proof2.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
declare -A RESULT
run() {
  local key="$1" desc="$2"; shift 2
  echo "############ START $key -- $desc ############"
  if bash "$@" > "/tmp/wfp2_$key.log" 2>&1; then
    RESULT[$key]="PASS"; echo "@@@ $key: PASS"; tail -4 "/tmp/wfp2_$key.log"
  else
    RESULT[$key]="FAIL"; echo "@@@ $key: FAIL"; tail -18 "/tmp/wfp2_$key.log"
  fi
  echo
}
run EXARGV   "#1/#2 compositor EX_ARGV ABI + spaced-argv proof" build_test/exargv_probe_smoke.sh
run PREBUILT "compositor guard + IDE boots"                     build_test/ide_prebuilt_smoke.sh
run RAID     "#3/#4 deadzone selftests"                         build_test/dz_raid_smoke.sh
run SRVTEST  "#2 phantom (deadzoned relink)"                    build_test/dz_srvtest_smoke.sh
run MP2      "#3 PLAYER_SPEED live co-op"                       build_test/dz_mp2_smoke.sh
run SMOKE    "default kernel 43/43"                             scripts/smoke_boot.sh
echo "================= WAVE-FIX-2 SUMMARY ================="
fail=0
for k in EXARGV PREBUILT RAID SRVTEST MP2 SMOKE; do
  printf "  %-9s %s\n" "$k" "${RESULT[$k]:-MISSING}"
  [ "${RESULT[$k]}" = "PASS" ] || fail=1
done
echo "===================================================="
[ "$fail" = "0" ] && echo "ALL GREEN" || echo "SOME FAILED (see /tmp/wfp2_*.log)"
exit $fail
