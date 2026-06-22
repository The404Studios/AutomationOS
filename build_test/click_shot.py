#!/usr/bin/env python3
"""Boot the SHOWCASE ISO with a usb-tablet, then CLICK each app's taskbar button
(absolute QMP input events) to raise that window and screendump it. Outputs
build/click_<name>.png. Lets us get a clean per-app shot (browser/cockpit/game/
sound) without per-app rebuilds."""
import socket, subprocess, time, os, json
ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
QMP  = "/tmp/qmp_click.sock"
SER  = "/tmp/click_serial.log"
W, H = 1280, 800
try: os.remove(QMP)
except OSError: pass
q = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-usb", "-device", "usb-tablet",
     "-qmp", f"unix:{QMP},server,nowait", "-display", "none",
     "-no-reboot", "-serial", f"file:{SER}"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(40):
    time.sleep(0.5)
    if os.path.exists(QMP): break
time.sleep(26)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(QMP)
f = s.makefile('rw')
f.readline()
def cmd(o):
    f.write(json.dumps(o) + "\n"); f.flush()
    while True:
        l = f.readline()
        if not l: return ""
        if '"return"' in l or '"error"' in l: return l.strip()
cmd({"execute": "qmp_capabilities"})

def ax(x): return int(x * 32767 / (W - 1))
def ay(y): return int(y * 32767 / (H - 1))
def click(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": ax(x)}},
        {"type": "abs", "data": {"axis": "y", "value": ay(y)}}]}})
    time.sleep(0.3)
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": True}}]}})
    time.sleep(0.15)
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": False}}]}})
    time.sleep(1.4)
def dump(name):
    png = f"{ROOT}/build/click_{name}.png"; ppm = f"/tmp/click_{name}.ppm"
    r = cmd({"execute": "screendump", "arguments": {"filename": png, "format": "png"}})
    if '"error"' in r:
        cmd({"execute": "screendump", "arguments": {"filename": ppm}})
        try:
            from PIL import Image; Image.open(ppm).save(png)
        except Exception:
            subprocess.run(["pnmtopng", ppm], stdout=open(png, "wb"))
    print(name, "ok" if os.path.exists(png) else "FAIL")

# Taskbar buttons live at y~782 on the 1280x800 FB. x positions read off sc_f0.
for name, x in [("browser", 513), ("sound", 596), ("cockpit", 720), ("game", 915)]:
    click(x, 782)
    dump(name)

q.terminate()
try: q.wait(timeout=5)
except subprocess.TimeoutExpired: q.kill()
