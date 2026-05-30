#ifndef BROWSER2_UI_H
#define BROWSER2_UI_H
/*
 * browser2_ui.h -- chrome / UX helpers for the DOM-rendering browser2.
 *
 * CONTRACT HEADER (authored by the integration owner). The three browser2-area
 * fixers share it: browser2.c (#includes + calls these), browser2_ui.c
 * (implements them). All drawing targets a 32-bit ARGB framebuffer `fb` of
 * width w / height h. The chrome occupies the TOP B2UI_CHROME_H pixels; page
 * content is painted below it by browser2.c. Freestanding: no libc beyond
 * userspace/libc string helpers, no syscalls except via browser2.c.
 *
 * browser2_ui.c MAY extend this header, but MUST NOT change the signatures or
 * the action-code values below (browser2.c is compiled against them).
 */
#define B2UI_CHROME_H 56

/* Action codes returned by b2ui_hit_chrome(). */
#define B2UI_ACT_NONE       0
#define B2UI_ACT_RELOAD     1
#define B2UI_ACT_BACK       2
#define B2UI_ACT_NEWTAB     3
#define B2UI_ACT_CLOSETAB   4
#define B2UI_ACT_ADDRFOCUS  200
#define B2UI_ACT_SELTAB     100   /* add the tab index: 100 + i */

/* Draw the chrome strip: address bar showing `url`, a tab strip of `ntabs`
 * tabs (active = `active_tab`), and a load-progress fill `load_pct` (0..100,
 * or -1 to hide). */
void b2ui_draw_chrome(unsigned int *fb, int w, int h,
                      const char *url, int load_pct,
                      int ntabs, int active_tab);

/* Hit-test a click at (x,y) against the chrome; returns a B2UI_ACT_* code
 * (B2UI_ACT_SELTAB + i for a tab click). */
int  b2ui_hit_chrome(int x, int y, int w);

/* Draw a link-hover highlight (e.g. underline / tint) over a content box at
 * (bx,by) size (bw,bh) in framebuffer coords. */
void b2ui_draw_link_hover(unsigned int *fb, int w, int h,
                          int bx, int by, int bw, int bh);

/* Draw a full-page error screen (e.g. "Could not load <url>") with `msg`. */
void b2ui_draw_error_page(unsigned int *fb, int w, int h, const char *msg);

/* Returns 0 on pass (draws into a scratch buffer and asserts pixels set). */
int  b2ui_selftest(void);

#endif /* BROWSER2_UI_H */
