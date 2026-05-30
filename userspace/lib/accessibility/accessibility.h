/**
 * AutomationOS Accessibility Framework
 *
 * WCAG 2.1 Level AA compliant accessibility system providing:
 * - Visual accessibility (high contrast, large text, screen readers)
 * - Motor accessibility (keyboard navigation, sticky keys, mouse keys)
 * - Auditory accessibility (visual alerts, captions)
 * - Cognitive accessibility (simple mode, clear messaging)
 */

#ifndef ACCESSIBILITY_H
#define ACCESSIBILITY_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// ACCESSIBILITY STANDARDS
// ============================================================================

// WCAG 2.1 Level AA Requirements
#define WCAG_MIN_CONTRAST_RATIO     4.5f    // 4.5:1 for normal text
#define WCAG_LARGE_TEXT_CONTRAST    3.0f    // 3:1 for large text (18pt+)
#define WCAG_MIN_TOUCH_TARGET       44      // 44x44px minimum touch target
#define WCAG_TEXT_MAX_WIDTH         80      // 80 characters per line max
#define WCAG_ANIMATION_MAX_FLASH    3       // Max 3 flashes per second

// Text size multipliers
#define TEXT_SIZE_DEFAULT           1.0f
#define TEXT_SIZE_LARGE_125         1.25f
#define TEXT_SIZE_LARGE_150         1.5f
#define TEXT_SIZE_LARGE_200         2.0f

// ============================================================================
// VISUAL ACCESSIBILITY
// ============================================================================

typedef enum {
    CONTRAST_MODE_NORMAL,
    CONTRAST_MODE_HIGH,
    CONTRAST_MODE_INVERTED,
    CONTRAST_MODE_CUSTOM,
} contrast_mode_t;

typedef enum {
    COLOR_BLIND_NONE,
    COLOR_BLIND_PROTANOPIA,      // Red-blind
    COLOR_BLIND_DEUTERANOPIA,    // Green-blind
    COLOR_BLIND_TRITANOPIA,      // Blue-blind
    COLOR_BLIND_ACHROMATOPSIA,   // Total color blindness
} color_blind_mode_t;

typedef enum {
    CURSOR_SIZE_NORMAL,
    CURSOR_SIZE_LARGE,
    CURSOR_SIZE_EXTRA_LARGE,
} cursor_size_t;

typedef struct {
    // Contrast
    contrast_mode_t contrast_mode;
    float contrast_ratio;                // Current contrast ratio

    // Text size
    float text_scale;                    // 1.0, 1.25, 1.5, 2.0
    uint32_t min_font_size;              // Minimum font size in pixels

    // Color blindness
    color_blind_mode_t color_blind_mode;
    bool color_blind_simulation;         // Show preview

    // Cursor
    cursor_size_t cursor_size;
    bool cursor_highlight;               // Highlight cursor position
    uint32_t cursor_blink_rate;          // Milliseconds (0 = no blink)

    // Screen reader
    bool screen_reader_enabled;
    bool announce_notifications;
    bool announce_window_changes;

    // Visual effects
    bool reduce_motion;                  // Disable/minimize animations
    bool reduce_transparency;            // Remove transparent effects
    bool remove_animations;              // Completely disable animations

    // Display
    bool grayscale_mode;
    bool invert_colors;
    float brightness_boost;              // 0.0 - 1.0
} visual_accessibility_t;

// ============================================================================
// MOTOR ACCESSIBILITY
// ============================================================================

typedef struct {
    // Keyboard navigation
    bool keyboard_nav_enabled;           // Tab navigation everywhere
    bool show_focus_indicators;          // Always show focus rings
    uint32_t focus_indicator_width;      // Width in pixels
    bool vim_keys_enabled;               // hjkl navigation

    // Sticky keys
    bool sticky_keys_enabled;
    bool sticky_keys_beep;
    bool sticky_keys_lock;

    // Slow keys
    bool slow_keys_enabled;
    uint32_t slow_keys_delay_ms;         // Delay before key accepted

    // Bounce keys
    bool bounce_keys_enabled;
    uint32_t bounce_keys_delay_ms;       // Ignore rapid repeated keys

    // Mouse keys
    bool mouse_keys_enabled;             // Control mouse with numpad
    uint32_t mouse_keys_speed;           // 1-10
    uint32_t mouse_keys_acceleration;    // 1-10

    // Voice control
    bool voice_control_enabled;
    char voice_command_prefix[32];       // e.g., "Computer, "

    // On-screen keyboard
    bool osk_enabled;
    uint32_t osk_size;                   // Percentage of screen

    // Dwell click
    bool dwell_click_enabled;
    uint32_t dwell_time_ms;              // Time to hover for click
} motor_accessibility_t;

// ============================================================================
// AUDITORY ACCESSIBILITY
// ============================================================================

typedef struct {
    // Visual alerts
    bool visual_alerts_enabled;          // Flash screen for sounds
    bool taskbar_flash;                  // Flash taskbar instead

    // Captions
    bool captions_enabled;
    uint32_t caption_font_size;
    char caption_font[64];

    // Audio
    bool mono_audio;                     // Convert stereo to mono
    float audio_amplification;           // 1.0 - 3.0
    bool audio_balance_left;
    bool audio_balance_right;
    float audio_balance;                 // -1.0 (left) to 1.0 (right)
} auditory_accessibility_t;

// ============================================================================
// COGNITIVE ACCESSIBILITY
// ============================================================================

typedef struct {
    // Simplification
    bool simple_mode;                    // Simplified UI
    bool large_buttons;
    bool clear_language;
    bool consistent_layout;

    // Timing
    bool extended_timeouts;
    uint32_t timeout_multiplier;         // 2x, 3x, etc.

    // Reading
    bool dyslexia_font;
    bool reading_guide;                  // Highlight line being read
    uint32_t reading_speed;              // For auto-scroll

    // Focus
    bool focus_assist;                   // Minimize distractions
    bool block_notifications;
    bool hide_animations;
} cognitive_accessibility_t;

// ============================================================================
// MAIN ACCESSIBILITY CONTEXT
// ============================================================================

typedef struct {
    visual_accessibility_t visual;
    motor_accessibility_t motor;
    auditory_accessibility_t auditory;
    cognitive_accessibility_t cognitive;

    // Global settings
    bool accessibility_enabled;
    bool auto_enable_features;           // Auto-detect and enable

    // Shortcuts
    bool triple_click_accessibility;     // Power button 3x
    bool hold_modifier_enable;           // Hold shift 5s

    // Profiles
    char active_profile[64];             // "Low Vision", "Motor", etc.
} accessibility_context_t;

// ============================================================================
// API FUNCTIONS
// ============================================================================

// Initialization
accessibility_context_t *accessibility_init(void);
void accessibility_cleanup(accessibility_context_t *ctx);
void accessibility_reset_defaults(accessibility_context_t *ctx);

// Configuration
bool accessibility_load_config(accessibility_context_t *ctx, const char *config_file);
bool accessibility_save_config(accessibility_context_t *ctx, const char *config_file);
bool accessibility_load_profile(accessibility_context_t *ctx, const char *profile_name);

// Visual
void accessibility_set_contrast_mode(accessibility_context_t *ctx, contrast_mode_t mode);
void accessibility_set_text_scale(accessibility_context_t *ctx, float scale);
void accessibility_set_color_blind_mode(accessibility_context_t *ctx, color_blind_mode_t mode);
void accessibility_enable_screen_reader(accessibility_context_t *ctx, bool enable);
float accessibility_calculate_contrast_ratio(uint32_t color1, uint32_t color2);
bool accessibility_meets_wcag_contrast(float ratio, bool large_text);

// Color adjustments
uint32_t accessibility_apply_color_blind_filter(uint32_t color, color_blind_mode_t mode);
uint32_t accessibility_apply_high_contrast(uint32_t fg, uint32_t bg);
uint32_t accessibility_invert_color(uint32_t color);

// Motor
void accessibility_enable_keyboard_nav(accessibility_context_t *ctx, bool enable);
void accessibility_enable_sticky_keys(accessibility_context_t *ctx, bool enable);
void accessibility_enable_mouse_keys(accessibility_context_t *ctx, bool enable);

// Auditory
void accessibility_enable_visual_alerts(accessibility_context_t *ctx, bool enable);
void accessibility_enable_captions(accessibility_context_t *ctx, bool enable);
void accessibility_set_mono_audio(accessibility_context_t *ctx, bool enable);

// Cognitive
void accessibility_enable_simple_mode(accessibility_context_t *ctx, bool enable);
void accessibility_enable_focus_assist(accessibility_context_t *ctx, bool enable);

// Screen reader
typedef void (*screen_reader_callback_t)(const char *text, void *user_data);
void accessibility_register_screen_reader(screen_reader_callback_t callback, void *user_data);
void accessibility_announce(accessibility_context_t *ctx, const char *text);
void accessibility_describe_element(accessibility_context_t *ctx, const char *role,
                                    const char *name, const char *state);

// Keyboard navigation
typedef enum {
    FOCUS_ORDER_SPATIAL,     // Based on screen position
    FOCUS_ORDER_LOGICAL,     // Based on tab order
} focus_order_t;

typedef struct {
    uint32_t element_id;
    rect_t bounds;
    int32_t tab_index;
    bool focusable;
    char role[32];           // "button", "textbox", etc.
    char label[256];
} focusable_element_t;

void accessibility_register_focusable(accessibility_context_t *ctx, focusable_element_t *element);
void accessibility_unregister_focusable(accessibility_context_t *ctx, uint32_t element_id);
void accessibility_focus_next(accessibility_context_t *ctx);
void accessibility_focus_previous(accessibility_context_t *ctx);
uint32_t accessibility_get_focused_element(accessibility_context_t *ctx);

// Validation
bool accessibility_validate_contrast(uint32_t fg, uint32_t bg, bool large_text);
bool accessibility_validate_touch_target(uint32_t width, uint32_t height);
bool accessibility_validate_text_width(uint32_t chars_per_line);
bool accessibility_validate_flash_rate(float flashes_per_second);

// Testing
typedef struct {
    bool keyboard_nav_test_passed;
    bool high_contrast_test_passed;
    bool screen_reader_test_passed;
    bool text_resize_test_passed;
    bool color_blind_test_passed;
    bool focus_visible_test_passed;
    uint32_t failed_contrast_checks;
    uint32_t failed_touch_targets;
    char report[4096];
} accessibility_test_report_t;

accessibility_test_report_t accessibility_run_tests(accessibility_context_t *ctx);
void accessibility_generate_report(accessibility_test_report_t *report, const char *output_file);

#endif // ACCESSIBILITY_H
