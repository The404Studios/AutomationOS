#!/usr/bin/env python3
"""Boot the SHOWCASE ISO, let the desktop + the curated headline apps render, and
Alt+Tab-cycle the window stack, screendumping each frame to build/sc_fN.png so a
clean per-app shot can be picked (game, AI cockpit, sound, browser, etc.)."""
import socket, subprocess, time, os
ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_sc.sock"
SER  = "/tmp/sc_serial.log"
try: os.remove(MON)
except OSError: pass
q = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none",
     "-no-reboot", "-serial", f"file:{SER}"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(40):
    time.sleep(0.5)
    if os.path.exists(MON): break
time.sleep(26)  # boot + all showcase apps render

def mon(cmds, settle=1.2):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(MON); time.sleep(0.3)
    try: s.recv(8192)
    except OSError: pass
    for c in cmds:
        s.sendall((c + "\n").encode()); time.sleep(settle)
    s.close()

def dump(tag):
    ppm = f"/tmp/sc_{tag}.ppm"; png = f"{ROOT}/build/sc_{tag}.png"
    try: os.remove(ppm)
    except OSError: pass
    mon([f"screendump {ppm}"]); time.sleep(0.6)
    if os.path.exists(ppm):
        try:
            from PIL import Image; Image.open(ppm).save(png); return png
        except Exception:
            try:
                with open(png, "wb") as o:
                    if subprocess.run(["pnmtopng", ppm], stdout=o).returncode == 0:
                        return png
            except Exception:
                pass
    return None

frames = [("f0", dump("f0"))]
for i in range(1, 8):
    mon(["sendkey alt-tab"]); time.sleep(1.4)
    frames.append((f"f{i}", dump(f"f{i}")))
q.terminate()
try: q.wait(timeout=5)
except subprocess.TimeoutExpired: q.kill()
for t, p in frames:
    print(t, p or "FAILED")
