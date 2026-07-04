/*
 * theme.h -- AutomationOS "Signature Dark" design tokens.
 * =============================================================================
 * THE single source of truth for the desktop's visual language. The compositor
 * chrome, the lib/ui widget library, and every app include this header instead
 * of defining their own colors, so the whole system reads as one design.
 *
 * See docs/superpowers/specs/2026-07-03-desktop-redesign-design.md.
 *
 * Aesthetic: deep graphite base + a distinctive TEAL accent (deliberately NOT
 * the iOS system blue the desktop used to clone). Hierarchy comes from
 * elevation-layered greys, three text-color tiers, size steps, and spacing --
 * the font is a single-weight 8x16 bitmap, so there are no font weights to lean
 * on.
 *
 * All colors are 0xAARRGGBB with alpha = 0xFF (opaque) unless noted.
 * Pure #defines, no dependencies -- safe to include anywhere (freestanding).
 */
#ifndef AUTOMATIONOS_UI_THEME_H
#define AUTOMATIONOS_UI_THEME_H

/* ---- Elevation-layered surfaces (dark -> light as things "lift") ---------- */
#define THEME_BG0        0xFF0E1013u   /* desktop / deepest backdrop           */
#define THEME_BG1        0xFF16191Fu   /* window body, top panel               */
#define THEME_BG2        0xFF1E222Au   /* card / surface / dock / toolbar      */
#define THEME_BG3        0xFF272C35u   /* hover                                */
#define THEME_BG4        0xFF313742u   /* pressed / active / selected row bg   */

/* ---- Text tiers ----------------------------------------------------------- */
#define THEME_TEXT       0xFFEDF0F4u   /* primary                              */
#define THEME_TEXT_DIM   0xFF9AA3AEu   /* secondary / labels                   */
#define THEME_TEXT_FAINT 0xFF6B7480u   /* tertiary / disabled / placeholder    */

/* ---- Accent (teal) -------------------------------------------------------- */
#define THEME_ACCENT     0xFF3DD6C4u   /* primary accent                       */
#define THEME_ACCENT_HI  0xFF55E0D0u   /* accent hover                         */
#define THEME_ACCENT_LO  0xFF2BB8A8u   /* accent pressed                       */
#define THEME_ON_ACCENT  0xFF06201Du   /* text/icon ON an accent fill          */

/* ---- Lines + semantics ---------------------------------------------------- */
#define THEME_HAIRLINE   0xFF2A2F38u   /* subtle divider / panel underline (decorative) */
#define THEME_BORDER     0xFF5E6979u   /* functional widget border -- >=3:1 on BG1 (WCAG 1.4.11) */
#define THEME_SUCCESS    0xFF4ADE80u   /* success / progress / online          */
#define THEME_WARN       0xFFFBBF24u   /* warning                              */
#define THEME_DANGER     0xFFF87171u   /* error / destructive / close-hover    */

/* ---- Window-chrome accents ------------------------------------------------ */
#define THEME_TITLEBAR       THEME_BG1     /* focused titlebar face            */
#define THEME_TITLEBAR_UNFOC 0xFF121419u   /* unfocused titlebar (dimmer)      */
#define THEME_FOCUS_RING     THEME_ACCENT  /* focused-window edge tint         */
#define THEME_SHADOW         0xFF000000u   /* shadow base (drawn translucent)  */

/* ---- Spacing scale (px), multiples of 4 ----------------------------------- */
#define THEME_SP_1   4
#define THEME_SP_2   8
#define THEME_SP_3   12
#define THEME_SP_4   16
#define THEME_SP_5   24
#define THEME_SP_6   32

/* ---- Corner radii (px) ---------------------------------------------------- */
#define THEME_R_SM    4
#define THEME_R_MD    8     /* window outer corners                            */
#define THEME_R_LG    12    /* dock / large surfaces                          */
#define THEME_R_PILL  999   /* toggles / fully-round                          */

/* ---- Type scale: integer multipliers of the 8x16 bitmap glyph ------------- */
#define THEME_FS_CAPTION  1
#define THEME_FS_BODY     1
#define THEME_FS_TITLE    2
#define THEME_FS_DISPLAY  3

/* ---- Motion (ms) ---------------------------------------------------------- */
#define THEME_MS_FAST  120
#define THEME_MS_BASE  180
#define THEME_MS_SLOW  240

#endif /* AUTOMATIONOS_UI_THEME_H */
