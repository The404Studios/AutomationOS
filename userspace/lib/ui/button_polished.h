/**
 * Polished Button Component
 *
 * Beautiful, accessible buttons with hover states, ripple effects,
 * focus indicators, and smooth animations.
 */

#ifndef BUTTON_POLISHED_H
#define BUTTON_POLISHED_H

#include <stdint.h>
#include <stdbool.h>
#include "../animation/animator.h"
#include "../../shell/theme/theme_colors.h"
#include "../../shell/theme/design_system.h"

// ============================================================================
// BUTTON TYPES
// ============================================================================

typedef enum {
    BUTTON_PRIMARY,      // Filled button with primary color
    BUTTON_SECONDARY,    // Outlined button
    BUTTON_TERTIARY,     // Text-only button
    BUTTON_DESTRUCTIVE,  // Destructive action (red)
    BUTTON_SUCCESS,      // Success action (green)
} button_style_t;

typedef enum {
    BUTTON_SIZE_SM,      // Small: 28px height
    BUTTON_SIZE_MD,      // Medium: 32px height (default)
    BUTTON_SIZE_LG,      // Large: 40px height
} button_size_t;

// ============================================================================
// BUTTON STATE
// ============================================================================

typedef struct {
    bool hovered;
    bool pressed;
    bool focused;
    bool disabled;

    // Animation state
    float scale;         // Current scale (1.0 = normal)
    float opacity;       // Current opacity (1.0 = full)

    // Ripple effect
    struct {
        bool active;
        float x, y;      // Ripple center (relative to button)
        float radius;    // Current radius
        float opacity;   // Current opacity
        uint64_t start_time;
    } ripple;
} button_state_t;

// ============================================================================
// BUTTON STRUCTURE
// ============================================================================

typedef struct {
    // Position and size
    int32_t x, y;
    uint32_t width, height;

    // Content
    char label[128];
    const char *icon;        // Optional icon (UTF-8 symbol or path)
    bool icon_left;          // Icon on left (true) or right (false)

    // Style
    button_style_t style;
    button_size_t size;
    uint32_t corner_radius;  // Override default radius

    // State
    button_state_t state;

    // Callback
    void (*on_click)(void *user_data);
    void *user_data;

    // Animation
    animator_t *animator;
} button_t;

// ============================================================================
// BUTTON API
// ============================================================================

/**
 * Create a new button
 */
button_t *button_create(const char *label, int32_t x, int32_t y);

/**
 * Create a button with specific style and size
 */
button_t *button_create_styled(const char *label, int32_t x, int32_t y,
                                button_style_t style, button_size_t size);

/**
 * Destroy button and free resources
 */
void button_destroy(button_t *button);

/**
 * Set button label
 */
void button_set_label(button_t *button, const char *label);

/**
 * Set button icon
 */
void button_set_icon(button_t *button, const char *icon, bool icon_left);

/**
 * Set button style
 */
void button_set_style(button_t *button, button_style_t style);

/**
 * Set button size
 */
void button_set_size(button_t *button, button_size_t size);

/**
 * Enable/disable button
 */
void button_set_enabled(button_t *button, bool enabled);

/**
 * Set click callback
 */
void button_set_callback(button_t *button, void (*on_click)(void *), void *user_data);

// ============================================================================
// EVENT HANDLING
// ============================================================================

/**
 * Handle mouse move event
 */
void button_on_mouse_move(button_t *button, int32_t mouse_x, int32_t mouse_y);

/**
 * Handle mouse button press
 */
void button_on_mouse_down(button_t *button, int32_t mouse_x, int32_t mouse_y);

/**
 * Handle mouse button release
 */
void button_on_mouse_up(button_t *button);

/**
 * Handle mouse leave (cursor left button area)
 */
void button_on_mouse_leave(button_t *button);

/**
 * Handle keyboard focus
 */
void button_on_focus(button_t *button);

/**
 * Handle keyboard blur
 */
void button_on_blur(button_t *button);

/**
 * Handle keyboard activation (Enter/Space)
 */
void button_on_key_press(button_t *button);

// ============================================================================
// RENDERING
// ============================================================================

/**
 * Update button animations
 */
void button_update(button_t *button, uint64_t delta_us);

/**
 * Render button with current theme
 */
void button_render(button_t *button, theme_colors_t *theme);

// ============================================================================
// ANIMATION HELPERS (Internal)
// ============================================================================

/**
 * Start hover animation
 */
void button_anim_hover_start(button_t *button);

/**
 * Start unhover animation
 */
void button_anim_hover_end(button_t *button);

/**
 * Start press animation
 */
void button_anim_press(button_t *button, int32_t x, int32_t y);

/**
 * Start release animation
 */
void button_anim_release(button_t *button);

/**
 * Update ripple effect
 */
void button_update_ripple(button_t *button, uint64_t current_time);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * Check if point is inside button
 */
bool button_contains_point(button_t *button, int32_t x, int32_t y);

/**
 * Calculate button dimensions based on size
 */
void button_calculate_size(button_t *button);

/**
 * Get button background color based on style and state
 */
color_rgba_t button_get_bg_color(button_t *button, theme_colors_t *theme);

/**
 * Get button text color based on style and state
 */
color_rgba_t button_get_text_color(button_t *button, theme_colors_t *theme);

/**
 * Get button border color based on style and state
 */
color_rgba_t button_get_border_color(button_t *button, theme_colors_t *theme);

#endif // BUTTON_POLISHED_H
