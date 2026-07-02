#!/usr/bin/env python3
"""GSIGNIN-MOCK-0 zero-cost proof: start the host mock device-flow server, boot
the OS with slirp networking + serial, and verify gsignin completes the full
RFC 8628 device flow against the mock (no Google, no key, no phone)."""
import subprocess, time, os, sys

ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
SER  = f"{ROOT}/build_test/_gsignin_serial.log"
WAIT = float(os.environ.get("WAIT", "75"))

for f in (SER,):
    try: os.remove(f)
    except OSError: pass

mock = subprocess.Popen(["python3", f"{ROOT}/scripts/oauth_mock.py"],
                        stderr=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
time.sleep(1.0)

qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", f"file:{SER}", "-display", "none", "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

time.sleep(WAIT)
qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
mock.terminate()
try: mock.wait(timeout=5)
except subprocess.TimeoutExpired: mock.kill()

lines = []
try:
    with open(SER, "rb") as f:
        for ln in f.read().decode("utf-8", "replace").splitlines():
            if "GSIGNIN" in ln:
                lines.append(ln)
except OSError:
    print("no serial log"); sys.exit(1)

print("=== gsignin device-flow trace ===")
for ln in lines:
    print(ln)
print("=== verdict ===")
print("PASS" if any("GSIGNIN: PASS" in l for l in lines) else "NO-PASS")
