/**
 * AutomationOS Terminal - Profile and Theme Management
 *
 * Profiles define shell, appearance, and behavior
 * Themes define color schemes
 */

#include "terminal.h"
#include "../../../userspace/libc/stdio.h"
#include "../../../userspace/libc/string.h"
#include <stdlib.h>

static uint32_t next_profile_id = 1;
static uint32_t next_theme_id = 1;

// ============================================================================
// PROFILES
// ============================================================================

/**
 * Create a new profile
 */
profile_t *profile_create(const char *name) {
    profile_t *profile = (profile_t *)malloc(sizeof(profile_t));
    if (!profile) {
        return NULL;
    }

    memset(profile, 0, sizeof(profile_t));
    profile->id = next_profile_id++;

    strncpy(profile->name, name, MAX_PROFILE_NAME - 1);
    strcpy(profile->shell, "/bin/bash");
    strcpy(profile->shell_args, "");
    strcpy(profile->working_dir, "/");
    strcpy(profile->font_name, "JetBrains Mono");
    profile->font_size = 14;
    profile->theme_id = -1; // Use default

    profile->scrollback_lines = SCROLLBACK_SIZE;
    profile->scroll_on_output = false;
    profile->scroll_on_keystroke = true;
    profile->cursor_blink_rate = 500; // ms

    profile->visual_bell = true;
    profile->audible_bell = false;

    return profile;
}

/**
 * Destroy profile
 */
void profile_destroy(profile_t *profile) {
    if (profile) {
        free(profile);
    }
}

/**
 * Load profile from file (stub)
 */
profile_t *profile_load(const char *filename) {
    // TODO: Implement JSON/INI parsing
    return profile_create("Loaded Profile");
}

/**
 * Save profile to file (stub)
 */
void profile_save(const profile_t *profile, const char *filename) {
    // TODO: Implement JSON/INI serialization
}

/**
 * Get default profile
 */
profile_t *profile_get_default(void) {
    profile_t *profile = profile_create("Default");
    if (!profile) {
        return NULL;
    }

    strcpy(profile->shell, "/bin/bash");
    strcpy(profile->font_name, "JetBrains Mono");
    profile->font_size = 14;

    return profile;
}

/**
 * Add profile to terminal
 */
void terminal_add_profile(terminal_t *term, profile_t *profile) {
    if (!term || !profile) {
        return;
    }

    if (term->profile_count >= MAX_PROFILES) {
        profile_destroy(profile);
        return;
    }

    term->profiles[term->profile_count++] = profile;
}

/**
 * Get profile by ID
 */
profile_t *terminal_get_profile(terminal_t *term, uint32_t id) {
    if (!term) {
        return NULL;
    }

    for (uint32_t i = 0; i < term->profile_count; i++) {
        if (term->profiles[i] && term->profiles[i]->id == id) {
            return term->profiles[i];
        }
    }

    return NULL;
}

// ============================================================================
// THEMES
// ============================================================================

/**
 * Create a new theme
 */
theme_t *theme_create(const char *name) {
    theme_t *theme = (theme_t *)malloc(sizeof(theme_t));
    if (!theme) {
        return NULL;
    }

    memset(theme, 0, sizeof(theme_t));
    theme->id = next_theme_id++;

    strncpy(theme->name, name, MAX_THEME_NAME - 1);

    // Set default colors
    theme->foreground = color_from_rgb(192, 192, 192);
    theme->background = color_from_rgb(0, 0, 0);
    theme->cursor = color_from_rgb(255, 255, 255);
    theme->selection_bg = color_from_rgb(100, 100, 200);
    theme->selection_fg = color_from_rgb(255, 255, 255);

    return theme;
}

/**
 * Destroy theme
 */
void theme_destroy(theme_t *theme) {
    if (theme) {
        free(theme);
    }
}

/**
 * Load theme from file (stub)
 */
theme_t *theme_load(const char *filename) {
    // TODO: Implement theme file parsing
    return theme_create("Loaded Theme");
}

/**
 * Save theme to file (stub)
 */
void theme_save(const theme_t *theme, const char *filename) {
    // TODO: Implement theme file serialization
}

/**
 * Generate 256-color extended palette
 */
void theme_generate_extended_colors(theme_t *theme) {
    if (!theme) {
        return;
    }

    // Colors 16-231: 6×6×6 color cube
    for (uint32_t i = 0; i < 216; i++) {
        uint32_t r = (i / 36) * 51;
        uint32_t g = ((i / 6) % 6) * 51;
        uint32_t b = (i % 6) * 51;
        theme->extended_colors[i] = color_from_rgb(r, g, b);
    }

    // Colors 232-255: Grayscale ramp
    for (uint32_t i = 0; i < 24; i++) {
        uint32_t gray = 8 + i * 10;
        theme->extended_colors[216 + i] = color_from_rgb(gray, gray, gray);
    }
}

/**
 * Get color by index
 */
color_t theme_get_color(const theme_t *theme, uint8_t index) {
    if (!theme) {
        return color_from_rgb(255, 255, 255);
    }

    if (index < 16) {
        return theme->ansi_colors[index];
    } else if (index < 256) {
        return theme->extended_colors[index - 16];
    }

    return theme->foreground;
}

/**
 * Get default dark theme
 */
theme_t *theme_get_default_dark(void) {
    theme_t *theme = theme_create("Dark");
    if (!theme) {
        return NULL;
    }

    theme->foreground = color_from_rgb(192, 192, 192);
    theme->background = color_from_rgb(16, 16, 16);
    theme->cursor = color_from_rgb(255, 255, 255);
    theme->selection_bg = color_from_rgb(80, 80, 160);
    theme->selection_fg = color_from_rgb(255, 255, 255);

    // ANSI colors (0-7: normal, 8-15: bright)
    theme->ansi_colors[0] = color_from_rgb(0, 0, 0);           // Black
    theme->ansi_colors[1] = color_from_rgb(205, 49, 49);       // Red
    theme->ansi_colors[2] = color_from_rgb(13, 188, 121);      // Green
    theme->ansi_colors[3] = color_from_rgb(229, 229, 16);      // Yellow
    theme->ansi_colors[4] = color_from_rgb(36, 114, 200);      // Blue
    theme->ansi_colors[5] = color_from_rgb(188, 63, 188);      // Magenta
    theme->ansi_colors[6] = color_from_rgb(17, 168, 205);      // Cyan
    theme->ansi_colors[7] = color_from_rgb(229, 229, 229);     // White

    theme->ansi_colors[8] = color_from_rgb(102, 102, 102);     // Bright Black
    theme->ansi_colors[9] = color_from_rgb(241, 76, 76);       // Bright Red
    theme->ansi_colors[10] = color_from_rgb(35, 209, 139);     // Bright Green
    theme->ansi_colors[11] = color_from_rgb(245, 245, 67);     // Bright Yellow
    theme->ansi_colors[12] = color_from_rgb(59, 142, 234);     // Bright Blue
    theme->ansi_colors[13] = color_from_rgb(214, 112, 214);    // Bright Magenta
    theme->ansi_colors[14] = color_from_rgb(41, 184, 219);     // Bright Cyan
    theme->ansi_colors[15] = color_from_rgb(255, 255, 255);    // Bright White

    // UI colors
    theme->tab_bar_bg = color_from_rgb(32, 32, 32);
    theme->tab_bar_fg = color_from_rgb(192, 192, 192);
    theme->tab_active_bg = color_from_rgb(48, 48, 48);
    theme->tab_active_fg = color_from_rgb(255, 255, 255);
    theme->scrollbar_bg = color_from_rgb(24, 24, 24);
    theme->scrollbar_fg = color_from_rgb(96, 96, 96);
    theme->border_color = color_from_rgb(64, 64, 64);

    theme_generate_extended_colors(theme);
    return theme;
}

/**
 * Get default light theme
 */
theme_t *theme_get_default_light(void) {
    theme_t *theme = theme_create("Light");
    if (!theme) {
        return NULL;
    }

    theme->foreground = color_from_rgb(32, 32, 32);
    theme->background = color_from_rgb(250, 250, 250);
    theme->cursor = color_from_rgb(0, 0, 0);
    theme->selection_bg = color_from_rgb(180, 180, 220);
    theme->selection_fg = color_from_rgb(0, 0, 0);

    // ANSI colors for light background
    theme->ansi_colors[0] = color_from_rgb(0, 0, 0);
    theme->ansi_colors[1] = color_from_rgb(170, 0, 0);
    theme->ansi_colors[2] = color_from_rgb(0, 170, 0);
    theme->ansi_colors[3] = color_from_rgb(170, 85, 0);
    theme->ansi_colors[4] = color_from_rgb(0, 0, 170);
    theme->ansi_colors[5] = color_from_rgb(170, 0, 170);
    theme->ansi_colors[6] = color_from_rgb(0, 170, 170);
    theme->ansi_colors[7] = color_from_rgb(170, 170, 170);

    theme->ansi_colors[8] = color_from_rgb(85, 85, 85);
    theme->ansi_colors[9] = color_from_rgb(255, 85, 85);
    theme->ansi_colors[10] = color_from_rgb(85, 255, 85);
    theme->ansi_colors[11] = color_from_rgb(255, 255, 85);
    theme->ansi_colors[12] = color_from_rgb(85, 85, 255);
    theme->ansi_colors[13] = color_from_rgb(255, 85, 255);
    theme->ansi_colors[14] = color_from_rgb(85, 255, 255);
    theme->ansi_colors[15] = color_from_rgb(255, 255, 255);

    // UI colors
    theme->tab_bar_bg = color_from_rgb(230, 230, 230);
    theme->tab_bar_fg = color_from_rgb(32, 32, 32);
    theme->tab_active_bg = color_from_rgb(255, 255, 255);
    theme->tab_active_fg = color_from_rgb(0, 0, 0);
    theme->scrollbar_bg = color_from_rgb(240, 240, 240);
    theme->scrollbar_fg = color_from_rgb(160, 160, 160);
    theme->border_color = color_from_rgb(200, 200, 200);

    theme_generate_extended_colors(theme);
    return theme;
}

/**
 * Get Monokai theme
 */
theme_t *theme_get_monokai(void) {
    theme_t *theme = theme_create("Monokai");
    if (!theme) {
        return NULL;
    }

    theme->foreground = color_from_rgb(248, 248, 242);
    theme->background = color_from_rgb(39, 40, 34);
    theme->cursor = color_from_rgb(253, 151, 31);
    theme->selection_bg = color_from_rgb(73, 72, 62);
    theme->selection_fg = color_from_rgb(248, 248, 242);

    theme->ansi_colors[0] = color_from_rgb(39, 40, 34);
    theme->ansi_colors[1] = color_from_rgb(249, 38, 114);
    theme->ansi_colors[2] = color_from_rgb(166, 226, 46);
    theme->ansi_colors[3] = color_from_rgb(244, 191, 117);
    theme->ansi_colors[4] = color_from_rgb(102, 217, 239);
    theme->ansi_colors[5] = color_from_rgb(174, 129, 255);
    theme->ansi_colors[6] = color_from_rgb(161, 239, 228);
    theme->ansi_colors[7] = color_from_rgb(248, 248, 242);

    theme->ansi_colors[8] = color_from_rgb(117, 113, 94);
    theme->ansi_colors[9] = color_from_rgb(249, 38, 114);
    theme->ansi_colors[10] = color_from_rgb(166, 226, 46);
    theme->ansi_colors[11] = color_from_rgb(244, 191, 117);
    theme->ansi_colors[12] = color_from_rgb(102, 217, 239);
    theme->ansi_colors[13] = color_from_rgb(174, 129, 255);
    theme->ansi_colors[14] = color_from_rgb(161, 239, 228);
    theme->ansi_colors[15] = color_from_rgb(249, 248, 245);

    theme->tab_bar_bg = color_from_rgb(32, 33, 27);
    theme->tab_bar_fg = color_from_rgb(248, 248, 242);
    theme->tab_active_bg = color_from_rgb(39, 40, 34);
    theme->tab_active_fg = color_from_rgb(253, 151, 31);
    theme->scrollbar_bg = color_from_rgb(32, 33, 27);
    theme->scrollbar_fg = color_from_rgb(117, 113, 94);
    theme->border_color = color_from_rgb(73, 72, 62);

    theme_generate_extended_colors(theme);
    return theme;
}

/**
 * Get Dracula theme
 */
theme_t *theme_get_dracula(void) {
    theme_t *theme = theme_create("Dracula");
    if (!theme) {
        return NULL;
    }

    theme->foreground = color_from_rgb(248, 248, 242);
    theme->background = color_from_rgb(40, 42, 54);
    theme->cursor = color_from_rgb(248, 248, 242);
    theme->selection_bg = color_from_rgb(68, 71, 90);
    theme->selection_fg = color_from_rgb(248, 248, 242);

    theme->ansi_colors[0] = color_from_rgb(33, 34, 44);
    theme->ansi_colors[1] = color_from_rgb(255, 85, 85);
    theme->ansi_colors[2] = color_from_rgb(80, 250, 123);
    theme->ansi_colors[3] = color_from_rgb(241, 250, 140);
    theme->ansi_colors[4] = color_from_rgb(189, 147, 249);
    theme->ansi_colors[5] = color_from_rgb(255, 121, 198);
    theme->ansi_colors[6] = color_from_rgb(139, 233, 253);
    theme->ansi_colors[7] = color_from_rgb(248, 248, 242);

    theme->ansi_colors[8] = color_from_rgb(98, 114, 164);
    theme->ansi_colors[9] = color_from_rgb(255, 110, 103);
    theme->ansi_colors[10] = color_from_rgb(90, 247, 142);
    theme->ansi_colors[11] = color_from_rgb(244, 249, 157);
    theme->ansi_colors[12] = color_from_rgb(202, 169, 250);
    theme->ansi_colors[13] = color_from_rgb(255, 146, 208);
    theme->ansi_colors[14] = color_from_rgb(154, 237, 254);
    theme->ansi_colors[15] = color_from_rgb(255, 255, 255);

    theme->tab_bar_bg = color_from_rgb(33, 34, 44);
    theme->tab_bar_fg = color_from_rgb(248, 248, 242);
    theme->tab_active_bg = color_from_rgb(40, 42, 54);
    theme->tab_active_fg = color_from_rgb(139, 233, 253);
    theme->scrollbar_bg = color_from_rgb(33, 34, 44);
    theme->scrollbar_fg = color_from_rgb(98, 114, 164);
    theme->border_color = color_from_rgb(68, 71, 90);

    theme_generate_extended_colors(theme);
    return theme;
}

/**
 * Solarized Dark (stub - implement if needed)
 */
theme_t *theme_get_solarized_dark(void) {
    return theme_get_default_dark();
}

/**
 * Solarized Light (stub - implement if needed)
 */
theme_t *theme_get_solarized_light(void) {
    return theme_get_default_light();
}

/**
 * Add theme to terminal
 */
void terminal_add_theme(terminal_t *term, theme_t *theme) {
    if (!term || !theme) {
        return;
    }

    if (term->theme_count >= MAX_THEMES) {
        theme_destroy(theme);
        return;
    }

    term->themes[term->theme_count++] = theme;
}

/**
 * Get theme by ID
 */
theme_t *terminal_get_theme(terminal_t *term, uint32_t id) {
    if (!term || id >= term->theme_count) {
        return NULL;
    }

    return term->themes[id];
}

// ============================================================================
// COLOR UTILITIES
// ============================================================================

color_t color_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (color_t){ r, g, b, 255 };
}

color_t color_from_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (color_t){ r, g, b, a };
}

uint32_t color_to_u32(const color_t *color) {
    if (!color) {
        return 0xFF000000;
    }
    return (color->a << 24) | (color->r << 16) | (color->g << 8) | color->b;
}

bool color_equals(const color_t *a, const color_t *b) {
    if (!a || !b) {
        return false;
    }
    return a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a;
}

void color_blend(color_t *result, const color_t *fg, const color_t *bg) {
    if (!result || !fg || !bg) {
        return;
    }

    float alpha = fg->a / 255.0f;
    result->r = (uint8_t)(fg->r * alpha + bg->r * (1.0f - alpha));
    result->g = (uint8_t)(fg->g * alpha + bg->g * (1.0f - alpha));
    result->b = (uint8_t)(fg->b * alpha + bg->b * (1.0f - alpha));
    result->a = 255;
}
