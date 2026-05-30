/**
 * IPC Protocol Definitions
 * =========================
 *
 * Standard message formats for all IPC communication between
 * desktop environment components.
 */

#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Core Message Structure
// ============================================================================

#define IPC_MAX_PAYLOAD 256

/**
 * Standard IPC message header (16 bytes)
 */
typedef struct {
    long mtype;              // Message type (for msgrcv filtering) - 8 bytes
    uint32_t sender_id;      // Sender process ID or component ID
    uint32_t sequence;       // Sequence number (for request/response matching)
    uint32_t payload_size;   // Size of payload in bytes
} ipc_msg_header_t;

/**
 * Complete IPC message (272 bytes total)
 */
typedef struct {
    ipc_msg_header_t header;
    uint8_t payload[IPC_MAX_PAYLOAD];
} ipc_message_t;

// ============================================================================
// Message Type Enumeration
// ============================================================================

typedef enum {
    // Compositor messages (1-50)
    MSG_COMPOSITOR_CREATE_WINDOW    = 1,
    MSG_COMPOSITOR_DESTROY_WINDOW   = 2,
    MSG_COMPOSITOR_MAP_WINDOW       = 3,
    MSG_COMPOSITOR_UNMAP_WINDOW     = 4,
    MSG_COMPOSITOR_UPDATE_SURFACE   = 5,
    MSG_COMPOSITOR_SET_TITLE        = 6,
    MSG_COMPOSITOR_MOVE_WINDOW      = 7,
    MSG_COMPOSITOR_RESIZE_WINDOW    = 8,
    MSG_COMPOSITOR_RAISE_WINDOW     = 9,
    MSG_COMPOSITOR_LOWER_WINDOW     = 10,
    MSG_COMPOSITOR_CONFIGURE        = 11,

    // Window manager messages (51-100)
    MSG_WM_CONFIGURE                = 51,
    MSG_WM_FOCUS_CHANGED            = 52,
    MSG_WM_GEOMETRY_UPDATE          = 53,
    MSG_WM_WORKSPACE_CHANGED        = 54,
    MSG_WM_MINIMIZE                 = 55,
    MSG_WM_MAXIMIZE                 = 56,
    MSG_WM_FULLSCREEN               = 57,
    MSG_WM_CLOSE                    = 58,
    MSG_WM_TILE                     = 59,

    // Application messages (101-150)
    MSG_APP_LAUNCH                  = 101,
    MSG_APP_EXIT                    = 102,
    MSG_APP_READY                   = 103,
    MSG_APP_REGISTER                = 104,
    MSG_APP_UNREGISTER              = 105,

    // Input events (151-200)
    MSG_EVENT_KEYBOARD              = 151,
    MSG_EVENT_MOUSE_MOTION          = 152,
    MSG_EVENT_MOUSE_BUTTON          = 153,
    MSG_EVENT_SCROLL                = 154,
    MSG_EVENT_TOUCH                 = 155,

    // Notifications (201-250)
    MSG_NOTIF_SEND                  = 201,
    MSG_NOTIF_UPDATE                = 202,
    MSG_NOTIF_CLOSE                 = 203,
    MSG_NOTIF_ACTION                = 204,

    // Responses (251-300)
    MSG_RESPONSE_SUCCESS            = 251,
    MSG_RESPONSE_ERROR              = 252,

} ipc_msg_type_t;

// ============================================================================
// Common Structures
// ============================================================================

/**
 * Rectangle (for geometry, damage regions, etc.)
 */
typedef struct {
    int32_t x, y;
    uint32_t width, height;
} rect_t;

/**
 * Color (RGBA)
 */
typedef struct {
    uint8_t r, g, b, a;
} color_t;

// ============================================================================
// Window Surface Shared Memory Layout
// ============================================================================

/**
 * Window surface structure (stored in shared memory)
 *
 * Layout:
 * - Header (this struct without pixels)
 * - Pixel data (width * height * 4 bytes RGBA)
 */
typedef struct {
    uint32_t width;              // Surface width in pixels
    uint32_t height;             // Surface height in pixels
    uint32_t format;             // Pixel format (0=RGBA8888)
    uint32_t stride;             // Bytes per row
    uint32_t version;            // Incremented on resize
    uint32_t damage_count;       // Number of damaged regions
    rect_t damage_rects[8];      // Damaged regions (for optimization)
    uint32_t pixels[];           // Pixel data (RGBA8888) - flexible array
} window_surface_t;

// Calculate required shared memory size
#define WINDOW_SURFACE_SIZE(w, h) \
    (sizeof(window_surface_t) + ((w) * (h) * 4))

// ============================================================================
// Compositor Message Payloads
// ============================================================================

/**
 * Create Window Request
 */
typedef struct {
    uint32_t window_type;        // 0=normal, 1=dialog, 2=utility, 3=popup
    int32_t x, y;                // Desired position (-1 = WM decides)
    uint32_t width, height;      // Desired size
    char title[128];             // Window title (null-terminated)
    uint32_t parent_id;          // Parent window (0 = top-level)
} create_window_request_t;

/**
 * Create Window Response
 */
typedef struct {
    uint32_t window_id;          // Assigned window ID
    int32_t shm_id;              // Shared memory ID for surface
    int32_t actual_x, actual_y;  // Actual position (WM may adjust)
    uint32_t actual_w, actual_h; // Actual size (WM may adjust)
} create_window_response_t;

/**
 * Update Surface Request
 */
typedef struct {
    uint32_t window_id;
    rect_t dirty_rect;           // Region that changed (0,0,0,0 = full repaint)
} update_surface_request_t;

/**
 * Move Window Request
 */
typedef struct {
    uint32_t window_id;
    int32_t x, y;
} move_window_request_t;

/**
 * Resize Window Request
 */
typedef struct {
    uint32_t window_id;
    uint32_t width, height;
} resize_window_request_t;

/**
 * Configure Window Request (move + resize)
 */
typedef struct {
    uint32_t window_id;
    int32_t x, y;
    uint32_t width, height;
    uint32_t flags;              // 0x1=animated, 0x2=user-initiated
} configure_window_request_t;

/**
 * Set Title Request
 */
typedef struct {
    uint32_t window_id;
    char title[128];
} set_title_request_t;

/**
 * Simple Window Operation (map, unmap, raise, lower, destroy)
 */
typedef struct {
    uint32_t window_id;
} window_operation_t;

// ============================================================================
// Window Manager Message Payloads
// ============================================================================

/**
 * WM Geometry Update Notification
 */
typedef struct {
    uint32_t window_id;
    int32_t x, y;
    uint32_t width, height;
    uint32_t flags;              // 0x1=animated, 0x2=user-initiated
} wm_geometry_update_t;

/**
 * WM Focus Changed Notification
 */
typedef struct {
    uint32_t old_window_id;      // Previously focused window (0=none)
    uint32_t new_window_id;      // Newly focused window
    uint32_t reason;             // 0=user click, 1=alt-tab, 2=programmatic
} wm_focus_changed_t;

/**
 * WM Workspace Changed Notification
 */
typedef struct {
    uint32_t old_workspace;
    uint32_t new_workspace;
    uint32_t animation_ms;       // Animation duration
} wm_workspace_changed_t;

// ============================================================================
// Application Message Payloads
// ============================================================================

/**
 * Launch Application Request
 */
typedef struct {
    char app_path[128];          // Path to executable
    char args[64];               // Command-line arguments (space-separated)
    char working_dir[64];        // Working directory (empty = current)
    uint32_t flags;              // 0x1=detached, 0x2=wait for exit
} app_launch_request_t;

/**
 * Application Ready Notification
 */
typedef struct {
    uint32_t app_id;             // Process ID
    char app_name[64];           // Human-readable name
    uint32_t capabilities;       // 0x1=notifications, 0x2=tray icon, 0x4=global hotkeys
    uint32_t primary_window_id;  // Main window ID (0 if none yet)
} app_ready_notification_t;

/**
 * Application Register Request
 */
typedef struct {
    char app_name[64];
    uint32_t version;            // App version number
    uint32_t capabilities;
} app_register_request_t;

// ============================================================================
// Input Event Payloads
// ============================================================================

/**
 * Keyboard Event
 */
typedef struct {
    uint32_t window_id;          // Target window (0=global hotkey)
    uint16_t keycode;            // Physical keycode
    uint16_t keysym;             // Logical key symbol
    uint32_t modifiers;          // Bit flags: 0x1=Shift, 0x2=Ctrl, 0x4=Alt, 0x8=Super
    uint8_t pressed;             // 1=press, 0=release
    uint8_t repeat;              // 1=auto-repeat event
    uint32_t timestamp;          // Event timestamp (milliseconds)
} keyboard_event_t;

/**
 * Mouse Motion Event
 */
typedef struct {
    uint32_t window_id;          // Window under cursor (0=none)
    int32_t x, y;                // Window-relative coordinates
    int32_t screen_x, screen_y;  // Screen-absolute coordinates
    uint32_t modifiers;          // Active modifier keys
    uint32_t timestamp;
} mouse_motion_event_t;

/**
 * Mouse Button Event
 */
typedef struct {
    uint32_t window_id;
    int32_t x, y;                // Window-relative coordinates
    uint8_t button;              // 1=left, 2=middle, 3=right, 4-7=extra
    uint8_t pressed;             // 1=press, 0=release
    uint8_t click_count;         // 1=single, 2=double, 3=triple
    uint32_t modifiers;
    uint32_t timestamp;
} mouse_button_event_t;

/**
 * Scroll Event
 */
typedef struct {
    uint32_t window_id;
    int32_t x, y;                // Window-relative coordinates
    int32_t delta_x, delta_y;    // Scroll deltas
    uint32_t modifiers;
    uint32_t timestamp;
} scroll_event_t;

// ============================================================================
// Notification Message Payloads
// ============================================================================

/**
 * Notification Action
 */
typedef struct {
    char label[32];              // Action label (e.g., "View", "Dismiss")
    uint32_t action_id;          // Unique action identifier
} notif_action_t;

/**
 * Send Notification Request
 */
typedef struct {
    char app_name[64];           // Sender application
    char summary[64];            // Notification title
    char body[128];              // Notification body
    uint32_t urgency;            // 0=low, 1=normal, 2=critical
    uint32_t timeout_ms;         // Auto-dismiss timeout (0=manual dismiss)
    uint32_t icon_id;            // Icon resource ID (0=default app icon)
    uint32_t action_count;       // Number of actions (0-4)
    notif_action_t actions[4];   // Available actions
} notif_send_request_t;

/**
 * Notification Action Response
 */
typedef struct {
    uint32_t notification_id;
    uint32_t action_id;          // Which action was clicked
    uint32_t timestamp;          // When action occurred
} notif_action_response_t;

/**
 * Close Notification Request
 */
typedef struct {
    uint32_t notification_id;
    uint32_t reason;             // 0=user dismissed, 1=timeout, 2=app closed
} notif_close_request_t;

// ============================================================================
// Response Messages
// ============================================================================

/**
 * Generic Response
 */
typedef struct {
    uint32_t request_sequence;   // Matches request sequence number
    int32_t result;              // 0=success, <0=error code
    char error_message[128];     // Human-readable error (if result < 0)
    uint8_t response_data[100];  // Optional response-specific data
} ipc_response_t;

// ============================================================================
// Error Codes
// ============================================================================

#define IPC_SUCCESS             0
#define IPC_ERR_INVALID_WINDOW  -1
#define IPC_ERR_NO_MEMORY       -2
#define IPC_ERR_PERMISSION      -3
#define IPC_ERR_TIMEOUT         -4
#define IPC_ERR_NOT_FOUND       -5
#define IPC_ERR_ALREADY_EXISTS  -6
#define IPC_ERR_PROTOCOL        -7
#define IPC_ERR_DISCONNECTED    -8
#define IPC_ERR_INVALID_ARG     -9
#define IPC_ERR_TOO_MANY        -10

// ============================================================================
// Helper Macros
// ============================================================================

/**
 * Initialize IPC message header
 */
#define IPC_MSG_INIT(msg, type, sender, seq) \
    do { \
        (msg)->header.mtype = (type); \
        (msg)->header.sender_id = (sender); \
        (msg)->header.sequence = (seq); \
        (msg)->header.payload_size = 0; \
    } while(0)

/**
 * Set payload for IPC message
 */
#define IPC_MSG_SET_PAYLOAD(msg, data, size) \
    do { \
        if ((size) <= IPC_MAX_PAYLOAD) { \
            memcpy((msg)->payload, (data), (size)); \
            (msg)->header.payload_size = (size); \
        } \
    } while(0)

/**
 * Get payload from IPC message
 */
#define IPC_MSG_GET_PAYLOAD(msg, type) \
    ((type *)((msg)->payload))

#endif // IPC_PROTOCOL_H
