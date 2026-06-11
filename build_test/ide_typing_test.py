#!/usr/bin/env python3
"""Boot the IDE (auto-starts focused in the editor on tower.c) and simulate
regular coding to verify: (1) the autocomplete popup appears after 2 chars,
(2) a bare Enter makes a NEWLINE (does NOT inject a suggestion), (3) typing
continues normally. Captures build/ide_type_popup.png (after 'in') and
build/ide_type_after.png (after Enter + 'x')."""
import socket, subprocess, time, os
ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_type.sock"
SER  = "/tmp/ide_type_serial.log"
try: os.remove(MON)
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
time.sleep(24)

def mon(cmds, settle=0.5):
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

# Type "in" -> popup should appear for int/include/...
mon(["sendkey i"]); mon(["sendkey n"])
time.sleep(0.6)
print("popup:", dump("type_popup") or "FAILED")
# Bare Enter (no popup navigation) -> MUST insert a newline, not a suggestion.
mon(["sendkey ret"]); time.sleep(0.4)
mon(["sendkey x"]); time.sleep(0.4)
print("after:", dump("type_after") or "FAILED")

qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
