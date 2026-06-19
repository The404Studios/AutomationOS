#!/bin/bash
# IWL-LOAD/OPS integration verify: the real DVM driver (iwl-hostcmd/fw-load/nvm/
# scan/ops) + the de-static'd iwl-trans (with the review fixes) + the iwlup trigger
# must COMPILE, LINK (iwl_wifi_bringup resolved by netsyscall), and BOOT HELD --
# the radio bring-up must NOT fire at boot. Plus a default-build regression.
# Run: wsl -d Arch bash build_test/iwl_integ_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
PASS=1

echo "=== [1/2] IWLWIFI=1: all iwl bricks compile + LINK + boot held (no card) ==="
IWLWIFI=1 bash scripts/quick_build.sh > /tmp/iwl_q.log 2>&1
for f in iwl-pci iwl-fw iwl-trans iwl-hostcmd iwl-fw-load iwl-nvm iwl-scan iwl-ops; do
  grep -qE "OK: .*${f}\.c" /tmp/iwl_q.log || { echo "  COMPILE MISS: ${f}.c"; PASS=0; }
done
grep -qiE 'error:|^FAIL:|undefined reference' /tmp/iwl_q.log && { echo "  BUILD ERR:"; grep -iE 'error:|FAIL:|undefined reference' /tmp/iwl_q.log | head; PASS=0; }
grep -q SUCCESS /tmp/iwl_q.log || { echo "  no SUCCESS (kernel link failed?)"; PASS=0; }
bash scripts/build_all.sh > /tmp/iwl_b.log 2>&1
grep -qiE 'error:|undefined reference' /tmp/iwl_b.log && { echo "  userspace ERR"; grep -iE 'error:|undefined reference' /tmp/iwl_b.log | head; PASS=0; }
[ -f /tmp/iwlup.elf ] && echo "  iwlup built OK" || { echo "  iwlup ELF MISSING"; PASS=0; }
grep -aE 'IWL-FW: (staged|no firmware)' /tmp/iwl_b.log | head -1
SER=build_test/iwl_integ_ser.log; rm -f "$SER"
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 -serial "file:$SER" -display none -no-reboot 2>/dev/null
grep -aE 'IWL:|IWL-FW:' "$SER" | head
{ grep -qaE 'IWL: no Intel WiFi card found' "$SER" && grep -qaE 'IWL-FW: PASS' "$SER" && grep -qaE 'All services started' "$SER"; } || { echo "  boot FAIL (no-card/fw/desktop)"; PASS=0; }
# the radio bring-up MUST be held -- iwl_trans_bringup never runs at boot:
if grep -qaE 'IWLTRANS: bring-up START' "$SER"; then echo "  CRITICAL: bring-up RAN at boot (must be held!)"; PASS=0; else echo "  bring-up correctly HELD (no boot-time radio power-up)"; fi

echo "=== [2/2] default (no flags): trigger glue + uapi flag don't regress ==="
bash scripts/quick_build.sh > /tmp/def_q.log 2>&1
grep -qiE 'error:|^FAIL:' /tmp/def_q.log && { echo "  default BUILD ERR"; grep -iE 'error:|FAIL:' /tmp/def_q.log | head; PASS=0; }
grep -q SUCCESS /tmp/def_q.log || { echo "  default no SUCCESS"; PASS=0; }
bash scripts/build_all.sh > /tmp/def_b.log 2>&1
SER2=build_test/iwl_def_ser.log; rm -f "$SER2"
timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 -netdev user,id=n0 -device e1000,netdev=n0 -serial "file:$SER2" -display none -no-reboot 2>/dev/null
grep -qaE 'All services started' "$SER2" || { echo "  default desktop FAIL"; PASS=0; }
if grep -qaE 'IWL: |IWL-FW:' "$SER2"; then echo "  WARN: iwl markers in a default build (gating leak?)"; PASS=0; else echo "  default build clean of iwl (gating correct)"; fi

echo ""
[ "$PASS" = 1 ] && echo "IWL-INTEG: PASS (real DVM driver compiles+links, boots HELD, trigger wired, default unregressed)" || echo "IWL-INTEG: FAIL (see above)"
[ "$PASS" = 1 ]
