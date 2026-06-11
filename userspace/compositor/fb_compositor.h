/**
 * AutomationOS Framebuffer Compositor - Header
 *
 * Software rendering compositor using direct framebuffer access.
 * Replaces GPU backend with CPU rendering for simplicity.
 */

#ifndef FB_COMPOSITOR_H
#define FB_COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "fb.h"

// Maximum limits
#define MAX_WINDOWS 64
#define MAX_DAMAGE_REGIONS 128

/**
 * Rectangle structure
 */
typedef struct {
    int32_t x, y;
    int32_t width, height;
} rect_t;

/**
 * Window types
 */
typedef enum {
    WINDOW_NORMAL,      // Regular application window
    WINDOW_DIALOG,      // Modal dialog
    WINDOW_UTILITY,     // Floating utility window
    WINDOW_TOOLBAR,     // Toolbar/panel
    WINDOW_MENU,        // Popup menu
    WINDOW_SPLASH,      // Splash screen
    WINDOW_DESKTOP,     // Desktop background
    WINDOW_DOCK,        // System dock/taskbar
} window_type_t;

/**
 * Window surface - pixel buffer for window content
 */
typedef struct {
    uint32_t width, height;
    uint32_t *pixels;       // ARGB32 format (0xAARRGGBB)
    uint32_t pitch;         // Bytes per scanline
    bool dirty;             // Surface has changed
} surface_t;

/**
 * Window structure
 */
typedef struct {
    uint32_t id;
    window_type_t type;

    rect_t geometry;         // Position and size
    rect_t frame_geometry;   // Including decorations

    bool mapped;             // Visible on screen
    bool minimized;
    bool maximized;
    bool fullscreen;
    bool focused;

    char title[256];
    uint32_t app_id;
    uint32_t z_order;        // Stacking order (higher = on top)

    surface_t *surface;      // Window pixel buffer
    float alpha;             // Window opacity (0.0 - 1.0)
    // Pixel capacity of the create-time SHM surface segment (width*height at create).
    // The segment is never resized, so window_update_surface clamps its memcpy SOURCE
    // read to this many pixels -- a client that sets a larger surface->width/height in
    // the shared header otherwise makes the compositor read past the end of the segment
    // (OOB read / info-leak across the IPC boundary).
    uint64_t surface_capacity_pixels;
} window_t;

// Hard per-dimension cap for untrusted window dimensions (from the create/update IPC).
// Bounds allocations and makes width*height fit in 64-bit without overflow.
#ifndef WINDOW_MAX_DIM
#define WINDOW_MAX_DIM 16384u
#endif

/**
 * Damage tracking structure
 */
typedef struct {
    rect_t regions[MAX_DAMAGE_REGIONS];
    uint32_t count;
    bool full_redraw;
} damage_tracker_t;

/**
 * Compositor structure
 */
typedef struct {
    framebuffer_t *fb;
    uint32_t *back_buffer;       // Double buffering

    window_t *windows[MAX_WINDOWS];
    uint32_t window_count;

    damage_tracker_t damage;

    // Performance metrics
    uint32_t fps;
    uint64_t last_fps_update;
    uint32_t frame_count;
    uint64_t last_frame_time;    // Last frame render time in microseconds

    // VSync timing
    uint64_t last_vsync_time;    // Timestamp of last VSync

    // Cursor state
    int32_t cursor_x;
    int32_t cursor_y;
    bool cursor_visible;

    // Settings
    bool use_alpha_blending;
    bool use_damage_tracking;
    bool vsync_enabled;          // Enable/disable VSync
} fb_compositor_t;

// Core compositor functions
fb_compositor_t *fb_compositor_init(void);
void fb_compositor_cleanup(fb_compositor_t *comp);
void fb_compositor_frame(fb_compositor_t *comp);

// Window management
int fb_compositor_add_window(fb_compositor_t *comp, window_t *window);
void fb_compositor_remove_window(fb_compositor_t *comp, uint32_t window_id);
window_t *fb_compositor_find_window(fb_compositor_t *comp, uint32_t window_id);
void fb_compositor_raise_window(fb_compositor_t *comp, uint32_t window_id);
void fb_compositor_lower_window(fb_compositor_t *comp, uint32_t window_id);

// Window creation/destruction
window_t *window_create(uint32_t id, window_type_t type, int32_t x, int32_t y,
                        uint32_t width, uint32_t height);
void window_destroy(window_t *window);
void window_set_title(window_t *window, const char *title);
void window_update_surface(window_t *window, const uint32_t *pixels,
                          uint32_t width, uint32_t height);

// Damage tracking
void damage_add_region(damage_tracker_t *damage, const rect_t *rect);
void damage_mark_full_redraw(damage_tracker_t *damage);
void damage_clear(damage_tracker_t *damage);
bool damage_is_region_damaged(damage_tracker_t *damage, const rect_t *rect);

// Cursor management
void fb_compositor_set_cursor_position(fb_compositor_t *comp, int32_t x, int32_t y);
void fb_compositor_get_cursor_position(fb_compositor_t *comp, int32_t *x, int32_t *y);
void fb_compositor_set_cursor_visible(fb_compositor_t *comp, bool visible);

// Settings
void fb_compositor_set_alpha_blending(fb_compositor_t *comp, bool enabled);
void fb_compositor_set_damage_tracking(fb_compositor_t *comp, bool enabled);
void fb_compositor_set_vsync(fb_compositor_t *comp, bool enabled);
uint32_t fb_compositor_get_fps(fb_compositor_t *comp);
uint64_t fb_compositor_get_frame_time(fb_compositor_t *comp);

// Utility functions
bool rect_intersects(const rect_t *a, const rect_t *b);
void rect_union(rect_t *result, const rect_t *a, const rect_t *b);
bool rect_contains_point(const rect_t *rect, int32_t x, int32_t y);

#endif // FB_COMPOSITOR_H
