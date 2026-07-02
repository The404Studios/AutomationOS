#!/bin/bash
# FULL system verification after the session's changes (WAVE-FIX-0 + 4 audit rounds +
# co-op demo + spawn-spread). Runs every key harness sequentially (shared build/),
# continues on failure, prints a SUMMARY. Run: wsl -d Arch bash build_test/wave_full_verify.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
declare -A R
run(){ local k="$1"; shift; echo "############ START $k  ($(date +%H:%M:%S)) ############"
  if bash "$@" >"/tmp/wfv_$k.log" 2>&1; then R[$k]=PASS; echo "@@@ $k: PASS"; tail -2 "/tmp/wfv_$k.log"
  else R[$k]=FAIL; echo "@@@ $k: FAIL"; tail -16 "/tmp/wfv_$k.log"; fi; echo; }
run MAPCAPS  build_test/ide_map_caps_smoke.sh
run SMOKE    scripts/smoke_boot.sh
run NETRIG   build_test/netrig_check.sh
run MPLIVE   build_test/dz_mplive_smoke.sh
run MP2      build_test/dz_mp2_smoke.sh
run SRVTEST  build_test/dz_srvtest_smoke.sh
run RAID     build_test/dz_raid_smoke.sh
run AUDIO    build_test/dz_audio_smoke.sh
run MPGUI    build_test/dz_mpgui_lean_check.sh
run PREBUILT build_test/ide_prebuilt_smoke.sh
run IDERUN   build_test/ide_run_smoke.sh
run EXARGV   build_test/exargv_probe_smoke.sh
run TASKMAN  build_test/taskman_proof.sh
echo "================= FULL VERIFY SUMMARY ================="
f=0; for k in MAPCAPS SMOKE NETRIG MPLIVE MP2 SRVTEST RAID AUDIO MPGUI PREBUILT IDERUN EXARGV TASKMAN; do
  printf "  %-9s %s\n" "$k" "${R[$k]:-MISSING}"; [ "${R[$k]}" = PASS ] || f=1; done
echo "======================================================"
[ "$f" = 0 ] && echo "ALL GREEN -- everything works" || echo "SOME FAILED (see /tmp/wfv_*.log)"
exit $f
