#!/bin/bash
# Commit the two iwlwifi bricks (file-based to avoid wsl -lc var/quote mangling).
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
set -e
I=kernel/drivers/net/wireless/intel/iwlwifi

# --- Brick 1: the real DVM driver core ---
git add \
  "$I/iwl-trans.c" "$I/iwl-trans.h" "$I/iwl-dvm-commands.h" \
  "$I/iwl-fw-load.c" "$I/iwl-fw-load.h" "$I/iwl-hostcmd.c" "$I/iwl-hostcmd.h" \
  "$I/iwl-nvm.c" "$I/iwl-nvm.h" "$I/iwl-ops.c" "$I/iwl-scan.c" "$I/iwl-scan.h" \
  scripts/quick_build.sh build_test/iwl_integ_check.sh
git commit -q \
  -m "feat(wifi): IWL-LOAD + IWL-OPS -- real Intel iwlwifi DVM driver core (held, multi-reviewed)" \
  -m "Makes an IWLWIFI=1 build register a REAL wlan0 (the #error guard forbids WIFI_SIM, so this is the un-simulated radio). iwl_wifi_bringup() chains iwl_trans_bringup to iwl_load_ucode (FH service-channel DMA, INIT ALIVE, calibration, RUNTIME ALIVE) to iwl_read_nvm (MAC/channels) to iwl_scan to netif_register wlan0 behind the same wifi_ops seam as wifisim. Targets the iwldvm (AGN) API since the T410 cards (1000/5000/6000) are DVM. Cited line-by-line vs Linux v5.10; every poll iteration-capped, marker before every risky MMIO, abort-clean, HELD (called only by the post-desktop trigger, never at boot)." \
  -m "A 4-agent adversarial review vs Linux caught 7 CRITICAL firmware-protocol bugs, all fixed here: command queue 0 to 4 (DVM services queue 4) plus the missing SCD byte-count/scheduler bring-up (iwl_scd_cmd_queue_init, scd_base read from the device); the uCode-load SRAM dest was right-shifted by 4 (now unshifted) and DMA-done was polled on BIT(9) the HW never sets (now CSR_FH_INT_TX_MASK = BIT0 or BIT1); calibration was a zeroed stub (now real CALIBRATION_CFG with SEND_COMPLETE, capture of the 0x66 results, and replay as REPLY_PHY_CALIBRATION_CMD post-ALIVE); the EEPROM ownership semaphore was missing and OTP read unimplemented (both added); the scan command head size was 200 vs the real 768; plus RX RBD re-post, ALIVE subtype check, and the swapped sequence field. IWL-TRANS also got two earlier-review CRITICALs (cmd-ring was 4KB vs the needed 32KB/8 pages; the prepare-card-hw NIC ownership handshake was missing). Honest TODO(HARDWARE-VALIDATE) only on family-specific values (RSSI AGC dword layout, OTP block size) with safe fallbacks. Verified: IWLWIFI=1 compiles and LINKS and boots HELD (graceful no-card on QEMU), default build unregressed." \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
echo "brick1: $(git rev-parse --short HEAD)"

# --- Brick 2: the trigger + firmware staging ---
git add userspace/apps/iwlup kernel/net/netsyscall.c kernel/include/uapi/net.h scripts/build_all.sh
git commit -q \
  -m "feat(wifi): IWL-TRIGGER -- post-desktop iwlwifi bring-up (iwlup) + firmware staging" \
  -m "Mirrors the proven e1000-PCH deferred-bringup pattern: iwlup (a /bin tool) fires the real-radio bring-up via SYS_NET_CONFIG NET_CONFIG_FLAG_WLAN_BRINGUP, which calls iwl_wifi_bringup() under ifdef IWLWIFI (clean ENOTSUP no-op otherwise). Never at boot, abort-clean on any stall (a miss costs a re-run, never a wedged machine). build_all stages firmware into the initrd at /lib/firmware/ (versioned blobs plus a stable iwlwifi.ucode alias the driver loads); the redistributable Intel blob is user-provided per docs/T410_IWLWIFI.md -- absent, the bring-up reports missing firmware and aborts clean. Verified: iwlup builds, the trigger is wired and held, the QEMU no-card path is graceful." \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
echo "brick2: $(git rev-parse --short HEAD)"
echo "uncommitted iwl/trigger left: $(git status --porcelain "$I" userspace/apps/iwlup kernel/net/netsyscall.c | wc -l)"
