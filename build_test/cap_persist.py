#!/usr/bin/env python3
"""BROWSER-PERSIST-0 proof: boot with slirp networking + serial, screendump at
22 s (well past the old 5 s browser deadline). If the desktop browser2 is now
persistent, a browser window is still on screen at 22 s. Also dumps the serial
so the bounded --smoke / about:imgtest self-test verdicts can be checked."""
import socket, subprocess, time, os, shutil

ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_p.sock"
PPM  = "/tmp/persist.ppm"
PNG  = f"{ROOT}/build/browser2_persist.png"
SER  = f"{ROOT}/build_test/_persist_serial.log"
WAIT = float(os.environ.get("RENDER_WAIT", "22"))

for f in (MON, PPM, SER):
    try: os.remove(f)
    except OSError: pass

qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", f"file:{SER}",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none", "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

for _ in range(int(WAIT * 2)):
    time.sleep(0.5)

def mon():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(MON); time.sleep(0.3)
    try: s.recv(8192)
    except OSError: pass
    return s

def cmd(s, c):
    s.sendall((c + "\n").encode()); time.sleep(0.5)
    try: return s.recv(8192)
    except OSError: return b""

s = mon()
cmd(s, f"screendump {PPM}")
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
        return False

print("shot:", to_png(PPM, PNG))
