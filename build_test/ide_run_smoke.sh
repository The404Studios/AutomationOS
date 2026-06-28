#!/bin/bash
# AUDIT-9 END-TO-END proof: the IDE drives the REAL Build->Run path on the loaded
# DeadZone project at launch, and the launched game reaches 'DEADZONE: ready' --
# closing step 7 (ide_do_run SYS_SPAWNs the resolved ELF) + step 8 (it actually
# launches) HEADLESSLY, through the real IDE wire (not the decoupled DZ_GAMETEST
# init->spawn path). This is the link ide_prebuilt_smoke.sh could not cover (the
# prebuilt probe stops at the gate; it leaves g_have=0 so Run never fires there).
#
# Build gates: IDE=1 (init autostarts sbin/ide on /Desktop/Projects/DeadZone via
# argv[1]) + IDE_RUN_PROBE=1 (ide.c, AFTER its own wl_create_window, drives
# ide_do_build -> ide_do_run once the project is active). Kept SEPARATE from
# ide_prebuilt_smoke.sh, which must NOT see a run marker (proves the paths stay
# decoupled). Run: wsl -d Arch bash build_test/ide_run_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[iderun] quick_build kernel (byte-unchanged)..."
bash scripts/quick_build.sh > /tmp/iderun_qb.log 2>&1
grep -q SUCCESS /tmp/iderun_qb.log || { echo "KERNEL BUILD FAIL"; tail -6 /tmp/iderun_qb.log; exit 1; }

echo "[iderun] IDE=1 IDE_RUN_PROBE=1 build_all..."
IDE=1 IDE_RUN_PROBE=1 bash scripts/build_all.sh > /tmp/iderun_ba.log 2>&1
if grep -qiE 'ide_(build|project|parse|model|pcore|pdecl|pstmt|pexpr|lex|ast)\.c.*error:|error:.*ide_|undefined reference' /tmp/iderun_ba.log; then
  echo "IDE COMPILE ERRORS:"; grep -iE 'error:|undefined reference' /tmp/iderun_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

echo ""
echo "=== boots + drives Build->Run (95s; IDE + a 2nd fullscreen game is heavy under TCG) ==="
SER=build_test/iderun_ser.log; rm -f "$SER"
timeout 100 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1

CRASH=0
for pat in 'PANIC' 'triple fault' 'double fault' 'KERNEL HALT'; do
  c=$(grep -icF "$pat" "$SER"); CRASH=$((CRASH+c))
done
LINES=$(wc -l < "$SER")
# TIER-1 (HARD, deterministic): ide_do_run SYS_SPAWNed the resolved ELF, pid>0.
RUNSPAWN=$(grep -acE '\[IDE\] run spawn pid=[1-9][0-9]* out=/Desktop/Projects/DeadZone/build/deadzone.elf' "$SER")
# TIER-2 (end-to-end): the IDE-spawned DeadZone reached ready (wl_connect + 5 selftests).
READY=$(grep -acF 'DEADZONE: ready' "$SER")
# corroboration: the prebuilt gate fired at project load (same marker as ide_prebuilt_smoke).
PROBE=$(grep -acF '[IDE] prebuilt probe handled=1 ok=1 out=/Desktop/Projects/DeadZone/build/deadzone.elf' "$SER")

echo "  lines=$LINES  kernel_fatal=$CRASH  run_spawn=$RUNSPAWN  deadzone_ready=$READY  prebuilt_fired=$PROBE"
echo "  --- [IDE] run + DEADZONE markers ---"
grep -aE '\[IDE\] run spawn|DEADZONE: ready' "$SER" | head
echo ""
if [ "$CRASH" -eq 0 ] && [ "$RUNSPAWN" -ge 1 ] && [ "$READY" -ge 1 ] && [ "$LINES" -ge 2000 ]; then
  echo "IDE-RUN: PASS (IDE drove Build->Run end-to-end: ide_do_run SYS_SPAWNed the shipped ELF"
  echo "  [run_spawn=$RUNSPAWN] and the launched DeadZone reached ready [deadzone_ready=$READY])"
  exit 0
else
  echo "IDE-RUN: FAIL (kernel_fatal=$CRASH run_spawn=$RUNSPAWN deadzone_ready=$READY lines=$LINES)"
  echo "--- [IDE]/DEADZONE markers ---"; grep -aE '\[IDE\]|DEADZONE:' "$SER" | head -20
  echo "--- tail 20 ---"; tail -20 "$SER"
  exit 1
fi
