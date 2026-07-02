#!/bin/bash
# Re-prove after the DeadZone whole-feature-audit fixes: A(MP-fire gate) B(zombie state)
# C(TPK revive) E(input-latch) G(proj overflow) H(hp clamp). deadzone.elf + deadzoned.elf
# both changed. Run: wsl -d Arch bash build_test/wave_gamefix_reprove.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
declare -A R
run(){ local k="$1"; shift; echo "############ $k ############"
  if bash "$@" >"/tmp/wgf_$k.log" 2>&1; then R[$k]=PASS; echo "@@@ $k: PASS"; tail -3 "/tmp/wgf_$k.log"
  else R[$k]=FAIL; echo "@@@ $k: FAIL"; tail -16 "/tmp/wgf_$k.log"; fi; echo; }
run MPGUI   build_test/dz_mpgui_lean_check.sh
run RAID    build_test/dz_raid_smoke.sh
run AUDIO   build_test/dz_audio_smoke.sh
run MPLIVE  build_test/dz_mplive_smoke.sh
run MP2     build_test/dz_mp2_smoke.sh
run SRVTEST build_test/dz_srvtest_smoke.sh
echo "================= GAME-FIX REPROVE SUMMARY ================="
f=0; for k in MPGUI RAID AUDIO MPLIVE MP2 SRVTEST; do printf "  %-8s %s\n" "$k" "${R[$k]:-MISSING}"; [ "${R[$k]}" = PASS ]||f=1; done
echo "==========================================================="
[ "$f" = 0 ] && echo "ALL GREEN" || echo "SOME FAILED (see /tmp/wgf_*.log)"
exit $f
