/**
 * AutomationOS Theme Color Implementation
 */

#include "theme_colors.h"
#include <math.h>

// ============================================================================
// THEME INITIALIZATION
// ============================================================================

void init_light_theme_colors(theme_colors_t *colors) {
    if (!colors) return;

    // Accent colors
    colors->primary = LIGHT_PRIMARY;
    colors->primary_hover = LIGHT_PRIMARY_HOVER;
    colors->primary_light = LIGHT_PRIMARY_LIGHT;
    colors->primary_dark = LIGHT_PRIMARY_DARK;
    colors->secondary = LIGHT_SECONDARY;
    colors->success = LIGHT_SUCCESS;
    colors->warning = LIGHT_WARNING;
    colors->error = LIGHT_ERROR;

    // Backgrounds
    colors->bg_primary = LIGHT_BG_PRIMARY;
    colors->bg_secondary = LIGHT_BG_SECONDARY;
    colors->bg_tertiary = LIGHT_BG_TERTIARY;
    colors->bg_quaternary = LIGHT_BG_QUATERNARY;

    // Text
    colors->text_primary = LIGHT_TEXT_PRIMARY;
    colors->text_secondary = LIGHT_TEXT_SECONDARY;
    colors->text_tertiary = LIGHT_TEXT_TERTIARY;
    colors->text_placeholder = LIGHT_TEXT_PLACEHOLDER;

    // UI Elements
    colors->panel_bg = LIGHT_PANEL_BG;
    colors->dock_bg = LIGHT_DOCK_BG;
    colors->window_bg = LIGHT_WINDOW_BG;
    colors->menu_bg = LIGHT_MENU_BG;

    // Borders
    colors->border = LIGHT_BORDER;
    colors->border_light = LIGHT_BORDER_LIGHT;
    colors->separator = LIGHT_SEPARATOR;

    // Overlays
    colors->overlay_light = LIGHT_OVERLAY_LIGHT;
    colors->overlay = LIGHT_OVERLAY;
    colors->overlay_heavy = LIGHT_OVERLAY_HEAVY;

    // Focus & Selection
    colors->focus_ring = LIGHT_FOCUS_RING;
    colors->selection_bg = LIGHT_SELECTION_BG;
    colors->selection_text = LIGHT_SELECTION_TEXT;
}

void init_dark_theme_colors(theme_colors_t *colors) {
    if (!colors) return;

    // Accent colors
    colors->primary = DARK_PRIMARY;
    colors->primary_hover = DARK_PRIMARY_HOVER;
    colors->primary_light = DARK_PRIMARY_LIGHT;
    colors->primary_dark = DARK_PRIMARY_DARK;
    colors->secondary = DARK_SECONDARY;
    colors->success = DARK_SUCCESS;
    colors->warning = DARK_WARNING;
    colors->error = DARK_ERROR;

    // Backgrounds
    colors->bg_primary = DARK_BG_PRIMARY;
    colors->bg_secondary = DARK_BG_SECONDARY;
    colors->bg_tertiary = DARK_BG_TERTIARY;
    colors->bg_quaternary = DARK_BG_QUATERNARY;

    // Text
    colors->text_primary = DARK_TEXT_PRIMARY;
    colors->text_secondary = DARK_TEXT_SECONDARY;
    colors->text_tertiary = DARK_TEXT_TERTIARY;
    colors->text_placeholder = DARK_TEXT_PLACEHOLDER;

    // UI Elements
    colors->panel_bg = DARK_PANEL_BG;
    colors->dock_bg = DARK_DOCK_BG;
    colors->window_bg = DARK_WINDOW_BG;
    colors->menu_bg = DARK_MENU_BG;

    // Borders
    colors->border = DARK_BORDER;
    colors->border_light = DARK_BORDER_LIGHT;
    colors->separator = DARK_SEPARATOR;

    // Overlays
    colors->overlay_light = DARK_OVERLAY_LIGHT;
    colors->overlay = DARK_OVERLAY;
    colors->overlay_heavy = DARK_OVERLAY_HEAVY;

    // Focus & Selection
    colors->focus_ring = DARK_FOCUS_RING;
    colors->selection_bg = DARK_SELECTION_BG;
    colors->selection_text = DARK_SELECTION_TEXT;
}

// ============================================================================
// COLOR MANIPULATION
// ============================================================================

color_rgba_t color_blend(color_rgba_t fg, color_rgba_t bg) {
    if (fg.a == 255) {
        return fg;
    }
    if (fg.a == 0) {
        return bg;
    }

    float alpha_fg = fg.a / 255.0f;
    float alpha_bg = bg.a / 255.0f;
    float alpha_out = alpha_fg + alpha_bg * (1.0f - alpha_fg);

    color_rgba_t result;
    result.r = (uint8_t)((fg.r * alpha_fg + bg.r * alpha_bg * (1.0f - alpha_fg)) / alpha_out);
    result.g = (uint8_t)((fg.g * alpha_fg + bg.g * alpha_bg * (1.0f - alpha_fg)) / alpha_out);
    result.b = (uint8_t)((fg.b * alpha_fg + bg.b * alpha_bg * (1.0f - alpha_fg)) / alpha_out);
    result.a = (uint8_t)(alpha_out * 255.0f);

    return result;
}

color_rgba_t color_lighten(color_rgba_t color, float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;

    color_rgba_t result;
    result.r = (uint8_t)(color.r + (255 - color.r) * amount);
    result.g = (uint8_t)(color.g + (255 - color.g) * amount);
    result.b = (uint8_t)(color.b + (255 - color.b) * amount);
    result.a = color.a;

    return result;
}

color_rgba_t color_darken(color_rgba_t color, float amount) {
    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;

    color_rgba_t result;
    result.r = (uint8_t)(color.r * (1.0f - amount));
    result.g = (uint8_t)(color.g * (1.0f - amount));
    result.b = (uint8_t)(color.b * (1.0f - amount));
    result.a = color.a;

    return result;
}

// ============================================================================
// CONTRAST CALCULATION (WCAG)
// ============================================================================

/**
 * Convert sRGB color component to linear RGB
 */
static float srgb_to_linear(uint8_t component) {
    float c = component / 255.0f;
    if (c <= 0.04045f) {
        return c / 12.92f;
    }
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

float color_luminance(color_rgba_t color) {
    // Convert to linear RGB
    float r = srgb_to_linear(color.r);
    float g = srgb_to_linear(color.g);
    float b = srgb_to_linear(color.b);

    // Calculate relative luminance
    // Formula from WCAG: 0.2126 * R + 0.7152 * G + 0.0722 * B
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

float color_contrast_ratio(color_rgba_t c1, color_rgba_t c2) {
    float l1 = color_luminance(c1);
    float l2 = color_luminance(c2);

    // Ensure l1 is the lighter color
    if (l1 < l2) {
        float temp = l1;
        l1 = l2;
        l2 = temp;
    }

    // Calculate contrast ratio
    // Formula: (L1 + 0.05) / (L2 + 0.05)
    return (l1 + 0.05f) / (l2 + 0.05f);
}
