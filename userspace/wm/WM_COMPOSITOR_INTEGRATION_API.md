# Window Manager - Compositor Integration API

## Quick Reference

This document describes the API between the Window Manager and Compositor.

## Core Integration Functions

### compositor_sync_geometry()

**Purpose**: Synchronize window geometry to compositor

**Signature**:
```c
void compositor_sync_geometry(window_manager_t *wm, window_t *window);
```

**Parameters**:
- `wm` - Window manager instance
- `window` - Window whose geometry changed

**When to Call**:
- After `wm_move_window()`
- After `wm_resize_window()`
- After any geometry modification

**IPC Message**:
- Type: `"geometry"`
- Data: `{int32_t x, y; uint32_t w, h;}`

**Example**:
```c
window_t *win = wm_create_window(wm, WINDOW_NORMAL, 800, 600, "My Window");
wm_move_window(wm, win, 100, 100);
// compositor_sync_geometry() is called automatically
```

---

### compositor_set_focus()

**Purpose**: Notify compositor which window has input focus

**Signature**:
```c
void compositor_set_focus(window_manager_t *wm, window_t *window);
```

**Parameters**:
- `wm` - Window manager instance
- `window` - Window to focus (NULL to clear focus)

**When to Call**:
- After `wm_focus_window()`
- When focus changes for any reason

**IPC Message**:
- Type: `"focus"`
- Data: `{uint32_t window_id;}`

**Example**:
```c
wm_focus_window(wm, window);
// compositor_set_focus() is called automatically
```

---

### compositor_render_decorations()

**Purpose**: Render window decorations (titlebar, borders, buttons)

**Signature**:
```c
void compositor_render_decorations(window_manager_t *wm, window_t *window,
                                   uint32_t *framebuffer, 
                                   uint32_t fb_width, uint32_t fb_height);
```

**Parameters**:
- `wm` - Window manager instance
- `window` - Window to render decorations for
- `framebuffer` - Framebuffer to draw into
- `fb_width` - Framebuffer width
- `fb_height` - Framebuffer height

**When to Call**:
- During compositor's render pass
- For each visible window with decorations
- Called by compositor, not by WM

**What It Draws**:
- Titlebar (blue if focused, gray if not)
- Close button (red, top-right)
- Maximize button (green, next to close)
- Minimize button (yellow, next to maximize)
- Window borders (1px by default)

**Example (Compositor Code)**:
```c
for (each window in render_list) {
    if (window->type == WINDOW_NORMAL && decorations_enabled) {
        compositor_render_decorations(wm, window, fb, width, height);
    }
}
```

---

## Notification Functions

These functions are called automatically by window manager operations to notify the compositor of changes.

### wm_notify_geometry_change()

**Purpose**: Mark damage regions for geometry changes

**Signature**:
```c
void wm_notify_geometry_change(window_manager_t *wm, window_t *window);
```

**Called By**:
- `wm_move_window()`
- `wm_resize_window()`

**Effect**:
- Marks old and new window positions as damaged
- Ensures compositor redraws affected areas

---

### wm_notify_mapping_change()

**Purpose**: Notify compositor of window visibility changes

**Signature**:
```c
void wm_notify_mapping_change(window_manager_t *wm, window_t *window, bool mapped);
```

**Parameters**:
- `mapped` - true if window is being shown, false if hidden

**Called By**:
- `wm_map_window()`
- `wm_unmap_window()`

**Effect**:
- Adds/removes window from compositor's render list
- Marks window area as damaged

---

### wm_notify_focus_change()

**Purpose**: Notify compositor of focus changes

**Signature**:
```c
void wm_notify_focus_change(window_manager_t *wm, 
                            window_t *old_focus, 
                            window_t *new_focus);
```

**Parameters**:
- `old_focus` - Previously focused window (can be NULL)
- `new_focus` - Newly focused window (can be NULL)

**Called By**:
- `wm_focus_window()`

**Effect**:
- Marks both windows as needing redraw
- Triggers decoration color update

---

## Query Functions

### wm_get_window_z_order()

**Purpose**: Get list of windows in render order (bottom to top)

**Signature**:
```c
uint32_t wm_get_window_z_order(window_manager_t *wm, 
                                window_t **out_windows, 
                                uint32_t max_windows);
```

**Parameters**:
- `out_windows` - Array to fill with window pointers
- `max_windows` - Maximum number of windows to return

**Returns**:
- Number of windows written to `out_windows`

**Example**:
```c
window_t *windows[MAX_WINDOWS];
uint32_t count = wm_get_window_z_order(wm, windows, MAX_WINDOWS);

for (uint32_t i = 0; i < count; i++) {
    render_window(windows[i]);  // Render bottom to top
}
```

---

### wm_get_window_geometry()

**Purpose**: Query window geometry

**Signature**:
```c
bool wm_get_window_geometry(window_manager_t *wm, 
                            uint32_t window_id,
                            rect_t *geometry, 
                            rect_t *frame_geometry);
```

**Parameters**:
- `window_id` - Window to query
- `geometry` - (Out) Window content geometry
- `frame_geometry` - (Out) Window frame geometry (including decorations)

**Returns**:
- `true` if window found, `false` otherwise

**Example**:
```c
rect_t geom, frame;
if (wm_get_window_geometry(wm, win_id, &geom, &frame)) {
    printf("Window at (%d,%d) size %ux%u\n", 
           geom.x, geom.y, geom.w, geom.h);
}
```

---

## IPC Message Protocol

### Message Structure

```c
typedef struct {
    uint32_t type;        // Message type (IPC_CREATE_WINDOW, etc.)
    uint32_t window_id;   // Target window ID
    uint32_t data_len;    // Length of payload data
    // ... payload data follows ...
} ipc_message_header_t;
```

### Message Types

| Type | Value | Direction | Purpose |
|------|-------|-----------|---------|
| `IPC_CREATE_WINDOW` | 1 | WM → Compositor | Create window |
| `IPC_DESTROY_WINDOW` | 2 | WM → Compositor | Destroy window |
| `IPC_UPDATE_WINDOW` | 3 | App → Compositor | Update surface |
| `IPC_MOVE_WINDOW` | 4 | WM → Compositor | Move window |
| `IPC_RESIZE_WINDOW` | 5 | WM → Compositor | Resize window |
| `IPC_MAP_WINDOW` | 6 | WM → Compositor | Show window |
| `IPC_UNMAP_WINDOW` | 7 | WM → Compositor | Hide window |
| `IPC_FOCUS_WINDOW` | 8 | WM → Compositor | Focus window |
| `IPC_RAISE_WINDOW` | 9 | WM → Compositor | Raise to top |
| `IPC_KEYBOARD_EVENT` | 12 | Compositor → WM | Key press/release |
| `IPC_MOUSE_EVENT` | 13 | Compositor → WM | Mouse event |
| `IPC_CONFIGURE_EVENT` | 14 | Compositor → App | Window configured |
| `IPC_DAMAGE_EVENT` | 15 | WM → Compositor | Damage region |

### Sending Messages

**From Window Manager**:
```c
int wm_ipc_send_event(uint32_t window_id, 
                      const char *event_type,
                      const void *data, 
                      size_t data_len);
```

**Example**:
```c
struct {
    int32_t x, y;
    uint32_t w, h;
} geom = {100, 100, 800, 600};

wm_ipc_send_event(win->id, "geometry", &geom, sizeof(geom));
```

---

## Synchronization

### Full Window Sync

**Purpose**: Synchronize all window states to compositor

**Function**:
```c
void wm_ipc_sync_windows(int comp_fd, window_manager_t *wm);
```

**When Called**:
- Every frame in main loop
- After major state changes
- On compositor reconnect

**Data Sent**:
- Window count
- For each window:
  - Window ID (4 bytes)
  - Geometry (16 bytes)
  - Flags (4 bytes): mapped, focused, minimized, maximized, fullscreen

---

## Integration Workflow

### Application Creates Window

```
Application                WM                     Compositor
    │                      │                          │
    ├─IPC_CREATE_WINDOW───>│                          │
    │                      ├─wm_create_window()       │
    │                      ├─wm_map_window()          │
    │                      │  ├─compositor_add_window()│
    │                      │  └─wm_notify_mapping_change()
    │                      │                          │
    │                      ├─IPC_CREATE_WINDOW────────>│
    │                      │                          ├─allocate_texture()
    │                      │                          ├─add_to_list()
    │                      │                          │
    │<─Window ID───────────┤                          │
```

### User Moves Window

```
User Input               WM                     Compositor
    │                    │                          │
    ├─Mouse Drag────────>│                          │
    │                    ├─wm_handle_mouse_motion() │
    │                    ├─wm_move_window()         │
    │                    │  ├─update geometry        │
    │                    │  ├─wm_notify_geometry_change()
    │                    │  └─compositor_sync_geometry()
    │                    │                          │
    │                    ├─IPC_MOVE_WINDOW──────────>│
    │                    │  {x, y, w, h}            ├─update_position()
    │                    │                          ├─mark_damage()
    │                    │                          ├─render_frame()
    │                    │                          │
```

### User Clicks Window

```
Hardware                Compositor              WM
    │                       │                    │
    ├─Mouse Click──────────>│                    │
    │                       ├─hit_test()         │
    │                       ├─find_window()      │
    │                       │                    │
    │                       ├─IPC_MOUSE_EVENT────>│
    │                       │  {x, y, button}    ├─wm_handle_mouse_button()
    │                       │                    ├─wm_focus_window()
    │                       │                    │  └─compositor_set_focus()
    │                       │                    │
    │                       │<─IPC_FOCUS_WINDOW──┤
    │                       ├─update_focus()     │
    │                       ├─mark_decorations_dirty()
    │                       ├─render_frame()     │
```

---

## Decoration Colors

### Titlebar Colors

| State | Color | Hex |
|-------|-------|-----|
| Focused | Blue | `0xFF3498DB` |
| Unfocused | Dark Gray | `0xFF34495E` |
| Text | Light Gray | `0xFFECF0F1` |

### Button Colors

| Button | Color | Hex |
|--------|-------|-----|
| Close | Red | `0xFFE74C3C` |
| Maximize | Green | `0xFF2ECC71` |
| Minimize | Yellow | `0xFFF39C12` |

### Border Colors

| State | Color | Hex |
|-------|-------|-----|
| Focused | Blue | `0xFF3498DB` |
| Unfocused | Light Gray | `0xFF95A5A6` |

---

## Performance Considerations

### IPC Overhead

- Message packing: ~0.1ms
- Socket send: ~0.2ms
- Total: <0.5ms per message
- Negligible for 60 FPS target (16.67ms budget)

### Optimization Tips

1. **Batch Updates**: Group multiple geometry changes
2. **Damage Tracking**: Only redraw changed regions
3. **Lazy Sync**: Don't sync invisible windows
4. **Throttle Input**: Limit mouse motion events to 60 Hz

### Recommended Sync Frequency

| Operation | Frequency |
|-----------|-----------|
| Geometry sync | Every change |
| Focus sync | Every focus change |
| Full sync | Every 60 frames |
| Input events | As received |

---

## Error Handling

### Connection Failures

```c
int comp_fd = wm_ipc_connect_compositor("/run/compositor.sock");
if (comp_fd < 0) {
    // Compositor not available
    // WM can run in standalone mode
    // Retry connection periodically
}
```

### Send Failures

```c
if (wm_ipc_send_event(win_id, "geometry", &geom, sizeof(geom)) < 0) {
    // Log error but continue
    // Next sync will retry
}
```

### Recovery

- Full sync every 60 frames ensures recovery from desync
- Compositor maintains authoritative state
- WM can reconnect if compositor restarts

---

## Testing Integration

### Unit Test

```c
// Create mock compositor
compositor_t *comp = create_mock_compositor();
window_manager_t *wm = wm_init(comp);

// Test geometry sync
window_t *win = wm_create_window(wm, WINDOW_NORMAL, 800, 600, "Test");
wm_move_window(wm, win, 100, 100);

// Verify sync called
assert(compositor_received_geometry_update);
```

### Integration Test

```bash
# Start compositor
servicectl start compositor

# Start window manager
servicectl start window-manager

# Create test window
./demo_simple_window

# Verify:
# - Window appears
# - Decorations rendered
# - Window can be moved
# - Focus changes work
```

---

## Debugging

### Enable Verbose Logging

```c
#define WM_DEBUG 1

// Logs all IPC messages
printf("[WM-COMPOSITOR] Synced geometry for window %u: (%d,%d) %ux%u\n", ...);
```

### Monitor IPC Traffic

```bash
# WM logs
tail -f /var/log/services/window-manager.log | grep "WM-COMPOSITOR"

# Compositor logs
tail -f /var/log/services/compositor.log | grep "IPC"
```

### Common Issues

1. **Windows not appearing**
   - Check compositor is running
   - Verify IPC connection established
   - Check `wm_map_window()` called

2. **Decorations not rendering**
   - Verify `compositor_render_decorations()` called
   - Check window type is `WINDOW_NORMAL`
   - Ensure decorations enabled in WM config

3. **Focus not working**
   - Verify `compositor_set_focus()` called
   - Check input events reaching WM
   - Ensure focused window is mapped

---

## API Checklist

Before releasing, verify:

- ✅ All integration functions implemented
- ✅ IPC messages defined and documented
- ✅ Synchronization working correctly
- ✅ Error handling in place
- ✅ Performance targets met
- ✅ Test suite passing
- ✅ Documentation complete

---

*For implementation details, see `userspace/wm/integration.c`*
*For IPC protocol, see `userspace/wm/ipc.c`*
*For test cases, see `userspace/wm/test_wm_integration.c`*
