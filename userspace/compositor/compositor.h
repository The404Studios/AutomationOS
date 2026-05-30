/**
 * AutomationOS Compositor - Header
 */

#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct gpu_context gpu_context_t;
typedef struct framebuffer framebuffer_t;
typedef struct texture texture_t;
typedef struct surface surface_t;
typedef struct display display_t;
typedef struct animation animation_t;

// Maximum limits
#define MAX_DISPLAYS 8
#define MAX_WINDOWS 256
#define MAX_DAMAGE_REGIONS 64

/**
 * Rectangle structure
 */
typedef struct {
    int32_t x, y;
    int32_t w, h;
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
 * Surface - Window pixel buffer
 */
typedef struct surface {
    uint32_t width, height;
    uint32_t *pixels;       // ARGB32 format
    bool dirty;             // Needs GPU upload
} surface_t;

/**
 * Window structure
 */
typedef struct window {
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

    surface_t *surface;      // Window contents
    texture_t *texture;      // GPU texture

    animation_t *animation;  // Current animation
} window_t;

/**
 * Display structure
 */
typedef struct display {
    uint32_t id;
    int32_t x, y;            // Position in desktop space
    uint32_t width, height;
    uint32_t refresh_rate;   // Hz
    char name[64];
    bool primary;
} display_t;

/**
 * Compositor structure
 */
typedef struct compositor {
    gpu_context_t *gpu;
    display_t *displays[MAX_DISPLAYS];
    window_t *windows[MAX_WINDOWS];
    framebuffer_t *framebuffers[3];  // Triple buffering

    uint32_t display_count;
    uint32_t window_count;
    uint32_t current_fb;             // Current framebuffer index

    uint32_t fps;
    uint64_t last_fps_update;
    uint32_t frame_count;

    bool vsync_enabled;
    bool effects_enabled;

    // Damage tracking
    rect_t damage_regions[MAX_DAMAGE_REGIONS];
    uint32_t damage_region_count;
    bool full_redraw;

    // Cursor state
    int32_t cursor_x;
    int32_t cursor_y;
    bool cursor_visible;
} compositor_t;

// Core functions
compositor_t *compositor_init(const char *gpu_device);
void compositor_cleanup(compositor_t *comp);
void compositor_frame(compositor_t *comp);

// Display management
int compositor_add_display(compositor_t *comp, display_t *display);
void compositor_remove_display(compositor_t *comp, uint32_t display_id);

// Window management
int compositor_add_window(compositor_t *comp, window_t *window);
void compositor_remove_window(compositor_t *comp, uint32_t window_id);
window_t *compositor_find_window(compositor_t *comp, uint32_t window_id);

// Damage tracking
void compositor_add_damage(compositor_t *comp, const rect_t *rect);
void compositor_mark_full_redraw(compositor_t *comp);

// Settings
void compositor_set_vsync(compositor_t *comp, bool enabled);
void compositor_set_effects(compositor_t *comp, bool enabled);
uint32_t compositor_get_fps(compositor_t *comp);

// Utility functions
bool rect_intersects(const rect_t *a, const rect_t *b);
void rect_union(rect_t *result, const rect_t *a, const rect_t *b);

// Window utilities
window_t *window_create(uint32_t id, window_type_t type, const rect_t *geometry);
void window_cleanup(gpu_context_t *gpu, window_t *window);
void window_set_title(window_t *window, const char *title);
void window_update_surface(window_t *window, const uint32_t *pixels, uint32_t width, uint32_t height);

// Cursor management
void compositor_set_cursor_position(compositor_t *comp, int32_t x, int32_t y);
void compositor_get_cursor_position(compositor_t *comp, int32_t *x, int32_t *y);
void compositor_set_cursor_visible(compositor_t *comp, bool visible);

// Display utilities
display_t *display_create(uint32_t id, uint32_t width, uint32_t height, uint32_t refresh_rate);
void display_cleanup(display_t *display);

// Rendering helpers
void render_window_decorations(compositor_t *comp, window_t *window);
void apply_window_effects(compositor_t *comp, window_t *window);
void apply_global_effects(compositor_t *comp);

#endif // COMPOSITOR_H
