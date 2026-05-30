/**
 * AutomationOS Theme Engine Implementation
 */

#include "theme.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

// Thread-local storage for last error
static __thread char last_error[256] = {0};

/**
 * Set error message
 */
static void set_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof(last_error), format, args);
    va_end(args);
}

/**
 * Get last error message
 */
const char *theme_get_last_error(void) {
    return last_error;
}

// ============================================================================
// THEME ENGINE INITIALIZATION
// ============================================================================

theme_engine_t *theme_engine_init(void) {
    theme_engine_t *engine = calloc(1, sizeof(theme_engine_t));
    if (!engine) {
        set_error("Failed to allocate theme engine");
        return NULL;
    }

    // Set default theme directories
    snprintf(engine->system_theme_dir, sizeof(engine->system_theme_dir),
             "/usr/share/themes");
    snprintf(engine->user_theme_dir, sizeof(engine->user_theme_dir),
             "%s/.config/themes", getenv("HOME") ?: "/root");

    // Default mode: auto
    engine->mode = THEME_MODE_AUTO;

    // Default auto mode schedule: 7am light, 7pm dark
    engine->auto_mode.light_hour = 7;
    engine->auto_mode.dark_hour = 19;

    // Initialize with default light theme
    engine->current_theme = theme_create_default_light();
    if (!engine->current_theme) {
        free(engine);
        return NULL;
    }

    printf("[Theme] Engine initialized\n");
    return engine;
}

void theme_engine_cleanup(theme_engine_t *engine) {
    if (!engine) return;

    // Free all loaded themes
    for (uint32_t i = 0; i < engine->theme_count; i++) {
        theme_free(engine->themes[i]);
    }

    // Free current theme if not in loaded list
    if (engine->current_theme) {
        theme_free(engine->current_theme);
    }

    free(engine);
    printf("[Theme] Engine cleaned up\n");
}

// ============================================================================
// THEME LOADING
// ============================================================================

theme_t *theme_load(theme_engine_t *engine, const char *path) {
    if (!engine || !path) {
        set_error("Invalid parameters");
        return NULL;
    }

    // Check if theme already loaded
    for (uint32_t i = 0; i < engine->theme_count; i++) {
        theme_t *t = engine->themes[i];
        if (strcmp(t->meta.name, path) == 0) {
            printf("[Theme] Theme '%s' already loaded\n", t->meta.name);
            return t;
        }
    }

    // Parse theme file
    theme_t *theme = theme_parse_file(path);
    if (!theme) {
        char errors[1024];
        theme_get_parse_errors(errors, sizeof(errors));
        set_error("Failed to parse theme: %s", errors);
        return NULL;
    }

    // Add to loaded themes
    if (engine->theme_count < 32) {
        engine->themes[engine->theme_count++] = theme;
        printf("[Theme] Loaded theme: %s\n", theme->meta.name);
    } else {
        set_error("Maximum theme limit reached");
        theme_free(theme);
        return NULL;
    }

    return theme;
}

int theme_load_all(theme_engine_t *engine) {
    if (!engine) return -1;

    int loaded = 0;

    // Load from system directory
    DIR *dir = opendir(engine->system_theme_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;

            char theme_path[512];
            snprintf(theme_path, sizeof(theme_path), "%s/%s/theme.conf",
                    engine->system_theme_dir, entry->d_name);

            // Check if theme.conf exists
            struct stat st;
            if (stat(theme_path, &st) == 0) {
                if (theme_load(engine, theme_path)) {
                    loaded++;
                }
            }
        }
        closedir(dir);
    }

    // Load from user directory
    dir = opendir(engine->user_theme_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;

            char theme_path[512];
            snprintf(theme_path, sizeof(theme_path), "%s/%s/theme.conf",
                    engine->user_theme_dir, entry->d_name);

            struct stat st;
            if (stat(theme_path, &st) == 0) {
                if (theme_load(engine, theme_path)) {
                    loaded++;
                }
            }
        }
        closedir(dir);
    }

    printf("[Theme] Loaded %d themes\n", loaded);
    return loaded;
}

// ============================================================================
// THEME ACTIVATION
// ============================================================================

int theme_set_active(theme_engine_t *engine, const char *name) {
    if (!engine || !name) return -1;

    // Find theme by name
    theme_t *theme = theme_find(engine, name);
    if (!theme) {
        set_error("Theme '%s' not found", name);
        return -1;
    }

    // Switch to new theme
    engine->current_theme = theme;

    // Notify callbacks
    theme_notify_change(engine);

    printf("[Theme] Activated theme: %s\n", name);
    return 0;
}

int theme_set_mode(theme_engine_t *engine, theme_mode_t mode) {
    if (!engine) return -1;

    engine->mode = mode;

    // If switching to light/dark, find appropriate theme
    if (mode == THEME_MODE_LIGHT) {
        theme_t *light = theme_find(engine, "Default Light");
        if (light) {
            engine->current_theme = light;
            theme_notify_change(engine);
        }
    } else if (mode == THEME_MODE_DARK) {
        theme_t *dark = theme_find(engine, "Default Dark");
        if (dark) {
            engine->current_theme = dark;
            theme_notify_change(engine);
        }
    } else if (mode == THEME_MODE_AUTO) {
        // Update based on current time
        theme_auto_mode_update(engine);
    }

    printf("[Theme] Set mode: %s\n",
           mode == THEME_MODE_LIGHT ? "light" :
           mode == THEME_MODE_DARK ? "dark" : "auto");
    return 0;
}

const theme_t *theme_get_active(theme_engine_t *engine) {
    return engine ? engine->current_theme : NULL;
}

theme_t *theme_find(theme_engine_t *engine, const char *name) {
    if (!engine || !name) return NULL;

    for (uint32_t i = 0; i < engine->theme_count; i++) {
        if (strcmp(engine->themes[i]->meta.name, name) == 0) {
            return engine->themes[i];
        }
    }

    return NULL;
}

void theme_unload(theme_engine_t *engine, const char *name) {
    if (!engine || !name) return;

    for (uint32_t i = 0; i < engine->theme_count; i++) {
        if (strcmp(engine->themes[i]->meta.name, name) == 0) {
            // Don't unload if it's the current theme
            if (engine->themes[i] == engine->current_theme) {
                set_error("Cannot unload active theme");
                return;
            }

            theme_free(engine->themes[i]);

            // Shift remaining themes
            for (uint32_t j = i; j < engine->theme_count - 1; j++) {
                engine->themes[j] = engine->themes[j + 1];
            }
            engine->theme_count--;

            printf("[Theme] Unloaded theme: %s\n", name);
            return;
        }
    }
}

int theme_reload(theme_engine_t *engine) {
    if (!engine || !engine->current_theme) return -1;

    // Find theme file path
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/theme.conf",
            engine->system_theme_dir, engine->current_theme->meta.name);

    // Parse fresh from disk
    theme_t *new_theme = theme_parse_file(path);
    if (!new_theme) {
        set_error("Failed to reload theme");
        return -1;
    }

    // Replace current theme
    theme_free(engine->current_theme);
    engine->current_theme = new_theme;

    // Notify callbacks
    theme_notify_change(engine);

    printf("[Theme] Reloaded theme: %s\n", new_theme->meta.name);
    return 0;
}

// ============================================================================
// THEME CHANGE NOTIFICATIONS
// ============================================================================

int theme_register_callback(theme_engine_t *engine, theme_change_callback_t callback, void *user_data) {
    if (!engine || !callback) return -1;

    if (engine->callback_count >= 16) {
        set_error("Maximum callback limit reached");
        return -1;
    }

    engine->callbacks[engine->callback_count] = callback;
    engine->callback_data[engine->callback_count] = user_data;
    engine->callback_count++;

    return 0;
}

void theme_unregister_callback(theme_engine_t *engine, theme_change_callback_t callback) {
    if (!engine || !callback) return;

    for (uint32_t i = 0; i < engine->callback_count; i++) {
        if (engine->callbacks[i] == callback) {
            // Shift remaining callbacks
            for (uint32_t j = i; j < engine->callback_count - 1; j++) {
                engine->callbacks[j] = engine->callbacks[j + 1];
                engine->callback_data[j] = engine->callback_data[j + 1];
            }
            engine->callback_count--;
            return;
        }
    }
}

void theme_notify_change(theme_engine_t *engine) {
    if (!engine) return;

    for (uint32_t i = 0; i < engine->callback_count; i++) {
        engine->callbacks[i](engine->current_theme, engine->callback_data[i]);
    }
}

// ============================================================================
// THEME QUERY API
// ============================================================================

color_rgba_t theme_get_color(theme_engine_t *engine, const char *name) {
    if (!engine || !engine->current_theme || !name) {
        return (color_rgba_t){0, 0, 0, 255};
    }

    theme_colors_t *colors = &engine->current_theme->colors;

    if (strcmp(name, "primary") == 0) return colors->primary;
    if (strcmp(name, "primary_hover") == 0) return colors->primary_hover;
    if (strcmp(name, "secondary") == 0) return colors->secondary;
    if (strcmp(name, "success") == 0) return colors->success;
    if (strcmp(name, "warning") == 0) return colors->warning;
    if (strcmp(name, "error") == 0) return colors->error;
    if (strcmp(name, "background") == 0) return colors->bg_primary;
    if (strcmp(name, "text") == 0) return colors->text_primary;

    return (color_rgba_t){0, 0, 0, 255};
}

void theme_get_window_settings(theme_engine_t *engine,
                               uint32_t *titlebar_height,
                               uint32_t *border_width,
                               uint32_t *corner_radius) {
    if (!engine || !engine->current_theme) return;

    if (titlebar_height) *titlebar_height = engine->current_theme->window.titlebar_height;
    if (border_width) *border_width = engine->current_theme->window.border_width;
    if (corner_radius) *corner_radius = engine->current_theme->window.corner_radius;
}

void theme_get_panel_settings(theme_engine_t *engine,
                              uint32_t *height,
                              uint32_t *padding,
                              uint32_t *icon_size,
                              shadow_t *shadow) {
    if (!engine || !engine->current_theme) return;

    if (height) *height = engine->current_theme->panel.height;
    if (padding) *padding = engine->current_theme->panel.padding;
    if (icon_size) *icon_size = engine->current_theme->panel.icon_size;
    if (shadow) *shadow = engine->current_theme->panel.shadow;
}

void theme_get_dock_settings(theme_engine_t *engine,
                             uint32_t *icon_size,
                             uint32_t *padding,
                             uint32_t *margin,
                             shadow_t *shadow,
                             bool *magnification) {
    if (!engine || !engine->current_theme) return;

    if (icon_size) *icon_size = engine->current_theme->dock.icon_size;
    if (padding) *padding = engine->current_theme->dock.padding;
    if (margin) *margin = engine->current_theme->dock.margin;
    if (shadow) *shadow = engine->current_theme->dock.shadow;
    if (magnification) *magnification = engine->current_theme->dock.magnification_enabled;
}

bool theme_animations_enabled(theme_engine_t *engine) {
    return engine && engine->current_theme ?
           engine->current_theme->animations.enabled : true;
}

float theme_animation_speed(theme_engine_t *engine) {
    return engine && engine->current_theme ?
           engine->current_theme->animations.speed_multiplier : 1.0f;
}

bool theme_high_contrast(theme_engine_t *engine) {
    return engine && engine->current_theme ?
           engine->current_theme->accessibility.high_contrast : false;
}

// ============================================================================
// AUTO MODE
// ============================================================================

void theme_auto_mode_update(theme_engine_t *engine) {
    if (!engine || engine->mode != THEME_MODE_AUTO) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    uint32_t current_hour = tm->tm_hour;

    bool should_be_light = (current_hour >= engine->auto_mode.light_hour &&
                           current_hour < engine->auto_mode.dark_hour);

    theme_mode_t target_mode = should_be_light ? THEME_MODE_LIGHT : THEME_MODE_DARK;

    // Switch theme if needed
    if (engine->current_theme->meta.mode != target_mode) {
        const char *theme_name = target_mode == THEME_MODE_LIGHT ?
                                 "Default Light" : "Default Dark";
        theme_set_active(engine, theme_name);
    }
}

void theme_set_auto_schedule(theme_engine_t *engine, uint32_t light_hour, uint32_t dark_hour) {
    if (!engine) return;

    engine->auto_mode.light_hour = light_hour;
    engine->auto_mode.dark_hour = dark_hour;

    if (engine->mode == THEME_MODE_AUTO) {
        theme_auto_mode_update(engine);
    }
}

// ============================================================================
// THEME CREATION
// ============================================================================

theme_t *theme_create_default_light(void) {
    theme_t *theme = calloc(1, sizeof(theme_t));
    if (!theme) return NULL;

    // Metadata
    snprintf(theme->meta.name, sizeof(theme->meta.name), "Default Light");
    snprintf(theme->meta.author, sizeof(theme->meta.author), "AutomationOS Team");
    snprintf(theme->meta.description, sizeof(theme->meta.description),
             "Clean, accessible light theme");
    snprintf(theme->meta.version, sizeof(theme->meta.version), "1.0");
    theme->meta.mode = THEME_MODE_LIGHT;

    // Colors
    init_light_theme_colors(&theme->colors);

    // Window decorations
    theme->window.titlebar_height = WINDOW_TITLEBAR_HEIGHT;
    theme->window.border_width = WINDOW_BORDER_WIDTH;
    theme->window.corner_radius = RADIUS_STANDARD;
    theme->window.show_traffic_lights = true;
    theme->window.show_title_text = true;

    // Panel
    theme->panel.height = PANEL_HEIGHT;
    theme->panel.padding = PANEL_PADDING;
    theme->panel.icon_size = PANEL_ICON_SIZE;
    theme->panel.shadow = SHADOW_SM;

    // Dock
    theme->dock.icon_size = DOCK_ICON_SIZE_MD;
    theme->dock.padding = DOCK_PADDING;
    theme->dock.margin = DOCK_MARGIN;
    theme->dock.shadow = SHADOW_MD;
    theme->dock.magnification_enabled = true;

    // Menu
    theme->menu.corner_radius = RADIUS_STANDARD;
    theme->menu.shadow = SHADOW;

    // Notification
    theme->notification.corner_radius = RADIUS_PROMINENT;
    theme->notification.shadow = SHADOW_MD;
    theme->notification.width = NOTIF_WIDTH;

    // Typography
    theme->typography.family = FONT_FAMILY_SYSTEM;
    theme->typography.base_size = 13;
    theme->typography.scale_factor = 1.0f;

    // Animations
    theme->animations.enabled = true;
    theme->animations.speed_multiplier = 1.0f;

    // Accessibility
    theme->accessibility.high_contrast = false;
    theme->accessibility.reduce_transparency = false;
    theme->accessibility.reduce_motion = false;
    theme->accessibility.text_scale = 1.0f;

    return theme;
}

theme_t *theme_create_default_dark(void) {
    theme_t *theme = calloc(1, sizeof(theme_t));
    if (!theme) return NULL;

    // Metadata
    snprintf(theme->meta.name, sizeof(theme->meta.name), "Default Dark");
    snprintf(theme->meta.author, sizeof(theme->meta.author), "AutomationOS Team");
    snprintf(theme->meta.description, sizeof(theme->meta.description),
             "Beautiful dark theme for low-light");
    snprintf(theme->meta.version, sizeof(theme->meta.version), "1.0");
    theme->meta.mode = THEME_MODE_DARK;

    // Colors
    init_dark_theme_colors(&theme->colors);

    // Window decorations
    theme->window.titlebar_height = WINDOW_TITLEBAR_HEIGHT;
    theme->window.border_width = WINDOW_BORDER_WIDTH;
    theme->window.corner_radius = RADIUS_STANDARD;
    theme->window.show_traffic_lights = true;
    theme->window.show_title_text = true;

    // Panel
    theme->panel.height = PANEL_HEIGHT;
    theme->panel.padding = PANEL_PADDING;
    theme->panel.icon_size = PANEL_ICON_SIZE;
    theme->panel.shadow = SHADOW_SM;

    // Dock
    theme->dock.icon_size = DOCK_ICON_SIZE_MD;
    theme->dock.padding = DOCK_PADDING;
    theme->dock.margin = DOCK_MARGIN;
    theme->dock.shadow = SHADOW_LG;
    theme->dock.magnification_enabled = true;

    // Menu
    theme->menu.corner_radius = RADIUS_STANDARD;
    theme->menu.shadow = SHADOW_MD;

    // Notification
    theme->notification.corner_radius = RADIUS_PROMINENT;
    theme->notification.shadow = SHADOW_LG;
    theme->notification.width = NOTIF_WIDTH;

    // Typography
    theme->typography.family = FONT_FAMILY_SYSTEM;
    theme->typography.base_size = 13;
    theme->typography.scale_factor = 1.0f;

    // Animations
    theme->animations.enabled = true;
    theme->animations.speed_multiplier = 1.0f;

    // Accessibility
    theme->accessibility.high_contrast = false;
    theme->accessibility.reduce_transparency = false;
    theme->accessibility.reduce_motion = false;
    theme->accessibility.text_scale = 1.0f;

    return theme;
}

theme_t *theme_clone(const theme_t *source) {
    if (!source) return NULL;

    theme_t *clone = malloc(sizeof(theme_t));
    if (!clone) return NULL;

    memcpy(clone, source, sizeof(theme_t));
    return clone;
}

void theme_free(theme_t *theme) {
    free(theme);
}

// ============================================================================
// VALIDATION
// ============================================================================

bool theme_validate_accessibility(const theme_t *theme) {
    if (!theme) return false;

    theme_colors_t *colors = (theme_colors_t *)&theme->colors;

    // Check primary text contrast
    float text_contrast = color_contrast_ratio(colors->text_primary, colors->bg_primary);
    if (text_contrast < CONTRAST_NORMAL_TEXT) {
        return false;
    }

    // Check primary button contrast
    float button_contrast = color_contrast_ratio(colors->primary, colors->bg_primary);
    if (button_contrast < CONTRAST_UI_ELEMENTS) {
        return false;
    }

    return true;
}

int theme_get_validation_report(const theme_t *theme, char *buffer, size_t size) {
    if (!theme || !buffer) return -1;

    int issues = 0;
    char *ptr = buffer;
    size_t remaining = size;

    theme_colors_t *colors = (theme_colors_t *)&theme->colors;

    // Check text contrast
    float text_contrast = color_contrast_ratio(colors->text_primary, colors->bg_primary);
    if (text_contrast < CONTRAST_NORMAL_TEXT) {
        int written = snprintf(ptr, remaining,
            "- Text contrast ratio %.2f:1 is below WCAG AA (4.5:1)\n",
            text_contrast);
        ptr += written;
        remaining -= written;
        issues++;
    }

    // Check button contrast
    float button_contrast = color_contrast_ratio(colors->primary, colors->bg_primary);
    if (button_contrast < CONTRAST_UI_ELEMENTS) {
        int written = snprintf(ptr, remaining,
            "- Button contrast ratio %.2f:1 is below WCAG AA (3.0:1)\n",
            button_contrast);
        ptr += written;
        remaining -= written;
        issues++;
    }

    if (issues == 0) {
        snprintf(buffer, size, "Theme passes all accessibility checks ✓\n");
    }

    return issues;
}
