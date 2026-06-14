#!/usr/bin/env bash
# pollselect_smoke.sh -- POLL-SELECT-0 (B10) proof gate.
#
# Boots the default ISO headless and asserts poll(2)/select(2) over the REAL
# fd-readiness probe + epoll level/edge triggering, via sbin/pollselftest
# (spawned by init), and that the kernel stays alive (desktop comes up).
#
# Proves end to end:
#   [1] poll() reports a ready (readable) regular file
#   [2] select() reports the same ready file
#   [3] poll() honors a timeout on an idle (unconnected) socket
#   [4] poll() over a mixed set distinguishes ready (file) from not-ready (socket)
#   [5] epoll LEVEL re-reports a still-ready fd; EDGE reports it once
#
# Usage: bash scripts/pollselect_smoke.sh
set -u
cd /mnt/c/Users/wilde/Desktop/Kernel
ISO="build/automationos.iso"
LOG="/tmp/pollselect_smoke.log"
rm -f "$LOG"

if [[ ! -f "$ISO" ]]; then
    echo "POLLSELECT: FAIL reason=no_iso ($ISO missing -- run scripts/build_all.sh)"
    exit 1
fi

timeout 50 qemu-system-x86_64 \
    -cdrom "$ISO" -m 512 \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 1

if [[ ! -s "$LOG" ]]; then echo "POLLSELECT: FAIL reason=no_serial_log"; exit 1; fi

c1=$(grep -cF '[1] PASS poll_ready_file=1'              "$LOG")
c2=$(grep -cF '[2] PASS select_ready_file=1'            "$LOG")
c3=$(grep -cF '[3] PASS poll_timeout_idle_socket=1'     "$LOG")
c4=$(grep -cF '[4] PASS mixed_file_ready_socket_not=1'  "$LOG")
c5=$(grep -cF '[5] PASS epoll_level_rereports=1 epoll_edge_once=1' "$LOG")
res=$(grep -cF 'POLLSELFTEST RESULT: PASS'              "$LOG")

panic=0
for pat in 'KERNEL PANIC' 'triple fault' 'double fault' 'schedule() returned after'; do
    panic=$((panic + $(grep -icF "$pat" "$LOG")))
done
desktop=$(grep -cE "COMP. fps window" "$LOG")

echo "--- pollselect_smoke ---"
echo "check1_poll_ready=$c1 check2_select_ready=$c2 check3_timeout=$c3"
echo "check4_mixed=$c4 check5_epoll_level_edge=$c5 result_pass=$res"
echo "kernel_panic=$panic desktop_up=$desktop"
echo "--- pollselftest serial lines ---"
grep -F 'POLLSELFTEST' "$LOG" || true

if [[ "$c1" -ge 1 && "$c2" -ge 1 && "$c3" -ge 1 && "$c4" -ge 1 && "$c5" -ge 1 \
   && "$res" -ge 1 && "$panic" -eq 0 && "$desktop" -ge 1 ]]; then
    echo "POLLSELECT: PASS poll_ready=1 select_ready=1 poll_timeout=1 mixed_fd=1 epoll_level=1 epoll_edge=1"
    exit 0
else
    echo "POLLSELECT: FAIL (see counts above)"
    exit 1
fi
