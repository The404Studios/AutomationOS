#!/bin/bash
# WAVE-FIX proof matrix: run every harness that exercises the 11 audit fixes,
# sequentially (they share build/automationos.iso so cannot run in parallel).
# Each sub-harness exits 0 on PASS / non-0 on FAIL; we continue on failure and
# print a final SUMMARY. Run: wsl -d Arch bash build_test/wave_fix_proof.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
declare -A RESULT

run() {
  local key="$1" desc="$2"; shift 2
  echo "############ START $key -- $desc ############"
  if bash "$@" > "/tmp/wfp_$key.log" 2>&1; then
    RESULT[$key]="PASS"; echo "@@@ $key: PASS"; tail -4 "/tmp/wfp_$key.log"
  else
    RESULT[$key]="FAIL"; echo "@@@ $key: FAIL"; tail -18 "/tmp/wfp_$key.log"
  fi
  echo
}

run MAPCAPS  "#10 IDE map naming + caps (host)"        build_test/ide_map_caps_smoke.sh
run RAID     "#1/#3/#4/#5/#6/#7 deadzone selftests"    build_test/dz_raid_smoke.sh
run SRVTEST  "#2 phantom-slot suppression"             build_test/dz_srvtest_smoke.sh
run MP2      "#1/#2 live 2-client co-op"               build_test/dz_mp2_smoke.sh
run AUDIO    "#8 audio partial-write (HDA)"            build_test/dz_audio_smoke.sh
run PREBUILT "IDE still boots (ide_semantic relink)"   build_test/ide_prebuilt_smoke.sh
run SMOKE    "default kernel 43/43 (#9/#11 comp)"      scripts/smoke_boot.sh

echo "================= WAVE-FIX SUMMARY ================="
fail=0
for k in MAPCAPS RAID SRVTEST MP2 AUDIO PREBUILT SMOKE; do
  printf "  %-9s %s\n" "$k" "${RESULT[$k]:-MISSING}"
  [ "${RESULT[$k]}" = "PASS" ] || fail=1
done
echo "==================================================="
[ "$fail" = "0" ] && echo "ALL GREEN" || echo "SOME FAILED (see /tmp/wfp_*.log)"
exit $fail
