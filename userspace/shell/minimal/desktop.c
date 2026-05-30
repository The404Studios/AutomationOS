/**
 * Desktop Background Module - Implementation
 *
 * Renders the desktop background (solid color or wallpaper)
 */

#include "desktop.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default desktop color (nice blue-gray)
#define DEFAULT_BG_COLOR 0xFF2C3E50

// ============================================================================
// INITIALIZATION
// ============================================================================

desktop_t *desktop_init(framebuffer_t *fb) {
    if (!fb) {
        return NULL;
    }

    desktop_t *desktop = calloc(1, sizeof(desktop_t));
    if (!desktop) {
        return NULL;
    }

    desktop->fb = fb;
    desktop->bg_color = DEFAULT_BG_COLOR;
    desktop->wallpaper_loaded = false;

    printf("[Desktop] Initialized (color: 0x%08X)\n", desktop->bg_color);

    return desktop;
}

// ============================================================================
// CLEANUP
// ============================================================================

void desktop_cleanup(desktop_t *desktop) {
    if (!desktop) {
        return;
    }

    // TODO: Free wallpaper image if loaded

    free(desktop);
}

// ============================================================================
// RENDERING
// ============================================================================

void desktop_render(desktop_t *desktop) {
    if (!desktop || !desktop->fb) {
        return;
    }

    if (desktop->wallpaper_loaded) {
        // TODO: Render wallpaper image
        // For now, just clear to color
        fb_clear(desktop->fb, desktop->bg_color);
    } else {
        // Clear to solid background color
        fb_clear(desktop->fb, desktop->bg_color);
    }
}

// ============================================================================
// WALLPAPER
// ============================================================================

void desktop_set_wallpaper(desktop_t *desktop, const char *path) {
    if (!desktop || !path) {
        return;
    }

    // TODO: Load image from path and set as wallpaper
    // For now, this is a stub
    printf("[Desktop] Wallpaper loading not implemented yet: %s\n", path);
}
