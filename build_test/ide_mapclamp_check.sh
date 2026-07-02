#!/bin/bash
# IDE-MAPCLAMP-0 proof: the pure ide_map_clamp_oy() bounds the overview pan so
# it can't scroll past the tile grid into blank space, AND the IDE still builds
# + boots (ide_prebuilt_smoke) with the render wiring in place.
#
# Before this brick the overview used the raw a->map_oy (arrow/wheel/drag add
# to it with no bound), so you could pan arbitrarily far into empty space. The
# unit KAT below asserts the clamp; the render call is at ide_map.c's overview
# scroll_y setup; ide_prebuilt_smoke proves no regression.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 9

echo "[mc] unit KAT of ide_map_clamp_oy (host) ..."
cat > /tmp/mapclamp_kat.c <<'EOF'
#include <stdio.h>
#include "userspace/apps/ide/ide_model.h"
static int fails = 0;
#define CK(cond,msg) do{ if(!(cond)){printf("  FAIL %s\n",msg);fails++;} else printf("  ok   %s\n",msg);}while(0)
int main(void){
    /* content fits the view -> no scroll (pinned to top). */
    CK(ide_map_clamp_oy(0,   100, 400) == 0, "fits: 0 stays 0");
    CK(ide_map_clamp_oy(-50, 100, 400) == 0, "fits: over-pan clamps to top 0");
    /* content taller than view: min = -(content-view) = -(1000-400) = -600. */
    CK(ide_map_clamp_oy(0,    1000, 400) == 0,    "tall: top is 0");
    CK(ide_map_clamp_oy(-300, 1000, 400) == -300, "tall: legal mid pan preserved");
    CK(ide_map_clamp_oy(-600, 1000, 400) == -600, "tall: exact bottom preserved");
    CK(ide_map_clamp_oy(-999, 1000, 400) == -600, "tall: OVER-pan clamps to bottom (no blank space)");
    CK(ide_map_clamp_oy(50,   1000, 400) == 0,    "positive oy clamps to top");
    if(fails){ printf("IDE-MAPCLAMP KAT: FAIL n=%d\n",fails); return 1; }
    printf("IDE-MAPCLAMP KAT: PASS\n"); return 0;
}
EOF
gcc -std=gnu11 -O2 -I "$PWD" -o /tmp/mapclamp_kat /tmp/mapclamp_kat.c && /tmp/mapclamp_kat
KAT=$?
[ "$KAT" = 0 ] || { echo "[mc] unit KAT failed"; exit 1; }

echo "[mc] the overview render wires the clamp (source guard) ..."
grep -qF 'ide_map_clamp_oy(a->map_oy' userspace/apps/ide/ide_map.c || { echo "[mc] render does not call the clamp"; exit 1; }
echo "  ok   ide_map.c overview calls ide_map_clamp_oy"

echo "[mc] IDE builds + boots unregressed (ide_prebuilt_smoke) ..."
bash build_test/ide_prebuilt_smoke.sh > /tmp/mc_ide.log 2>&1
tail -1 /tmp/mc_ide.log
grep -qE 'IDE-PREBUILT PASS|PASS' /tmp/mc_ide.log || { echo "[mc] ide_prebuilt_smoke FAIL"; exit 1; }

echo ""
echo "IDE-MAPCLAMP-0: PASS (clamp KAT + render wiring + IDE unregressed)"
