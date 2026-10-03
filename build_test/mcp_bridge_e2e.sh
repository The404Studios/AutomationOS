#!/bin/bash
# mcp_bridge_e2e.sh -- full-stack proof: MCP client -> bridge -> agentd (guest, serve mode) -> kernel.
#
#   wsl -d Arch bash /mnt/c/Users/wilde/Desktop/Kernel/build_test/mcp_bridge_e2e.sh            # build + run
#   NOBUILD=1 ... mcp_bridge_e2e.sh                                                              # reuse the ISO
#
# Builds the LEAN desktop profile with AGENTD_SERVE=1 (the T410-shaped profile plus the tool host),
# starts the bridge (loopback only, random token), boots QEMU with hostfwd tcp 18080 -> guest 8080,
# and runs build_test/mcp_bridge_e2e.py. Prints MCP-E2E: PASS/FAIL.
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=/tmp/mcp_e2e_ser.log
BLOG=/tmp/mcp_e2e_bridge.log
rm -f "$SER" "$BLOG"

if [ "${NOBUILD:-0}" != "1" ]; then
    echo "[e2e] building (DESKTOP_MINIMAL=1 AGENTD_SERVE=1)..."
    bash scripts/quick_build.sh > /tmp/mcp_e2e_kernel.log 2>&1
    if grep -qE "^FAIL|error:|undefined reference" /tmp/mcp_e2e_kernel.log; then
        echo "[e2e] KERNEL BUILD ERRORS"; grep -E "^FAIL|error:|undefined reference" /tmp/mcp_e2e_kernel.log | head -5; exit 1; fi
    DESKTOP_MINIMAL=1 AGENTD_SERVE=1 E2E_HTTPD=1 bash scripts/build_all.sh > /tmp/mcp_e2e_all.log 2>&1
    if grep -qE "error:|undefined reference" /tmp/mcp_e2e_all.log; then
        echo "[e2e] BUILD ERRORS"; grep -E "error:|undefined reference" /tmp/mcp_e2e_all.log | head -5; exit 1; fi
    tail -1 /tmp/mcp_e2e_all.log
fi

export CL_BRIDGE_TOKEN="$(head -c 24 /dev/urandom | base64 | tr -d '+/=' | head -c 32)"
python3 scripts/chainlayer_mcp_bridge.py --allow-control --listen-port 8940 --os-host 127.0.0.1 --os-port 8433 > "$BLOG" 2>&1 &
BR=$!
sleep 1

qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:18080-:8080" -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot >/dev/null 2>&1 &
QP=$!

MCP_URL=http://127.0.0.1:8940/mcp SERIAL_LOG="$SER" PROBE_PORT=18080 python3 build_test/mcp_bridge_e2e.py
RC=$?

kill $QP $BR 2>/dev/null
echo "---- bridge log"; tail -12 "$BLOG"
echo "---- serial markers"; grep -a -E "AGENTD|LEDGER|CAPD=|\[FW\]" "$SER" | grep -v -E "POLICY-DROP|BAD-DROP" | cut -c1-140 | tail -25
exit $RC
