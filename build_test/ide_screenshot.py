#!/usr/bin/env python3
"""Boot the IDE ISO under QEMU, let the desktop + auto-started IDE render, and
screendump two views: the default (editor) and after Ctrl+E (Semantic LEGO Map).
Outputs build/ide_view1.png (editor) and build/ide_view2.png (lego map)."""
import socket, subprocess, time, os
ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_ide.sock"
SER  = "/tmp/ide_shot_serial.log"
for f in (MON,):
    try: os.remove(f)
    except OSError: pass

qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none",
     "-no-reboot", "-serial", f"file:{SER}"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

for _ in range(40):
    time.sleep(0.5)
    if os.path.exists(MON): break
time.sleep(24)  # boot + compositor + IDE render

def mon(cmds, settle=1.8):
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
    mon([f"screendump {ppm}"])
    time.sleep(0.6)
    if os.path.exists(ppm):
        try:
            from PIL import Image; Image.open(ppm).save(png); return png
        except Exception:
            # Fallback: pnmtopng. Fixed, code-controlled paths (no untrusted
            # input), invoked without a shell via subprocess to avoid injection.
            try:
                with open(png, "wb") as out:
                    if subprocess.run(["pnmtopng", ppm], stdout=out).returncode == 0:
                        return png
            except Exception:
                pass
    return None

p1 = dump("view1")                       # default (editor workspace)
mon(["sendkey ctrl-e"]); time.sleep(2.0) # toggle to LEGO map workspace
p2 = dump("view2")
mon(["sendkey 6"]); time.sleep(2.0)      # VIZ-6 shortcut -> Settings panel
p3 = dump("view3")
print("view3:", p3 or "FAILED")
# Keyboard-nav proof: Down moves the selection, Space toggles that row.
mon(["sendkey down"]); time.sleep(0.6)
mon(["sendkey spc"]);  time.sleep(0.8)
p4 = dump("view4")
print("view4:", p4 or "FAILED")
# Inspector keyboard nav: '2' -> VIZ-2, Right cycles the sub-tab.
mon(["sendkey 2"]); time.sleep(0.8)
mon(["sendkey right"]); time.sleep(0.6)
mon(["sendkey right"]); time.sleep(0.6)
p5 = dump("view5")
print("view5:", p5 or "FAILED")
# Map-overview keyboard nav: '1' -> VIZ-1 map, Down selects a node.
mon(["sendkey 1"]); time.sleep(0.8)
mon(["sendkey down"]); time.sleep(0.5)
mon(["sendkey right"]); time.sleep(0.5)
p6 = dump("view6")
print("view6:", p6 or "FAILED")

qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
print("view1:", p1 or "FAILED")
print("view2:", p2 or "FAILED")
