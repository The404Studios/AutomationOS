/**
 * AutomationOS Design System
 *
 * Complete design system with spacing, typography, colors, shadows,
 * and animation constants for consistent, beautiful UI.
 */

#ifndef DESIGN_SYSTEM_H
#define DESIGN_SYSTEM_H

#include <stdint.h>

// ============================================================================
// SPACING SYSTEM (8px Grid)
// ============================================================================

#define SPACING_XS   4    // Icon padding, tight spacing
#define SPACING_SM   8    // Small gaps, compact elements
#define SPACING_MD   16   // Standard padding, comfortable spacing
#define SPACING_LG   24   // Section spacing, generous padding
#define SPACING_XL   32   // Major sections, prominent spacing
#define SPACING_XXL  48   // Hero sections, dramatic spacing

// ============================================================================
// CORNER RADIUS SYSTEM
// ============================================================================

#define RADIUS_SUBTLE     4   // Buttons, inputs
#define RADIUS_STANDARD   8   // Cards, panels
#define RADIUS_PROMINENT  12  // Dialogs, large cards
#define RADIUS_DRAMATIC   16  // Dock, floating panels
#define RADIUS_HERO       24  // Splash screens, special elements

// ============================================================================
// TYPOGRAPHY SYSTEM
// ============================================================================

typedef struct {
    uint32_t size;          // Font size in pixels
    float line_height;      // Line height multiplier
    uint32_t weight;        // Font weight (400=regular, 600=semibold, 700=bold)
} typography_scale_t;

#define FONT_XS    { .size = 11, .line_height = 1.4f, .weight = 400 }
#define FONT_SM    { .size = 12, .line_height = 1.4f, .weight = 400 }
#define FONT_BASE  { .size = 13, .line_height = 1.5f, .weight = 400 }
#define FONT_MD    { .size = 14, .line_height = 1.5f, .weight = 500 }
#define FONT_LG    { .size = 16, .line_height = 1.5f, .weight = 600 }
#define FONT_XL    { .size = 20, .line_height = 1.4f, .weight = 600 }
#define FONT_2XL   { .size = 24, .line_height = 1.3f, .weight = 700 }

#define FONT_FAMILY_SYSTEM "Inter"

// ============================================================================
// SHADOW SYSTEM
// ============================================================================

typedef struct {
    int32_t offset_x;       // Horizontal offset
    int32_t offset_y;       // Vertical offset
    uint32_t blur_radius;   // Blur radius
    uint32_t spread;        // Spread radius
    uint8_t opacity;        // Opacity (0-255)
} shadow_t;

// Shadow presets (multiply opacity by 255)
#define SHADOW_SM  { .offset_x = 0, .offset_y = 1, .blur_radius = 2,  .spread = 0, .opacity = 13  }  // 0.05
#define SHADOW     { .offset_x = 0, .offset_y = 2, .blur_radius = 4,  .spread = 0, .opacity = 20  }  // 0.08
#define SHADOW_MD  { .offset_x = 0, .offset_y = 4, .blur_radius = 8,  .spread = 0, .opacity = 31  }  // 0.12
#define SHADOW_LG  { .offset_x = 0, .offset_y = 8, .blur_radius = 16, .spread = 0, .opacity = 41  }  // 0.16
#define SHADOW_XL  { .offset_x = 0, .offset_y = 16, .blur_radius = 32, .spread = 0, .opacity = 51 }  // 0.20

// ============================================================================
// ANIMATION SYSTEM
// ============================================================================

// Duration constants (microseconds)
#define ANIM_INSTANT  0
#define ANIM_FAST     150000    // 150ms - Quick feedback
#define ANIM_NORMAL   250000    // 250ms - Standard transitions
#define ANIM_SLOW     400000    // 400ms - Smooth emphasis
#define ANIM_SLOWER   600000    // 600ms - Dramatic effects

// Easing function types
typedef enum {
    EASE_LINEAR,
    EASE_IN_QUAD,
    EASE_OUT_QUAD,
    EASE_IN_OUT_QUAD,
    EASE_IN_CUBIC,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_CUBIC,
    EASE_OUT_BACK,      // Overshoot effect
    EASE_SPRING,        // Spring physics
} easing_t;

// Scale values for hover/active states
#define SCALE_HOVER     1.05f
#define SCALE_ACTIVE    0.95f
#define SCALE_MAGNIFY   1.5f

// Opacity values
#define OPACITY_DISABLED      0.4f
#define OPACITY_SECONDARY     0.6f
#define OPACITY_PLACEHOLDER   0.5f
#define OPACITY_OVERLAY_LIGHT 0.05f
#define OPACITY_OVERLAY       0.3f
#define OPACITY_OVERLAY_HEAVY 0.6f

// ============================================================================
// TOUCH TARGET SIZES
// ============================================================================

#define TOUCH_TARGET_MIN     44   // Minimum touch target size
#define TOUCH_TARGET_SPACING 8    // Minimum spacing between targets

// ============================================================================
// FOCUS RING
// ============================================================================

#define FOCUS_RING_WIDTH  2
#define FOCUS_RING_OFFSET 2

// ============================================================================
// BLUR EFFECTS
// ============================================================================

#define BLUR_SUBTLE    10   // Subtle background blur
#define BLUR_STANDARD  20   // Standard panel blur
#define BLUR_HEAVY     30   // Heavy dramatic blur

// ============================================================================
// Z-INDEX LAYERS
// ============================================================================

typedef enum {
    Z_DESKTOP      = 0,
    Z_WINDOWS      = 100,
    Z_DOCK         = 200,
    Z_PANEL        = 300,
    Z_MENUS        = 400,
    Z_NOTIFICATIONS = 500,
    Z_TOOLTIPS     = 600,
    Z_DIALOGS      = 700,
    Z_POPOVERS     = 800,
} z_index_t;

// ============================================================================
// COMPONENT SIZES
// ============================================================================

// Panel
#define PANEL_HEIGHT       32
#define PANEL_PADDING      8
#define PANEL_ICON_SIZE    16

// Dock
#define DOCK_ICON_SIZE_SM  48
#define DOCK_ICON_SIZE_MD  64
#define DOCK_ICON_SIZE_LG  96
#define DOCK_PADDING       8
#define DOCK_MARGIN        16

// Buttons
#define BUTTON_HEIGHT_SM   28
#define BUTTON_HEIGHT_MD   32
#define BUTTON_HEIGHT_LG   40
#define BUTTON_PADDING_H   16
#define BUTTON_PADDING_V   8

// Inputs
#define INPUT_HEIGHT       32
#define INPUT_PADDING_H    12
#define INPUT_PADDING_V    8
#define INPUT_BORDER_WIDTH 1

// Icons
#define ICON_SIZE_XS  12
#define ICON_SIZE_SM  16
#define ICON_SIZE_MD  24
#define ICON_SIZE_LG  32
#define ICON_SIZE_XL  48

// Notifications
#define NOTIF_WIDTH    360
#define NOTIF_PADDING  16
#define NOTIF_ICON_SIZE 40

// Tooltips
#define TOOLTIP_PADDING_H  8
#define TOOLTIP_PADDING_V  4
#define TOOLTIP_DELAY_MS   500

// Menus
#define MENU_ITEM_HEIGHT  32
#define MENU_PADDING      4
#define MENU_SEPARATOR    1

// Windows
#define WINDOW_TITLEBAR_HEIGHT  32
#define WINDOW_BORDER_WIDTH     1
#define WINDOW_RESIZE_HANDLE    4
#define WINDOW_TRAFFIC_LIGHT    12
#define WINDOW_TRAFFIC_SPACING  8

// ============================================================================
// SOUND SYSTEM
// ============================================================================

typedef struct {
    const char *name;
    uint32_t duration_ms;
    uint32_t frequency_hz;
    float volume;           // 0.0 - 1.0
} sound_effect_t;

// Sound effect definitions
#define SOUND_BUTTON_CLICK    { .name = "click",     .duration_ms = 40,  .frequency_hz = 1000, .volume = 0.3f }
#define SOUND_MENU_OPEN       { .name = "whoosh",    .duration_ms = 100, .frequency_hz = 800,  .volume = 0.2f }
#define SOUND_NOTIFICATION    { .name = "chime",     .duration_ms = 200, .frequency_hz = 880,  .volume = 0.4f }
#define SOUND_ERROR           { .name = "error",     .duration_ms = 150, .frequency_hz = 400,  .volume = 0.3f }
#define SOUND_SUCCESS         { .name = "success",   .duration_ms = 200, .frequency_hz = 1200, .volume = 0.3f }
#define SOUND_WINDOW_MINIMIZE { .name = "swoosh",    .duration_ms = 200, .frequency_hz = 600,  .volume = 0.2f }
#define SOUND_DOCK_MAGNIFY    { .name = "pop",       .duration_ms = 50,  .frequency_hz = 1500, .volume = 0.15f }

// ============================================================================
// CONTRAST RATIOS (WCAG Compliance)
// ============================================================================

#define CONTRAST_NORMAL_TEXT  4.5f   // WCAG AA for normal text
#define CONTRAST_LARGE_TEXT   3.0f   // WCAG AA for large text (18px+)
#define CONTRAST_UI_ELEMENTS  3.0f   // WCAG AA for UI components

// ============================================================================
// PERFORMANCE TARGETS
// ============================================================================

#define FPS_TARGET        60      // Target frame rate
#define FRAME_TIME_TARGET 16667   // 16.67ms per frame (60 FPS)
#define FPS_ACCEPTABLE    30      // Acceptable fallback
#define MAX_ANIMATIONS    10      // Maximum simultaneous animations

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * Interpolate between two values using easing function
 */
float ease_interpolate(float t, easing_t easing);

/**
 * Calculate shadow color with opacity
 */
uint32_t shadow_color(uint8_t opacity);

/**
 * Check if contrast ratio meets WCAG standards
 */
bool check_contrast_ratio(uint32_t color1, uint32_t color2, float min_ratio);

/**
 * Convert duration from microseconds to milliseconds
 */
static inline uint32_t us_to_ms(uint64_t us) {
    return (uint32_t)(us / 1000);
}

/**
 * Convert duration from milliseconds to microseconds
 */
static inline uint64_t ms_to_us(uint32_t ms) {
    return (uint64_t)ms * 1000;
}

#endif // DESIGN_SYSTEM_H
