#!/bin/bash
# chainlayer_e2e.sh -- ChainLayerTwo's real MCP client -> bridge -> agentd (guest) -> kernel, end to end.
#
#   wsl -d Arch bash /mnt/c/Users/wilde/Desktop/Kernel/build_test/chainlayer_e2e.sh          # build + run
#   NOBUILD=1 ... chainlayer_e2e.sh                                                           # reuse the ISO
#
# Same OS side as build_test/mcp_bridge_e2e.sh (LEAN desktop + AGENTD_SERVE, bridge on loopback with a random
# token, QEMU/slirp so the guest dials the bridge), but the CLIENT is ChainLayerTwo's own code
# (chainlayercli/src/mcp.ts via build_test/chainlayer_client_e2e.mjs), run by the WINDOWS Node that ChainLayerTwo is
# installed with (WSL interop; Windows reaches the WSL-side bridge through localhost forwarding).
# Prints CHAINLAYER-E2E: PASS/FAIL. Nothing in ~/.chainlayer is read or written and the CLI's EULA gate is not involved.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
CL_ROOT="${CHAINLAYER_ROOT:-/mnt/c/Users/wilde/Desktop/New folder (4)/chainlayercli}"
SER=/tmp/cl_e2e_ser.log
BLOG=/tmp/cl_e2e_bridge.log
rm -f "$SER" "$BLOG"

command -v node.exe >/dev/null 2>&1 || { echo "[cl-e2e] node.exe not reachable from WSL (Windows interop off?)"; exit 3; }
[ -f "$CL_ROOT/src/mcp.ts" ] || { echo "[cl-e2e] ChainLayerTwo not found at: $CL_ROOT"; exit 3; }

if [ "${NOBUILD:-0}" != "1" ]; then
    echo "[cl-e2e] building (default kernel + DESKTOP_MINIMAL=1 AGENTD_SERVE=1)..."
    bash scripts/quick_build.sh > /tmp/cl_e2e_kernel.log 2>&1
    if grep -qE "^FAIL|error:|undefined reference" /tmp/cl_e2e_kernel.log; then
        echo "[cl-e2e] KERNEL BUILD ERRORS"; grep -E "^FAIL|error:|undefined reference" /tmp/cl_e2e_kernel.log | head -5; exit 1; fi
    DESKTOP_MINIMAL=1 AGENTD_SERVE=1 bash scripts/build_all.sh > /tmp/cl_e2e_all.log 2>&1
    if grep -qE "error:|undefined reference" /tmp/cl_e2e_all.log; then
        echo "[cl-e2e] BUILD ERRORS"; grep -E "error:|undefined reference" /tmp/cl_e2e_all.log | head -5; exit 1; fi
    tail -1 /tmp/cl_e2e_all.log
fi

TOKEN="$(head -c 24 /dev/urandom | base64 | tr -d '+/=' | head -c 32)"
export CL_BRIDGE_TOKEN="$TOKEN"          # the bridge reads its token from the environment AT STARTUP: export first
python3 scripts/chainlayer_mcp_bridge.py --listen-port 8940 --os-host 127.0.0.1 --os-port 8433 > "$BLOG" 2>&1 &
BR=$!
sleep 1

qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev "user,id=n0" -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot >/dev/null 2>&1 &
QP=$!

# The Windows node must start in chainlayercli so `tsx` and @modelcontextprotocol/sdk resolve from its node_modules.
# Hand Node an explicit WINDOWS path: WSLENV's /w auto-conversion did not translate a path containing spaces and parentheses
# ("New folder (4)") and Node saw `C:/mnt/c/...`.
export BRIDGE_URL="http://127.0.0.1:8940/mcp" BRIDGE_TOKEN="$TOKEN" CHAINLAYER_ROOT="$(wslpath -m "$CL_ROOT")"
export WSLENV="BRIDGE_URL:BRIDGE_TOKEN:CHAINLAYER_ROOT"
SCRIPT_WIN="$(wslpath -w /mnt/c/Users/wilde/Desktop/Kernel/build_test/chainlayer_client_e2e.mjs)"
( cd "$CL_ROOT" && node.exe --import tsx "$SCRIPT_WIN" )
RC=$?

kill $QP $BR 2>/dev/null
echo "---- bridge log"; tail -12 "$BLOG"
echo "---- serial markers"; grep -a -E "AGENTD|LEDGER|CAPD=|\[FW\]" "$SER" | grep -v -E "POLICY-DROP|BAD-DROP" | cut -c1-140 | tail -20
exit $RC
