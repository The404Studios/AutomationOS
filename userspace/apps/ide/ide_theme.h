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
/* Larger default window (the 950x680 felt cramped/"too small"). The IDE relays
 * out every frame from the live window size, so this also gives Alt+Enter /
 * titlebar Maximize real room to grow toward the 1280x800 desktop. */
/* Constants grown to match the 2x text (IDE_UI_SCALE): bars that hold a 32px
 * glyph need >= ~40px, list rows >= ~36px, and the side columns need ~2x width
 * to fit the same character counts. The IDE re-layouts from the live window
 * size, so on a maximized 1280-wide desktop the editor still gets ~550px. */
#define IDE_W          1200
#define IDE_H          740
#define TOPBAR_H       44    /* VIZ tab bar              */
#define STATUS_H       38    /* bottom shortcuts bar     */
#define LEFT_W         340   /* explorer + funcs column  */
#define RIGHT_W        380   /* inspector column         */
#define RUNTIME_H      150    /* bottom runtime-flow strip*/
#define ROW_H          36    /* list row height          */
#define PAD            8

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
