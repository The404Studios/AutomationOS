/**
 * AutomationOS Theme Engine
 *
 * Runtime theme management with hot-reload support for light/dark modes
 * and custom styling. Integrates with the existing theme_colors system.
 */

#ifndef THEME_ENGINE_H
#define THEME_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "../../shell/theme/theme_colors.h"
#include "../../shell/theme/design_system.h"

// ============================================================================
// THEME TYPES
// ============================================================================

typedef enum {
    THEME_MODE_LIGHT,
    THEME_MODE_DARK,
    THEME_MODE_AUTO,    // Follow system time or preference
} theme_mode_t;

/**
 * Theme metadata
 */
typedef struct {
    char name[64];
    char author[128];
    char description[256];
    char version[16];
    theme_mode_t mode;
} theme_meta_t;

/**
 * Complete theme definition
 */
typedef struct {
    theme_meta_t meta;
    theme_colors_t colors;

    // Window decoration settings
    struct {
        uint32_t titlebar_height;
        uint32_t border_width;
        uint32_t corner_radius;
        bool show_traffic_lights;
        bool show_title_text;
    } window;

    // Component customization
    struct {
        uint32_t height;
        uint32_t padding;
        uint32_t icon_size;
        shadow_t shadow;
    } panel;

    struct {
        uint32_t icon_size;
        uint32_t padding;
        uint32_t margin;
        shadow_t shadow;
        bool magnification_enabled;
    } dock;

    struct {
        uint32_t corner_radius;
        shadow_t shadow;
    } menu;

    struct {
        uint32_t corner_radius;
        shadow_t shadow;
        uint32_t width;
    } notification;

    // Typography overrides
    struct {
        const char *family;
        uint32_t base_size;
        float scale_factor;
    } typography;

    // Animation preferences
    struct {
        bool enabled;
        float speed_multiplier;  // 1.0 = normal, 0.5 = half speed, 2.0 = double
    } animations;

    // Accessibility
    struct {
        bool high_contrast;
        bool reduce_transparency;
        bool reduce_motion;
        float text_scale;
    } accessibility;
} theme_t;

/**
 * Theme change callback
 */
typedef void (*theme_change_callback_t)(const theme_t *new_theme, void *user_data);

/**
 * Theme engine state
 */
typedef struct {
    theme_t *current_theme;
    theme_mode_t mode;

    // Loaded themes
    theme_t *themes[32];
    uint32_t theme_count;

    // Theme directories
    char system_theme_dir[256];   // /usr/share/themes
    char user_theme_dir[256];     // ~/.config/themes

    // Change notifications
    theme_change_callback_t callbacks[16];
    void *callback_data[16];
    uint32_t callback_count;

    // Auto mode settings
    struct {
        uint32_t light_hour;    // Hour to switch to light (e.g., 7 = 7am)
        uint32_t dark_hour;     // Hour to switch to dark (e.g., 19 = 7pm)
    } auto_mode;
} theme_engine_t;

// ============================================================================
// THEME ENGINE API
// ============================================================================

/**
 * Initialize theme engine
 */
theme_engine_t *theme_engine_init(void);

/**
 * Cleanup theme engine
 */
void theme_engine_cleanup(theme_engine_t *engine);

/**
 * Load theme from file
 *
 * @param engine Theme engine
 * @param path Path to theme.conf file
 * @return Loaded theme or NULL on error
 */
theme_t *theme_load(theme_engine_t *engine, const char *path);

/**
 * Load all themes from directories
 */
int theme_load_all(theme_engine_t *engine);

/**
 * Set active theme by name
 */
int theme_set_active(theme_engine_t *engine, const char *name);

/**
 * Set theme mode (light/dark/auto)
 */
int theme_set_mode(theme_engine_t *engine, theme_mode_t mode);

/**
 * Get current active theme
 */
const theme_t *theme_get_active(theme_engine_t *engine);

/**
 * Find theme by name
 */
theme_t *theme_find(theme_engine_t *engine, const char *name);

/**
 * Unload theme
 */
void theme_unload(theme_engine_t *engine, const char *name);

/**
 * Reload current theme from disk (hot-reload)
 */
int theme_reload(theme_engine_t *engine);

// ============================================================================
// THEME CHANGE NOTIFICATIONS
// ============================================================================

/**
 * Register callback for theme changes
 */
int theme_register_callback(theme_engine_t *engine, theme_change_callback_t callback, void *user_data);

/**
 * Unregister callback
 */
void theme_unregister_callback(theme_engine_t *engine, theme_change_callback_t callback);

/**
 * Notify all callbacks of theme change
 */
void theme_notify_change(theme_engine_t *engine);

// ============================================================================
// THEME QUERY API (For Widgets)
// ============================================================================

/**
 * Get color by semantic name
 *
 * Usage: theme_get_color(engine, "primary")
 */
color_rgba_t theme_get_color(theme_engine_t *engine, const char *name);

/**
 * Get window decoration settings
 */
void theme_get_window_settings(theme_engine_t *engine,
                               uint32_t *titlebar_height,
                               uint32_t *border_width,
                               uint32_t *corner_radius);

/**
 * Get panel settings
 */
void theme_get_panel_settings(theme_engine_t *engine,
                              uint32_t *height,
                              uint32_t *padding,
                              uint32_t *icon_size,
                              shadow_t *shadow);

/**
 * Get dock settings
 */
void theme_get_dock_settings(theme_engine_t *engine,
                             uint32_t *icon_size,
                             uint32_t *padding,
                             uint32_t *margin,
                             shadow_t *shadow,
                             bool *magnification);

/**
 * Check if animations are enabled
 */
bool theme_animations_enabled(theme_engine_t *engine);

/**
 * Get animation speed multiplier
 */
float theme_animation_speed(theme_engine_t *engine);

/**
 * Check if high contrast mode is enabled
 */
bool theme_high_contrast(theme_engine_t *engine);

// ============================================================================
// AUTO MODE HELPERS
// ============================================================================

/**
 * Update auto mode based on current time
 * Call this periodically (e.g., every minute)
 */
void theme_auto_mode_update(theme_engine_t *engine);

/**
 * Set auto mode schedule
 */
void theme_set_auto_schedule(theme_engine_t *engine, uint32_t light_hour, uint32_t dark_hour);

// ============================================================================
// THEME CREATION HELPERS
// ============================================================================

/**
 * Create default light theme
 */
theme_t *theme_create_default_light(void);

/**
 * Create default dark theme
 */
theme_t *theme_create_default_dark(void);

/**
 * Clone theme
 */
theme_t *theme_clone(const theme_t *source);

/**
 * Free theme
 */
void theme_free(theme_t *theme);

// ============================================================================
// THEME VALIDATION
// ============================================================================

/**
 * Validate theme for accessibility compliance
 *
 * @return true if theme meets WCAG AA standards
 */
bool theme_validate_accessibility(const theme_t *theme);

/**
 * Get validation report
 *
 * @param theme Theme to validate
 * @param buffer Buffer to write report
 * @param size Buffer size
 * @return Number of issues found
 */
int theme_get_validation_report(const theme_t *theme, char *buffer, size_t size);

#endif // THEME_ENGINE_H
