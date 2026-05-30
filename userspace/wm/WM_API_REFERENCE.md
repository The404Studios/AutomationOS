# Window Manager API Reference

Quick reference for developers building GUI applications for AutomationOS.

---

## Creating a Window

```c
#include "wm/window_manager.h"

// Get window manager instance
window_manager_t *wm = get_global_wm();  // Or via IPC

// Create window
window_t *window = wm_create_window(
    wm,
    WINDOW_NORMAL,      // Window type
    800, 600,           // Width, height
    "My Application"    // Title
);

// Map (show) window
wm_map_window(wm, window);
```

---

## Window Operations

### Moving Windows
```c
wm_move_window(wm, window, 100, 100);     // Move to (100, 100)
wm_center_window(wm, window);             // Center on screen
```

### Resizing Windows
```c
wm_resize_window(wm, window, 1024, 768);  // Resize to 1024×768
```

### Window States
```c
wm_minimize_window(wm, window);           // Minimize to dock
wm_maximize_window(wm, window);           // Maximize (toggle)
wm_fullscreen_window(wm, window);         // Fullscreen (toggle)
wm_close_window(wm, window);              // Request close
```

### Focus
```c
wm_focus_window(wm, window);              // Give focus to window
wm_focus_next(wm);                        // Focus next window
wm_focus_prev(wm);                        // Focus previous window
```

---

## Window Types

```c
typedef enum {
    WINDOW_NORMAL,      // Regular app window (has decorations)
    WINDOW_DIALOG,      // Modal dialog
    WINDOW_UTILITY,     // Floating utility window
    WINDOW_TOOLBAR,     // Top panel (no decorations)
    WINDOW_MENU,        // Popup menu
    WINDOW_SPLASH,      // Splash screen
    WINDOW_DESKTOP,     // Desktop background
    WINDOW_DOCK,        // Taskbar/dock
} window_type_t;
```

---

## Window Structure

```c
typedef struct window {
    uint32_t id;                // Unique window ID
    window_type_t type;
    
    rect_t geometry;            // Content area (x, y, w, h)
    rect_t frame_geometry;      // Including decorations
    
    bool mapped;                // Visible on screen
    bool minimized;
    bool maximized;
    bool fullscreen;
    bool focused;
    
    char title[256];            // Window title
    
    surface_t *surface;         // Pixel buffer
    texture_t *texture;         // GPU texture (if applicable)
    
    animation_t *animation;     // Current animation
} window_t;
```

---

## Drawing to Window

```c
// Get window surface
surface_t *surface = window->surface;

// Draw pixels (ARGB32 format)
for (uint32_t y = 0; y < surface->height; y++) {
    for (uint32_t x = 0; x < surface->width; x++) {
        uint32_t offset = y * surface->width + x;
        
        // Format: 0xAARRGGBB
        uint32_t color = 0xFF3498DB;  // Opaque blue
        surface->pixels[offset] = color;
    }
}

// Mark surface as dirty
surface->dirty = true;

// Notify compositor to redraw
compositor_add_damage(wm->compositor, &window->geometry);
```

---

## IPC Communication

### Client Side (Application)

```c
#include <sys/socket.h>
#include <sys/un.h>

// Connect to window manager
int wm_fd = socket(AF_UNIX, SOCK_STREAM, 0);
struct sockaddr_un addr = {
    .sun_family = AF_UNIX,
    .sun_path = "/run/wm.sock"
};
connect(wm_fd, (struct sockaddr *)&addr, sizeof(addr));

// Create window via IPC
typedef struct {
    uint32_t type;
    uint32_t window_id;
    uint32_t data_len;
} ipc_message_header_t;

ipc_message_header_t msg = {
    .type = IPC_CREATE_WINDOW,
    .window_id = 0,
    .data_len = sizeof(uint32_t) * 3 + strlen("My App") + 1
};

uint32_t params[] = {
    WINDOW_NORMAL,  // type
    800,            // width
    600             // height
    // followed by title string
};

send(wm_fd, &msg, sizeof(msg), 0);
send(wm_fd, params, sizeof(params), 0);
send(wm_fd, "My App", strlen("My App") + 1, 0);

// Receive response with window ID
recv(wm_fd, &msg, sizeof(msg), 0);
uint32_t window_id = msg.window_id;
```

### Server Side (Window Manager)

Already implemented in `ipc.c`:
- `wm_ipc_init()` - Start IPC server
- `wm_ipc_handle_events()` - Process client messages
- `wm_ipc_send_event()` - Send event to client

---

## Input Events

### Mouse Events

```c
void wm_handle_mouse_button(window_manager_t *wm, 
                            int32_t x, int32_t y,
                            uint32_t button, 
                            bool pressed);

void wm_handle_mouse_motion(window_manager_t *wm,
                            int32_t x, int32_t y);

void wm_handle_scroll(window_manager_t *wm,
                     int32_t x, int32_t y,
                     int32_t scroll_x, int32_t scroll_y);
```

### Keyboard Events

```c
void wm_handle_key(window_manager_t *wm,
                  uint32_t key,
                  uint32_t modifiers,
                  bool pressed);
```

**Modifiers:**
```c
#define MOD_SHIFT   (1 << 0)
#define MOD_CTRL    (1 << 1)
#define MOD_ALT     (1 << 2)
#define MOD_SUPER   (1 << 3)
```

---

## Window Rules

Customize window behavior based on application name:

```c
window_rule_t rule = {
    .app_name = "terminal",
    .type = WINDOW_NORMAL,
    .placement = PLACEMENT_FLOATING,
    .decorations = true,
    .workspace = -1  // Current workspace
};
wm_add_rule(wm, &rule);
```

---

## Tiling Modes

```c
typedef enum {
    TILING_NONE,
    TILING_HORIZONTAL,      // Side by side
    TILING_VERTICAL,        // Stacked
    TILING_GRID,            // Grid layout
    TILING_MASTER_STACK,    // Master + stack (i3-style)
} tiling_mode_t;

// Set tiling mode for current workspace
wm_set_tiling_mode(wm, TILING_HORIZONTAL);
```

---

## Workspaces

```c
// Create workspace
workspace_t *ws = wm_create_workspace(wm, "Development");

// Switch workspace
wm_switch_workspace(wm, workspace_id);

// Move window to different workspace
wm_move_window_to_workspace(wm, window, workspace_id);
```

---

## Decorations

### Enable/Disable Decorations

```c
wm_set_window_decorations(wm, window, true);   // Enable
wm_set_window_decorations(wm, window, false);  // Disable
```

### Decoration Hit Testing

```c
bool on_titlebar = wm_hit_test_titlebar(wm, window, x, y);
bool on_close = wm_hit_test_close_button(wm, window, x, y);
bool on_max = wm_hit_test_maximize_button(wm, window, x, y);
bool on_min = wm_hit_test_minimize_button(wm, window, x, y);
```

### Custom Theme

```c
decoration_theme_t theme = {
    .titlebar_active = 0xFF3498DB,
    .titlebar_inactive = 0xFF34495E,
    .border_active = 0xFF3498DB,
    .border_inactive = 0xFF95A5A6,
    .text_color = 0xFFECF0F1,
    .close_btn_color = 0xFFE74C3C,
    .max_btn_color = 0xFF2ECC71,
    .min_btn_color = 0xFFF39C12,
};
wm_set_decoration_theme(&theme);
```

---

## Drag and Drop

### Start Drag

```c
// In mouse button handler
if (on_titlebar && button == 1 && pressed) {
    wm_start_drag(wm, window, mouse_x, mouse_y);
}
```

### Update Drag

```c
// In mouse motion handler
if (wm_is_dragging(wm)) {
    wm_update_drag(wm, mouse_x, mouse_y);
}
```

### End Drag

```c
// In mouse button handler
if (!pressed) {
    wm_end_drag(wm);
}
```

### Snap Behavior

```c
// With snapping
wm_update_drag_with_snap(wm, mouse_x, mouse_y);

// Check for Aero snap
wm_check_aero_snap(wm, mouse_x, mouse_y);
```

---

## Resize Operations

### Handle Resize Input

```c
// In mouse handler
if (wm_handle_resize_input(wm, window, x, y, pressed)) {
    // Resize operation started/updated/ended
}
```

### Manual Resize

```c
wm_resize_window_to(wm, window, 1024, 768);
wm_resize_window_proportional(wm, window, 1.5f);  // 150% scale
```

### Update Cursor for Resize

```c
// In mouse motion handler
wm_update_resize_cursor(wm, mouse_x, mouse_y);
```

---

## Compositor Integration

### Frame Sync (Compositor calls this)

```c
// Each frame at 60 FPS
wm_compositor_frame_sync(wm);
```

### Query Z-Order

```c
window_t *windows[MAX_WINDOWS];
uint32_t count = wm_get_window_z_order(wm, windows, MAX_WINDOWS);

// Render bottom to top
for (uint32_t i = 0; i < count; i++) {
    render_window(windows[i]);
}
```

### Render Decorations

```c
// Compositor calls this for each window
wm_render_window_decorations(wm, window, framebuffer, fb_width, fb_height);
```

### Damage Tracking

```c
// WM calls these to notify compositor
wm_notify_geometry_change(wm, window);
wm_notify_mapping_change(wm, window, mapped);
wm_notify_focus_change(wm, old_focus, new_focus);
```

---

## Example: Simple Window Application

```c
#include "wm/window_manager.h"
#include <stdio.h>

int main() {
    // Connect to window manager
    window_manager_t *wm = wm_connect();
    
    // Create window
    window_t *window = wm_create_window(wm, WINDOW_NORMAL, 640, 480, "Hello World");
    
    // Fill with blue
    surface_t *surface = window->surface;
    for (uint32_t i = 0; i < surface->width * surface->height; i++) {
        surface->pixels[i] = 0xFF3498DB;  // Blue
    }
    surface->dirty = true;
    
    // Show window
    wm_map_window(wm, window);
    
    // Main loop
    while (1) {
        // Handle events
        // Update window content
        // Sleep to maintain frame rate
    }
    
    // Cleanup
    wm_destroy_window(wm, window->id);
    wm_disconnect();
    
    return 0;
}
```

---

## Performance Tips

1. **Minimize redraws** - Only mark surface dirty when content changes
2. **Use damage tracking** - Update only changed regions
3. **Batch operations** - Group multiple window updates
4. **Avoid full redraw** - Use partial updates when possible
5. **Respect vsync** - Don't update faster than 60 FPS

---

## Common Patterns

### Modal Dialog

```c
window_t *dialog = wm_create_window(wm, WINDOW_DIALOG, 400, 300, "Confirm");
wm_center_window(wm, dialog);
wm_map_window(wm, dialog);

// Block main window input until dialog closes
```

### Splash Screen

```c
window_t *splash = wm_create_window(wm, WINDOW_SPLASH, 640, 480, "");
wm_set_window_decorations(wm, splash, false);
wm_center_window(wm, splash);
wm_map_window(wm, splash);

// Show for 3 seconds, then close
sleep(3);
wm_destroy_window(wm, splash->id);
```

### Utility Palette

```c
window_t *palette = wm_create_window(wm, WINDOW_UTILITY, 200, 400, "Tools");
// Utility windows stay on top of normal windows
wm_map_window(wm, palette);
```

---

## Error Handling

```c
window_t *window = wm_create_window(wm, WINDOW_NORMAL, 800, 600, "App");
if (!window) {
    fprintf(stderr, "Failed to create window\n");
    return 1;
}
```

---

**For more details, see:**
- `window_manager.h` - Full API documentation
- `AGENT8_INTEGRATION_COMPLETE.md` - Implementation details
- `INTEGRATION.md` - Architecture overview
