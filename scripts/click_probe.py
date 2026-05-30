#!/usr/bin/env python3
"""Boot headless, inject a left-click on the dock launcher via QEMU monitor,
and dump the serial lines around the click so we can see if it reaches the
compositor's dispatch (it prints [SHELL] launch... on a launcher click)."""
import socket, subprocess, time, os

ROOT = "/mnt/c/Users/wilde/Desktop/Kernel"
ISO  = f"{ROOT}/build/automationos.iso"
MON  = "/tmp/qmon_click.sock"
LOG  = "/tmp/click_serial.log"

for f in (MON, LOG):
    try: os.remove(f)
    except OSError: pass

q = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "512", "-vga", "std",
     "-serial", f"file:{LOG}", "-monitor", f"unix:{MON},server,nowait",
     "-display", "none", "-no-reboot"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

time.sleep(14)  # let desktop come up

def mon():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(MON)
    time.sleep(0.3)
    try: s.recv(8192)
    except OSError: pass
    return s
def cmd(s, c):
    s.sendall((c + "\n").encode()); time.sleep(0.4)
    try: return s.recv(8192)
    except OSError: return b""

s = mon()
def move_by(dx, dy, step=40):
    # PS/2 relative mouse: QEMU clamps each mouse_move to ~int8, so step it.
    ax = 1 if dx >= 0 else -1
    ay = 1 if dy >= 0 else -1
    rx, ry = abs(dx), abs(dy)
    while rx > 0 or ry > 0:
        sx = min(step, rx); sy = min(step, ry)
        cmd(s, f"mouse_move {ax*sx} {ay*sy}")
        rx -= sx; ry -= sy
        time.sleep(0.03)

# Cursor starts at center (512,384). 1) Click the launcher "T" (~26,748) to open
# the start menu: dx=-486, dy=+364.
move_by(-486, 364)
time.sleep(0.4)
cmd(s, "mouse_button 1"); time.sleep(0.2); cmd(s, "mouse_button 0")
time.sleep(1.0)
# 2) Click the first start-menu row (~60,405): from (26,748) dx=+34, dy=-343.
move_by(34, -343)
time.sleep(0.4)
cmd(s, "mouse_button 1"); time.sleep(0.2); cmd(s, "mouse_button 0")
time.sleep(1.2)
# 3) Right-click the desktop (~400,400) to open the context menu: from (60,405).
move_by(340, -5)
time.sleep(0.4)
cmd(s, "mouse_button 2"); time.sleep(0.2); cmd(s, "mouse_button 0")
time.sleep(1.0)
# 4) Click the first context-menu row (~ +14,+34 below the click point).
move_by(14, 40)
time.sleep(0.3)
cmd(s, "mouse_button 1"); time.sleep(0.2); cmd(s, "mouse_button 0")
time.sleep(1.2)
s.close()
q.terminate()
try: q.wait(timeout=5)
except subprocess.TimeoutExpired: q.kill()

time.sleep(1)
with open(LOG, "rb") as f:
    data = f.read().decode("latin1")
lines = data.splitlines()
# Print everything mentioning shell/spawn/launch/mouse/btn near the end.
keys = ("SHELL", "spawn", "launch", "focus", "BTN", "btn", "MOUSE", "mouse", "rdock", "SPAWN")
hits = [l for l in lines if any(k in l for k in keys)]
print("=== matching lines (last 40) ===")
for l in hits[-40:]:
    print(l)
print(f"=== total serial lines: {len(lines)} ; matches: {len(hits)} ===")
