/**
 * AutomationOS Theme Color Definitions
 *
 * Complete color system for light and dark themes with proper
 * contrast ratios and accessibility compliance.
 */

#ifndef THEME_COLORS_H
#define THEME_COLORS_H

#include <stdint.h>

// ============================================================================
// COLOR STRUCTURE
// ============================================================================

typedef struct {
    uint8_t r, g, b, a;
} color_rgba_t;

// Helper macros for color definition
#define RGB(r, g, b)       ((color_rgba_t){ r, g, b, 255 })
#define RGBA(r, g, b, a)   ((color_rgba_t){ r, g, b, a })
#define HEX(hex)           RGB((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF)
#define HEXA(hex, a)       RGBA((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, a)

// ============================================================================
// LIGHT THEME COLORS
// ============================================================================

// Accent Colors (Vibrant & Clear)
#define LIGHT_PRIMARY           HEX(0x007AFF)   // Bright blue
#define LIGHT_PRIMARY_HOVER     HEX(0x0066DD)   // Darker blue
#define LIGHT_PRIMARY_LIGHT     HEX(0xE6F2FF)   // Light blue background
#define LIGHT_PRIMARY_DARK      HEX(0x0055BB)   // Very dark blue

#define LIGHT_SECONDARY         HEX(0x5856D6)   // Purple
#define LIGHT_SUCCESS           HEX(0x34C759)   // Green
#define LIGHT_WARNING           HEX(0xFF9500)   // Orange
#define LIGHT_ERROR             HEX(0xFF3B30)   // Red

// Background Colors (Layered Depth)
#define LIGHT_BG_PRIMARY        HEX(0xFFFFFF)   // Base layer (white)
#define LIGHT_BG_SECONDARY      HEX(0xF8F8F8)   // Elevated layer
#define LIGHT_BG_TERTIARY       HEX(0xF0F0F0)   // Hover states
#define LIGHT_BG_QUATERNARY     HEX(0xE8E8E8)   // Active states

// Text Colors (Proper Contrast)
#define LIGHT_TEXT_PRIMARY      HEX(0x1C1C1E)   // Main text (WCAG AAA)
#define LIGHT_TEXT_SECONDARY    HEX(0x636366)   // Secondary text (WCAG AA)
#define LIGHT_TEXT_TERTIARY     HEX(0x8E8E93)   // Tertiary text
#define LIGHT_TEXT_PLACEHOLDER  HEXA(0x8E8E93, 128) // 50% opacity

// UI Element Colors
#define LIGHT_PANEL_BG          RGBA(240, 240, 240, 230)  // Semi-transparent
#define LIGHT_DOCK_BG           RGBA(255, 255, 255, 200)  // Semi-transparent white
#define LIGHT_WINDOW_BG         HEX(0xFFFFFF)
#define LIGHT_MENU_BG           RGBA(255, 255, 255, 245)  // Almost opaque

// Borders & Dividers
#define LIGHT_BORDER            HEX(0xD1D1D6)
#define LIGHT_BORDER_LIGHT      HEX(0xE5E5EA)
#define LIGHT_SEPARATOR         HEX(0xC6C6C8)

// Overlays
#define LIGHT_OVERLAY_LIGHT     RGBA(0, 0, 0, 13)    // 5% black
#define LIGHT_OVERLAY           RGBA(0, 0, 0, 77)    // 30% black
#define LIGHT_OVERLAY_HEAVY     RGBA(0, 0, 0, 153)   // 60% black

// Focus & Selection
#define LIGHT_FOCUS_RING        LIGHT_PRIMARY
#define LIGHT_SELECTION_BG      LIGHT_PRIMARY_LIGHT
#define LIGHT_SELECTION_TEXT    LIGHT_PRIMARY_DARK

// ============================================================================
// DARK THEME COLORS
// ============================================================================

// Accent Colors (Brighter for Dark Backgrounds)
#define DARK_PRIMARY            HEX(0x0A84FF)   // Brighter blue
#define DARK_PRIMARY_HOVER      HEX(0x409CFF)   // Even brighter
#define DARK_PRIMARY_LIGHT      HEX(0x1C3A52)   // Dark blue background
#define DARK_PRIMARY_DARK       HEX(0x0055BB)   // Deep blue

#define DARK_SECONDARY          HEX(0x5E5CE6)   // Bright purple
#define DARK_SUCCESS            HEX(0x32D74B)   // Bright green
#define DARK_WARNING            HEX(0xFF9F0A)   // Bright orange
#define DARK_ERROR              HEX(0xFF453A)   // Bright red

// Background Colors (Layered Depth)
#define DARK_BG_PRIMARY         HEX(0x1C1C1E)   // Base layer (dark)
#define DARK_BG_SECONDARY       HEX(0x2C2C2E)   // Elevated layer
#define DARK_BG_TERTIARY        HEX(0x3A3A3C)   // Hover states
#define DARK_BG_QUATERNARY      HEX(0x48484A)   // Active states

// Text Colors
#define DARK_TEXT_PRIMARY       HEX(0xFFFFFF)   // White (WCAG AAA)
#define DARK_TEXT_SECONDARY     HEX(0xAEAEB2)   // Light gray (WCAG AA)
#define DARK_TEXT_TERTIARY      HEX(0x8E8E93)   // Medium gray
#define DARK_TEXT_PLACEHOLDER   HEXA(0x8E8E93, 128) // 50% opacity

// UI Element Colors
#define DARK_PANEL_BG           RGBA(45, 45, 45, 230)   // Semi-transparent
#define DARK_DOCK_BG            RGBA(30, 30, 30, 200)   // Semi-transparent dark
#define DARK_WINDOW_BG          HEX(0x2C2C2E)
#define DARK_MENU_BG            RGBA(45, 45, 45, 245)   // Almost opaque

// Borders & Dividers
#define DARK_BORDER             HEX(0x38383A)
#define DARK_BORDER_LIGHT       HEX(0x2C2C2E)
#define DARK_SEPARATOR          HEX(0x48484A)

// Overlays
#define DARK_OVERLAY_LIGHT      RGBA(255, 255, 255, 13)  // 5% white
#define DARK_OVERLAY            RGBA(0, 0, 0, 128)       // 50% black
#define DARK_OVERLAY_HEAVY      RGBA(0, 0, 0, 204)       // 80% black

// Focus & Selection
#define DARK_FOCUS_RING         DARK_PRIMARY
#define DARK_SELECTION_BG       DARK_PRIMARY_LIGHT
#define DARK_SELECTION_TEXT     DARK_PRIMARY

// ============================================================================
// SEMANTIC COLORS (Theme-Aware)
// ============================================================================

typedef struct {
    // Accent colors
    color_rgba_t primary;
    color_rgba_t primary_hover;
    color_rgba_t primary_light;
    color_rgba_t primary_dark;
    color_rgba_t secondary;
    color_rgba_t success;
    color_rgba_t warning;
    color_rgba_t error;

    // Backgrounds
    color_rgba_t bg_primary;
    color_rgba_t bg_secondary;
    color_rgba_t bg_tertiary;
    color_rgba_t bg_quaternary;

    // Text
    color_rgba_t text_primary;
    color_rgba_t text_secondary;
    color_rgba_t text_tertiary;
    color_rgba_t text_placeholder;

    // UI Elements
    color_rgba_t panel_bg;
    color_rgba_t dock_bg;
    color_rgba_t window_bg;
    color_rgba_t menu_bg;

    // Borders
    color_rgba_t border;
    color_rgba_t border_light;
    color_rgba_t separator;

    // Overlays
    color_rgba_t overlay_light;
    color_rgba_t overlay;
    color_rgba_t overlay_heavy;

    // Focus & Selection
    color_rgba_t focus_ring;
    color_rgba_t selection_bg;
    color_rgba_t selection_text;
} theme_colors_t;

// ============================================================================
// TRAFFIC LIGHT COLORS (macOS Style)
// ============================================================================

#define TRAFFIC_LIGHT_CLOSE     RGB(255, 95, 87)   // Red
#define TRAFFIC_LIGHT_MINIMIZE  RGB(255, 189, 71)  // Yellow
#define TRAFFIC_LIGHT_MAXIMIZE  RGB(40, 201, 64)   // Green

#define TRAFFIC_LIGHT_CLOSE_HOVER     RGB(255, 65, 54)
#define TRAFFIC_LIGHT_MINIMIZE_HOVER  RGB(255, 169, 41)
#define TRAFFIC_LIGHT_MAXIMIZE_HOVER  RGB(20, 181, 44)

// Icons for traffic lights when hovered
#define TRAFFIC_LIGHT_ICON_CLOSE    "×"  // Close symbol
#define TRAFFIC_LIGHT_ICON_MINIMIZE "−"  // Minimize symbol
#define TRAFFIC_LIGHT_ICON_MAXIMIZE "□"  // Maximize symbol (or ⤢ for fullscreen)

// ============================================================================
// NOTIFICATION COLORS
// ============================================================================

#define NOTIF_INFO_BG       LIGHT_PRIMARY_LIGHT
#define NOTIF_SUCCESS_BG    HEX(0xE8F8EC)
#define NOTIF_WARNING_BG    HEX(0xFFF4E6)
#define NOTIF_ERROR_BG      HEX(0xFFE6E6)

#define NOTIF_INFO_TEXT     LIGHT_PRIMARY_DARK
#define NOTIF_SUCCESS_TEXT  HEX(0x1E6B2F)
#define NOTIF_WARNING_TEXT  HEX(0x8B5000)
#define NOTIF_ERROR_TEXT    HEX(0xC41E1E)

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Initialize light theme colors
 */
void init_light_theme_colors(theme_colors_t *colors);

/**
 * Initialize dark theme colors
 */
void init_dark_theme_colors(theme_colors_t *colors);

/**
 * Convert color to ARGB32 format (for rendering)
 */
static inline uint32_t color_to_argb32(color_rgba_t color) {
    return ((uint32_t)color.a << 24) |
           ((uint32_t)color.r << 16) |
           ((uint32_t)color.g << 8) |
           ((uint32_t)color.b);
}

/**
 * Convert ARGB32 to color structure
 */
static inline color_rgba_t argb32_to_color(uint32_t argb) {
    return (color_rgba_t){
        .r = (argb >> 16) & 0xFF,
        .g = (argb >> 8) & 0xFF,
        .b = argb & 0xFF,
        .a = (argb >> 24) & 0xFF
    };
}

/**
 * Blend two colors with alpha
 */
color_rgba_t color_blend(color_rgba_t fg, color_rgba_t bg);

/**
 * Adjust color opacity
 */
static inline color_rgba_t color_with_opacity(color_rgba_t color, uint8_t opacity) {
    color_rgba_t result = color;
    result.a = opacity;
    return result;
}

/**
 * Lighten color by percentage (0.0 - 1.0)
 */
color_rgba_t color_lighten(color_rgba_t color, float amount);

/**
 * Darken color by percentage (0.0 - 1.0)
 */
color_rgba_t color_darken(color_rgba_t color, float amount);

/**
 * Calculate relative luminance for contrast ratio
 */
float color_luminance(color_rgba_t color);

/**
 * Calculate contrast ratio between two colors
 */
float color_contrast_ratio(color_rgba_t c1, color_rgba_t c2);

#endif // THEME_COLORS_H
