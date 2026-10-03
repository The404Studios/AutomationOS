# Meridian shell -- design spec (original work)

Status: design + asset pipeline started 2026-10-02. This is an *original* shell. It takes its structural
inspiration from console-dashboard conventions (a navigable rail of sections, a row of large focusable tiles,
a translucent flowing background) but uses its own name, palette, art, typography and sounds. We copy
mechanics, not trade dress: no Microsoft marks, no Convection/Segoe fonts, no copied wallpapers or audio.
(Correction recorded from the research: the horizontal strip was the 2005 "Blades" dashboard; the 2008 NXE
is a vertical list of hubs with a horizontal card row per hub.)

## Goals (from the owner)

1. **Modern and really nice**; flowing translucent ribbons; glass tiles.
2. **App launch = ribbon + chain swirl** around the launching tile, and a **sparkle when the app opens
   correctly**. The sparkle is a *real* success signal: it fires on the compositor's window-created event
   (`WL_EVT_CREATED`), not on a timer. If the app dies or no window appears within N seconds the chain
   *snaps* and fades (no sparkle) -- honest feedback.
3. **Satisfying and optimized**: spring/settle motion, instant press feedback, never a dropped input. Optimized
   is enforced, not hoped for: a frame-time budget and a `proofkit` frame-rate assertion (below).
4. **The IDE's text is animated and nice**: gliding caret, smooth scroll, eased character pop-in, bracket-match
   pulse, find-hit pulse, selection fade, current-line crossfade -- all inside the same frame budget.

## Information architecture (original names)

Shell "Meridian"; vertical list of **Decks** on the left: Home, Discover, Library, Media, People, Settings.
Left/Right moves through the focused Deck's **Tiles**; the **Quick Panel** (guide overlay) slides in from the
left and is reachable from anywhere. A pushes a page, B pops it; last focused tile is remembered per Deck.
Reference layout 1280x720; title-safe margin 64x36; focused tile 320x180 (16:9) or 168x236 (box art);
neighbours 0.86x then 0.74x with reflections; footer hint bar 48 px.

## Visual language

* Base graphite #101312 -> #1A1F1D vertical gradient; glass = white at 8-14 % alpha with a top sheen
  (18 % -> 0 by mid-height), 1 px top highlight (35 %), 1 px bottom line (25 % black).
* Accent = the OS's existing teal (`userspace/lib/ui/theme.h`, #3DD6C4). Per-Deck hue (E): Home teal,
  Discover amber (40), Library blue (215), Media violet (275), People coral (350), Settings slate.
* **Ribbons**: three flowing translucent bands, real 3D twisted geometry baked in Blender
  (`tools/blender/meridian_ribbons.py`) to 2-channel (intensity + coverage) 320x180 layers that tile
  seamlessly; tinted per Deck at runtime through a 256-entry LUT; scrolled with parallax.
* **Chain**: a ring of metallic-glass links (alternating orientation) used in the launch swirl and as an
  identity motif (nod to the ChainLayer ball-and-chain mark). Links are Blender-rendered sprites at 8 angles.
* **Sparkle**: 4-point star glints with additive blend + a brief radial flash.
* Typography: an OFL humanist sans (e.g. Open Sans) -- hub label 40 px light, tile title 20 semibold, body >=16.

## Motion (time-based, never frame-count based)

| event | duration | curve / detail |
|---|---|---|
| card scroll L/R | 240 ms | ease-out cubic |
| deck change U/D | 320 ms | label crossfade 160 ms; tile stagger 40 ms, max 5 |
| focus | 140 ms | scale 1.00 -> 1.08 with a small overshoot-and-settle; halo 0 -> 100 % in 120 ms |
| quick panel in / out | 260 / 200 ms | slide 420 px; scene dims to 55 % |
| idle | -- | halo pulse 1.6 s; ribbons drift 6/12/22 px/s (at 320-wide scale) |
| **launch swirl** | 600-900 ms | 3 ribbons orbit the tile on contracting ellipses; chain ring spins up and tightens |
| **open sparkle** | 450 ms | 14-24 glints radiate (ease-out, fade), flash 120 ms; fired on window-created |
| **launch failure** | 350 ms | chain snaps (two halves fall + fade), ribbons dissolve; no sparkle |
| startup | ~2.2 s | black 300 ms, ribbons fade in staggered 200 ms, wordmark 500 ms, rail slides in 350 ms |

Integer easing table: `ease[i] = 256 - (((64-i)^3) >> 10)`, `t64 = elapsed*64/dur`,
`pos = a + (((b-a) * ease[t64]) >> 8)`. A damped-spring variant (fixed-point) is used for settle/overshoot.

## Making it fast on a 2010 laptop (software renderer)

* Ribbon: composite the 3 layers into a 320x180 buffer (L2-resident) through the tint LUT, then 4x upscale with
  fixed-point bilinear **only when the background changed**; ribbon runs ~5 fps while idle and freezes
  (cached background) during UI transitions. Per-scanline wobble table for the flowing feel.
* Free frosted glass: sample the already-smooth low-res buffer for panel interiors; no blur pass.
* Halo/shadows: pre-baked 9-slice 8-bit alpha sprites, tinted by LUT; pulse = 32-entry integer sine, dirty-rect.
* Launch FX: sprite blits only (chain-link sprites + ribbon strips + glints), clipped to a small dirty rect
  around the tile; no per-pixel math beyond alpha blits.
* Cards: cache scaled variants (1.0/0.86/0.74) + reflection. Text: pre-rasterised glyph atlases, never per frame.
* **Budget:** steady-state frame <= 12 ms CPU0 on QEMU/TCG-equivalent work, launch FX <= 25 % of a frame, zero
  full-screen composites while only a tile animates. Enforced by a `proofkit` assertion on the compositor's
  `[COMP] fps window ... fps_x10=` lines (target >= 24 fps_x10 steady in QEMU, no regression vs. baseline).

## IDE text animation (budgeted)

Caret glide (60-90 ms ease-out, hard-stop on direction change), smooth scroll (interpolated, 140 ms), character
pop-in (alpha + 2 px rise over 70 ms, only for the last few typed glyphs), bracket-match pulse (2 cycles),
find-hit pulse, selection fade-in/out (90 ms), current-line highlight crossfade, autocomplete popup slide/fade.
All animations are cancel-safe (typing never waits on an animation) and dirty-rect bounded.

## Asset pipeline

`tools/blender/*.py` (headless Blender, factory settings; never touches an open session) -> `build/meridian/*`
-> packed assets shipped in the initrd under `/usr/share/meridian/` -> loaded by the `meridian` shell.
Done: ribbon layers. Next: glass tile 9-slice, halo, chain-link sprite sheet, sparkle glints, bokeh, and a
Blender-rendered motion reference (showreel) of the launch swirl + sparkle.

## Proof (assertions, not vibes)

* `proofkit` scenario `meridian`: shell boots, tile focus changes redraw only a bounded dirty rect (a debug
  counter), launching an app shows the swirl then the sparkle **after** the window-created event (ordered
  serial markers), a deliberately failing app shows the snap and *no* sparkle, steady-state fps >= budget,
  screenshots are not blank and contain the accent hue.
