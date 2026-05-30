/**
 * Desktop Background Module - Header
 */

#ifndef DESKTOP_H
#define DESKTOP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct framebuffer framebuffer_t;

// ============================================================================
// DESKTOP STRUCTURE
// ============================================================================

typedef struct desktop {
    framebuffer_t *fb;
    uint32_t bg_color;      // Desktop background color
    bool wallpaper_loaded;  // Whether we have a wallpaper image
} desktop_t;

// ============================================================================
// DESKTOP API
// ============================================================================

desktop_t *desktop_init(framebuffer_t *fb);
void desktop_cleanup(desktop_t *desktop);
void desktop_render(desktop_t *desktop);
void desktop_set_wallpaper(desktop_t *desktop, const char *path);

#endif // DESKTOP_H
