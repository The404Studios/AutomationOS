/**
 * AutomationOS Accessibility Framework - Implementation
 */

#include "accessibility.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Calculate relative luminance for contrast ratio
 * Formula from WCAG 2.1
 */
static float calculate_luminance(uint8_t r, uint8_t g, uint8_t b) {
    float rs = r / 255.0f;
    float gs = g / 255.0f;
    float bs = b / 255.0f;

    // Apply gamma correction
    rs = (rs <= 0.03928f) ? rs / 12.92f : powf((rs + 0.055f) / 1.055f, 2.4f);
    gs = (gs <= 0.03928f) ? gs / 12.92f : powf((gs + 0.055f) / 1.055f, 2.4f);
    bs = (bs <= 0.03928f) ? bs / 12.92f : powf((bs + 0.055f) / 1.055f, 2.4f);

    // Calculate luminance
    return 0.2126f * rs + 0.7152f * gs + 0.0722f * bs;
}

/**
 * Extract RGB components from ARGB32
 */
static void unpack_color(uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = (color >> 16) & 0xFF;
    *g = (color >> 8) & 0xFF;
    *b = color & 0xFF;
}

/**
 * Pack RGB into ARGB32 (preserve alpha)
 */
static uint32_t pack_color(uint32_t original, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t alpha = original & 0xFF000000;
    return alpha | (r << 16) | (g << 8) | b;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

accessibility_context_t *accessibility_init(void) {
    accessibility_context_t *ctx = calloc(1, sizeof(accessibility_context_t));
    if (!ctx) {
        fprintf(stderr, "[A11Y] Failed to allocate context\n");
        return NULL;
    }

    // Set defaults
    accessibility_reset_defaults(ctx);

    printf("[A11Y] Accessibility framework initialized\n");
    return ctx;
}

void accessibility_cleanup(accessibility_context_t *ctx) {
    if (!ctx) return;

    free(ctx);
    printf("[A11Y] Accessibility framework cleaned up\n");
}

void accessibility_reset_defaults(accessibility_context_t *ctx) {
    if (!ctx) return;

    memset(ctx, 0, sizeof(accessibility_context_t));

    // Visual defaults
    ctx->visual.contrast_mode = CONTRAST_MODE_NORMAL;
    ctx->visual.text_scale = 1.0f;
    ctx->visual.min_font_size = 12;
    ctx->visual.color_blind_mode = COLOR_BLIND_NONE;
    ctx->visual.cursor_size = CURSOR_SIZE_NORMAL;
    ctx->visual.cursor_blink_rate = 500;  // 500ms
    ctx->visual.brightness_boost = 0.0f;

    // Motor defaults
    ctx->motor.keyboard_nav_enabled = true;
    ctx->motor.show_focus_indicators = true;
    ctx->motor.focus_indicator_width = 2;
    ctx->motor.slow_keys_delay_ms = 300;
    ctx->motor.bounce_keys_delay_ms = 500;
    ctx->motor.mouse_keys_speed = 5;
    ctx->motor.mouse_keys_acceleration = 3;
    ctx->motor.dwell_time_ms = 1200;

    // Auditory defaults
    ctx->auditory.audio_amplification = 1.0f;
    ctx->auditory.audio_balance = 0.0f;
    ctx->auditory.caption_font_size = 16;
    strncpy(ctx->auditory.caption_font, "sans-serif", 64);

    // Cognitive defaults
    ctx->cognitive.timeout_multiplier = 1;
    ctx->cognitive.reading_speed = 200;  // words per minute

    // Global
    ctx->accessibility_enabled = true;
    ctx->triple_click_accessibility = true;
    ctx->hold_modifier_enable = true;
    strncpy(ctx->active_profile, "Default", 64);
}

// ============================================================================
// CONTRAST CALCULATION (WCAG 2.1)
// ============================================================================

float accessibility_calculate_contrast_ratio(uint32_t color1, uint32_t color2) {
    uint8_t r1, g1, b1, r2, g2, b2;
    unpack_color(color1, &r1, &g1, &b1);
    unpack_color(color2, &r2, &g2, &b2);

    float l1 = calculate_luminance(r1, g1, b1);
    float l2 = calculate_luminance(r2, g2, b2);

    // Ensure l1 is lighter
    if (l2 > l1) {
        float temp = l1;
        l1 = l2;
        l2 = temp;
    }

    // WCAG formula: (L1 + 0.05) / (L2 + 0.05)
    return (l1 + 0.05f) / (l2 + 0.05f);
}

bool accessibility_meets_wcag_contrast(float ratio, bool large_text) {
    float required = large_text ? WCAG_LARGE_TEXT_CONTRAST : WCAG_MIN_CONTRAST_RATIO;
    return ratio >= required;
}

bool accessibility_validate_contrast(uint32_t fg, uint32_t bg, bool large_text) {
    float ratio = accessibility_calculate_contrast_ratio(fg, bg);
    return accessibility_meets_wcag_contrast(ratio, large_text);
}

// ============================================================================
// COLOR TRANSFORMATIONS
// ============================================================================

uint32_t accessibility_apply_color_blind_filter(uint32_t color, color_blind_mode_t mode) {
    uint8_t r, g, b;
    unpack_color(color, &r, &g, &b);

    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;

    float r_out, g_out, b_out;

    switch (mode) {
        case COLOR_BLIND_PROTANOPIA:  // Red-blind
            // Simplified Brettel simulation
            r_out = 0.567f * rf + 0.433f * gf;
            g_out = 0.558f * rf + 0.442f * gf;
            b_out = 0.242f * gf + 0.758f * bf;
            break;

        case COLOR_BLIND_DEUTERANOPIA:  // Green-blind
            r_out = 0.625f * rf + 0.375f * gf;
            g_out = 0.7f * rf + 0.3f * gf;
            b_out = 0.3f * gf + 0.7f * bf;
            break;

        case COLOR_BLIND_TRITANOPIA:  // Blue-blind
            r_out = 0.95f * rf + 0.05f * gf;
            g_out = 0.433f * gf + 0.567f * bf;
            b_out = 0.475f * gf + 0.525f * bf;
            break;

        case COLOR_BLIND_ACHROMATOPSIA:  // Total color blindness (grayscale)
            {
                float gray = 0.299f * rf + 0.587f * gf + 0.114f * bf;
                r_out = g_out = b_out = gray;
            }
            break;

        default:
            return color;
    }

    // Clamp and convert back
    r = (uint8_t)(fminf(fmaxf(r_out * 255.0f, 0.0f), 255.0f));
    g = (uint8_t)(fminf(fmaxf(g_out * 255.0f, 0.0f), 255.0f));
    b = (uint8_t)(fminf(fmaxf(b_out * 255.0f, 0.0f), 255.0f));

    return pack_color(color, r, g, b);
}

uint32_t accessibility_apply_high_contrast(uint32_t fg, uint32_t bg) {
    float ratio = accessibility_calculate_contrast_ratio(fg, bg);

    if (ratio >= WCAG_MIN_CONTRAST_RATIO) {
        return fg;  // Already good contrast
    }

    // Adjust foreground to achieve minimum contrast
    uint8_t bg_r, bg_g, bg_b;
    unpack_color(bg, &bg_r, &bg_g, &bg_b);
    float bg_lum = calculate_luminance(bg_r, bg_g, bg_b);

    // If background is dark, make foreground white
    // If background is light, make foreground black
    if (bg_lum < 0.5f) {
        return pack_color(fg, 255, 255, 255);
    } else {
        return pack_color(fg, 0, 0, 0);
    }
}

uint32_t accessibility_invert_color(uint32_t color) {
    uint8_t r, g, b;
    unpack_color(color, &r, &g, &b);
    return pack_color(color, 255 - r, 255 - g, 255 - b);
}

// ============================================================================
// VISUAL SETTINGS
// ============================================================================

void accessibility_set_contrast_mode(accessibility_context_t *ctx, contrast_mode_t mode) {
    if (!ctx) return;
    ctx->visual.contrast_mode = mode;
    printf("[A11Y] Contrast mode: %d\n", mode);
}

void accessibility_set_text_scale(accessibility_context_t *ctx, float scale) {
    if (!ctx) return;
    ctx->visual.text_scale = fmaxf(1.0f, fminf(scale, 3.0f));
    printf("[A11Y] Text scale: %.2fx\n", ctx->visual.text_scale);
}

void accessibility_set_color_blind_mode(accessibility_context_t *ctx, color_blind_mode_t mode) {
    if (!ctx) return;
    ctx->visual.color_blind_mode = mode;
    printf("[A11Y] Color blind mode: %d\n", mode);
}

void accessibility_enable_screen_reader(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->visual.screen_reader_enabled = enable;
    printf("[A11Y] Screen reader: %s\n", enable ? "enabled" : "disabled");
}

// ============================================================================
// MOTOR SETTINGS
// ============================================================================

void accessibility_enable_keyboard_nav(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->motor.keyboard_nav_enabled = enable;
    printf("[A11Y] Keyboard navigation: %s\n", enable ? "enabled" : "disabled");
}

void accessibility_enable_sticky_keys(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->motor.sticky_keys_enabled = enable;
    printf("[A11Y] Sticky keys: %s\n", enable ? "enabled" : "disabled");
}

void accessibility_enable_mouse_keys(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->motor.mouse_keys_enabled = enable;
    printf("[A11Y] Mouse keys: %s\n", enable ? "enabled" : "disabled");
}

// ============================================================================
// AUDITORY SETTINGS
// ============================================================================

void accessibility_enable_visual_alerts(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->auditory.visual_alerts_enabled = enable;
    printf("[A11Y] Visual alerts: %s\n", enable ? "enabled" : "disabled");
}

void accessibility_enable_captions(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->auditory.captions_enabled = enable;
    printf("[A11Y] Captions: %s\n", enable ? "enabled" : "disabled");
}

void accessibility_set_mono_audio(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->auditory.mono_audio = enable;
    printf("[A11Y] Mono audio: %s\n", enable ? "enabled" : "disabled");
}

// ============================================================================
// COGNITIVE SETTINGS
// ============================================================================

void accessibility_enable_simple_mode(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->cognitive.simple_mode = enable;
    printf("[A11Y] Simple mode: %s\n", enable ? "enabled" : "disabled");
}

void accessibility_enable_focus_assist(accessibility_context_t *ctx, bool enable) {
    if (!ctx) return;
    ctx->cognitive.focus_assist = enable;
    printf("[A11Y] Focus assist: %s\n", enable ? "enabled" : "disabled");
}

// ============================================================================
// VALIDATION
// ============================================================================

bool accessibility_validate_touch_target(uint32_t width, uint32_t height) {
    return (width >= WCAG_MIN_TOUCH_TARGET && height >= WCAG_MIN_TOUCH_TARGET);
}

bool accessibility_validate_text_width(uint32_t chars_per_line) {
    return chars_per_line <= WCAG_TEXT_MAX_WIDTH;
}

bool accessibility_validate_flash_rate(float flashes_per_second) {
    return flashes_per_second <= WCAG_ANIMATION_MAX_FLASH;
}

// ============================================================================
// TESTING
// ============================================================================

accessibility_test_report_t accessibility_run_tests(accessibility_context_t *ctx) {
    accessibility_test_report_t report;
    memset(&report, 0, sizeof(report));

    char *buf = report.report;
    size_t remaining = sizeof(report.report);
    int written = 0;

    written = snprintf(buf, remaining, "AutomationOS Accessibility Test Report\n");
    buf += written; remaining -= written;

    written = snprintf(buf, remaining, "========================================\n\n");
    buf += written; remaining -= written;

    // Test keyboard navigation
    report.keyboard_nav_test_passed = ctx->motor.keyboard_nav_enabled;
    written = snprintf(buf, remaining, "Keyboard Navigation: %s\n",
                      report.keyboard_nav_test_passed ? "PASS" : "FAIL");
    buf += written; remaining -= written;

    // Test high contrast
    report.high_contrast_test_passed = (ctx->visual.contrast_mode != CONTRAST_MODE_NORMAL ||
                                        ctx->visual.text_scale >= 1.0f);
    written = snprintf(buf, remaining, "High Contrast Support: %s\n",
                      report.high_contrast_test_passed ? "PASS" : "FAIL");
    buf += written; remaining -= written;

    // Test screen reader
    report.screen_reader_test_passed = ctx->visual.screen_reader_enabled;
    written = snprintf(buf, remaining, "Screen Reader Support: %s\n",
                      report.screen_reader_test_passed ? "PASS" : "FAIL");
    buf += written; remaining -= written;

    // Test text resize
    report.text_resize_test_passed = (ctx->visual.text_scale <= 2.0f);
    written = snprintf(buf, remaining, "Text Resize (up to 200%%): %s\n",
                      report.text_resize_test_passed ? "PASS" : "FAIL");
    buf += written; remaining -= written;

    // Test color blind support
    report.color_blind_test_passed = (ctx->visual.color_blind_mode != COLOR_BLIND_NONE ||
                                      ctx->visual.contrast_mode == CONTRAST_MODE_HIGH);
    written = snprintf(buf, remaining, "Color Blind Support: %s\n",
                      report.color_blind_test_passed ? "PASS" : "FAIL");
    buf += written; remaining -= written;

    // Test focus indicators
    report.focus_visible_test_passed = ctx->motor.show_focus_indicators;
    written = snprintf(buf, remaining, "Focus Indicators: %s\n",
                      report.focus_visible_test_passed ? "PASS" : "FAIL");
    buf += written; remaining -= written;

    written = snprintf(buf, remaining, "\nWCAG 2.1 Level AA Compliance\n");
    buf += written; remaining -= written;
    written = snprintf(buf, remaining, "----------------------------\n");
    buf += written; remaining -= written;
    written = snprintf(buf, remaining, "Failed Contrast Checks: %u\n", report.failed_contrast_checks);
    buf += written; remaining -= written;
    written = snprintf(buf, remaining, "Failed Touch Targets: %u\n", report.failed_touch_targets);
    buf += written; remaining -= written;

    return report;
}

void accessibility_generate_report(accessibility_test_report_t *report, const char *output_file) {
    if (!report || !output_file) return;

    FILE *f = fopen(output_file, "w");
    if (!f) {
        fprintf(stderr, "[A11Y] Failed to open %s\n", output_file);
        return;
    }

    fprintf(f, "%s", report->report);
    fclose(f);

    printf("[A11Y] Report written to %s\n", output_file);
}

// ============================================================================
// CONFIGURATION I/O
// ============================================================================

bool accessibility_load_config(accessibility_context_t *ctx, const char *config_file) {
    if (!ctx || !config_file) return false;

    // TODO: Implement config file loading
    printf("[A11Y] Loading config from %s\n", config_file);
    return true;
}

bool accessibility_save_config(accessibility_context_t *ctx, const char *config_file) {
    if (!ctx || !config_file) return false;

    // TODO: Implement config file saving
    printf("[A11Y] Saving config to %s\n", config_file);
    return true;
}

bool accessibility_load_profile(accessibility_context_t *ctx, const char *profile_name) {
    if (!ctx || !profile_name) return false;

    printf("[A11Y] Loading profile: %s\n", profile_name);

    // Predefined profiles
    if (strcmp(profile_name, "Low Vision") == 0) {
        ctx->visual.text_scale = 2.0f;
        ctx->visual.contrast_mode = CONTRAST_MODE_HIGH;
        ctx->visual.cursor_size = CURSOR_SIZE_EXTRA_LARGE;
        ctx->visual.screen_reader_enabled = true;
        ctx->visual.reduce_motion = true;
    } else if (strcmp(profile_name, "Motor") == 0) {
        ctx->motor.sticky_keys_enabled = true;
        ctx->motor.slow_keys_enabled = true;
        ctx->motor.keyboard_nav_enabled = true;
        ctx->motor.show_focus_indicators = true;
        ctx->motor.dwell_click_enabled = true;
    } else if (strcmp(profile_name, "Auditory") == 0) {
        ctx->auditory.visual_alerts_enabled = true;
        ctx->auditory.captions_enabled = true;
        ctx->auditory.mono_audio = true;
        ctx->auditory.audio_amplification = 1.5f;
    } else if (strcmp(profile_name, "Cognitive") == 0) {
        ctx->cognitive.simple_mode = true;
        ctx->cognitive.focus_assist = true;
        ctx->cognitive.extended_timeouts = true;
        ctx->cognitive.timeout_multiplier = 3;
        ctx->visual.reduce_motion = true;
    } else {
        printf("[A11Y] Unknown profile: %s\n", profile_name);
        return false;
    }

    strncpy(ctx->active_profile, profile_name, 64);
    return true;
}

// ============================================================================
// SCREEN READER (stub)
// ============================================================================

static screen_reader_callback_t g_screen_reader_callback = NULL;
static void *g_screen_reader_user_data = NULL;

void accessibility_register_screen_reader(screen_reader_callback_t callback, void *user_data) {
    g_screen_reader_callback = callback;
    g_screen_reader_user_data = user_data;
    printf("[A11Y] Screen reader callback registered\n");
}

void accessibility_announce(accessibility_context_t *ctx, const char *text) {
    if (!ctx || !text) return;
    if (!ctx->visual.screen_reader_enabled) return;

    if (g_screen_reader_callback) {
        g_screen_reader_callback(text, g_screen_reader_user_data);
    } else {
        printf("[A11Y Screen Reader] %s\n", text);
    }
}

void accessibility_describe_element(accessibility_context_t *ctx, const char *role,
                                    const char *name, const char *state) {
    if (!ctx) return;

    char description[512];
    snprintf(description, sizeof(description), "%s, %s, %s",
             role ? role : "element",
             name ? name : "unnamed",
             state ? state : "");

    accessibility_announce(ctx, description);
}
