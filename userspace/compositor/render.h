/**
 * Rendering and Compositing Module - Header
 *
 * Implements window compositing using the painter's algorithm.
 * Handles double buffering to prevent tearing.
 */

#ifndef RENDER_H
#define RENDER_H

#include "fb.h"
#include <stdint.h>
#include <stdbool.h>

// Maximum number of windows we can composite
#define MAX_WINDOWS 64

/**
 * Window structure
 *
 * Represents a single window to be composited.
 */
typedef struct {
    uint32_t id;                // Unique window ID
    int32_t x, y;               // Position on screen
    uint32_t width, height;     // Dimensions
    uint32_t *pixels;           // Window pixel buffer
    uint32_t pitch;             // Bytes per scanline
    bool visible;               // Whether window should be rendered
    uint32_t z_order;           // Z-order for layering (higher = on top)
    bool has_alpha;             // Whether to blend with alpha channel
} window_t;

/**
 * Renderer structure
 *
 * Manages compositing state and double buffering.
 */
typedef struct {
    framebuffer_t *fb;          // Target framebuffer
    uint32_t *back_buffer;      // Back buffer for double buffering
    window_t *windows[MAX_WINDOWS];  // Array of windows to composite
    uint32_t window_count;      // Number of active windows
    bool use_alpha_blending;    // Enable/disable alpha blending

    // Cursor state
    int32_t cursor_x;
    int32_t cursor_y;
    bool cursor_visible;
} renderer_t;

/**
 * Initialize the renderer
 *
 * @param fb Framebuffer to render to
 * @return Renderer instance, or NULL on failure
 */
renderer_t *renderer_init(framebuffer_t *fb);

/**
 * Cleanup renderer resources
 *
 * @param renderer Renderer to cleanup
 */
void renderer_cleanup(renderer_t *renderer);

/**
 * Clear the back buffer to a solid color
 *
 * @param renderer Renderer
 * @param color 32-bit RGBA color
 */
void renderer_clear(renderer_t *renderer, uint32_t color);

/**
 * Add a window to the compositor
 *
 * @param renderer Renderer
 * @param window Window to add
 * @return 0 on success, -1 on failure
 */
int renderer_add_window(renderer_t *renderer, window_t *window);

/**
 * Remove a window from the compositor
 *
 * @param renderer Renderer
 * @param window_id Window ID to remove
 * @return 0 on success, -1 if not found
 */
int renderer_remove_window(renderer_t *renderer, uint32_t window_id);

/**
 * Get a window by ID
 *
 * @param renderer Renderer
 * @param window_id Window ID
 * @return Window pointer, or NULL if not found
 */
window_t *renderer_get_window(renderer_t *renderer, uint32_t window_id);

/**
 * Composite all visible windows to the back buffer
 *
 * Uses the painter's algorithm: render back-to-front based on z_order.
 *
 * @param renderer Renderer
 */
void renderer_composite_windows(renderer_t *renderer);

/**
 * Present (flip) the back buffer to the framebuffer
 *
 * @param renderer Renderer
 */
void renderer_present(renderer_t *renderer);

/**
 * Create a test window with solid color
 *
 * Helper function for testing.
 *
 * @param id Window ID
 * @param x X position
 * @param y Y position
 * @param width Window width
 * @param height Window height
 * @param color Fill color
 * @return Window pointer, or NULL on failure
 */
window_t *renderer_create_test_window(uint32_t id, int32_t x, int32_t y,
                                       uint32_t width, uint32_t height,
                                       uint32_t color);

/**
 * Destroy a window and free its resources
 *
 * @param window Window to destroy
 */
void renderer_destroy_window(window_t *window);

/**
 * Alpha blend two colors
 *
 * @param src Source color (RGBA)
 * @param dst Destination color (RGBA)
 * @return Blended color
 */
uint32_t alpha_blend(uint32_t src, uint32_t dst);

/**
 * Set cursor position
 *
 * @param renderer Renderer
 * @param x Cursor X position
 * @param y Cursor Y position
 */
void renderer_set_cursor_position(renderer_t *renderer, int32_t x, int32_t y);

/**
 * Get cursor position
 *
 * @param renderer Renderer
 * @param x Output cursor X position
 * @param y Output cursor Y position
 */
void renderer_get_cursor_position(renderer_t *renderer, int32_t *x, int32_t *y);

/**
 * Set cursor visibility
 *
 * @param renderer Renderer
 * @param visible Whether cursor should be visible
 */
void renderer_set_cursor_visible(renderer_t *renderer, bool visible);

/**
 * Draw cursor on back buffer
 *
 * @param renderer Renderer
 */
void renderer_draw_cursor(renderer_t *renderer);

#endif /* RENDER_H */
