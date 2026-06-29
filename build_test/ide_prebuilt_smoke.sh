#!/bin/bash
# IDE-PREBUILT proof: the IDE can Build+Run a PREBUILT game project (DeadZone)
# AND the raised LEGO-map caps don't break the IDE. Headless scope:
#   1. the whole IDE (caps + kind field + prebuilt build/run branch) COMPILES clean
#   2. ide.elf ships; the DeadZone project ships with kind=prebuilt + a real ELF
#   3. the IDE-autostart ISO BOOTS with no crash (the ~700KB cap bump didn't OOM)
# The actual Build->Run *click* is GUI/QEMU-visual (deferred, like MP-GUI-1): the
# code path (ide_do_build -> ide_build_prebuilt -> g_res.out_path; ide_do_run
# spawns g_res.out_path) is proven by inspection + project_kind_check.
# Run: wsl -d Arch bash build_test/ide_prebuilt_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1

echo "[idepb] quick_build kernel (byte-unchanged)..."
bash scripts/quick_build.sh > /tmp/idepb_qb.log 2>&1
grep -q SUCCESS /tmp/idepb_qb.log || { echo "KERNEL BUILD FAIL"; tail -6 /tmp/idepb_qb.log; exit 1; }

echo "[idepb] IDE=1 build_all (IDE autostart + DeadZone project)..."
IDE=1 bash scripts/build_all.sh > /tmp/idepb_ba.log 2>&1
# IDE compile errors? (scope the grep so unrelated lines don't trip it)
if grep -qiE 'ide_(build|project|parse|model|pcore|pdecl|pstmt|pexpr|lex|ast|semantic|map|inspector|chrome)\.c.*error:|error:.*ide_|undefined reference' /tmp/idepb_ba.log; then
  echo "IDE COMPILE ERRORS:"; grep -iE 'error:|undefined reference' /tmp/idepb_ba.log | head; exit 1
fi
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

echo ""
echo "=== ships correctly ==="
PJ=/tmp/ird/Desktop/Projects/DeadZone/project.json
if [ -f "$PJ" ]; then
  echo "  project.json:"; sed 's/^/    /' "$PJ"
  grep -q '^kind=prebuilt' "$PJ" && echo "  [OK] kind=prebuilt present" || { echo "  [FAIL] kind=prebuilt missing"; exit 1; }
else
  echo "  [WARN] staged project.json not found at $PJ (checking initrd)"
fi
ELF=/tmp/ird/Desktop/Projects/DeadZone/build/deadzone.elf
if [ -f "$ELF" ]; then
  head -c4 "$ELF" | grep -q $'\x7fELF' && echo "  [OK] build/deadzone.elf is a real ELF ($(wc -c < "$ELF") bytes)" || { echo "  [FAIL] run_target not an ELF"; exit 1; }
fi

echo ""
echo "=== compositor DI_PROJECT -> sbin/ide wiring (static, fail-closed) ==="
# AUDIT-9: the desktop project icon must open the IDE (not the file manager).
# The GUI double-click is QEMU-visual, but a refactor dropping/breaking the wiring
# must not pass silently. Pin the EXACT EX_ARGV call shape (opcode + path + buffer +
# length args): a bare 'sbin/ide' string grep is blind to a wrong syscall/arg order
# (the class of bug a prior audit found here), so assert the sc6(SYS_SPAWN_EX_ARGV,..)
# call itself -- a revert to plain SYS_SPAWN or swapped buf/len args now FAILS.
if grep -q 'di->kind == DI_PROJECT' userspace/compositor/compositor_m8.c && \
   grep -A8 'di->kind == DI_PROJECT' userspace/compositor/compositor_m8.c \
     | grep -q 'sc6(SYS_SPAWN_EX_ARGV, (long)"sbin/ide", (long)ide_av, (long)ide_n'; then
  echo "  [OK] DI_PROJECT double-click spawns sbin/ide via SYS_SPAWN_EX_ARGV (buf+len)"
else
  echo "  [FAIL] DI_PROJECT EX_ARGV spawn wiring missing/changed"; exit 1
fi

echo ""
echo "=== boots with raised caps (35s) ==="
SER=build_test/idepb_ser.log; rm -f "$SER"
timeout 40 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -serial "file:$SER" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1
# KERNEL-FATAL only (these never occur in a healthy boot). The full selftest
# storm includes sbin/sigtest, which DELIBERATELY page-faults at userspace
# CR2=0x4000 to prove signal/fault handling -- the kernel terminates that
# process and boots on, so a bare "CPU EXCEPTION" is NOT a crash (netrig
# discriminator). A real regression PANICs or triple-faults and HALTS the boot.
CRASH=0
for pat in 'PANIC' 'triple fault' 'double fault' 'KERNEL HALT'; do
  c=$(grep -icF "$pat" "$SER"); CRASH=$((CRASH+c))
done
LINES=$(wc -l < "$SER")
AUTO=$(grep -cF 'IDE_AUTOSTART' "$SER")
# AUDIT-9 RUNTIME PROOF (fail-closed). MARK proves the manifest was loaded LIVE
# (a->project.active set at runtime); grep the FULL line incl. kind=prebuilt so a
# default/garbage manifest (which prints kind=c) also fails. PROBE proves the
# EXACT prebuilt gate FIRED and resolved the shipped ELF -- not just inspection.
MARK=$(grep -acF '[IDE] project active root=/Desktop/Projects/DeadZone kind=prebuilt' "$SER")
PROBE=$(grep -acF '[IDE] prebuilt probe handled=1 ok=1 out=/Desktop/Projects/DeadZone/build/deadzone.elf' "$SER")
echo "  lines=$LINES  kernel_fatal=$CRASH  IDE_AUTOSTART=$AUTO  proj_active=$MARK  prebuilt_fired=$PROBE"
echo "  (note: any 'CPU EXCEPTION' is sbin/sigtest's deliberate CR2=0x4000 fault-injection, handled)"
echo "  --- tail 4 ---"; tail -4 "$SER" | sed 's/^/    /'

echo ""
if [ "$CRASH" -eq 0 ] && [ "$AUTO" -ge 1 ] && [ "$LINES" -ge 2000 ] && [ "$MARK" -ge 1 ] && [ "$PROBE" -ge 1 ]; then
  echo "IDE-PREBUILT: PASS (IDE compiles + ships DeadZone[kind=prebuilt] + boots clean with raised map caps;"
  echo "  project loaded at RUNTIME [proj_active=$MARK] + prebuilt gate FIRED on the shipped ELF [prebuilt_fired=$PROBE])"
  exit 0
else
  echo "IDE-PREBUILT: FAIL (kernel_fatal=$CRASH autostart=$AUTO lines=$LINES proj_active=$MARK prebuilt_fired=$PROBE)"
  echo "--- [IDE] markers seen ---"; grep -aF '[IDE] ' "$SER" | head
  echo "--- tail 20 ---"; tail -20 "$SER"
  exit 1
fi
