#!/usr/bin/env python3
"""Verify autocomplete Tab-accept: type 'wh' (popup -> 'while'), press Tab ->
the buffer must contain the full word 'while' (prefix replaced, correct casing).
Captures build/ide_tab_accept.png."""
import socket, subprocess, time, os
ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_tab.sock"
try: os.remove(MON)
except OSError: pass
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none",
     "-no-reboot", "-serial", "file:/tmp/ide_tab_serial.log"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(40):
    time.sleep(0.5)
    if os.path.exists(MON): break
time.sleep(24)

def mon(cmds, settle=0.45):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(MON)
    time.sleep(0.3)
    try: s.recv(8192)
    except OSError: pass
    for c in cmds:
        s.sendall((c + "\n").encode()); time.sleep(settle)
    s.close()

def dump(tag):
    ppm = f"/tmp/ide_{tag}.ppm"; png = f"{ROOT}/build/ide_{tag}.png"
    try: os.remove(ppm)
    except OSError: pass
    mon([f"screendump {ppm}"], settle=1.6); time.sleep(0.6)
    if os.path.exists(ppm):
        try:
            from PIL import Image; Image.open(ppm).save(png); return png
        except Exception:
            try:
                with open(png, "wb") as out:
                    if subprocess.run(["pnmtopng", ppm], stdout=out).returncode == 0:
                        return png
            except Exception: pass
    return None

mon(["sendkey w"]); mon(["sendkey h"])   # 'wh' -> popup should show 'while'
time.sleep(0.5)
mon(["sendkey tab"])                       # Tab accepts -> 'while'
time.sleep(0.5)
print("tab_accept:", dump("tab_accept") or "FAILED")

qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
