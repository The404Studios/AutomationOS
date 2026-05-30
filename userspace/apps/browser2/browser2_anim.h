#ifndef BROWSER2_ANIM_H
#define BROWSER2_ANIM_H
/*
 * browser2_anim.h -- animation helpers for browser2: inertial scrolling and
 * ARGB buffer cross-fade. CONTRACT HEADER (authored by the integration owner).
 * browser2.c #includes + calls these; browser2_anim.c implements them.
 *
 * Pure, deterministic, freestanding: no libc, no syscalls, no global I/O. All
 * state is module-static; browser2.c drives it from its render loop. The owner
 * MAY extend this header but MUST NOT change the signatures below.
 */

/* Feed a scroll impulse in pixels (+ = scroll down) from wheel / Page keys. */
void b2anim_scroll_input(int delta_px);

/* Advance the scroll model by `dt_ms`; result is clamped to
 * [0, max(0, content_h - viewport_h)]. */
void b2anim_scroll_tick(int dt_ms, int content_h, int viewport_h);

/* Current clamped scroll offset (pixels). browser2.c paints layout at -y. */
int  b2anim_scroll_y(void);

/* 1 while the scroll is still settling (browser2.c should keep rendering),
 * 0 once it has come to rest. */
int  b2anim_scroll_active(void);

/* Cross-fade `from` -> `to` into `out` over `npx` pixels; `t` in 0..255
 * (0 = all `from`, 255 = all `to`). Per-channel linear blend of ARGB32. */
void b2anim_crossfade(unsigned int *out, const unsigned int *from,
                      const unsigned int *to, int npx, int t);

/* Returns 0 on pass (asserts scroll settles + crossfade midpoint blends). */
int  b2anim_selftest(void);

#endif /* BROWSER2_ANIM_H */
