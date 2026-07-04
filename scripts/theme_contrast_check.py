#!/usr/bin/env python3
"""theme_contrast_check.py -- WCAG contrast regression for the Signature Dark
design tokens (userspace/lib/ui/theme.h).

Parses THEME_* 0xAARRGGBB values out of theme.h and computes the WCAG 2.1
relative-luminance contrast ratio for every foreground/background pair the UI
actually uses. Thresholds:
  * 4.5 : normal body text (AA)
  * 3.0 : large text, UI components / graphical objects, borders, icons (AA)
Disabled text (TEXT_FAINT) is checked at 3.0 as guidance (WCAG exempts disabled
controls, but placeholders should still be legible).

Exit 0 if every pair meets its threshold, 1 otherwise. Run:
  python3 scripts/theme_contrast_check.py
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THEME = os.path.join(ROOT, "userspace", "lib", "ui", "theme.h")

def parse_tokens(path):
    tok = {}
    rx = re.compile(r'#define\s+(THEME_[A-Z0-9_]+)\s+0x([0-9A-Fa-f]{8})u?')
    with open(path) as f:
        for line in f:
            m = rx.search(line)
            if m:
                v = int(m.group(2), 16)
                tok[m.group(1)] = ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)
    return tok

def _lin(c):
    cs = c / 255.0
    return cs / 12.92 if cs <= 0.03928 else ((cs + 0.055) / 1.055) ** 2.4

def luminance(rgb):
    r, g, b = (_lin(x) for x in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b

def contrast(fg, bg):
    l1, l2 = luminance(fg), luminance(bg)
    hi, lo = max(l1, l2), min(l1, l2)
    return (hi + 0.05) / (lo + 0.05)

def main():
    t = parse_tokens(THEME)
    bgs = ["THEME_BG0", "THEME_BG1", "THEME_BG2", "THEME_BG3", "THEME_BG4"]
    pairs = []  # (fg, bg, min_ratio, role)
    for bg in bgs:
        pairs.append(("THEME_TEXT", bg, 4.5, "primary text"))
    for bg in bgs[:4]:
        pairs.append(("THEME_TEXT_DIM", bg, 4.5, "secondary text/labels"))
    for bg in bgs[:3]:
        pairs.append(("THEME_TEXT_FAINT", bg, 3.0, "tertiary/placeholder"))
    pairs.append(("THEME_ON_ACCENT", "THEME_ACCENT", 4.5, "text on accent fill"))
    for bg in bgs[:3]:
        pairs.append(("THEME_ACCENT", bg, 3.0, "accent text/link/icon"))
    for sem in ("THEME_SUCCESS", "THEME_WARN", "THEME_DANGER"):
        pairs.append((sem, "THEME_BG1", 3.0, "status indicator"))
        pairs.append((sem, "THEME_BG2", 3.0, "status indicator"))
    pairs.append(("THEME_BORDER", "THEME_BG1", 3.0, "border visibility (UI)"))
    pairs.append(("THEME_ACCENT", "THEME_BG4", 3.0, "focus ring on selected"))

    print(f"{'foreground':<18}{'background':<12}{'ratio':>7}  {'min':>4}  {'role'}")
    print("-" * 78)
    fails = 0
    for fg, bg, mn, role in pairs:
        if fg not in t or bg not in t:
            print(f"  MISSING TOKEN: {fg} or {bg}")
            fails += 1
            continue
        r = contrast(t[fg], t[bg])
        ok = r >= mn
        flag = "PASS" if ok else "FAIL"
        if not ok:
            fails += 1
        print(f"{fg:<18}{bg:<12}{r:>6.2f}  {mn:>4.1f}  [{flag}] {role}")
    print("-" * 78)
    if fails:
        print(f"RESULT: FAIL -- {fails} pair(s) below threshold")
        return 1
    print("RESULT: PASS -- all token pairs meet WCAG AA")
    return 0

if __name__ == "__main__":
    sys.exit(main())
