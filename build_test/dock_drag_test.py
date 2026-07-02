#!/usr/bin/env python3
"""DOCK-DND-0 input proof: boot, drive the QEMU monitor mouse to press on a dock
icon, drag UP onto another, and release -- then check the serial for the
[DOCK] drag start / drop logs and screenshot the result (a new box)."""
import socket, subprocess, time, os, shutil

ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_dd.sock"
PPM  = "/tmp/dock_dd.ppm"
PNG  = f"{ROOT}/build/dock_drag.png"
SER  = f"{ROOT}/build_test/_dock_drag_serial.log"
WAIT = float(os.environ.get("RENDER_WAIT", "40"))

for f in (MON, PPM, SER):
    try: os.remove(f)
    except OSError: pass

qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-serial", f"file:{SER}",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none", "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

time.sleep(WAIT)

def mon():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON); time.sleep(0.3)
    try: s.recv(8192)
    except OSError: pass
    return s

def cmd(s, c, wait=0.4):
    s.sendall((c + "\n").encode()); time.sleep(wait)
    try: return s.recv(8192)
    except OSError: return b""

s = mon()
# Park the cursor on the right-side dock (proven corner technique), low down.
cmd(s, "mouse_move 4000 0", 0.6)     # far right edge
cmd(s, "mouse_move 0 -4000", 0.6)    # top
cmd(s, "mouse_move 0 360", 0.6)      # down onto a LOWER dock app icon
time.sleep(0.6)
cmd(s, f"screendump {ROOT}/build/dock_drag_pre.ppm", 0.6)   # confirm cursor-on-dock
# Press, drag UP onto a higher icon, release -> combine into a box.
cmd(s, "mouse_button 1", 0.6)        # left press -> arms the dock icon
cmd(s, "mouse_move 0 -12", 0.4)      # pass the >6px drag threshold
cmd(s, "mouse_move 0 -30", 0.4)
cmd(s, "mouse_move 0 -35", 0.4)      # now hovering a higher icon = drop target
time.sleep(0.4)
cmd(s, "mouse_button 0", 0.8)        # release -> drop
time.sleep(0.8)
cmd(s, f"screendump {PPM}", 0.6)
time.sleep(1.0)
s.close()
qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()

def to_png(ppm, png):
    if not os.path.exists(ppm): print("MISSING", ppm); return False
    try:
        from PIL import Image
        Image.open(ppm).save(png); return True
    except Exception:
        return shutil.which("pnmtopng") and os.system(f"pnmtopng '{ppm}' > '{png}'") == 0

print("shot:", to_png(PPM, PNG))
print("=== [DOCK] serial events ===")
try:
    with open(SER, "rb") as f:
        for ln in f.read().decode("utf-8", "replace").splitlines():
            if "[DOCK]" in ln: print(ln)
except OSError:
    print("no serial")
