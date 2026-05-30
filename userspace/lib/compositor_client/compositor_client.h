/**
 * Compositor Client Library - Public API
 *
 * Allows applications to create windows and communicate with the compositor
 * via IPC (shared memory + UNIX sockets).
 */

#ifndef COMPOSITOR_CLIENT_H
#define COMPOSITOR_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct comp_client comp_client_t;
typedef struct comp_window comp_window_t;

// Window types (must match compositor.h)
typedef enum {
    COMP_WINDOW_NORMAL,      // Regular application window
    COMP_WINDOW_DIALOG,      // Modal dialog
    COMP_WINDOW_UTILITY,     // Floating utility window
    COMP_WINDOW_TOOLBAR,     // Toolbar/panel
    COMP_WINDOW_MENU,        // Popup menu
    COMP_WINDOW_SPLASH,      // Splash screen
    COMP_WINDOW_DESKTOP,     // Desktop background
    COMP_WINDOW_DOCK,        // System dock/taskbar
} comp_window_type_t;

/**
 * Window structure
 */
struct comp_window {
    uint32_t id;                    // Window ID (assigned by compositor)
    comp_window_type_t type;
    int32_t x, y;                   // Position
    uint32_t width, height;         // Size
    uint32_t *pixels;               // Pixel buffer (ARGB32)
    bool mapped;                    // Is window visible
    char title[256];                // Window title
};

/**
 * Initialize compositor client connection
 *
 * @param socket_path Path to compositor socket (default: /run/compositor.sock)
 * @return Client handle, or NULL on failure
 */
comp_client_t *comp_client_init(const char *socket_path);

/**
 * Cleanup compositor client and close connection
 */
void comp_client_cleanup(comp_client_t *client);

/**
 * Create a new window
 *
 * @param client Compositor client
 * @param type Window type
 * @param x X position
 * @param y Y position
 * @param width Window width
 * @param height Window height
 * @param title Window title
 * @return Window handle, or NULL on failure
 */
comp_window_t *comp_create_window(comp_client_t *client, comp_window_type_t type,
                                   int32_t x, int32_t y, uint32_t width, uint32_t height,
                                   const char *title);

/**
 * Destroy window
 */
void comp_destroy_window(comp_client_t *client, comp_window_t *window);

/**
 * Map (show) window
 */
int comp_map_window(comp_client_t *client, comp_window_t *window);

/**
 * Unmap (hide) window
 */
int comp_unmap_window(comp_client_t *client, comp_window_t *window);

/**
 * Update window surface (notify compositor of pixel changes)
 */
int comp_update_surface(comp_client_t *client, comp_window_t *window);

/**
 * Set window title
 */
int comp_set_title(comp_client_t *client, comp_window_t *window, const char *title);

/**
 * Move window to new position
 */
int comp_move_window(comp_client_t *client, comp_window_t *window, int32_t x, int32_t y);

/**
 * Resize window
 */
int comp_resize_window(comp_client_t *client, comp_window_t *window, uint32_t width, uint32_t height);

/**
 * Raise window to top
 */
int comp_raise_window(comp_client_t *client, comp_window_t *window);

/**
 * Get window pixel buffer for drawing
 */
uint32_t *comp_get_pixels(comp_window_t *window);

/**
 * Flush all pending requests to compositor
 */
int comp_flush(comp_client_t *client);

#endif // COMPOSITOR_CLIENT_H
