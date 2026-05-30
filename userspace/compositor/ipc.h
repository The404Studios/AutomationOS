/**
 * Compositor IPC Interface - Header
 *
 * Protocol for applications to communicate with the compositor.
 * Uses shared memory for window surfaces and message queue for commands.
 */

#ifndef COMPOSITOR_IPC_H
#define COMPOSITOR_IPC_H

#include <stdint.h>
#include "fb_compositor.h"

// IPC message types
typedef enum {
    MSG_CREATE_WINDOW,      // Create a new window
    MSG_DESTROY_WINDOW,     // Destroy window
    MSG_MAP_WINDOW,         // Make window visible
    MSG_UNMAP_WINDOW,       // Hide window
    MSG_UPDATE_SURFACE,     // Window surface has changed
    MSG_SET_TITLE,          // Change window title
    MSG_MOVE_WINDOW,        // Move window to new position
    MSG_RESIZE_WINDOW,      // Resize window
    MSG_FOCUS_WINDOW,       // Focus window
    MSG_RAISE_WINDOW,       // Raise to top
    MSG_LOWER_WINDOW,       // Lower to bottom
} ipc_msg_type_t;

// Maximum message payload size
#define IPC_MAX_PAYLOAD 256

/**
 * IPC message structure
 */
typedef struct {
    uint32_t client_id;         // Sender application ID
    ipc_msg_type_t type;        // Message type
    uint32_t window_id;         // Target window ID
    uint8_t payload[IPC_MAX_PAYLOAD];  // Message-specific data
} ipc_message_t;

/**
 * Create window request payload
 */
typedef struct {
    window_type_t type;
    int32_t x, y;
    uint32_t width, height;
    char title[128];
} create_window_request_t;

/**
 * Update surface request payload
 */
typedef struct {
    uint32_t shm_id;            // Shared memory segment ID
    uint32_t width, height;
    uint32_t offset;            // Offset into shared memory
} update_surface_request_t;

/**
 * Move window request payload
 */
typedef struct {
    int32_t x, y;
} move_window_request_t;

/**
 * Resize window request payload
 */
typedef struct {
    uint32_t width, height;
} resize_window_request_t;

/**
 * Set title request payload
 */
typedef struct {
    char title[128];
} set_title_request_t;

// IPC functions
int ipc_init_compositor(void);
void ipc_cleanup_compositor(void);
int ipc_receive_message(ipc_message_t *msg);
int ipc_send_response(uint32_t client_id, int32_t result);

// Message handlers
int ipc_handle_create_window(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_destroy_window(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_map_window(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_unmap_window(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_update_surface(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_set_title(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_move_window(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_resize_window(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_focus_window(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_raise_window(fb_compositor_t *comp, ipc_message_t *msg);
int ipc_handle_lower_window(fb_compositor_t *comp, ipc_message_t *msg);

// Dispatch message to appropriate handler
int ipc_dispatch_message(fb_compositor_t *comp, ipc_message_t *msg);

#endif // COMPOSITOR_IPC_H
