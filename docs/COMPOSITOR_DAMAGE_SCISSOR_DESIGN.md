# Compositor Damage-Scissor Design (IDE-open lag fix)

**Status:** designed + adversarially reviewed (5 pitfalls resolved), NOT yet implemented.
**Goal:** the desktop must stay fast when the IDE window is open. Today it drops to
~20 fps (~50 ms/frame) because `composite()` unconditionally re-blits **every** window
(opaque blit + 4-layer soft-shadow + alpha) on every dirty frame, even when only the
IDE caret moved. `present_diff()` already bounds the *framebuffer* write; the cost being
attacked here is the **CPU re-blit into `back`** in `composite()`.

File: `userspace/compositor/compositor_m8.c` (the SHIPPED compositor; `main.c`/`render.c`
are NOT built).

---

## Approach: a global scene-damage SCISSOR (not per-window skip)

`composite()` keeps its **exact** existing draw sequence (wallpaper → icons → all windows
back-to-front → chrome → overlays), preserving z-order / shadow / alpha / occlusion
correctness automatically. The change: every low-level rasterizer intersects its clamp
against a **global active scissor rect** `[g_scis_x0,x1) × [g_scis_y0,y1)`. When the frame's
damage is a small rect (typing in the IDE), `composite()` still walks every window but each
draw call collapses to the damage area — per-pixel cost drops from full-scene to the damage
rect. **No window is ever skipped**, so the hardest pitfall (occlusion of overlapping clean
windows) cannot occur — the whole z-stack is faithfully re-rendered inside the rect.

Damage is accumulated per frame from real change sources:
- `drain_inbox` translates each client's commit damage — **`wl_req_commit_t.x/y/w/h` is
  ALREADY sent in the protocol but currently thrown away** — into the owning window's
  on-screen footprint and unions it.
- Every WM/chrome/anim/hover/clock path that calls `mark_dirty()` ALSO unions its rect;
  any path that can't cheaply bound itself unions **full-screen** (safe fallback).

The scissor defaults to full-screen, so with damage accumulation disabled the behavior is
**byte-identical** to today. `present_diff()` is left UNCHANGED — it still bounds the slow-FB
write by scanning `back` vs `prev`; the scissored composite only touched the damage rect, so
`present_diff`'s bounding box naturally collapses to it.

Separate the **accumulator** (`g_dmg_*`, `g_dmg_any`) from the **active scissor** (`g_scis_*`):
composite sets the scissor once per frame from accumulated damage; present stays independent.

---

## Implementation steps

1. **Scissor state + API** (near `mark_dirty`, ~line 207). Declare file-scope
   `g_scis_x0/y0/x1/y1` and `g_dmg_x0/y0/x1/y1 + g_dmg_any`. Inline helpers:
   `scissor_reset_full()` → active scissor = `[0,g_fb_w)×[0,g_fb_h)`; `damage_reset()` →
   `g_dmg_any=0`; `damage_add(x0,y0,x1,y1)` → clamp to screen + union into accumulator,
   set `g_dmg_any=1`; `damage_add_full()` → union `[0,W)×[0,H)`. Helpers read `g_fb_w/g_fb_h`
   **at call time inside function bodies** (not in initializers) — sidesteps the
   use-before-declaration hazard (`g_fb_w` is defined at ~1142).
2. **Scissor globals + `scissor_clip()` helper** declared just above `fill_rect` (~line 408)
   so all rasterizers below see them. `scissor_clip(*x1,*y1,*x2,*y2)` intersects a span with
   the active scissor.
3. **Funnels — `fill_rect` (410-416) + `blend_rect` (425-431).** After the existing
   clamp to `[0,bw)/[0,bh)`, clamp `x1/y1/x2/y2` to `g_scis_*` before the
   `if (x1>=x2||y1>=y2) return;` guard. These two are the funnels: `stroke_rect`,
   `fill_round_rect`, `fill_round_top_rect`, `draw_soft_shadow`, `render_panel/dock/
   right_dock`, `render_desktop` bands all bottom out here. **Two edits cover the bulk.**
4. **Independent blit loops — `blit_surface_clip` (519-529) + `blit_surface_scaled_alpha`
   (563-578).** They do NOT route through `fill_rect`, so add `g_scis_*` x/y guards directly
   in their loops (using destination coords). `blit_surface_clip` is the opaque IDE-content
   blit — scissoring it is what stops re-copying the whole IDE surface on small typing damage.
5. **Two inline `buf[]` loops in `render_window_static`** — fade-titlebar (1683-1694) +
   rounded-corner punch (1736-1746). Wrap both with the active scissor (same idiom). The
   corner-punch runs every static window render, so it MUST be scissored or corners won't
   repaint inside a partial rect.
6. **`cz_text` (948-951).** Pass the active horizontal scissor into
   `font2_draw_cell_clip`'s `clip_x0/clip_x1` instead of `(0,bw)`:
   `cx0 = max(0,g_scis_x0); cx1 = min(bw,g_scis_x1)`.
7. **`win_footprint()` helper** (above `render_window_static`, ~1558). Computes
   `outer_x=win->x-BORDER_W`, `outer_y=win->y-BORDER_W`,
   `full_w=win->w+2*BORDER_W`, `full_h=win->h+TITLEBAR_H+2*BORDER_W` (INCLUDES TITLEBAR_H),
   then pads by `#define SHADOW_PAD 24` on every side (clamped to screen) to cover the
   4-layer soft shadow. **Single source of truth** for every footprint consumer — no caller
   forgets the pad. (Measured worst-case shadow extent ≈ left −12 / top −5 / right +16 /
   bottom +23 relative to outer; SHADOW_PAD=24 covers the +23 bottom.)
8. **`drain_inbox` (3280-3291) + WM/clock/anim sites.** Replace the blanket `mark_dirty()`
   at 3284 with PER-MESSAGE damage:
   - `WL_REQ_COMMIT`: `mark_dirty()` + after the slot resolves, `damage_add(win_footprint)`;
     if the commit carried a tight rect (`msg.commit.x/y/w/h`), translate to screen coords
     (offset by `win->x`, `win->y+TITLEBAR_H`), pad if it touches a window edge, intersect
     with the footprint, union THAT (tighter than full footprint for mid-surface typing).
   - `WL_REQ_CREATE/DESTROY/RESIZE`: `mark_dirty()` + `damage_add_full()` (new shadows /
     z-order / wallpaper reveal change globally).
   - per-second clock pulse (~4393): `damage_add()` the PANEL_H band only.
   - `z_push_front`/focus/snap/minimize/maximize/drag/dock-hover/Alt+Tab/wallpaper/
     `cz_set_scale` zoom: `damage_add_full()`.
   - `anim_tick` while anything animates: `damage_add_full()` (don't bound an in-flight anim).
   - Keep `mark_dirty()` everywhere it is today — the gate is unchanged; damage is ADDITIVE.
9. **Frame loop wiring** (dirty block ~4410-4423; init ~4260). (a) After `g_fb_w=W;g_fb_h=H;`
   call `scissor_reset_full()`. (b) In the dirty block: when `(back!=hw && prev)` AND
   `g_dmg_any` AND NOT `boot_fading` AND no full-screen request, set active scissor from the
   accumulator; otherwise `scissor_reset_full()`. Call `composite()`. After present, call
   `scissor_reset_full()` + `damage_reset()` so the next frame starts clean and any stray
   direct draw is full-screen-safe. Leave `present_diff(hw,back,prev,W,H,stride)` UNCHANGED.
10. **Gate** (near `COMPOSITOR_STATS`, ~199): `#ifndef COMP_DAMAGE_SCISSOR / #define
    COMP_DAMAGE_SCISSOR 1 / #endif`. When 0, the per-frame narrowing compiles to
    `scissor_reset_full()` / `damage_add_full()` → byte-identical to today (the primitive
    clamps stay but are no-ops against a full-screen scissor). One-line revert / A-B proof.

---

## Pitfalls (all RESOLVED by the above)
1. **Shadow+titlebar footprint** → `win_footprint()` pads by SHADOW_PAD=24, `full_h` already
   includes TITLEBAR_H; single helper; `present_diff` full back-vs-prev scan is the safety net.
2. **Overlap/occlusion of clean windows** → resolved by design: never skip a window; the full
   z-walk re-runs every frame, scissor only limits WHERE pixels land.
3. **`g_fb_w` use-before-declaration** → scissor globals are plain ints above `fill_rect`,
   not initialized from `g_fb_w`; the helpers reading it are functions called after `g_fb_w=W`.
4. **Full-screen fallback must survive** → boot iris keeps its whole-screen present + forces
   full scissor; wallpaper/zoom/resize/single-buffer/restack/anim all `damage_add_full()`;
   active scissor reset to full after every present; `COMP_DAMAGE_SCISSOR=0` is the hard global.
5. **`drain_inbox` full mark_dirty defeats everything** → replaced by per-message damage;
   COMMIT uses the already-sent-but-ignored client damage rect.

## Gates
- Build the compositor in the **Arch** (not Ubuntu) WSL toolchain via `scripts/quick_build.sh`;
  confirm it links (no rasterizer signature changes, only internal clamps).
- Default smoke **43/43** (no shadow/title "turds", missing corners, or stale content when a
  window types over another).
- `smp_smoke.sh` under `-smp 2`: pass count unchanged (CPU0 userspace-only change).
- Manual/visual: (a) open IDE + type fast → FPS no longer collapses; with Alt+S stats on, the
  `px` + frame-time ms drop on typing frames. (b) drag a floating window over the IDE → no
  shadow turds, z-order correct. (c) per-second panel clock still ticks idle. (d) boot iris
  still full-frame. (e) Alt+wheel zoom + wallpaper still full-repaint cleanly.
- Build once with `COMP_DAMAGE_SCISSOR=0` → visually identical to pre-change (isolates any
  artifact to the scissor), then default =1.
- Edge cases: maximize IDE (footprint ≈ full screen → no corruption); >1 overlapping windows
  while one commits; minimize/restore + Alt+Tab while another animates (force full damage).

---

*Source: 30-agent modernization sweep (workflow wf_2d5342cf-6d8), `design-bounded-composite`
agent, adversarially reviewed. Companion to `GUI-LAT` (the mouse re-pump + cursor fast-path
already shipped in commit 7f5587c).*
