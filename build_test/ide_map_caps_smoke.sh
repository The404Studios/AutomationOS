#!/bin/bash
# IDE-MAP-CAPS proof: the Semantic LEGO Map renders REAL game sources COMPLETELY
# (no silent truncation). Builds the host-side map_caps_check (same engine TUs as
# the on-device IDE: lexer + AST + recursive-descent parser + ide_parse model
# builder), runs it on deadzone.c + other real apps, and asserts every file maps
# with ZERO arrays at their cap. Before the cap raise, deadzone.c truncated funcs
# (64/64), globals (64/64) and macros (32/32); after, 66/75/89 all fit (cap 128).
# Run: wsl -d Arch bash build_test/ide_map_caps_smoke.sh
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
IDE=userspace/apps/ide
BIN=/tmp/map_caps_check

echo "[mapcaps] building host harness..."
gcc -I "$IDE" -w -o "$BIN" \
   "$IDE/test/map_caps_check.c" "$IDE/ide_parse.c" "$IDE/ide_pcore.c" \
   "$IDE/ide_pdecl.c" "$IDE/ide_pstmt.c" "$IDE/ide_pexpr.c" \
   "$IDE/ide_lex.c" "$IDE/ide_ast.c" 2> /tmp/mapcaps_build.log
if [ ! -x "$BIN" ]; then echo "HARNESS BUILD FAILED"; cat /tmp/mapcaps_build.log; exit 1; fi

APPS="userspace/apps/deadzone/deadzone.c
userspace/apps/derby/derby.c
userspace/apps/dzclient/dzclient.c
userspace/apps/ide/sample/towerdefense/tower.c"

fails=0
echo ""
while IFS= read -r app; do
  [ -f "$app" ] || continue
  out=$("$BIN" "$app" 2>&1); rc=$?
  trunc=$(printf '%s' "$out" | grep -c TRUNCATED)
  corrupt=$(printf '%s' "$out" | grep -c 'CORRUPT MODEL')
  verdict=$(printf '%s' "$out" | grep '=>' | sed 's/=> //')
  if [ "$rc" -ge 2 ]; then verdict="*** CRASH/ERROR rc=$rc ***"; fi
  printf "  %-48s trunc=%s corrupt=%s  %s\n" "$(basename "$app")" "$trunc" "$corrupt" "$verdict"
  if [ "$trunc" -ne 0 ] || [ "$corrupt" -ne 0 ] || [ "$rc" -ge 2 ]; then fails=$((fails+1)); fi
done <<< "$APPS"

echo ""
echo "--- deadzone.c map detail (completeness + parser correctness) ---"
"$BIN" userspace/apps/deadzone/deadzone.c | grep -E 'funcs |globals |macros |tokens|camera_view|correct names|phantom \(|_start edges|=>'

echo ""
if [ "$fails" -eq 0 ]; then
  echo "IDE-MAP-CAPS: PASS (all real sources map COMPLETELY + CORRECTLY -- no truncation, no parser corruption)"
  exit 0
else
  echo "IDE-MAP-CAPS: FAIL ($fails file(s) truncated/corrupt/crashed)"
  exit 1
fi
