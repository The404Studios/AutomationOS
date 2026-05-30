/**
 * AutomationOS Settings Application
 *
 * Complete system control panel for managing all OS settings:
 * - Display (resolution, refresh rate, scaling, night light)
 * - Appearance (theme, colors, wallpaper, fonts)
 * - Sound (audio devices, volume, effects)
 * - Network (Wi-Fi, Ethernet, VPN, proxy)
 * - Users & Accounts (user management, permissions)
 * - Applications (default apps, startup apps, permissions)
 * - Privacy & Security (capabilities, sandbox, firewall)
 * - System (about, updates, recovery, developer options)
 *
 * UI Design: Two-panel layout with category sidebar and content pane.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "../../shell/desktop/desktop_shell.h"

// ============================================================================
// CONSTANTS
// ============================================================================

#define SETTINGS_WINDOW_WIDTH   1000
#define SETTINGS_WINDOW_HEIGHT  700
#define SIDEBAR_WIDTH          240
#define CONTENT_PADDING        32
#define ITEM_HEIGHT            56
#define TOGGLE_WIDTH           52
#define TOGGLE_HEIGHT          32

#define MAX_SETTINGS_ITEMS     64
#define MAX_DROPDOWN_ITEMS     32
#define MAX_WIFI_NETWORKS      64
#define MAX_USERS              16
#define MAX_APPS               128

// ============================================================================
// ENUMS
// ============================================================================

typedef enum {
    CATEGORY_DISPLAY,
    CATEGORY_APPEARANCE,
    CATEGORY_SOUND,
    CATEGORY_NETWORK,
    CATEGORY_USERS,
    CATEGORY_APPLICATIONS,
    CATEGORY_ACCESSIBILITY,
    CATEGORY_PRIVACY,
    CATEGORY_SYSTEM,
    CATEGORY_COUNT
} settings_category_t;

typedef enum {
    WIDGET_LABEL,
    WIDGET_TOGGLE,
    WIDGET_SLIDER,
    WIDGET_DROPDOWN,
    WIDGET_BUTTON,
    WIDGET_TEXT_INPUT,
    WIDGET_COLOR_PICKER,
    WIDGET_LIST,
    WIDGET_SECTION_HEADER,
} widget_type_t;

typedef enum {
    RESOLUTION_1920x1080,
    RESOLUTION_2560x1440,
    RESOLUTION_3840x2160,
    RESOLUTION_1366x768,
    RESOLUTION_1600x900,
} display_resolution_t;

typedef enum {
    REFRESH_60HZ,
    REFRESH_75HZ,
    REFRESH_120HZ,
    REFRESH_144HZ,
    REFRESH_240HZ,
} display_refresh_t;

typedef enum {
    SCALE_100,
    SCALE_125,
    SCALE_150,
    SCALE_175,
    SCALE_200,
} display_scale_t;

typedef enum {
    THEME_LIGHT,
    THEME_DARK,
    THEME_AUTO,
} theme_mode_t;

typedef enum {
    ACCENT_BLUE,
    ACCENT_PURPLE,
    ACCENT_GREEN,
    ACCENT_ORANGE,
    ACCENT_RED,
    ACCENT_PINK,
    ACCENT_CUSTOM,
} accent_color_t;

typedef enum {
    AUDIO_OUTPUT,
    AUDIO_INPUT,
} audio_device_type_t;

typedef enum {
    NETWORK_TYPE_WIFI,
    NETWORK_TYPE_ETHERNET,
    NETWORK_TYPE_VPN,
} network_type_t;

typedef enum {
    WIFI_SECURITY_NONE,
    WIFI_SECURITY_WEP,
    WIFI_SECURITY_WPA,
    WIFI_SECURITY_WPA2,
    WIFI_SECURITY_WPA3,
} wifi_security_t;

typedef enum {
    USER_ROLE_ADMIN,
    USER_ROLE_STANDARD,
    USER_ROLE_GUEST,
} user_role_t;

// ============================================================================
// SETTINGS DATA STRUCTURES
// ============================================================================

/**
 * Display settings
 */
typedef struct {
    // Resolution & refresh
    display_resolution_t resolution;
    display_refresh_t refresh_rate;
    display_scale_t scale;
    uint32_t orientation;       // 0=normal, 90, 180, 270

    // Multi-monitor
    bool multi_monitor_enabled;
    uint32_t monitor_count;
    bool mirror_displays;
    uint32_t primary_monitor;

    // Night light
    bool night_light_enabled;
    uint32_t night_light_temp;  // Color temperature (K)
    uint32_t night_light_start; // Minutes since midnight
    uint32_t night_light_end;   // Minutes since midnight

    // Advanced
    bool vsync_enabled;
    bool hardware_acceleration;
} display_settings_t;

/**
 * Appearance settings
 */
typedef struct {
    // Theme
    theme_mode_t theme_mode;
    accent_color_t accent_color;
    color_t custom_accent;

    // Wallpaper
    char wallpaper_path[512];
    bool wallpaper_fit;         // true=fit, false=fill

    // Icon theme
    char icon_theme[128];
    uint32_t icon_size;         // 32, 48, 64

    // Fonts
    char font_system[128];
    char font_monospace[128];
    uint32_t font_size_small;
    uint32_t font_size_normal;
    uint32_t font_size_large;

    // Effects
    bool transparency_effects;
    bool animations_enabled;
    uint32_t animation_speed;   // 0=slow, 1=normal, 2=fast
    uint32_t blur_radius;
} appearance_settings_t;

/**
 * Audio device
 */
typedef struct {
    char name[128];
    char id[64];
    audio_device_type_t type;
    uint32_t sample_rate;
    uint32_t channels;
    bool is_default;
    bool enabled;
} audio_device_t;

/**
 * Sound settings
 */
typedef struct {
    // Output devices
    audio_device_t output_devices[8];
    uint32_t output_device_count;
    uint32_t default_output;

    // Input devices
    audio_device_t input_devices[8];
    uint32_t input_device_count;
    uint32_t default_input;

    // Volume levels
    float master_volume;        // 0.0 - 1.0
    float output_volume;
    float input_volume;
    bool muted;

    // Sound effects
    bool system_sounds_enabled;
    bool notification_sounds;
    bool startup_sound;
    float effects_volume;
} sound_settings_t;

/**
 * Wi-Fi network
 */
typedef struct {
    char ssid[128];
    wifi_security_t security;
    int32_t signal_strength;    // 0-100
    bool connected;
    bool saved;
    char password[256];
} wifi_network_t;

/**
 * VPN connection
 */
typedef struct {
    char name[128];
    char server[256];
    char username[128];
    char password[256];
    bool enabled;
    bool connected;
} vpn_connection_t;

/**
 * Network settings
 */
typedef struct {
    // Wi-Fi
    bool wifi_enabled;
    wifi_network_t wifi_networks[MAX_WIFI_NETWORKS];
    uint32_t wifi_network_count;

    // Ethernet
    bool ethernet_enabled;
    char ethernet_ip[64];
    char ethernet_mac[32];
    bool ethernet_dhcp;

    // VPN
    vpn_connection_t vpn_connections[8];
    uint32_t vpn_count;

    // Proxy
    bool proxy_enabled;
    char proxy_host[256];
    uint32_t proxy_port;
    char proxy_username[128];
    char proxy_password[256];

    // Advanced
    char dns_primary[64];
    char dns_secondary[64];
    bool ipv6_enabled;
} network_settings_t;

/**
 * User account
 */
typedef struct {
    uint32_t uid;
    char username[128];
    char full_name[256];
    char email[256];
    user_role_t role;
    bool enabled;
    bool auto_login;
    char avatar_path[512];
    uint64_t last_login;
} user_account_t;

/**
 * Users & accounts settings
 */
typedef struct {
    user_account_t users[MAX_USERS];
    uint32_t user_count;
    uint32_t current_user;

    // Login options
    bool require_password;
    uint32_t auto_logout_minutes;
    bool show_users_on_login;
    bool fast_user_switching;
} users_settings_t;

/**
 * Application entry
 */
typedef struct {
    char name[128];
    char app_id[64];            // com.example.app
    char exec_path[512];
    char icon_path[512];
    bool autostart;
    bool default_for_type[16];  // File type associations
} app_entry_t;

/**
 * Applications settings
 */
typedef struct {
    // Default applications
    char default_browser[128];
    char default_email[128];
    char default_editor[128];
    char default_terminal[128];
    char default_file_manager[128];

    // Startup applications
    app_entry_t startup_apps[MAX_APPS];
    uint32_t startup_app_count;

    // All applications
    app_entry_t all_apps[MAX_APPS];
    uint32_t all_app_count;

    // App permissions
    bool apps_can_access_location;
    bool apps_can_access_camera;
    bool apps_can_access_microphone;
} applications_settings_t;

/**
 * Capability permission
 */
typedef struct {
    char app_id[64];
    capability_type_t capability;
    bool granted;
    char details[256];
} capability_permission_t;

/**
 * Privacy & security settings
 */
typedef struct {
    // Capabilities
    capability_permission_t permissions[128];
    uint32_t permission_count;

    // Sandbox settings
    bool sandboxing_enabled;
    bool strict_mode;
    bool audit_sandbox_violations;

    // Firewall
    bool firewall_enabled;
    bool block_incoming;
    bool stealth_mode;
    uint32_t firewall_rule_count;

    // Encryption
    bool encrypt_home_folder;
    bool encrypt_swap;
    bool secure_delete;

    // Privacy
    bool location_services;
    bool diagnostics_reporting;
    bool usage_analytics;
} privacy_settings_t;

/**
 * System information
 */
typedef struct {
    char os_name[128];
    char os_version[64];
    char kernel_version[64];
    char build_date[64];

    // Hardware
    char cpu_model[256];
    uint32_t cpu_cores;
    uint64_t memory_total_mb;
    uint64_t memory_used_mb;
    char gpu_model[256];
    uint64_t disk_total_gb;
    uint64_t disk_used_gb;

    // Uptime
    uint64_t uptime_seconds;
} system_info_t;

/**
 * System settings
 */
typedef struct {
    system_info_t info;

    // Updates
    bool auto_check_updates;
    bool auto_download_updates;
    bool auto_install_updates;
    char update_channel[32];    // "stable", "beta", "dev"
    uint64_t last_update_check;

    // Recovery
    bool recovery_partition_enabled;
    uint64_t last_backup;
    char backup_location[512];

    // Developer options
    bool developer_mode;
    bool show_debug_info;
    bool enable_core_dumps;
    bool kernel_logging_verbose;
} system_settings_t;

// ============================================================================
// UI WIDGET STRUCTURES
// ============================================================================

/**
 * Generic widget
 */
typedef struct widget {
    widget_type_t type;
    rect_t bounds;
    char label[128];
    bool visible;
    bool enabled;
    bool hovered;

    union {
        // Toggle
        struct {
            bool value;
            void (*on_change)(bool value, void *user_data);
        } toggle;

        // Slider
        struct {
            float value;            // 0.0 - 1.0
            float min_value;
            float max_value;
            void (*on_change)(float value, void *user_data);
        } slider;

        // Dropdown
        struct {
            char items[MAX_DROPDOWN_ITEMS][128];
            uint32_t item_count;
            int32_t selected_index;
            bool expanded;
            void (*on_select)(int32_t index, void *user_data);
        } dropdown;

        // Button
        struct {
            void (*on_click)(void *user_data);
        } button;

        // Text input
        struct {
            char value[512];
            uint32_t cursor_pos;
            bool focused;
            void (*on_change)(const char *value, void *user_data);
        } text_input;

        // Color picker
        struct {
            color_t value;
            bool picker_open;
            void (*on_change)(color_t value, void *user_data);
        } color_picker;

        // List
        struct {
            void **items;
            uint32_t item_count;
            int32_t selected_index;
            uint32_t scroll_offset;
            void (*on_select)(int32_t index, void *user_data);
        } list;
    } data;

    void *user_data;
    struct widget *next;
} widget_t;

/**
 * Settings category item (sidebar)
 */
typedef struct {
    settings_category_t category;
    char label[64];
    char icon_name[32];
    texture_t *icon;
    bool selected;
    rect_t bounds;
} category_item_t;

/**
 * Settings panel (content area)
 */
typedef struct {
    settings_category_t category;
    char title[128];
    widget_t *widgets;          // Linked list of widgets
    uint32_t widget_count;
    uint32_t scroll_offset;
    uint32_t content_height;
} settings_panel_t;

// ============================================================================
// MAIN SETTINGS APPLICATION
// ============================================================================

typedef struct {
    window_t *window;
    theme_t *theme;

    // UI layout
    rect_t sidebar_rect;
    rect_t content_rect;
    rect_t titlebar_rect;

    // Sidebar
    category_item_t categories[CATEGORY_COUNT];
    settings_category_t current_category;

    // Content panels
    settings_panel_t panels[CATEGORY_COUNT];

    // Settings data
    display_settings_t display;
    appearance_settings_t appearance;
    sound_settings_t sound;
    network_settings_t network;
    users_settings_t users;
    applications_settings_t applications;
    privacy_settings_t privacy;
    system_settings_t system;

    // State
    bool running;
    bool dirty;                 // Has unsaved changes
    point_t mouse_pos;
    uint32_t mouse_buttons;

    // Search
    char search_query[256];
    bool search_active;
} settings_app_t;

// ============================================================================
// API FUNCTIONS
// ============================================================================

// Application lifecycle
settings_app_t *settings_app_create(theme_t *theme);
void settings_app_destroy(settings_app_t *app);
void settings_app_run(settings_app_t *app);
void settings_app_quit(settings_app_t *app);

// Event handling
void settings_app_handle_mouse(settings_app_t *app, int32_t x, int32_t y, uint32_t buttons);
void settings_app_handle_keyboard(settings_app_t *app, uint32_t keycode, bool pressed);
void settings_app_handle_scroll(settings_app_t *app, int32_t delta);

// Category management
void settings_app_select_category(settings_app_t *app, settings_category_t category);
void settings_app_search(settings_app_t *app, const char *query);

// Settings I/O
bool settings_app_load(settings_app_t *app);
bool settings_app_save(settings_app_t *app);
void settings_app_reset_defaults(settings_app_t *app);

// Panel creation
void settings_create_display_panel(settings_app_t *app);
void settings_create_appearance_panel(settings_app_t *app);
void settings_create_sound_panel(settings_app_t *app);
void settings_create_network_panel(settings_app_t *app);
void settings_create_users_panel(settings_app_t *app);
void settings_create_applications_panel(settings_app_t *app);
void settings_create_accessibility_panel(settings_app_t *app);
void settings_create_privacy_panel(settings_app_t *app);
void settings_create_system_panel(settings_app_t *app);

// Widget helpers
widget_t *widget_create(widget_type_t type, const char *label);
void widget_destroy(widget_t *widget);
void widget_add_to_panel(settings_panel_t *panel, widget_t *widget);
widget_t *widget_create_toggle(const char *label, bool initial_value,
                               void (*on_change)(bool, void*), void *user_data);
widget_t *widget_create_slider(const char *label, float initial_value,
                               float min, float max,
                               void (*on_change)(float, void*), void *user_data);
widget_t *widget_create_dropdown(const char *label, const char **items,
                                 uint32_t item_count, int32_t selected,
                                 void (*on_select)(int32_t, void*), void *user_data);
widget_t *widget_create_button(const char *label,
                               void (*on_click)(void*), void *user_data);

// Rendering
void settings_app_render(settings_app_t *app);
void settings_render_titlebar(settings_app_t *app);
void settings_render_sidebar(settings_app_t *app);
void settings_render_content(settings_app_t *app);
void settings_render_widget(settings_app_t *app, widget_t *widget);

// System integration
void settings_apply_display(settings_app_t *app);
void settings_apply_appearance(settings_app_t *app);
void settings_apply_sound(settings_app_t *app);
void settings_apply_network(settings_app_t *app);
void settings_apply_users(settings_app_t *app);
void settings_apply_applications(settings_app_t *app);
void settings_apply_privacy(settings_app_t *app);
void settings_apply_system(settings_app_t *app);

// Utility functions
const char *settings_resolution_to_string(display_resolution_t res);
const char *settings_refresh_to_string(display_refresh_t refresh);
const char *settings_scale_to_string(display_scale_t scale);
void settings_get_system_info(system_info_t *info);

#endif // SETTINGS_H
