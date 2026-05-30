/**
 * IPC Well-Known Keys
 * ===================
 *
 * Defines all well-known IPC keys for service discovery.
 * Applications can connect to system services using these keys.
 */

#ifndef IPC_KEYS_H
#define IPC_KEYS_H

// ============================================================================
// Component Message Queues
// ============================================================================

// Compositor (0x1000-0x1FFF)
#define IPC_KEY_COMPOSITOR        0x1000    // Main compositor command queue
#define IPC_KEY_COMPOSITOR_EVENTS 0x1001    // Event distribution from compositor

// Window Manager (0x2000-0x2FFF)
#define IPC_KEY_WM                0x2000    // Window manager command queue
#define IPC_KEY_WM_EVENTS         0x2001    // WM events (focus, geometry changes)

// Desktop Shell (0x3000-0x3FFF)
#define IPC_KEY_DESKTOP_SHELL     0x3000    // Desktop shell command queue
#define IPC_KEY_DESKTOP_EVENTS    0x3001    // Desktop events (app launch, etc.)

// Notification Daemon (0x4000-0x4FFF)
#define IPC_KEY_NOTIFICATION      0x4000    // Notification queue
#define IPC_KEY_NOTIFICATION_RESP 0x4001    // Notification responses

// Input System (0x5000-0x5FFF)
#define IPC_KEY_INPUT             0x5000    // Raw input event queue
#define IPC_KEY_INPUT_CONFIG      0x5001    // Input configuration

// System Services (0x6000-0x6FFF)
#define IPC_KEY_AUDIO             0x6000    // Audio server
#define IPC_KEY_NETWORK           0x6001    // Network manager
#define IPC_KEY_POWER             0x6002    // Power management
#define IPC_KEY_STORAGE           0x6003    // Storage monitor

// ============================================================================
// Application Key Ranges
// ============================================================================

// Applications use dynamic keys (0x10000+)
#define IPC_KEY_APP_BASE          0x10000
#define IPC_KEY_APP_MAX           0xFFFF

// Generate application-specific key
// Example: ftok("/app/terminal", 'Q') or (IPC_KEY_APP_BASE + app_id)

// ============================================================================
// Shared Memory Key Ranges
// ============================================================================

// Window surfaces (0x100000+)
#define IPC_KEY_WINDOW_SURFACE_BASE  0x100000

// Generate window surface key from window ID
#define IPC_KEY_WINDOW_SURFACE(wid)  (IPC_KEY_WINDOW_SURFACE_BASE + (wid))

// Font cache (0x200000+)
#define IPC_KEY_FONT_CACHE_BASE      0x200000

// Cursor/icon cache (0x300000+)
#define IPC_KEY_CURSOR_CACHE_BASE    0x300000

// Clipboard (0x400000+)
#define IPC_KEY_CLIPBOARD            0x400000

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Generate application-specific message queue key
 *
 * @param app_name Application name (e.g., "terminal", "filemanager")
 * @param suffix   Suffix character (e.g., 'Q' for queue, 'E' for events)
 * @return IPC key
 */
static inline int ipc_app_key(const char *app_name, int suffix) {
    // Use ftok with a base path
    return ftok(app_name, suffix);
}

/**
 * Generate response queue key for client
 *
 * Applications create response queues using their PID as identifier
 *
 * @param pid Process ID
 * @return IPC key for response queue
 */
static inline int ipc_response_key(int pid) {
    return IPC_KEY_APP_BASE + (pid & 0xFFFF);
}

#endif // IPC_KEYS_H
