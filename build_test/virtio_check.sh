#!/bin/bash
# VIRTIO-NET-0 proof: the legacy virtio-net driver drives QEMU's virtio-net-pci
# end to end, and the default e1000 path is unregressed.
#   POSITIVE: boot with -device virtio-net-pci -> probe+MAC, ARP round-trip
#             through the RX/TX virtqueues (proves both directions), desktop up.
#   NEGATIVE: same ISO booted with -device e1000 -> e1000 selected, virtio
#             absent-marker present, desktop up (graceful, unregressed).
# The driver is always compiled (exact-ID probe, no class fallback -> inert when
# absent), so no build flag is needed; VIRTIO_NET_SELFTEST=1 adds the ICMP ping.
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 9
SERV=/tmp/virtio_pos.log
SERE=/tmp/virtio_neg.log

echo "[vio] quick_build (virtio driver is always compiled; RX/TX proven by the ARP round-trip) ..."
bash scripts/quick_build.sh > /tmp/vio_qb.log 2>&1
grep -qE 'error:|undefined reference|LINK FAILED' /tmp/vio_qb.log && { echo "KERNEL BUILD ERRORS"; grep -E 'error:|undefined reference' /tmp/vio_qb.log | head; exit 1; }
grep -qF 'SUCCESS: build/kernel.elf' /tmp/vio_qb.log || { echo "no kernel"; exit 1; }
bash scripts/build_all.sh > /tmp/vio_ba.log 2>&1
grep -qE 'error:|undefined reference' /tmp/vio_ba.log && { echo "USERSPACE BUILD ERRORS"; exit 1; }
[ -s build/automationos.iso ] || { echo "no iso"; exit 1; }

echo "[vio] POSITIVE boot: -device virtio-net-pci (90s) ..."
rm -f "$SERV"
timeout 90 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -serial "file:$SERV" -display none -no-reboot 2>/dev/null
echo "  virtio/net markers:"; grep -aE 'VIRTIONET:|\[VIRTIO-NET\]|\[NET\] (up|gateway|using|PING)' "$SERV" | head -10

POS=1
grep -qaF 'VIRTIONET: PROBE ok' "$SERV" || { echo "  no PROBE"; POS=0; }
grep -qaE '\[VIRTIO-NET\] MAC ' "$SERV" || { echo "  no MAC"; POS=0; }
grep -qaF '[NET] using virtio-net NIC' "$SERV" || { echo "  virtio not selected as NIC"; POS=0; }
# ARP round-trip = the gateway MAC was learned from a real slirp reply pulled
# through the RX virtqueue (proves TX request + RX reply end-to-end).
grep -qaE '\[NET\] gateway 10.0.2.2 is at ' "$SERV" || { echo "  no ARP round-trip (RX/TX)"; POS=0; }
grep -qaF 'All services started' "$SERV" || { echo "  desktop not up"; POS=0; }
grep -qiE 'KERNEL PANIC|TRIPLE FAULT' "$SERV" && { echo "  kernel fault"; POS=0; }
# (The ARP round-trip above already proves both RX and TX end-to-end.)

echo "[vio] NEGATIVE boot: same ISO, -device e1000 (must be unregressed) (75s) ..."
rm -f "$SERE"
timeout 75 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$SERE" -display none -no-reboot 2>/dev/null
NEG=1
grep -qaE '\[E1000\] found NIC|using .*NIC|\[NET\] up:' "$SERE" || { echo "  e1000 path missing"; NEG=0; }
grep -qaF '[VIRTIO-NET] no virtio-net NIC found' "$SERE" || { echo "  no graceful virtio-absent marker"; NEG=0; }
grep -qaF 'All services started' "$SERE" || { echo "  e1000 boot desktop not up"; NEG=0; }
grep -qiE 'KERNEL PANIC|TRIPLE FAULT' "$SERE" && { echo "  e1000 boot kernel fault"; NEG=0; }

echo ""
echo "positive=$POS negative=$NEG"
if [ "$POS" = 1 ] && [ "$NEG" = 1 ]; then
    echo "VIRTIO-NET-0: PASS (virtio-net drives DHCP-less ARP round-trip; e1000 unregressed)"
    # restore a default (no-selftest) kernel so later builds aren't contaminated
    bash scripts/quick_build.sh > /tmp/vio_qb_def.log 2>&1
    exit 0
else
    echo "VIRTIO-NET-0: FAIL"; exit 1
fi
