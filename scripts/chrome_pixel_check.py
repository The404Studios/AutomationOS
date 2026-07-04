#!/usr/bin/env python3
"""chrome_pixel_check.py -- pixel/geometry regression for the Signature Dark
compositor chrome.

Reads a desktop screendump PNG (default build/smp_desktop.png, produced by
scripts/smp_screenshot.py) and asserts that the fixed-position chrome renders
in the expected token color families. Region/statistical checks (not single
pixels) so it is robust to antialiasing and to which app windows happen to be
open. Catches a regression to a light theme, a missing teal accent, or a
recolored panel/dock/launcher.

Run:  python3 scripts/chrome_pixel_check.py [png]
Exit 0 if all checks pass, 1 otherwise.
"""
import os, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Tokens (see userspace/lib/ui/theme.h)
ACCENT = (0x3D, 0xD6, 0xC4)
DANGER = (0xF8, 0x71, 0x71)
WARN   = (0xFB, 0xBF, 0x24)

def lum(px):
    return 0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]

def near(px, ref, tol):
    return all(abs(px[i] - ref[i]) <= tol for i in range(3))

def region_mean(im, x0, y0, x1, y1):
    px = im.load()
    n = r = g = b = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[x, y]
            r += p[0]; g += p[1]; b += p[2]; n += 1
    return (r / n, g / n, b / n)

def count_near(im, ref, tol):
    px = im.load()
    W, H = im.size
    c = 0
    for y in range(0, H, 2):          # sample every other pixel (speed)
        for x in range(0, W, 2):
            if near(px[x, y], ref, tol):
                c += 1
    return c

def main():
    png = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "smp_desktop.png")
    if not os.path.exists(png):
        print(f"FAIL: screendump not found: {png}")
        return 1
    im = Image.open(png).convert("RGB")
    W, H = im.size
    print(f"screendump {W}x{H}: {png}")
    fails = 0

    def check(name, cond, detail):
        nonlocal fails
        print(f"  [{'PASS' if cond else 'FAIL'}] {name} -- {detail}")
        if not cond:
            fails += 1

    # 1. Top panel band (y 0..22, full width) must be DARK chrome (not a light theme).
    pm = region_mean(im, 0, 2, W, 20)
    check("top panel is dark chrome", lum(pm) < 60,
          f"mean={tuple(round(v) for v in pm)} lum={lum(pm):.1f} (<60)")

    # 2. Launcher button (bottom-left corner) must contain the TEAL accent.
    lb = count_near(im.crop((2, H - 30, 46, H - 2)), ACCENT, 40)
    check("launcher button shows accent teal", lb > 20,
          f"{lb} accent px in bottom-left button (>20)")

    # 3. Wallpaper (bottom-left desktop area, below/left of any window) must be
    #    dark graphite -- catches a light-theme regression of the backdrop.
    wm = region_mean(im, 8, H - 180, 90, H - 90)
    check("wallpaper is dark graphite", lum(wm) < 45,
          f"mean={tuple(round(v) for v in wm)} lum={lum(wm):.1f} (<45)")

    # 4. Teal accent present across the whole frame (launcher + focused taskbar +
    #    active tab + folder icons) -- catches accent removal / revert to blue.
    ac = count_near(im, ACCENT, 36)
    check("accent teal present desktop-wide", ac > 300,
          f"{ac} accent px (>300)")

    # 5. Window controls: the semantic close(DANGER) + minimize(WARN) chips must
    #    both appear somewhere (top-right of the focused/other windows).
    dc = count_near(im, DANGER, 40)
    wc = count_near(im, WARN, 44)
    check("close control (danger red) present", dc > 15, f"{dc} danger px (>15)")
    check("minimize control (warn amber) present", wc > 15, f"{wc} warn px (>15)")

    print("-" * 60)
    if fails:
        print(f"RESULT: FAIL -- {fails} chrome check(s) failed")
        return 1
    print("RESULT: PASS -- compositor chrome renders on Signature Dark")
    return 0

if __name__ == "__main__":
    sys.exit(main())
