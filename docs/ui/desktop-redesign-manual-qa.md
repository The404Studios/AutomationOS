# DESKTOP-REDESIGN-0 — release QA checklist

The automated gates (`smoke_boot` 50/50, `theme_contrast_check.py`,
`chrome_pixel_check.py`) prove **build health, WCAG contrast, and broad chrome
consistency**. They do NOT prove interactive states, edge-case content, or
per-app visual polish. This checklist closes that gap. Run it on a real boot
(QEMU with a display, or the T410) before declaring the visual language frozen.

Pass criterion for every item: reads as the same Signature Dark system (graphite
surfaces, teal accent, legible text tiers), no light/white chrome, no clipped or
unreadable text, no invisible interactive affordance.

## A. Compositor chrome & window management
- [ ] **Focus/unfocus** — click between two windows; focused window shows the
      teal edge, unfocused titlebar dims (`THEME_TITLEBAR_UNFOC`).
- [ ] **Hover** — hover dock items and titlebar controls; hover tint is `BG3`,
      distinct from the teal focus.
- [ ] **Window controls** — close chip is danger-red, minimize is warn-amber;
      both legible; hover states visible.
- [ ] **Resize** — drag a window edge/corner from tiny to full-screen; chrome,
      shadow, and rounded corners re-render cleanly at every size; no tearing of
      the titlebar or content clip.
- [ ] **Minimize / restore** — Alt+M then restore via taskbar; animation is
      smooth; taskbar entry stays themed.
- [ ] **Snap** — drag a titlebar to the left/right/top edge; snap preview and
      snapped geometry render on-theme.
- [ ] **Alt+Tab** — MRU ring overlay (if drawn) is on-theme.

## B. Shared widgets (open `uidemo` — it showcases the lib/ui set)
- [ ] Buttons: normal / hover / pressed all distinct and legible.
- [ ] **Disabled** buttons/controls: use `THEME_TEXT_FAINT`, clearly read as
      inactive but still legible (contrast >= 3:1, verified in tokens).
- [ ] Text field: **focused** shows the teal ring + blinking caret; unfocused
      shows the grey `THEME_BORDER` (now >= 3:1 on the body).
- [ ] Checkbox / toggle: on-state teal, off-state `BG3/BG4`; knob legible.
- [ ] Slider: track `BG3`, fill teal, knob `TEXT_DIM`.
- [ ] List: selected row teal accent; hover row `BG3`; both distinct.
- [ ] Progress / spinner / signal bars: teal, legible on their surface.
- [ ] Scrollbar thumb (`THEME_BORDER`) is visible against the track.

## C. Content-length / edge cases
- [ ] **Long filename** in filemanager (e.g. a 120-char name): truncates with
      ellipsis or wraps — never overflows the row or paints outside the window.
- [ ] **Long window title**: elides in the titlebar + taskbar, no overrun.
- [ ] **Empty states**: an empty folder / empty list shows an on-theme empty
      message, not a blank light rectangle.
- [ ] **Error state**: trigger a failure (e.g. netman with the NIC down, a bad
      URL in browser2) — error text uses `THEME_DANGER`, legible on dark.
- [ ] **Long text body**: notes/editor with a very long paragraph scrolls and
      stays legible; no clipping at the chrome boundary.
- [ ] **Modal dialog** (if any app raises one): dialog surface, buttons, and
      backdrop are on-theme; the dialog is clearly elevated (`BG2`+shadow).

## D. Per-app visual review (open each; confirm no light/off-brand chrome)
Flagships: `filemanager`, `terminal`, `settings`, `browser2`, `ide`.
Tier A: aiconsole, calendar, clockapp, dashboard, editor, notes, sheet,
stopwatch, musicplayer, photos, paint, procmon, soundtest, stress, welcome,
imageviewer, screenshot, synth.
Tier B: startmenu, controlcenter, taskman, sysmon, cockpit, dateapp, netman,
soundman, sysinfo, claudechat, anthropic, calculator, clock, applauncher.
- [ ] Each opens with graphite chrome + teal accents, legible text.
- [ ] **Content preserved** (must NOT be dark-forced): paint canvas + swatches,
      photos/imageviewer images, musicplayer/synth/soundtest visualizers,
      sysmon/taskman/stress data graphs, terminal ANSI colors, ide syntax
      colors, startmenu brand icons, game boards. Confirm these still render in
      their intended colors.

## E. Login / lock screen
- [ ] If a login/lock surface exists, confirm it is on-theme (graphite bg, teal
      primary action, legible fields). If none exists, note it (the desktop
      currently boots straight to the shell).

## Golden screenshots (visual baselines)
Automated diffing is unreliable here (the panel clock changes every boot and
window positions vary), so goldens are **human-reviewed visual references**, and
`chrome_pixel_check.py` is the automated regression. See `tests/goldens/README.md`
for how to (re)capture. Capture set: the default desktop (auto), plus one shot of
each surface above taken during this manual sweep.
