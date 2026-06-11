#!/usr/bin/env python3
"""Boot the SMP_SCHED kernel under QEMU -smp 2, let the desktop come up, and
screendump it to build/smp_desktop.png. Proves the SMP scheduler-foundation kernel
(CPU1 with its own TSS/syscall-stack/runqueue/LAPIC-timer) still boots the full
userspace desktop on the BSP."""
import socket, subprocess, time, os
ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_smp.sock"
PPM  = "/tmp/smp_desktop.ppm"
PNG  = f"{ROOT}/build/smp_desktop.png"
SER  = "/tmp/smp_serial.log"
for f in (MON, PPM):
    try: os.remove(f)
    except OSError: pass
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512", "-smp", "2",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none",
     "-no-reboot", "-serial", f"file:{SER}"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(40):
    time.sleep(0.5)
    if os.path.exists(MON): break
time.sleep(24)                                  # boot + compositor render
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(MON)
time.sleep(0.3)
try: s.recv(8192)
except OSError: pass
s.sendall(f"screendump {PPM}\n".encode()); time.sleep(1.8)
s.close()
time.sleep(1.0)
qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
ok = False
if os.path.exists(PPM):
    try:
        from PIL import Image; Image.open(PPM).save(PNG); ok = True
    except Exception:
        ok = (os.system(f"pnmtopng '{PPM}' > '{PNG}' 2>/dev/null") == 0)
print("screenshot:", "OK" if ok else "FAILED", PNG if ok else "")
