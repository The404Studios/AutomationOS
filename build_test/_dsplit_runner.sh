#!/bin/bash
# DESKTOP-SPLIT-0 detached runner: world rebuild + the full dsplit smoke
# (baseline boot + 30-minute soak). Writes everything to /tmp/ds_run.log and
# ends with the DSPLIT_RUN_COMPLETE sentinel the session monitors for.
cd /mnt/c/Users/wilde/Desktop/Kernel
IDE=1 bash scripts/build_all.sh > /tmp/ds_ba.log 2>&1
echo "world_errors=$(grep -cE 'error:|undefined reference' /tmp/ds_ba.log)" > /tmp/ds_run.log
bash scripts/dsplit_smoke.sh >> /tmp/ds_run.log 2>&1
echo DSPLIT_RUN_COMPLETE >> /tmp/ds_run.log
