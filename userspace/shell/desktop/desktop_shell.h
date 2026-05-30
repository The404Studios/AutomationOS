/**
 * AutomationOS Desktop Shell
 *
 * Complete desktop environment with panel, dock, desktop,
 * overview, notifications, and quick settings.
 *
 * Design inspired by macOS, built from scratch for AutomationOS.
 */

#ifndef DESKTOP_SHELL_H
#define DESKTOP_SHELL_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

typedef struct window window_t;
typedef struct texture texture_t;
typedef struct rect rect_t;
typedef struct point point_t;
typedef struct color color_t;

// ============================================================================
// GEOMETRY TYPES
// ============================================================================

struct point {
    int32_t x;
    int32_t y;
};

struct rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

struct color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

// ============================================================================
// THEME SYSTEM
// ============================================================================

typedef enum {
    THEME_LIGHT,
    THEME_DARK,
    THEME_AUTO,     // Auto switch based on time
} theme_mode_t;

typedef struct {
    theme_mode_t mode;

    // Primary colors
    color_t primary;            // #007AFF
    color_t secondary;          // #5856D6
    color_t success;            // #34C759
    color_t warning;            // #FF9500
    color_t error;              // #FF3B30

    // Background colors
    color_t bg_primary;         // Main background
    color_t bg_secondary;       // Secondary background
    color_t bg_tertiary;        // Tertiary background

    // Text colors
    color_t text_primary;       // Primary text
    color_t text_secondary;     // Secondary text
    color_t text_tertiary;      // Tertiary text

    // UI element colors
    color_t panel_bg;           // Panel background
    color_t dock_bg;            // Dock background
    color_t window_bg;          // Window background
    color_t separator;          // Separator lines

    // Effects
    uint8_t blur_radius;        // Blur effect strength
    uint8_t shadow_opacity;     // Shadow opacity
    uint8_t corner_radius;      // Rounded corners

    // Fonts
    char font_system[64];       // System font name
    uint32_t font_size_small;   // 11px
    uint32_t font_size_body;    // 13px
    uint32_t font_size_heading; // 16px
} theme_t;

// ============================================================================
// WINDOW MANAGEMENT
// ============================================================================

typedef enum {
    WINDOW_NORMAL,
    WINDOW_FULLSCREEN,
    WINDOW_MAXIMIZED,
    WINDOW_MINIMIZED,
} window_state_t;

struct window {
    uint32_t id;
    char title[256];
    char app_id[64];            // com.example.app

    rect_t bounds;
    window_state_t state;

    texture_t *surface;         // Window contents

    bool visible;
    bool focused;
    float opacity;

    struct window *next;
};

// ============================================================================
// PANEL (TOP BAR)
// ============================================================================

typedef struct button {
    rect_t bounds;
    char label[128];
    texture_t *icon;

    bool hovered;
    bool pressed;

    void (*on_click)(struct button *btn, void *user_data);
    void *user_data;
} button_t;

typedef struct label {
    rect_t bounds;
    char text[256];
    color_t color;
    uint32_t font_size;
} label_t;

typedef struct {
    rect_t bounds;
    texture_t *icon;
    char tooltip[128];
    uint32_t badge_count;       // Notification badge
} tray_icon_t;

typedef struct {
    tray_icon_t items[16];
    uint32_t count;
} tray_t;

typedef struct {
    char time_str[32];          // "3:45 PM"
    char date_str[64];          // "Monday, May 26"
    bool calendar_open;
} clock_t;

typedef struct {
    window_t *window;           // Panel window (fullwidth, top)
    void *comp_window;          // Compositor window handle
    void *comp_client;          // Compositor client
    void *font;                 // Font for text rendering

    // Left side
    button_t *activities;       // "Activities" button
    label_t *app_title;         // Current app title

    // Right side
    tray_t *system_tray;        // System tray icons
    clock_t *clock;             // Clock widget
    button_t *user_menu;        // User menu button

    bool autohide;
    uint32_t height;            // Pixels (default: 32)

    theme_t *theme;
} panel_t;

// ============================================================================
// DOCK (APPLICATION LAUNCHER)
// ============================================================================

typedef enum {
    DOCK_BOTTOM,
    DOCK_LEFT,
    DOCK_RIGHT,
    DOCK_FLOATING,
} dock_position_t;

typedef struct {
    char app_id[64];            // com.example.app
    char name[128];             // Display name
    char exec_path[512];        // Executable path
    texture_t *icon;            // App icon (48x48 or larger)

    bool pinned;                // Is pinned to dock
    bool running;               // Is currently running
    uint32_t window_count;      // Number of windows
    uint32_t notification_count; // Notification badge

    rect_t bounds;              // Position in dock
    float scale;                // For magnification effect
} dock_item_t;

typedef struct {
    window_t *window;           // Dock window
    void *comp_window;          // Compositor window handle
    void *comp_client;          // Compositor client

    dock_item_t *items[64];     // App icons
    uint32_t count;

    dock_position_t position;   // BOTTOM, LEFT, etc.

    bool autohide;
    bool magnify_on_hover;      // macOS-style magnification
    uint32_t icon_size;         // 48, 64, 96 pixels
    uint32_t padding;           // Spacing between icons

    // Hover state
    int32_t hover_index;        // -1 if no hover
    point_t mouse_pos;

    theme_t *theme;
} dock_t;

// ============================================================================
// DESKTOP (WORKSPACE)
// ============================================================================

typedef enum {
    ICON_FILE,
    ICON_FOLDER,
    ICON_APP,
    ICON_TRASH,
    ICON_VOLUME,
} icon_type_t;

typedef struct {
    char name[256];
    char path[1024];            // File path
    icon_type_t type;

    texture_t *icon;            // 64x64 icon
    rect_t bounds;              // Position on desktop
    bool selected;
} desktop_icon_t;

typedef struct {
    window_t *window;           // Desktop window (fullscreen)
    void *comp_window;          // Compositor window handle
    void *comp_client;          // Compositor client

    texture_t *wallpaper;       // Wallpaper image
    color_t bg_color;           // Fallback color

    desktop_icon_t *icons[128]; // Desktop icons
    uint32_t icon_count;

    bool show_icons;            // Toggle desktop icons
    uint32_t grid_size;         // Icon grid spacing (96px)

    theme_t *theme;
} desktop_t;

// ============================================================================
// OVERVIEW (MISSION CONTROL / ACTIVITIES)
// ============================================================================

typedef struct {
    char query[256];            // Search query

    struct {
        char name[128];
        char path[512];
        float score;            // Relevance score
        icon_type_t type;
        texture_t *icon;
    } results[32];

    uint32_t result_count;
} search_bar_t;

typedef struct {
    bool active;                // Is overview open
    window_t *window;           // Fullscreen overlay

    search_bar_t *search;       // Search bar at top

    // App grid
    struct {
        char name[128];
        char app_id[64];
        texture_t *icon;
        rect_t bounds;
    } apps[256];
    uint32_t app_count;

    // Window thumbnails
    window_t **windows;
    uint32_t window_count;

    // Workspace switcher
    uint32_t workspace_count;
    uint32_t current_workspace;

    theme_t *theme;
} overview_t;

// ============================================================================
// NOTIFICATIONS
// ============================================================================

typedef enum {
    NOTIF_INFO,
    NOTIF_WARNING,
    NOTIF_ERROR,
    NOTIF_SUCCESS,
} notif_urgency_t;

typedef struct {
    char label[64];
    void (*callback)(void *user_data);
    void *user_data;
} notif_action_t;

typedef struct {
    uint32_t id;
    char app_name[64];
    char summary[128];          // Title
    char body[512];             // Description
    texture_t *icon;

    notif_urgency_t urgency;
    uint64_t timestamp;
    uint32_t timeout_ms;        // Auto-dismiss (0 = persist)

    notif_action_t actions[4];  // Action buttons
    uint32_t action_count;

    bool dismissed;
} notification_t;

typedef struct {
    window_t *window;           // Notification center panel

    notification_t *queue[100]; // Notification queue
    uint32_t count;

    bool do_not_disturb;
    bool visible;               // Is panel open

    theme_t *theme;
} notification_center_t;

// ============================================================================
// QUICK SETTINGS
// ============================================================================

typedef struct {
    char label[64];
    bool enabled;
    void (*on_toggle)(bool enabled, void *user_data);
    void *user_data;
} quick_toggle_t;

typedef struct {
    char label[64];
    float value;                // 0.0 - 1.0
    void (*on_change)(float value, void *user_data);
    void *user_data;
} quick_slider_t;

typedef struct {
    window_t *window;           // Quick settings panel
    bool visible;

    // Toggles
    quick_toggle_t wifi;
    quick_toggle_t bluetooth;
    quick_toggle_t dnd;         // Do Not Disturb

    // Sliders
    quick_slider_t volume;
    quick_slider_t brightness;

    // Action buttons
    button_t settings_btn;
    button_t power_btn;

    theme_t *theme;
} quick_settings_t;

// ============================================================================
// SYSTEM MENU
// ============================================================================

typedef struct {
    char label[128];
    char shortcut[32];
    void (*callback)(void *user_data);
    void *user_data;
    bool separator_after;
} menu_item_t;

typedef struct {
    window_t *window;           // System menu dropdown
    bool visible;

    menu_item_t items[32];
    uint32_t count;

    int32_t hover_index;        // -1 if no hover

    theme_t *theme;
} system_menu_t;

// ============================================================================
// DESKTOP SHELL (MAIN)
// ============================================================================

typedef struct {
    // Core components
    panel_t *panel;
    dock_t *dock;
    desktop_t *desktop;
    overview_t *overview;
    notification_center_t *notifications;
    quick_settings_t *quick_settings;
    system_menu_t *system_menu;

    // Compositor connection
    void *comp_client;          // Compositor client handle

    // Window list
    window_t *windows;
    uint32_t window_count;
    window_t *focused_window;

    // Workspaces
    uint32_t workspace_count;
    uint32_t current_workspace;

    // Theme
    theme_t theme;

    // State
    bool running;
    uint32_t screen_width;
    uint32_t screen_height;

    // Performance metrics
    uint32_t fps;
    uint64_t frame_time_us;
} desktop_shell_t;

// ============================================================================
// API FUNCTIONS
// ============================================================================

// Core shell lifecycle
desktop_shell_t *desktop_shell_create(uint32_t width, uint32_t height);
void desktop_shell_destroy(desktop_shell_t *shell);
void desktop_shell_run(desktop_shell_t *shell);
void desktop_shell_quit(desktop_shell_t *shell);

// Event handling
void desktop_shell_handle_mouse(desktop_shell_t *shell, int32_t x, int32_t y, uint32_t buttons);
void desktop_shell_handle_keyboard(desktop_shell_t *shell, uint32_t keycode, bool pressed);

// Panel API
panel_t *panel_create(desktop_shell_t *shell);
void panel_destroy(panel_t *panel);
void panel_update(panel_t *panel, uint64_t delta_us);
void panel_render(panel_t *panel);

// Dock API
dock_t *dock_create(desktop_shell_t *shell);
void dock_destroy(dock_t *dock);
void dock_add_app(dock_t *dock, const char *app_id, const char *name, bool pinned);
void dock_remove_app(dock_t *dock, const char *app_id);
void dock_update(dock_t *dock, uint64_t delta_us);
void dock_render(dock_t *dock);
void dock_handle_click(dock_t *dock, int32_t x, int32_t y);

// Desktop API
desktop_t *desktop_create(desktop_shell_t *shell);
void desktop_destroy(desktop_t *desktop);
void desktop_set_wallpaper(desktop_t *desktop, const char *path);
void desktop_add_icon(desktop_t *desktop, const char *name, const char *path, icon_type_t type);
void desktop_render(desktop_t *desktop);

// Overview API
overview_t *overview_create(desktop_shell_t *shell);
void overview_destroy(overview_t *overview);
void overview_open(overview_t *overview);
void overview_close(overview_t *overview);
void overview_search(overview_t *overview, const char *query);
void overview_render(overview_t *overview);

// Notification API
notification_center_t *notification_center_create(desktop_shell_t *shell);
void notification_center_destroy(notification_center_t *center);
uint32_t notification_send(notification_center_t *center, const char *app_name,
                          const char *summary, const char *body,
                          notif_urgency_t urgency);
void notification_dismiss(notification_center_t *center, uint32_t id);
void notification_center_render(notification_center_t *center);

// Quick settings API
quick_settings_t *quick_settings_create(desktop_shell_t *shell);
void quick_settings_destroy(quick_settings_t *qs);
void quick_settings_toggle(quick_settings_t *qs);
void quick_settings_render(quick_settings_t *qs);

// System menu API
system_menu_t *system_menu_create(desktop_shell_t *shell);
void system_menu_destroy(system_menu_t *menu);
void system_menu_open(system_menu_t *menu, int32_t x, int32_t y);
void system_menu_close(system_menu_t *menu);
void system_menu_render(system_menu_t *menu);

// Theme API
void theme_init_light(theme_t *theme);
void theme_init_dark(theme_t *theme);
void theme_apply(desktop_shell_t *shell, theme_mode_t mode);

// Utility functions
color_t color_rgb(uint8_t r, uint8_t g, uint8_t b);
color_t color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
color_t color_hex(uint32_t hex);
bool rect_contains(const rect_t *rect, int32_t x, int32_t y);
bool rect_intersects(const rect_t *a, const rect_t *b);

// Launcher functions
int launcher_launch_app(const char *app_id);
int launcher_launch_path(const char *exec_path);
bool launcher_is_running(const char *app_id);
uint32_t launcher_get_window_count(const char *app_id);
void launcher_update(void);

#endif // DESKTOP_SHELL_H
