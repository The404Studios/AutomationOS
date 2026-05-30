/**
 * AutomationOS Settings Application - Main Entry Point
 */

#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

// Global application instance
static settings_app_t *g_app = NULL;

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int signum) {
    (void)signum;
    if (g_app) {
        printf("\n[Settings] Received shutdown signal\n");
        settings_app_quit(g_app);
    }
}

/**
 * Print usage information
 */
static void print_usage(const char *progname) {
    printf("Usage: %s [OPTIONS]\n", progname);
    printf("\n");
    printf("AutomationOS Settings Application\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help             Show this help message\n");
    printf("  -v, --version          Show version information\n");
    printf("  -c, --category NAME    Open specific category\n");
    printf("                         (display, appearance, sound, network,\n");
    printf("                          users, applications, privacy, system)\n");
    printf("  -r, --reset            Reset all settings to defaults\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                     Launch settings application\n", progname);
    printf("  %s -c display          Open display settings\n", progname);
    printf("  %s --reset             Reset all settings\n", progname);
    printf("\n");
}

/**
 * Print version information
 */
static void print_version(void) {
    printf("AutomationOS Settings v0.1.0\n");
    printf("Build date: 2026-05-26\n");
    printf("\n");
    printf("Copyright (c) 2026 AutomationOS Project\n");
    printf("This is free and open source software.\n");
}

/**
 * Parse category name to enum
 */
static settings_category_t parse_category(const char *name) {
    if (strcmp(name, "display") == 0) return CATEGORY_DISPLAY;
    if (strcmp(name, "appearance") == 0) return CATEGORY_APPEARANCE;
    if (strcmp(name, "sound") == 0) return CATEGORY_SOUND;
    if (strcmp(name, "network") == 0) return CATEGORY_NETWORK;
    if (strcmp(name, "users") == 0) return CATEGORY_USERS;
    if (strcmp(name, "applications") == 0) return CATEGORY_APPLICATIONS;
    if (strcmp(name, "privacy") == 0) return CATEGORY_PRIVACY;
    if (strcmp(name, "system") == 0) return CATEGORY_SYSTEM;
    return CATEGORY_DISPLAY;  // Default
}

/**
 * Main entry point
 */
int main(int argc, char *argv[]) {
    printf("====================================\n");
    printf("  AutomationOS Settings v0.1.0\n");
    printf("====================================\n\n");

    // Parse command line arguments
    settings_category_t initial_category = CATEGORY_DISPLAY;
    bool reset_settings = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--category") == 0) {
            if (i + 1 < argc) {
                initial_category = parse_category(argv[++i]);
            } else {
                fprintf(stderr, "Error: -c/--category requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reset") == 0) {
            reset_settings = true;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // TODO: Initialize desktop shell theme
    // For now, use a placeholder theme
    theme_t theme;
    theme_init_light(&theme);

    // Create settings application
    g_app = settings_app_create(&theme);
    if (!g_app) {
        fprintf(stderr, "Error: Failed to create settings application\n");
        return 1;
    }

    // Handle reset if requested
    if (reset_settings) {
        printf("[Settings] Resetting all settings to defaults...\n");
        settings_app_reset_defaults(g_app);
        settings_apply_all(g_app);
        printf("[Settings] Settings reset complete\n");
        settings_app_destroy(g_app);
        return 0;
    }

    // Select initial category
    settings_app_select_category(g_app, initial_category);

    // Run application main loop
    printf("[Settings] Entering main loop...\n");
    settings_app_run(g_app);

    // Cleanup
    printf("[Settings] Shutting down...\n");
    settings_app_destroy(g_app);
    g_app = NULL;

    printf("[Settings] Goodbye!\n");
    return 0;
}

/**
 * Stub implementations for theme functions
 * TODO: Replace with actual theme API
 */
void theme_init_light(theme_t *theme) {
    if (!theme) return;

    theme->mode = THEME_LIGHT;

    // Primary colors
    theme->primary = color_rgb(0, 122, 255);
    theme->secondary = color_rgb(88, 86, 214);
    theme->success = color_rgb(52, 199, 89);
    theme->warning = color_rgb(255, 149, 0);
    theme->error = color_rgb(255, 59, 48);

    // Background colors (light mode)
    theme->bg_primary = color_rgb(255, 255, 255);
    theme->bg_secondary = color_rgb(242, 242, 247);
    theme->bg_tertiary = color_rgb(230, 230, 235);

    // Text colors (light mode)
    theme->text_primary = color_rgb(0, 0, 0);
    theme->text_secondary = color_rgb(99, 99, 102);
    theme->text_tertiary = color_rgb(142, 142, 147);

    // UI element colors
    theme->panel_bg = color_rgb(248, 248, 248);
    theme->dock_bg = color_rgba(255, 255, 255, 200);
    theme->window_bg = color_rgb(255, 255, 255);
    theme->separator = color_rgb(200, 200, 200);

    // Effects
    theme->blur_radius = 20;
    theme->shadow_opacity = 50;
    theme->corner_radius = 8;

    // Fonts
    strncpy(theme->font_system, "SF Pro", sizeof(theme->font_system) - 1);
    theme->font_size_small = 11;
    theme->font_size_body = 13;
    theme->font_size_heading = 16;
}

void theme_init_dark(theme_t *theme) {
    if (!theme) return;

    theme->mode = THEME_DARK;

    // Primary colors (same as light)
    theme->primary = color_rgb(10, 132, 255);
    theme->secondary = color_rgb(94, 92, 230);
    theme->success = color_rgb(48, 209, 88);
    theme->warning = color_rgb(255, 159, 10);
    theme->error = color_rgb(255, 69, 58);

    // Background colors (dark mode)
    theme->bg_primary = color_rgb(28, 28, 30);
    theme->bg_secondary = color_rgb(44, 44, 46);
    theme->bg_tertiary = color_rgb(58, 58, 60);

    // Text colors (dark mode)
    theme->text_primary = color_rgb(255, 255, 255);
    theme->text_secondary = color_rgb(174, 174, 178);
    theme->text_tertiary = color_rgb(142, 142, 147);

    // UI element colors
    theme->panel_bg = color_rgb(36, 36, 38);
    theme->dock_bg = color_rgba(44, 44, 46, 200);
    theme->window_bg = color_rgb(28, 28, 30);
    theme->separator = color_rgb(72, 72, 74);

    // Effects
    theme->blur_radius = 20;
    theme->shadow_opacity = 80;
    theme->corner_radius = 8;

    // Fonts
    strncpy(theme->font_system, "SF Pro", sizeof(theme->font_system) - 1);
    theme->font_size_small = 11;
    theme->font_size_body = 13;
    theme->font_size_heading = 16;
}

color_t color_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (color_t){r, g, b, 255};
}

color_t color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (color_t){r, g, b, a};
}

bool rect_contains(const rect_t *rect, int32_t x, int32_t y) {
    if (!rect) return false;
    return x >= rect->x && x < rect->x + (int32_t)rect->width &&
           y >= rect->y && y < rect->y + (int32_t)rect->height;
}
