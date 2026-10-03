#!/bin/bash
# dns_lease_check.sh -- DNS-LEASE-0: does the resolver follow the DHCP-learned DNS server?
#
#   wsl -d Arch bash /mnt/c/Users/wilde/Desktop/Kernel/build_test/dns_lease_check.sh        # build + boot
#   NOBUILD=1 ... dns_lease_check.sh                                                          # reuse the ISO
#
# Builds the LEAN profile with DNS_TEST=1 (init spawns sbin/dnstest) and boots QEMU with
# `-netdev user,dns=10.0.2.4` so the DHCP lease is NOT the old hard-wired 10.0.2.3. Before the fix
# dnstest prints "lease=10.0.2.4 using=10.0.2.3 ... FAIL"; after it, "using=10.0.2.4 ... PASS".
cd /mnt/c/Users/wilde/Desktop/Kernel || exit 1
SER=/tmp/dns_lease_ser.log; rm -f "$SER"
if [ "${NOBUILD:-0}" != "1" ]; then
    bash scripts/quick_build.sh > /tmp/dnsl_k.log 2>&1
    grep -qE "^FAIL|error:|undefined reference" /tmp/dnsl_k.log && { grep -E "^FAIL|error:|undefined reference" /tmp/dnsl_k.log | head -5; exit 1; }
    DNS_TEST=1 bash scripts/build_all.sh > /tmp/dnsl_a.log 2>&1
    grep -qE "error:|undefined reference" /tmp/dnsl_a.log && { grep -E "error:|undefined reference" /tmp/dnsl_a.log | head -5; exit 1; }
    tail -1 /tmp/dnsl_a.log
fi
timeout 90 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev "user,id=n0,dns=10.0.2.4" -device e1000,netdev=n0 \
    -serial "file:$SER" -display none -no-reboot >/dev/null 2>&1
grep -a -E "DNSTEST|DHCP|dhcp.*lease|autodhcp" "$SER" | cut -c1-140 | head -12
grep -aq "DNSTEST: PASS" "$SER" && { echo "DNS-LEASE: PASS"; exit 0; }
grep -aq "DNSTEST: FAIL" "$SER" && { echo "DNS-LEASE: FAIL (resolver ignores the lease)"; exit 1; }
echo "DNS-LEASE: NO-RESULT (dnstest did not report)"; exit 2
