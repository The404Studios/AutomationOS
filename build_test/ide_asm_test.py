#!/usr/bin/env python3
"""Verify ASM files populate the Semantic LEGO Map: focus the terminal, run
`edit /usr/src/native/add.asm`, then Ctrl+E into the LEGO map -> the .asm labels
should render as nodes (control-flow graph). Captures build/ide_asm_map.png."""
import socket, subprocess, time, os
ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_asm.sock"
try: os.remove(MON)
except OSError: pass
qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-monitor", f"unix:{MON},server,nowait", "-display", "none",
     "-no-reboot", "-serial", "file:/tmp/ide_asm_serial.log"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(40):
    time.sleep(0.5)
    if os.path.exists(MON): break
time.sleep(24)

def mon(cmds, settle=0.16):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(MON)
    time.sleep(0.3)
    try: s.recv(8192)
    except OSError: pass
    for c in cmds:
        s.sendall((c + "\n").encode()); time.sleep(settle)
    s.close()

def typ(text):
    km = {' ':'spc', '/':'slash', '.':'dot'}
    for ch in text:
        mon(["sendkey " + km.get(ch, ch)])

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

mon(["sendkey ctrl-j"]); time.sleep(0.4)             # focus terminal
typ("edit /usr/src/native/add.asm")
mon(["sendkey ret"]); time.sleep(1.0)                # open the .asm in the editor
mon(["sendkey ctrl-e"]); time.sleep(1.5)             # -> LEGO map
print("asm_map:", dump("asm_map") or "FAILED")

qemu.terminate()
try: qemu.wait(timeout=5)
except subprocess.TimeoutExpired: qemu.kill()
