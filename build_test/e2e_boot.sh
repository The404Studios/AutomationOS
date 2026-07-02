#!/bin/bash
# FS-E2E-BOOT-0: full end-to-end boot proof --
#   kernel -> virtio-net online -> mount initrd -> load /sbin/init (ELF) ->
#   init spawns sbin/e2etest which: (1) VFS ls /bin, (2) loads+runs
#   /bin/hello.elf, (3) drives tool_shell to run a command.
#
# Builds the kernel (default, already hardened) + an E2E=1 initrd (init compiled
# -DE2E_TEST; hello_e2e + e2etest packaged), boots QEMU with a LEGACY virtio-net
# NIC, and asserts every link of the chain from the serial log.
#
# Run: wsl -d Arch bash build_test/e2e_boot.sh
set -uo pipefail
cd /mnt/c/Users/wilde/Desktop/Kernel

echo "[e2e] quick_build (kernel)..."
bash scripts/quick_build.sh > /tmp/e2e_qb.log 2>&1
grep -q SUCCESS /tmp/e2e_qb.log || { echo "[e2e] KERNEL BUILD FAILED"; tail -8 /tmp/e2e_qb.log; exit 1; }

echo "[e2e] build_all E2E=1 (initrd with e2etest + /bin/hello.elf)..."
E2E=1 bash scripts/build_all.sh > /tmp/e2e_ba.log 2>&1
if grep -qiE 'error:|undefined reference' /tmp/e2e_ba.log; then
    echo "[e2e] INITRD BUILD ERRORS:"; grep -iE 'error:|undefined reference' /tmp/e2e_ba.log | head; exit 1
fi
grep -qF 'E2E: packaged' /tmp/e2e_ba.log || { echo "[e2e] E2E binaries were not packaged"; exit 1; }
[ -s build/automationos.iso ] || { echo "[e2e] no iso"; exit 1; }

LOG=/tmp/e2e_serial.log
rm -f "$LOG"
echo "[e2e] booting (60s, legacy virtio-net NIC)..."
timeout 60 qemu-system-x86_64 -cdrom build/automationos.iso -m 512 \
    -netdev user,id=n0 -device virtio-net-pci,disable-modern=on,netdev=n0 \
    -serial "file:$LOG" -display none -no-reboot >/dev/null 2>&1 || true
sleep 2

echo "=== chain markers ==="
grep -aE 'VIRTIO-NET\] init complete|VIRTIONET: PROBE|\[INIT\].*PID 1|E2E_TEST: running|E2E-LS:|E2E-HELLO:|E2E-ELF:|E2E-SHELL:|SHELL hello exit=|E2E: ALL DONE' "$LOG"

P=1
chk() { if grep -qaE "$1" "$LOG"; then echo "  PASS  $2"; else echo "  FAIL  $2"; P=0; fi; }

echo "=== assertions ==="
chk 'VIRTIO-NET\] init complete|VIRTIONET: PROBE ok'      "1) virtio-net online"
chk '\[INIT\].*(PID 1|Init \(PID 1\))'                     "   init loaded from initrd (ELF) + running as PID 1"
chk 'E2E-LS: PASS ls /bin entries='                        "2) VFS: mount initrd + ls /bin"
chk 'E2E-HELLO: hello from /bin/hello.elf'                 "   /bin/hello.elf actually executed in ring-3"
chk 'E2E-ELF: PASS loaded\+ran /bin/hello.elf'             "3) ELF loader: load + run /bin/hello.elf"
chk 'SHELL hello exit=0'                                   "   shell (tool_shell) ran the command"
chk 'E2E-SHELL: PASS'                                      "4) Shell: run a command by name"
chk 'E2E: ALL DONE'                                        "   full chain reached the end"

# Real kernel faults fail the run (a HANDLED ring-3 fault that terminates the
# faulting process is fine; an unhandled kernel-context fault is not).
if grep -qaiE 'KERNEL PANIC|TRIPLE FAULT' "$LOG"; then echo "  FAIL  no kernel fault"; P=0; fi
exc=$(grep -aciE 'CPU EXCEPTION' "$LOG"); handled=$(grep -aciE 'Terminating faulting process' "$LOG")
if [ "$exc" -gt "$handled" ]; then echo "  FAIL  unhandled kernel exception (exc=$exc handled=$handled)"; P=0; fi

echo "======================================================"
if [ "$P" = "1" ]; then
    echo "E2E BOOT: PASS -- kernel -> virtio-net -> initrd -> init -> VFS/ELF/shell all proven"
    exit 0
else
    echo "E2E BOOT: FAIL (see $LOG)"; exit 1
fi
