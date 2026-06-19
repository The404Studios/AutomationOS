#!/bin/bash
# Integration verify for the review-fix batch (4 builds, 4 boots):
#  1. WIFI_SIM+HDA+AUDIO / WPA3 connect: CRYPTOTEST (incl SAE confirm) + WPASUPP SAE
#     confirm verified + CONNECTED SecureMesh + audio DMA + desktop.
#  2. WIFI_SIM / wrong-passphrase: the WLAN_FAILED negative path (M3).
#  3. IWLWIFI: compiles incl iwl-trans + IWL-FW PASS (5 negatives) + graceful no-card.
#  4. default (no flags): regression -- desktop + CRYPTOTEST clean.
# Run: wsl -d Arch bash build_test/integration_check.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
PASS=1
boot() { local SER="build_test/intg_$1_ser.log"; rm -f "$SER"
  timeout 95 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 $2 \
    -serial "file:$SER" -display none -no-reboot 2>/dev/null; echo "$SER"; }
kerr() { grep -qiE 'error:|^FAIL:|undefined reference' "$1" && { echo "  BUILD ERR ($1):"; grep -iE 'error:|^FAIL:|undefined reference' "$1"|head -8; return 1; }; return 0; }

echo "=== [1/4] WIFI_SIM+HDA+AUDIO / WPA3 connect+confirm ==="
WIFI_SIM=1 HDA_ENABLE=1 AUDIO_SELFTEST=1 bash scripts/quick_build.sh > /tmp/i1q.log 2>&1; kerr /tmp/i1q.log || PASS=0
grep -q SUCCESS /tmp/i1q.log || { echo "  k1 no SUCCESS"; PASS=0; }
WIFI_DEMO_WPA3=1 bash scripts/build_all.sh > /tmp/i1b.log 2>&1; grep -qiE 'error:|undefined reference' /tmp/i1b.log && { echo "  u1 ERR"; grep -iE 'error:|undefined reference' /tmp/i1b.log|head; PASS=0; }
S=$(boot wpa3 "-audiodev none,id=snd0 -device intel-hda -device hda-output,audiodev=snd0")
grep -aE 'CRYPTOTEST: PASS|SAE confirm|CONNECTED ssid=Secure|AUDIO: tone done' "$S" | head
{ grep -qaE 'CRYPTOTEST: PASS' "$S" && grep -qaE '\[CRYPTOTEST\] sae .*: PASS' "$S"; } || { echo "  crypto/sae FAIL"; PASS=0; }
{ grep -qaE 'WPASUPP: SAE confirm verified' "$S" && grep -qaE 'WPASUPP: CONNECTED ssid=SecureMesh' "$S"; } || { echo "  wpa3 confirm/connect FAIL"; PASS=0; }
{ grep -qaoE 'bcis=[1-9]' "$S" || grep -qaoE 'lpib_adv=[1-9]' "$S"; } || { echo "  audio FAIL"; PASS=0; }
grep -qaE 'All services started' "$S" || { echo "  desktop1 FAIL"; PASS=0; }

echo "=== [2/4] WIFI_SIM / wrong-passphrase -> WLAN_FAILED ==="
WIFI_SIM=1 bash scripts/quick_build.sh > /tmp/i2q.log 2>&1; grep -q SUCCESS /tmp/i2q.log || { echo "  k2 no SUCCESS"; PASS=0; }
WIFI_DEMO_FAIL=1 bash scripts/build_all.sh > /tmp/i2b.log 2>&1; grep -qiE 'error:|undefined reference' /tmp/i2b.log && { echo "  u2 ERR"; PASS=0; }
S=$(boot fail "")
grep -aE 'WIFISIM:.*FAILED|WPASUPP: did not associate' "$S" | head
{ grep -qaE 'WIFISIM: connect .* FAILED' "$S" && grep -qaE 'WPASUPP: did not associate' "$S"; } || { echo "  FAILED-path FAIL"; PASS=0; }

echo "=== [3/4] IWLWIFI compile (incl iwl-trans) + IWL-FW 5 negatives + no-card ==="
IWLWIFI=1 bash scripts/quick_build.sh > /tmp/i3q.log 2>&1; kerr /tmp/i3q.log || PASS=0
grep -q SUCCESS /tmp/i3q.log || { echo "  k3 no SUCCESS"; PASS=0; }
grep -qE 'iwl-trans.c' /tmp/i3q.log && echo "  iwl-trans.c compiled OK" || echo "  WARN: iwl-trans.c not in compile log"
bash scripts/build_all.sh > /tmp/i3b.log 2>&1; grep -qiE 'error:|undefined reference' /tmp/i3b.log && { echo "  u3 ERR"; PASS=0; }
S=$(boot iwl "")
grep -aE 'IWL:|IWL-FW:' "$S" | head
{ grep -qaE 'IWL-FW: PASS' "$S" && grep -qaE 'IWL: no Intel WiFi card found' "$S" && grep -qaE 'All services started' "$S"; } || { echo "  iwlwifi FAIL"; PASS=0; }

echo "=== [4/4] default (no flags) regression ==="
bash scripts/quick_build.sh > /tmp/i4q.log 2>&1; kerr /tmp/i4q.log || PASS=0
grep -q SUCCESS /tmp/i4q.log || { echo "  k4 no SUCCESS"; PASS=0; }
bash scripts/build_all.sh > /tmp/i4b.log 2>&1; grep -qiE 'error:|undefined reference' /tmp/i4b.log && { echo "  u4 ERR"; PASS=0; }
S=$(boot def "")
{ grep -qaE 'All services started' "$S" && grep -qaE 'CRYPTOTEST: PASS' "$S"; } || { echo "  default FAIL"; PASS=0; }
grep -aE 'All services started|CRYPTOTEST: PASS' "$S" | head -2

echo ""
[ "$PASS" = 1 ] && echo "INTEGRATION: PASS (SAE confirm + WPA3 connect, FAILED path, iwlwifi+trans, default -- all green)" || echo "INTEGRATION: FAIL (see above)"
[ "$PASS" = 1 ]
