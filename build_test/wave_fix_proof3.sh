#!/bin/bash
# WAVE-FIX-0 round-3 re-prove: the 3 deep-audit fixes (mp_move_wire refactor +
# speed_scale selftest, mp_game_connect world-blank, tightened DI_PROJECT guard).
# RAID exercises deadzone's selftests (incl. the new 'mp PASS' speed_scale assert),
# PREBUILT exercises the tightened static guard, SMOKE re-confirms the default
# kernel 43/43. Run: wsl -d Arch bash build_test/wave_fix_proof3.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
declare -A RESULT
run() {
  local key="$1"; shift
  echo "############ $key ############"
  if bash "$@" > "/tmp/wfp3_$key.log" 2>&1; then RESULT[$key]=PASS; echo "@@@ $key: PASS"; tail -4 "/tmp/wfp3_$key.log"
  else RESULT[$key]=FAIL; echo "@@@ $key: FAIL"; tail -18 "/tmp/wfp3_$key.log"; fi
  echo
}
run RAID     build_test/dz_raid_smoke.sh
run PREBUILT build_test/ide_prebuilt_smoke.sh
run SMOKE    scripts/smoke_boot.sh
echo "================= WAVE-FIX-3 SUMMARY ================="
fail=0
for k in RAID PREBUILT SMOKE; do printf "  %-9s %s\n" "$k" "${RESULT[$k]:-MISSING}"; [ "${RESULT[$k]}" = PASS ] || fail=1; done
echo "===================================================="
[ "$fail" = "0" ] && echo "ALL GREEN" || echo "SOME FAILED (see /tmp/wfp3_*.log)"
exit $fail
