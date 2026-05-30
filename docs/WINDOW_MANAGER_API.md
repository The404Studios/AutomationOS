# Window Manager API Reference

## Overview

The AutomationOS Window Manager provides a complete API for managing windows, workspaces, and user interactions. It handles window placement, focus, decorations, and advanced features like tiling and multi-workspace support.

## Core Concepts

### Window Lifecycle

```
Create ──▶ Map ──▶ Focus ──▶ Minimize/Maximize ──▶ Unmap ──▶ Destroy
           │                       │                  │
           └───────── Raise ───────┘                  │
                                                      │
           ┌──────────────────────────────────────────┘
           │
           ▼
        Restore ──▶ Map
```

## Initialization

### wm_init

Create and initialize window manager.

```c
window_manager_t *wm_init(compositor_t *comp);
```

**Parameters:**
- `comp` - Compositor instance (must be initialized first)

**Returns:**
- Pointer to window manager instance
- `NULL` on failure

**Example:**

```c
compositor_t *comp = compositor_init("/dev/dri/card0");
window_manager_t *wm = wm_init(comp);
if (!wm) {
    fprintf(stderr, "Failed to initialize window manager\n");
    return 1;
}
```

### wm_cleanup

Cleanup and free window manager resources.

```c
void wm_cleanup(window_manager_t *wm);
```

**Parameters:**
- `wm` - Window manager instance

**Example:**

```c
wm_cleanup(wm);
```

## Window Management

### wm_create_window

Create a new window.

```c
window_t *wm_create_window(window_manager_t *wm,
                          window_type_t type,
                          uint32_t width,
                          uint32_t height,
                          const char *title);
```

**Parameters:**
- `wm` - Window manager instance
- `type` - Window type (see Window Types)
- `width` - Window width in pixels
- `height` - Window height in pixels
- `title` - Window title string

**Returns:**
- Pointer to window structure
- `NULL` on failure

**Example:**

```c
window_t *window = wm_create_window(wm, WINDOW_NORMAL, 
                                   800, 600, 
                                   "My Application");
if (window) {
    // Setup window content
    wm_map_window(wm, window);
}
```

### Window Types

```c
typedef enum {
    WINDOW_NORMAL,      // Regular application window
    WINDOW_DIALOG,      // Modal dialog box
    WINDOW_UTILITY,     // Floating utility window
    WINDOW_TOOLBAR,     // Toolbar or panel
    WINDOW_MENU,        // Popup menu
    WINDOW_SPLASH,      // Splash screen
    WINDOW_DESKTOP,     // Desktop background
    WINDOW_DOCK,        // System dock/taskbar
} window_type_t;
```

**Window Type Behaviors:**

| Type | Decorations | Focus | Always On Top | Minimize |
|------|------------|-------|---------------|----------|
| NORMAL | Yes | Yes | No | Yes |
| DIALOG | Yes | Yes | No | No |
| UTILITY | No | Yes | Yes | No |
| TOOLBAR | No | No | Yes | No |
| MENU | No | Yes | Yes | No |
| SPLASH | No | No | Yes | No |
| DESKTOP | No | No | No | No |
| DOCK | No | No | Yes | No |

### wm_map_window

Make window visible on screen.

```c
void wm_map_window(window_manager_t *wm, window_t *window);
```

**Parameters:**
- `wm` - Window manager instance
- `window` - Window to map

**Behavior:**
- Adds window to compositor
- Plays open animation (fade + scale)
- Automatically focuses window
- Raises window to top of stack

**Example:**

```c
wm_map_window(wm, window);
```

### wm_unmap_window

Hide window from screen.

```c
void wm_unmap_window(window_manager_t *wm, window_t *window);
```

**Parameters:**
- `wm` - Window manager instance
- `window` - Window to unmap

**Example:**

```c
wm_unmap_window(wm, window);
```

### wm_destroy_window

Destroy window and free resources.

```c
void wm_destroy_window(window_manager_t *wm, uint32_t window_id);
```

**Parameters:**
- `wm` - Window manager instance
- `window_id` - ID of window to destroy

**Behavior:**
- Plays close animation
- Removes from compositor
- Frees all resources

**Example:**

```c
wm_destroy_window(wm, window->id);
```

## Window Operations

### wm_focus_window

Give keyboard focus to window.

```c
void wm_focus_window(window_manager_t *wm, window_t *window);
```

**Parameters:**
- `wm` - Window manager instance
- `window` - Window to focus

**Behavior:**
- Unfocuses previous window
- Updates focus state
- Raises window to top
- Redraws decorations (focused style)

**Example:**

```c
wm_focus_window(wm, window);
```

### wm_raise_window

Raise window to top of stacking order.

```c
void wm_raise_window(window_manager_t *wm, window_t *window);
```

**Example:**

```c
wm_raise_window(wm, window);
```

### wm_minimize_window

Minimize window to taskbar.

```c
void wm_minimize_window(window_manager_t *wm, window_t *window);
```

**Behavior:**
- Plays minimize animation (slide to taskbar)
- Unmaps window
- Maintains window state

**Example:**

```c
wm_minimize_window(wm, window);
```

### wm_maximize_window

Maximize window to full screen (with decorations).

```c
void wm_maximize_window(window_manager_t *wm, window_t *window);
```

**Behavior:**
- If not maximized: saves current geometry, expands to display size
- If maximized: restores previous geometry
- Plays maximize animation

**Example:**

```c
// Toggle maximize
wm_maximize_window(wm, window);
```

### wm_fullscreen_window

Toggle fullscreen mode (no decorations).

```c
void wm_fullscreen_window(window_manager_t *wm, window_t *window);
```

**Behavior:**
- Removes decorations
- Expands to full display size
- Raises window above all others

**Example:**

```c
wm_fullscreen_window(wm, window);
```

### wm_close_window

Request window closure.

```c
void wm_close_window(window_manager_t *wm, window_t *window);
```

**Behavior:**
- Sends close request to application
- Application should call `wm_destroy_window` when ready

**Example:**

```c
wm_close_window(wm, window);
```

## Window Placement

### wm_move_window

Move window to new position.

```c
void wm_move_window(window_manager_t *wm, 
                   window_t *window, 
                   int32_t x, 
                   int32_t y);
```

**Parameters:**
- `wm` - Window manager instance
- `window` - Window to move
- `x`, `y` - New position (top-left corner)

**Example:**

```c
// Move to center of screen
wm_move_window(wm, window, 560, 290);
```

### wm_resize_window

Resize window.

```c
void wm_resize_window(window_manager_t *wm,
                     window_t *window,
                     uint32_t width,
                     uint32_t height);
```

**Parameters:**
- `wm` - Window manager instance
- `window` - Window to resize
- `width`, `height` - New size in pixels

**Behavior:**
- Updates window geometry
- Reallocates surface buffer
- Marks surface as dirty

**Example:**

```c
wm_resize_window(wm, window, 1024, 768);
```

### wm_center_window

Center window on screen.

```c
void wm_center_window(window_manager_t *wm, window_t *window);
```

**Example:**

```c
wm_center_window(wm, window);
```

## Workspace Management

### wm_create_workspace

Create a new virtual workspace.

```c
workspace_t *wm_create_workspace(window_manager_t *wm, 
                                const char *name);
```

**Parameters:**
- `wm` - Window manager instance
- `name` - Workspace name

**Returns:**
- Pointer to workspace structure
- `NULL` on failure (max 16 workspaces)

**Example:**

```c
workspace_t *ws = wm_create_workspace(wm, "Development");
```

### wm_switch_workspace

Switch to different workspace.

```c
void wm_switch_workspace(window_manager_t *wm, uint32_t workspace_id);
```

**Parameters:**
- `wm` - Window manager instance
- `workspace_id` - ID of workspace to switch to

**Behavior:**
- Unmaps windows from current workspace
- Maps windows from new workspace
- Plays workspace switch animation

**Example:**

```c
wm_switch_workspace(wm, 1);  // Switch to workspace 1
```

### wm_move_window_to_workspace

Move window to different workspace.

```c
void wm_move_window_to_workspace(window_manager_t *wm,
                                window_t *window,
                                uint32_t workspace_id);
```

**Example:**

```c
wm_move_window_to_workspace(wm, window, 2);
```

## Tiling Window Management

### Tiling Modes

```c
typedef enum {
    TILING_NONE,            // Floating (manual placement)
    TILING_HORIZONTAL,      // Side-by-side
    TILING_VERTICAL,        // Stacked
    TILING_GRID,            // Grid layout
    TILING_MASTER_STACK,    // i3-style master + stack
} tiling_mode_t;
```

### wm_set_tiling_mode

Set tiling mode for current workspace.

```c
void wm_set_tiling_mode(window_manager_t *wm, tiling_mode_t mode);
```

**Parameters:**
- `wm` - Window manager instance
- `mode` - Tiling mode to activate

**Example:**

```c
// Enable side-by-side tiling
wm_set_tiling_mode(wm, TILING_HORIZONTAL);
```

### wm_tile_windows

Manually trigger window tiling.

```c
void wm_tile_windows(window_manager_t *wm, workspace_t *workspace);
```

**Behavior:**
- Calculates optimal layout based on tiling mode
- Resizes and repositions all windows
- Respects gap size settings

**Example:**

```c
workspace_t *ws = wm->workspaces[wm->active_workspace];
wm_tile_windows(wm, ws);
```

## Window Rules

Window rules allow per-application configuration.

### wm_add_rule

Add window placement rule.

```c
void wm_add_rule(window_manager_t *wm, const window_rule_t *rule);
```

**Rule Structure:**

```c
typedef struct {
    char app_name[64];          // Application name
    window_type_t type;         // Window type override
    placement_mode_t placement; // Floating or tiling
    bool decorations;           // Show decorations?
    int32_t workspace;          // Target workspace (-1 = current)
} window_rule_t;
```

**Example:**

```c
window_rule_t rule = {
    .app_name = "terminal",
    .type = WINDOW_NORMAL,
    .placement = PLACEMENT_TILING,
    .decorations = true,
    .workspace = -1,  // Current workspace
};
wm_add_rule(wm, &rule);
```

### wm_find_rule

Find rule for application.

```c
const window_rule_t *wm_find_rule(window_manager_t *wm, 
                                 const char *app_name);
```

## Input Handling

### wm_handle_mouse_button

Handle mouse button press/release.

```c
void wm_handle_mouse_button(window_manager_t *wm,
                           int32_t x,
                           int32_t y,
                           uint32_t button,
                           bool pressed);
```

**Parameters:**
- `wm` - Window manager instance
- `x`, `y` - Mouse position
- `button` - Button number (1=left, 2=middle, 3=right)
- `pressed` - True if pressed, false if released

**Behavior:**
- Finds window under cursor
- Focuses window on click
- Initiates drag/resize operations
- Handles decoration button clicks

**Example:**

```c
// Left mouse button pressed at (100, 200)
wm_handle_mouse_button(wm, 100, 200, 1, true);
```

### wm_handle_mouse_motion

Handle mouse movement.

```c
void wm_handle_mouse_motion(window_manager_t *wm,
                           int32_t x,
                           int32_t y);
```

**Parameters:**
- `wm` - Window manager instance
- `x`, `y` - New mouse position

**Behavior:**
- Updates window position during drag
- Updates window size during resize
- Updates cursor appearance

**Example:**

```c
wm_handle_mouse_motion(wm, 150, 250);
```

### wm_handle_key

Handle keyboard input.

```c
void wm_handle_key(window_manager_t *wm,
                  uint32_t key,
                  uint32_t modifiers,
                  bool pressed);
```

**Parameters:**
- `wm` - Window manager instance
- `key` - Key code
- `modifiers` - Modifier keys (Alt, Ctrl, Shift, Super)
- `pressed` - True if pressed, false if released

**Modifiers:**

```c
#define MOD_SHIFT   (1 << 0)
#define MOD_CTRL    (1 << 1)
#define MOD_ALT     (1 << 2)
#define MOD_SUPER   (1 << 3)
```

**Example:**

```c
// Alt+Tab pressed
wm_handle_key(wm, KEY_TAB, MOD_ALT, true);
```

## Utility Functions

### wm_window_at

Find window at screen coordinates.

```c
window_t *wm_window_at(window_manager_t *wm, int32_t x, int32_t y);
```

**Returns:**
- Pointer to topmost window at position
- `NULL` if no window at position

**Example:**

```c
window_t *window = wm_window_at(wm, 500, 300);
if (window) {
    printf("Found window: %s\n", window->title);
}
```

### wm_bring_to_front

Bring window to front of stacking order.

```c
void wm_bring_to_front(window_manager_t *wm, window_t *window);
```

## Window Structure

```c
typedef struct window {
    uint32_t id;                // Unique window ID
    window_type_t type;         // Window type
    
    rect_t geometry;            // Window content area
    rect_t frame_geometry;      // Including decorations
    
    bool mapped;                // Is window visible?
    bool minimized;
    bool maximized;
    bool fullscreen;
    bool focused;               // Has keyboard focus?
    
    char title[256];            // Window title
    uint32_t app_id;            // Application ID
    
    surface_t *surface;         // Window content pixels
    texture_t *texture;         // GPU texture
    
    animation_t *animation;     // Current animation
} window_t;
```

## Complete Example

```c
#include "compositor.h"
#include "window_manager.h"

int main(void) {
    // Initialize compositor
    compositor_t *comp = compositor_init("/dev/dri/card0");
    
    // Add display
    display_t *display = display_create(0, 1920, 1080, 60);
    compositor_add_display(comp, display);
    
    // Initialize window manager
    window_manager_t *wm = wm_init(comp);
    
    // Create and map window
    window_t *window = wm_create_window(wm, WINDOW_NORMAL, 
                                       800, 600, 
                                       "Hello World");
    wm_center_window(wm, window);
    wm_map_window(wm, window);
    
    // Main loop
    while (running) {
        // Handle input events
        // ...
        
        // Update window manager
        wm_update(wm);
        
        // Render frame
        compositor_frame(comp);
    }
    
    // Cleanup
    wm_cleanup(wm);
    compositor_cleanup(comp);
    
    return 0;
}
```

## See Also

- [COMPOSITOR_ARCHITECTURE.md](COMPOSITOR_ARCHITECTURE.md) - Compositor internals
- [EFFECTS_GUIDE.md](EFFECTS_GUIDE.md) - Visual effects and animations
- [compositor.conf](../userspace/compositor/compositor.conf) - Configuration reference
