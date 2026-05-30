/**
 * AutomationOS Desktop Shell - Main Implementation
 *
 * Core desktop shell orchestration and lifecycle management.
 */

#include "desktop_shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// ============================================================================
// COLOR UTILITIES
// ============================================================================

color_t color_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (color_t){r, g, b, 255};
}

color_t color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (color_t){r, g, b, a};
}

color_t color_hex(uint32_t hex) {
    return (color_t){
        .r = (hex >> 16) & 0xFF,
        .g = (hex >> 8) & 0xFF,
        .b = hex & 0xFF,
        .a = 255
    };
}

// ============================================================================
// GEOMETRY UTILITIES
// ============================================================================

bool rect_contains(const rect_t *rect, int32_t x, int32_t y) {
    return x >= rect->x && x < rect->x + (int32_t)rect->width &&
           y >= rect->y && y < rect->y + (int32_t)rect->height;
}

bool rect_intersects(const rect_t *a, const rect_t *b) {
    return !(a->x + (int32_t)a->width < b->x ||
             b->x + (int32_t)b->width < a->x ||
             a->y + (int32_t)a->height < b->y ||
             b->y + (int32_t)b->height < a->y);
}

// ============================================================================
// THEME INITIALIZATION
// ============================================================================

void theme_init_light(theme_t *theme) {
    memset(theme, 0, sizeof(theme_t));

    theme->mode = THEME_LIGHT;

    // Primary colors
    theme->primary = color_hex(0x007AFF);      // Blue
    theme->secondary = color_hex(0x5856D6);    // Purple
    theme->success = color_hex(0x34C759);      // Green
    theme->warning = color_hex(0xFF9500);      // Orange
    theme->error = color_hex(0xFF3B30);        // Red

    // Background colors
    theme->bg_primary = color_hex(0xFFFFFF);   // White
    theme->bg_secondary = color_hex(0xF5F5F5); // Light gray
    theme->bg_tertiary = color_hex(0xE5E5E5);  // Medium gray

    // Text colors
    theme->text_primary = color_hex(0x000000);     // Black
    theme->text_secondary = color_hex(0x666666);   // Dark gray
    theme->text_tertiary = color_hex(0x999999);    // Medium gray

    // UI element colors
    theme->panel_bg = color_rgba(240, 240, 240, 230);
    theme->dock_bg = color_rgba(255, 255, 255, 200);
    theme->window_bg = color_hex(0xFFFFFF);
    theme->separator = color_hex(0xCCCCCC);

    // Effects
    theme->blur_radius = 20;
    theme->shadow_opacity = 30;
    theme->corner_radius = 8;

    // Fonts
    strncpy(theme->font_system, "Inter", sizeof(theme->font_system) - 1);
    theme->font_size_small = 11;
    theme->font_size_body = 13;
    theme->font_size_heading = 16;
}

void theme_init_dark(theme_t *theme) {
    memset(theme, 0, sizeof(theme_t));

    theme->mode = THEME_DARK;

    // Primary colors (same as light)
    theme->primary = color_hex(0x007AFF);
    theme->secondary = color_hex(0x5856D6);
    theme->success = color_hex(0x34C759);
    theme->warning = color_hex(0xFF9500);
    theme->error = color_hex(0xFF3B30);

    // Background colors
    theme->bg_primary = color_hex(0x1E1E1E);   // Dark gray
    theme->bg_secondary = color_hex(0x2D2D2D); // Medium dark
    theme->bg_tertiary = color_hex(0x3A3A3A);  // Light dark

    // Text colors
    theme->text_primary = color_hex(0xFFFFFF);     // White
    theme->text_secondary = color_hex(0xAAAAAA);   // Light gray
    theme->text_tertiary = color_hex(0x888888);    // Medium gray

    // UI element colors
    theme->panel_bg = color_rgba(45, 45, 45, 230);
    theme->dock_bg = color_rgba(30, 30, 30, 200);
    theme->window_bg = color_hex(0x2D2D2D);
    theme->separator = color_hex(0x444444);

    // Effects (same as light)
    theme->blur_radius = 20;
    theme->shadow_opacity = 50;  // Slightly stronger for dark mode
    theme->corner_radius = 8;

    // Fonts
    strncpy(theme->font_system, "Inter", sizeof(theme->font_system) - 1);
    theme->font_size_small = 11;
    theme->font_size_body = 13;
    theme->font_size_heading = 16;
}

void theme_apply(desktop_shell_t *shell, theme_mode_t mode) {
    if (mode == THEME_AUTO) {
        // Auto-detect based on time (6am-6pm = light, otherwise dark)
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        int hour = local->tm_hour;
        mode = (hour >= 6 && hour < 18) ? THEME_LIGHT : THEME_DARK;
    }

    if (mode == THEME_LIGHT) {
        theme_init_light(&shell->theme);
    } else {
        theme_init_dark(&shell->theme);
    }

    // Update all component themes
    if (shell->panel) shell->panel->theme = &shell->theme;
    if (shell->dock) shell->dock->theme = &shell->theme;
    if (shell->desktop) shell->desktop->theme = &shell->theme;
    if (shell->overview) shell->overview->theme = &shell->theme;
    if (shell->notifications) shell->notifications->theme = &shell->theme;
    if (shell->quick_settings) shell->quick_settings->theme = &shell->theme;
    if (shell->system_menu) shell->system_menu->theme = &shell->theme;
}

// ============================================================================
// DESKTOP SHELL LIFECYCLE
// ============================================================================

desktop_shell_t *desktop_shell_create(uint32_t width, uint32_t height) {
    printf("[Shell] Creating desktop shell (%ux%u)\n", width, height);

    desktop_shell_t *shell = calloc(1, sizeof(desktop_shell_t));
    if (!shell) {
        fprintf(stderr, "[Shell] ERROR: Failed to allocate shell\n");
        return NULL;
    }

    shell->screen_width = width;
    shell->screen_height = height;
    shell->running = true;
    shell->workspace_count = 4;  // Default 4 workspaces
    shell->current_workspace = 0;

    // Initialize theme (light by default)
    theme_init_light(&shell->theme);

    // Create desktop (render first, behind everything)
    shell->desktop = desktop_create(shell);
    if (!shell->desktop) {
        fprintf(stderr, "[Shell] ERROR: Failed to create desktop\n");
        desktop_shell_destroy(shell);
        return NULL;
    }

    // Create panel (top bar)
    shell->panel = panel_create(shell);
    if (!shell->panel) {
        fprintf(stderr, "[Shell] ERROR: Failed to create panel\n");
        desktop_shell_destroy(shell);
        return NULL;
    }

    // Create dock
    shell->dock = dock_create(shell);
    if (!shell->dock) {
        fprintf(stderr, "[Shell] ERROR: Failed to create dock\n");
        desktop_shell_destroy(shell);
        return NULL;
    }

    // Create overview
    shell->overview = overview_create(shell);
    if (!shell->overview) {
        fprintf(stderr, "[Shell] ERROR: Failed to create overview\n");
        desktop_shell_destroy(shell);
        return NULL;
    }

    // Create notification center
    shell->notifications = notification_center_create(shell);
    if (!shell->notifications) {
        fprintf(stderr, "[Shell] ERROR: Failed to create notification center\n");
        desktop_shell_destroy(shell);
        return NULL;
    }

    // Create quick settings
    shell->quick_settings = quick_settings_create(shell);
    if (!shell->quick_settings) {
        fprintf(stderr, "[Shell] ERROR: Failed to create quick settings\n");
        desktop_shell_destroy(shell);
        return NULL;
    }

    // Create system menu
    shell->system_menu = system_menu_create(shell);
    if (!shell->system_menu) {
        fprintf(stderr, "[Shell] ERROR: Failed to create system menu\n");
        desktop_shell_destroy(shell);
        return NULL;
    }

    printf("[Shell] Desktop shell created successfully\n");
    return shell;
}

void desktop_shell_destroy(desktop_shell_t *shell) {
    if (!shell) return;

    printf("[Shell] Destroying desktop shell\n");

    if (shell->system_menu) system_menu_destroy(shell->system_menu);
    if (shell->quick_settings) quick_settings_destroy(shell->quick_settings);
    if (shell->notifications) notification_center_destroy(shell->notifications);
    if (shell->overview) overview_destroy(shell->overview);
    if (shell->dock) dock_destroy(shell->dock);
    if (shell->panel) panel_destroy(shell->panel);
    if (shell->desktop) desktop_destroy(shell->desktop);

    free(shell);
}

void desktop_shell_quit(desktop_shell_t *shell) {
    if (shell) {
        shell->running = false;
    }
}

// ============================================================================
// EVENT HANDLING
// ============================================================================

void desktop_shell_handle_mouse(desktop_shell_t *shell, int32_t x, int32_t y, uint32_t buttons) {
    if (!shell) return;

    // TODO: Route mouse events to appropriate component
    // Priority order: system_menu -> quick_settings -> overview -> panel -> dock -> windows -> desktop

    // For now, just update global mouse state
    (void)x;
    (void)y;
    (void)buttons;
}

void desktop_shell_handle_keyboard(desktop_shell_t *shell, uint32_t keycode, bool pressed) {
    if (!shell || !pressed) return;

    // Handle global keyboard shortcuts
    // TODO: Implement proper keyboard handling

    // Super key -> Open overview
    if (keycode == 0x5B) {  // Left Super/Windows key
        if (shell->overview->active) {
            overview_close(shell->overview);
        } else {
            overview_open(shell->overview);
        }
    }
}

// ============================================================================
// MAIN RENDER LOOP
// ============================================================================

static void desktop_shell_render(desktop_shell_t *shell) {
    // Render order (back to front):
    // 1. Desktop (wallpaper + icons)
    // 2. Windows
    // 3. Dock
    // 4. Panel
    // 5. Overview (if open)
    // 6. Quick settings (if open)
    // 7. System menu (if open)
    // 8. Notification center (if open)

    desktop_render(shell->desktop);

    // Render windows
    for (window_t *win = shell->windows; win != NULL; win = win->next) {
        if (win->visible) {
            // TODO: Render window
        }
    }

    dock_render(shell->dock);
    panel_render(shell->panel);

    if (shell->overview->active) {
        overview_render(shell->overview);
    }

    if (shell->quick_settings->visible) {
        quick_settings_render(shell->quick_settings);
    }

    if (shell->system_menu->visible) {
        system_menu_render(shell->system_menu);
    }

    if (shell->notifications->visible) {
        notification_center_render(shell->notifications);
    }
}

static void desktop_shell_update(desktop_shell_t *shell, uint64_t delta_us) {
    // Update all components
    panel_update(shell->panel, delta_us);
    dock_update(shell->dock, delta_us);

    // Update performance metrics
    shell->frame_time_us = delta_us;
    if (delta_us > 0) {
        shell->fps = (uint32_t)(1000000 / delta_us);
    }
}

void desktop_shell_run(desktop_shell_t *shell) {
    if (!shell) return;

    printf("[Shell] Starting desktop shell main loop\n");

    uint64_t last_frame_time = 0;  // TODO: Get actual time

    while (shell->running) {
        uint64_t current_time = 0;  // TODO: Get actual time
        uint64_t delta_us = current_time - last_frame_time;
        last_frame_time = current_time;

        // Cap delta at 100ms to prevent huge jumps
        if (delta_us > 100000) {
            delta_us = 100000;
        }

        // Update state
        desktop_shell_update(shell, delta_us);

        // Render
        desktop_shell_render(shell);

        // TODO: VSync / frame limiting
        // Target 60 FPS = 16666 us per frame
    }

    printf("[Shell] Desktop shell main loop exited\n");
}
