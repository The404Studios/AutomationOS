#!/usr/bin/env python3
"""Boot headless, enable the compositor stats overlay (Alt+S), jitter the mouse in
the central fast-zone to exercise the smooth-mouse fast path, and screendump so the
FPS/frame-time numbers in the overlay can be read. Outputs build/fps_shot.png."""
import socket, subprocess, time, os, shutil
ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_fps.sock"
PPM  = "/tmp/fps.ppm"
PNG  = f"{ROOT}/build/fps_shot.png"
SMP  = os.environ.get("SMP_QEMU", "")          # e.g. "-smp 2" to test dual-CPU
for f in (MON, PPM):
    try: os.remove(f)
    except OSError: pass
args = ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
        "-monitor", f"unix:{MON},server,nowait", "-display", "none", "-no-reboot"]
if SMP: args += SMP.split()
qemu = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(40):
    time.sleep(0.5)
    if os.path.exists(MON): break
time.sleep(20)                                  # boot + compositor render
def mon():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(MON)
    time.sleep(0.3)
    try: s.recv(8192)
    except OSError: pass
    return s
def cmd(s, c):
    s.sendall((c + "\n").encode()); time.sleep(0.12)
    try: return s.recv(8192)
    except OSError: return b""
s = mon()
cmd(s, "sendkey alt-s")                          # toggle the stats overlay ON
time.sleep(0.4)
# Park near screen center then jitter in the central fast-zone (away from chrome).
cmd(s, "mouse_move -4000 -4000"); cmd(s, "mouse_move 500 350")
for i in range(120):                             # ~continuous movement
    dx = 6 if (i % 2 == 0) else -6
    dy = 4 if (i % 4 < 2) else -4
    cmd(s, f"mouse_move {dx} {dy}")
    time.sleep(0.02)
time.sleep(0.3)
cmd(s, f"screendump {PPM}")
time.sleep(1.0)
s.close(); qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
def to_png(ppm, png):
    if not os.path.exists(ppm): print("MISSING", ppm); return False
    try:
        from PIL import Image; Image.open(ppm).save(png); return True
    except Exception:
        if shutil.which("pnmtopng"): return os.system(f"pnmtopng '{ppm}' > '{png}'") == 0
        return False
print("fps_shot:", to_png(PPM, PNG))
