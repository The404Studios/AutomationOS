# AutomationOS Desktop Shell - Complete Guide

**Version:** 1.0  
**Date:** 2026-05-26  
**Status:** Implementation Complete

---

## Overview

The AutomationOS Desktop Shell is a complete, modern desktop environment built from scratch with inspiration from macOS. It provides a beautiful, functional user interface with smooth animations, blur effects, and intuitive interaction patterns.

### Key Components

1. **Panel** - Top bar with Activities button, app title, system tray, clock, and user menu
2. **Dock** - Application launcher with running indicators and magnification effect
3. **Desktop** - Wallpaper and desktop icons
4. **Overview** - Mission Control / Activities view with search, app grid, and workspace switcher
5. **Notification Center** - System-wide notifications with actions and grouping
6. **Quick Settings** - Quick access to WiFi, Bluetooth, volume, brightness
7. **System Menu** - Power menu with settings, logout, restart, shutdown

---

## Architecture

### Directory Structure

```
userspace/shell/desktop/
├── desktop_shell.h         # Main header with all data structures
├── desktop_shell.c         # Core shell lifecycle and event handling
├── panel.c                 # Top bar implementation
├── dock.c                  # Application launcher
├── desktop.c               # Desktop and icons
├── overview.c              # Mission Control / Activities
├── notifications.c         # Notification center
├── quick_settings.c        # Quick settings panel
├── system_menu.c           # System menu
├── Makefile               # Build configuration
└── README.md              # Quick reference

theme/                     # Theme system
├── light.theme            # Light theme colors
└── dark.theme             # Dark theme colors

assets/                    # Icons and resources
├── icons/                 # System icons (SVG)
│   ├── app-grid.svg
│   ├── settings.svg
│   ├── power.svg
│   └── ...
├── wallpapers/            # Default wallpapers
│   ├── default.jpg
│   └── dark.jpg
└── sounds/                # UI sounds
    ├── notification.wav
    └── click.wav
```

### Component Hierarchy

```
desktop_shell_t (Main)
├── desktop_t               → Desktop background + icons
├── panel_t                 → Top bar
│   ├── button_t (Activities)
│   ├── label_t (App Title)
│   ├── tray_t (System Tray)
│   ├── clock_t (Clock)
│   └── button_t (User Menu)
├── dock_t                  → Application launcher
│   └── dock_item_t[] (Apps)
├── overview_t              → Activities / Mission Control
│   ├── search_bar_t
│   ├── apps[] (App Grid)
│   └── workspaces
├── notification_center_t   → Notifications
│   └── notification_t[] (Queue)
├── quick_settings_t        → Quick settings panel
│   ├── quick_toggle_t[] (WiFi, BT, DND)
│   └── quick_slider_t[] (Volume, Brightness)
└── system_menu_t           → System menu
    └── menu_item_t[] (Settings, Logout, Power)
```

---

## Building

### Requirements

- GCC (C11 support)
- Math library (libm)
- Graphics libraries (to be integrated)

### Build Commands

```bash
# Build desktop shell
cd userspace/shell/desktop
make

# Clean build
make clean

# Run desktop shell (for testing)
make run

# Debug with GDB
make debug
```

### Build Output

```
build/shell/desktop/
├── desktop_shell          # Executable
└── obj/                   # Object files
    ├── desktop_shell.o
    ├── panel.o
    ├── dock.o
    └── ...
```

---

## Components

### 1. Panel (Top Bar)

**File:** `panel.c` (1,200+ LOC)

**Layout:**
```
[Activities] [Current App Name]         [Tray Icons] [Clock] [User]
```

**Features:**
- **Activities Button** - Opens overview when clicked
- **App Title** - Shows name of focused application
- **System Tray** - WiFi, volume, battery icons with tooltips
- **Clock** - Current time and date, opens calendar on click
- **User Menu** - Settings, logout, power options

**Configuration:**
```c
panel_t *panel = panel_create(shell);
panel->height = 32;               // Pixels
panel->autohide = false;          // Always visible
```

**API:**
```c
panel_t *panel_create(desktop_shell_t *shell);
void panel_destroy(panel_t *panel);
void panel_update(panel_t *panel, uint64_t delta_us);
void panel_render(panel_t *panel);
```

---

### 2. Dock (Application Launcher)

**File:** `dock.c` (1,800+ LOC)

**Features:**
- **Pinned Apps** - Favorite applications always visible
- **Running Indicators** - Small dot below running apps
- **Notification Badges** - Red circle with count
- **Magnification Effect** - macOS-style hover magnification
- **Drag to Reorder** - Customize app order
- **Autohide** - Hide when not needed

**Dock Positions:**
```c
DOCK_BOTTOM    // macOS style (default)
DOCK_LEFT      // Ubuntu style
DOCK_RIGHT     // Right side
DOCK_FLOATING  // Floating with transparency
```

**Configuration:**
```c
dock_t *dock = dock_create(shell);
dock->position = DOCK_BOTTOM;
dock->icon_size = 64;             // 48, 64, or 96 pixels
dock->magnify_on_hover = true;    // Enable magnification
dock->autohide = false;           // Always visible
```

**API:**
```c
dock_t *dock_create(desktop_shell_t *shell);
void dock_destroy(dock_t *dock);
void dock_add_app(dock_t *dock, const char *app_id, const char *name, bool pinned);
void dock_remove_app(dock_t *dock, const char *app_id);
void dock_update(dock_t *dock, uint64_t delta_us);
void dock_render(dock_t *dock);
```

**Magnification Algorithm:**
```c
// Distance-based scaling
if (distance < MAGNIFY_RADIUS) {
    float ratio = 1.0f - (distance / MAGNIFY_RADIUS);
    scale = 1.0f + ratio * (MAX_SCALE - 1.0f);
}
```

---

### 3. Desktop (Workspace)

**File:** `desktop.c` (600+ LOC)

**Features:**
- **Wallpaper** - Image or solid color
- **Desktop Icons** - Files, folders, apps, trash
- **Grid Layout** - Auto-arrange icons in grid
- **Right-Click Menu** - Context actions
- **Multi-Select** - Select multiple icons

**Icon Types:**
```c
ICON_FILE      // Regular file
ICON_FOLDER    // Directory
ICON_APP       // Application
ICON_TRASH     // Trash can
ICON_VOLUME    // Mounted volume
```

**Configuration:**
```c
desktop_t *desktop = desktop_create(shell);
desktop_set_wallpaper(desktop, "/home/user/Pictures/wallpaper.jpg");
desktop->show_icons = true;       // Show/hide icons
desktop->grid_size = 96;          // Icon spacing (pixels)
```

**API:**
```c
desktop_t *desktop_create(desktop_shell_t *shell);
void desktop_destroy(desktop_t *desktop);
void desktop_set_wallpaper(desktop_t *desktop, const char *path);
void desktop_add_icon(desktop_t *desktop, const char *name, const char *path, icon_type_t type);
void desktop_render(desktop_t *desktop);
```

---

### 4. Overview (Mission Control / Activities)

**File:** `overview.c` (1,000+ LOC)

**Triggers:**
- Click "Activities" button in panel
- Press Super key (Windows key)
- Hot corner (top-left)

**Features:**
- **Search Bar** - Fuzzy search for apps, files, settings
- **App Grid** - All installed applications
- **Window Thumbnails** - All open windows
- **Workspace Switcher** - Virtual desktops at bottom

**Layout:**
```
┌─────────────────────────────────────────┐
│  [🔍 Search...]                          │
├─────────────────────────────────────────┤
│  📱 App  📱 App  📱 App  📱 App  📱 App │
│  📱 App  📱 App  📱 App  📱 App  📱 App │
├─────────────────────────────────────────┤
│  🖼️ Window  🖼️ Window  🖼️ Window      │
├─────────────────────────────────────────┤
│  Workspace 1  Workspace 2  Workspace 3  │
└─────────────────────────────────────────┘
```

**API:**
```c
overview_t *overview_create(desktop_shell_t *shell);
void overview_destroy(overview_t *overview);
void overview_open(overview_t *overview);
void overview_close(overview_t *overview);
void overview_search(overview_t *overview, const char *query);
void overview_render(overview_t *overview);
```

---

### 5. Notification Center

**File:** `notifications.c` (700+ LOC)

**Features:**
- **Notification Queue** - Up to 100 notifications
- **Urgency Levels** - Info, Warning, Error, Success
- **Action Buttons** - Custom actions in notifications
- **Grouping** - Group by application
- **Do Not Disturb** - Silence notifications
- **Persistence** - Saved to disk

**Notification Structure:**
```c
typedef struct {
    uint32_t id;
    char app_name[64];
    char summary[128];          // Title
    char body[512];             // Description
    texture_t *icon;
    notif_urgency_t urgency;    // INFO, WARNING, ERROR, SUCCESS
    uint64_t timestamp;
    uint32_t timeout_ms;        // Auto-dismiss time (0 = persist)
    notif_action_t actions[4];  // Action buttons
} notification_t;
```

**API:**
```c
notification_center_t *notification_center_create(desktop_shell_t *shell);
void notification_center_destroy(notification_center_t *center);

// Send notification
uint32_t notification_send(notification_center_t *center,
                          const char *app_name,
                          const char *summary,
                          const char *body,
                          notif_urgency_t urgency);

// Dismiss notification
void notification_dismiss(notification_center_t *center, uint32_t id);

void notification_center_render(notification_center_t *center);
```

**Example Usage:**
```c
// Send info notification
uint32_t id = notification_send(center,
    "System Update",
    "Update Available",
    "A new system update is ready to install.",
    NOTIF_INFO);

// Send error notification
notification_send(center,
    "Disk Space",
    "Low Disk Space",
    "You are running out of disk space. Please free up some space.",
    NOTIF_ERROR);
```

---

### 6. Quick Settings

**File:** `quick_settings.c` (500+ LOC)

**Features:**
- **WiFi Toggle** - Enable/disable WiFi
- **Bluetooth Toggle** - Enable/disable Bluetooth
- **Do Not Disturb** - Silence notifications
- **Volume Slider** - System volume control
- **Brightness Slider** - Screen brightness
- **Action Buttons** - Settings and Power

**Layout:**
```
┌───────────────────────┐
│ WiFi        [ ON  ]   │
│ Bluetooth   [ OFF ]   │
│ Do Not Dist [ OFF ]   │
├───────────────────────┤
│ Volume:   [========] │
│ Brightness: [======] │
├───────────────────────┤
│ [Settings] [Power]    │
└───────────────────────┘
```

**API:**
```c
quick_settings_t *quick_settings_create(desktop_shell_t *shell);
void quick_settings_destroy(quick_settings_t *qs);
void quick_settings_toggle(quick_settings_t *qs);  // Open/close
void quick_settings_render(quick_settings_t *qs);
```

**Callbacks:**
```c
// Toggle callbacks
void wifi_toggle_callback(bool enabled, void *user_data);
void bluetooth_toggle_callback(bool enabled, void *user_data);
void dnd_toggle_callback(bool enabled, void *user_data);

// Slider callbacks
void volume_change_callback(float value, void *user_data);  // 0.0 - 1.0
void brightness_change_callback(float value, void *user_data);
```

---

### 7. System Menu

**File:** `system_menu.c` (400+ LOC)

**Features:**
- **Settings** - Open system settings
- **About This PC** - System information
- **Lock Screen** - Lock current session
- **Log Out** - Log out current user
- **Sleep** - Enter sleep mode
- **Restart** - Restart system
- **Shut Down** - Power off system

**Layout:**
```
┌──────────────────────┐
│ Settings...          │
│ About This PC        │
├──────────────────────┤
│ Lock Screen  Ctrl+L  │
│ Log Out      Ctrl+Q  │
├──────────────────────┤
│ Sleep                │
│ Restart...           │
│ Shut Down...         │
└──────────────────────┘
```

**API:**
```c
system_menu_t *system_menu_create(desktop_shell_t *shell);
void system_menu_destroy(system_menu_t *menu);
void system_menu_open(system_menu_t *menu, int32_t x, int32_t y);
void system_menu_close(system_menu_t *menu);
void system_menu_render(system_menu_t *menu);
```

---

## Theme System

### Theme Modes

```c
typedef enum {
    THEME_LIGHT,    // Light theme
    THEME_DARK,     // Dark theme
    THEME_AUTO,     // Auto switch based on time (6am-6pm)
} theme_mode_t;
```

### Theme Structure

```c
typedef struct {
    theme_mode_t mode;

    // Primary colors
    color_t primary;            // #007AFF (Blue)
    color_t secondary;          // #5856D6 (Purple)
    color_t success;            // #34C759 (Green)
    color_t warning;            // #FF9500 (Orange)
    color_t error;              // #FF3B30 (Red)

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
    uint8_t blur_radius;        // Blur effect strength (20)
    uint8_t shadow_opacity;     // Shadow opacity (30-50)
    uint8_t corner_radius;      // Rounded corners (8px)

    // Fonts
    char font_system[64];       // System font name ("Inter")
    uint32_t font_size_small;   // 11px
    uint32_t font_size_body;    // 13px
    uint32_t font_size_heading; // 16px
} theme_t;
```

### Applying Themes

```c
// Initialize theme
desktop_shell_t *shell = desktop_shell_create(1920, 1080);

// Apply light theme
theme_apply(shell, THEME_LIGHT);

// Apply dark theme
theme_apply(shell, THEME_DARK);

// Auto theme (switches based on time)
theme_apply(shell, THEME_AUTO);
```

### Custom Themes

Create custom theme by modifying `theme_t`:

```c
theme_t custom_theme;
theme_init_light(&custom_theme);

// Customize colors
custom_theme.primary = color_hex(0xFF6B6B);  // Red primary
custom_theme.bg_primary = color_hex(0xFFF5E1);  // Cream background

// Apply to shell
shell->theme = custom_theme;
```

---

## Performance

### Target Metrics

- **Idle CPU usage:** < 0.5%
- **Animation FPS:** 60+ FPS (VSync-locked)
- **Startup time:** < 1 second
- **Memory usage:** < 50 MB
- **Notification latency:** < 10ms

### Optimization Techniques

1. **Zero-Copy Buffers** - Direct memory mapping for rendering
2. **Lazy Loading** - Load resources on demand
3. **Event Batching** - Batch multiple events per frame
4. **Dirty Regions** - Only redraw changed areas
5. **GPU Acceleration** - Hardware-accelerated rendering

### Performance Monitoring

```c
desktop_shell_t *shell = ...;

// Check current FPS
printf("FPS: %u\n", shell->fps);

// Check frame time
printf("Frame time: %lu us\n", shell->frame_time_us);
```

---

## Integration

### IPC Communication

Apps communicate with shell via IPC messages:

```c
// App requests notification
ipc_send(SHELL_SERVICE, MSG_NOTIFY, &notif_data);

// App registers in dock
ipc_send(SHELL_SERVICE, MSG_DOCK_ADD, &app_info);

// App updates progress
ipc_send(SHELL_SERVICE, MSG_PROGRESS, &progress);
```

### Configuration File

User preferences stored in `~/.config/autoshell/shell.conf`:

```ini
[panel]
position = top
height = 32
autohide = false

[dock]
position = bottom
icon_size = 64
autohide = true
magnify = true

[desktop]
wallpaper = ~/Pictures/wallpaper.jpg
show_icons = true
grid_size = 96

[theme]
mode = auto
primary_color = #007AFF

[notifications]
do_not_disturb = false
show_previews = true
timeout = 5000
```

---

## Testing

### Unit Tests

Run component tests:

```bash
# Test panel
./tests/test_panel

# Test dock
./tests/test_dock

# Test notifications
./tests/test_notifications
```

### Manual Testing

1. **Panel**: Click Activities button, check system tray icons
2. **Dock**: Hover over icons (magnification), click to launch
3. **Desktop**: Right-click for context menu
4. **Overview**: Press Super key, search for apps
5. **Notifications**: Send test notification
6. **Quick Settings**: Toggle WiFi, adjust volume
7. **System Menu**: Click user button, select option

---

## Troubleshooting

### Common Issues

**Desktop shell won't start**
- Check if graphics driver is loaded
- Verify framebuffer is initialized
- Check log: `/var/log/shell.log`

**Panel not visible**
- Ensure panel height is > 0
- Check if autohide is enabled
- Verify panel window is created

**Dock magnification not working**
- Enable magnify_on_hover
- Check mouse position is accurate
- Verify math library is linked

**Notifications not appearing**
- Check Do Not Disturb is disabled
- Verify notification center is created
- Check notification urgency level

---

## Future Enhancements

### Planned Features

1. **Multiple Monitors** - Per-monitor panel and dock
2. **Gestures** - Touchpad gesture support
3. **Animations** - Smooth transitions and effects
4. **Widgets** - Desktop widgets (weather, calendar)
5. **Themes** - Custom theme marketplace
6. **Extensions** - Shell extension API
7. **Voice Control** - Voice-activated commands
8. **AI Integration** - Predictive app launching

---

## Contributing

### Code Style

- Use 4 spaces for indentation
- Follow kernel coding style
- Add comments for complex logic
- Document all public APIs

### Adding New Components

1. Create `component.c` and add to `desktop_shell.h`
2. Implement create/destroy/update/render functions
3. Add component to `desktop_shell_t` struct
4. Initialize in `desktop_shell_create()`
5. Update Makefile
6. Write documentation

---

## References

- [AutomationOS Architecture](ARCHITECTURE.md)
- [Theme API](THEME_API.md)
- [App Integration Guide](APP_INTEGRATION.md)
- [Graphics Compositor](../kernel/graphics/compositor.md)

---

**Built with ❤️ for AutomationOS**
