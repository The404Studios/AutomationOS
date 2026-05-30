#!/usr/bin/env python3
"""Boot the ISO headless and capture desktop screenshots via the QEMU monitor.

Captures two frames: a plain desktop and one with the cursor pushed to the
right-side dock (to show the hover-magnify). Outputs PNGs into build/.
Run: wsl -d Arch bash -lc 'cd /mnt/c/Users/wilde/Desktop/Kernel && python3 scripts/capture_shot.py'
"""
import socket, subprocess, time, os, shutil

ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon.sock"
PPM1 = "/tmp/desktop.ppm"
PPM2 = "/tmp/desktop_hover.ppm"
PNG1 = f"{ROOT}/build/desktop_m8.png"
PNG2 = f"{ROOT}/build/desktop_m8_hover.png"
RENDER_WAIT = float(os.environ.get("RENDER_WAIT", "16"))

for f in (MON, PPM1, PPM2):
    try: os.remove(f)
    except OSError: pass

qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none", "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# Wait for boot + compositor to render.
for _ in range(int(RENDER_WAIT * 2)):
    time.sleep(0.5)
    if os.path.exists(MON):
        pass

def mon():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON)
    time.sleep(0.3)
    try: s.recv(8192)
    except OSError: pass
    return s

def cmd(s, c):
    s.sendall((c + "\n").encode())
    time.sleep(0.5)
    try: return s.recv(8192)
    except OSError: return b""

s = mon()
cmd(s, f"screendump {PPM1}")
time.sleep(1.0)
# Park cursor at top-right (over the right-side dock) to trigger hover-magnify.
cmd(s, "mouse_move 4000 0")
cmd(s, "mouse_move 0 -4000")
cmd(s, "mouse_move 0 300")
time.sleep(1.5)
cmd(s, f"screendump {PPM2}")
time.sleep(1.0)
s.close()
qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()

def to_png(ppm, png):
    if not os.path.exists(ppm):
        print("MISSING", ppm); return False
    try:
        from PIL import Image
        Image.open(ppm).save(png); return True
    except Exception:
        if shutil.which("pnmtopng"):
            return os.system(f"pnmtopng '{ppm}' > '{png}'") == 0
        if shutil.which("convert"):
            return os.system(f"convert '{ppm}' '{png}'") == 0
        print("no PNG converter (need python-pillow or netpbm or imagemagick)")
        return False

print("shot1:", to_png(PPM1, PNG1))
print("shot2:", to_png(PPM2, PNG2))
