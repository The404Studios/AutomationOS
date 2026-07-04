# DESKTOP-REDESIGN — "Signature Dark" unified design language

Date: 2026-07-03 · Branch: brick/wave0-tcp-robust-0

## Problem

The AutomationOS desktop is a competent but derivative macOS-clone dark theme
(Apple's literal system palette: `0x0A84FF` iOS blue, `0x30D158` iOS green,
`0x8E8E93` gray), and its styling is **fragmented**: `lib/ui/ui.c` defines one
palette, `compositor_m8.c` duplicates it, and `browser2`/`ide` each carry their
own theme headers. There is no single source of truth, so the look is
inconsistent across surfaces and can't be evolved coherently.

## Goal

Replace the look across "most of everything" with **one distinctive, cohesive
dark design language** driven by a single token header, applied to the
compositor chrome, the shared widget library, and every app. Aesthetic:
**Signature Dark** — deeper graphite base, a custom **teal** accent (not iOS
blue), tightened type/spacing/radius/elevation.

## Constraints

- **Font:** single-weight `8×16` bitmap (`lib/font/bitfont.c`) at integer
  scales. Hierarchy must come from **size steps + text-color tiers + spacing**,
  not font weights.
- **Freestanding C:** tokens are compile-time `#define`s (a shared header), not
  a runtime struct. A runtime light/dark toggle is an explicit non-goal.
- **Must stay green:** `smoke_boot` functional gates (currently 50/50) must not
  regress; the compositor's damage-scissor / animations / snapping must keep
  working.
- **Verifiable:** design is evaluated via headless QEMU-monitor `screendump`
  (adapted from `scripts/smp_screenshot.py`) → PNG, reviewed against this spec.

## Token system — `userspace/lib/ui/theme.h` (single source of truth)

All ARGB `0xFF……`. Elevation-layered greys:

| Token | Hex | Use |
|-------|-----|-----|
| `THEME_BG0` | `0xFF0E1013` | desktop / deepest |
| `THEME_BG1` | `0xFF16191F` | window body / top panel |
| `THEME_BG2` | `0xFF1E222A` | card / surface / dock |
| `THEME_BG3` | `0xFF272C35` | hover |
| `THEME_BG4` | `0xFF313742` | pressed / active |
| `THEME_TEXT` | `0xFFEDF0F4` | primary text |
| `THEME_TEXT_DIM` | `0xFF9AA3AE` | secondary text |
| `THEME_TEXT_FAINT`| `0xFF6B7480` | tertiary / disabled |
| `THEME_ACCENT` | `0xFF3DD6C4` | teal accent |
| `THEME_ACCENT_HI` | `0xFF55E0D0` | accent hover |
| `THEME_ACCENT_LO` | `0xFF2BB8A8` | accent pressed |
| `THEME_ON_ACCENT` | `0xFF06201D` | text on accent fills |
| `THEME_HAIRLINE` | `0xFF2A2F38` | subtle divider |
| `THEME_BORDER` | `0xFF3A414D` | strong border |
| `THEME_SUCCESS` | `0xFF4ADE80` | success |
| `THEME_WARN` | `0xFFFBBF24` | warning |
| `THEME_DANGER` | `0xFFF87171` | error / close-hover |

Scales: **spacing** `4/8/12/16/24/32` (`THEME_SP_1..6`); **radius** `THEME_R_SM 4`,
`THEME_R_MD 8` (windows), `THEME_R_LG 12` (dock), `THEME_R_PILL 999`;
**type** `THEME_FS_CAPTION/BODY 1×`, `THEME_FS_TITLE 2×`, `THEME_FS_DISPLAY 3×`;
**motion** `THEME_MS_FAST 120`, `THEME_MS_BASE 180`, `THEME_MS_SLOW 240`.

## Rollout (bricks)

- **DESKTOP-REDESIGN-0 (foundation):** `theme.h`; `lib/ui/ui.c` `COL_*` repointed
  to tokens; `compositor_m8.c` palette repointed + signature chrome (wallpaper
  gradient with faint teal cast + vignette, top-panel hairline, floating dock on
  `BG2`/`R_LG` with teal active-dots, window frames `BG1` + `R_MD` + retuned
  shadow + accent-tinted focus vs. dimmed unfocused, refined controls).
- **DESKTOP-REDESIGN-1 (flagships):** `filemanager`, `terminal`, `settings`,
  `browser2`, `ide` drop local palettes for `theme.h`.
- **DESKTOP-REDESIGN-2+ (fill every gap):** the ~40 remaining apps adopt
  `theme.h` in batches (parallel), a few per brick, until every surface is on the
  token system.

## Method (per batch)

1. Repoint colors to `theme.h` tokens (mechanical, low-risk — apps become
   correct-by-construction).
2. Targeted chrome/layout polish where it adds value.
3. Build in WSL Arch (`quick_build` if kernel touched — it isn't — else just
   `FULL=1 build_all`).
4. Screendump the desktop + the batch's surfaces; review vs. this spec; iterate.
5. `smoke_boot` must stay green.
6. Commit the brick.

## Non-goals

Runtime theme switching; light mode; new font/glyph set; app *functional*
changes; touching syntax-highlight palettes (IDE code colors stay distinct).
