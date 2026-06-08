/*
 * ide_theme.h -- dark palette + layout constants for the Semantic LEGO Map IDE.
 * Colours are ARGB32 (0xAARRGGBB). Tuned to resemble the project mockup.
 */
#ifndef IDE_THEME_H
#define IDE_THEME_H

/* ---- base surfaces ---- */
#define TH_BG          0xFF0C1018u  /* app background (near-black blue)   */
#define TH_PANEL       0xFF121A28u  /* panel fill                         */
#define TH_PANEL2      0xFF0F1622u  /* alt panel fill                     */
#define TH_HEADER      0xFF1A2536u  /* panel header bar                   */
#define TH_BORDER      0xFF26344Cu  /* hairline border                    */
#define TH_BORDER_LT   0xFF35496Bu  /* lighter border / divider           */
#define TH_SELECT      0xFF1D2D45u  /* selected row                       */
#define TH_HOVER       0xFF182338u  /* hover row                          */

/* ---- text ---- */
#define TH_TEXT        0xFFD7DEE8u  /* primary text                       */
#define TH_TEXT_DIM    0xFF8A98AAu  /* secondary text                     */
#define TH_TEXT_FAINT  0xFF5C6A7Eu  /* tertiary / line numbers            */

/* ---- accents (also used for port/edge colours) ---- */
#define TH_BLUE        0xFF4D9BE6u  /* input / primary accent             */
#define TH_CYAN        0xFF49C5D6u  /* state_read                         */
#define TH_GREEN       0xFF54D17Au  /* connected / safe / write-ok        */
#define TH_YELLOW      0xFFE6C24Au  /* control / weak                     */
#define TH_ORANGE      0xFFE69A4Au  /* risk / warning                     */
#define TH_RED         0xFFE2574Au  /* absent / danger                    */
#define TH_PURPLE      0xFF9B6BE2u  /* focused node accent                */
#define TH_MAGENTA     0xFFD86BB0u  /* lifecycle                          */

/* ---- pipeline status dots ---- */
#define TH_OK          TH_GREEN
#define TH_PENDING     TH_TEXT_FAINT

/* ---- layout (pixels) ---- */
/* IDE_W/IDE_H are the default window size, kept <= a 1024x768 panel so the IDE
 * never opens larger than the physical screen (the app also clamps to the live
 * work area). The text-sensitive constants below DERIVE from the runtime cell
 * (GFX_FW/GFX_FH), so when the user zooms (Ctrl+wheel) the WHOLE layout reflows
 * coherently -- rows, bars and side columns all track the glyph size. */
/* Open WIDE so the IDE fills a typical 1280-wide screen on launch (the old 1024
 * left the IDE narrow + scrunched on a 1280 desktop; the compositor clamps an
 * over-large request to the display, and the IDE reflows to win->w/h every frame
 * via the resize protocol). Height fits a 720p work area with the dock visible. */
#define IDE_W          1180
#define IDE_H          688
#define TOPBAR_H       (GFX_FH + 8)    /* VIZ tab bar (fits one glyph + pad)  */
#define STATUS_H       (GFX_FH + 4)    /* bottom shortcuts bar                */
#define LEFT_W         (20 * GFX_FW)   /* explorer column (~20 chars, ~200px @ 130%) */
#define RIGHT_W        (27 * GFX_FW)   /* inspector column (~27 chars)        */
#define RUNTIME_H      (5  * GFX_FH)   /* bottom runtime-flow strip (~5 rows) */
#define ROW_H          (GFX_FH + 2)    /* list row height (tighter for density) */
#define PAD            6

/* port-type -> accent colour mapping helper (inline) */
static inline unsigned int th_port_color(int port_type) {
    switch (port_type) {
        case 0: return TH_BLUE;     /* PORT_INPUT        */
        case 1: return TH_CYAN;     /* PORT_STATE_READ   */
        case 2: return TH_GREEN;    /* PORT_STATE_WRITE  */
        case 3: return TH_YELLOW;   /* PORT_CONTROL      */
        case 4: return TH_RED;      /* PORT_CONTROL_GATE */
        case 5: return TH_MAGENTA;  /* PORT_LIFECYCLE    */
        default: return TH_TEXT_DIM;
    }
}

#endif /* IDE_THEME_H */
