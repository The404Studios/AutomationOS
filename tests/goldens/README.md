# Desktop golden screenshots

Visual baselines for the Signature Dark desktop (DESKTOP-REDESIGN-0).

## What lives here
- `desktop.png` — the canonical desktop boot: compositor chrome (panel, dock,
  wallpaper, window frames, teal accent) plus the default window set
  (terminal, filemanager, browser2, network manager, sound). Captured
  automatically by `scripts/smp_screenshot.py`.

## How goldens are used
These are **human-reviewed visual references**, not automated pixel-diffs.
Pixel-exact diffing is unreliable on this desktop because:
- the top-panel **clock changes every boot**, and
- window positions can vary.

So the automated regression is the **statistical** `scripts/chrome_pixel_check.py`
(dark-chrome + teal-accent + semantic-control checks that ignore exact pixels),
and these PNGs are for eyeballing during release review.

## (Re)capturing
```sh
# from WSL Arch, repo root, after a build:
python3 scripts/smp_screenshot.py            # -> build/smp_desktop.png
cp build/smp_desktop.png tests/goldens/desktop.png   # accept as new golden
python3 scripts/chrome_pixel_check.py        # automated regression gate
```

## Per-surface goldens (settings / ide / startmenu / login / individual apps)
The default boot covers the compositor + 5 flagship-class windows. The remaining
surfaces are not open at boot, so capture them during the manual QA sweep
(`docs/ui/desktop-redesign-manual-qa.md`): open the app, then run
`python3 scripts/smp_screenshot.py` and save the PNG here as `<app>.png`.
Apps with an autostart build flag (e.g. `IDE=1 FULL=1 bash scripts/build_all.sh`
auto-opens the IDE) can be captured without manual interaction.
