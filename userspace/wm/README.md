# AutomationOS Window Manager

Hardware-accelerated window manager with smooth animations and advanced window management features.

## Architecture

```
┌─────────────────┐
│  Applications   │
└────────┬────────┘
         │
┌────────▼────────┐
│ Window Manager  │  ◄── This component
│  - Placement    │
│  - Animations   │
│  - Workspaces   │
│  - Tiling       │
└────────┬────────┘
         │
┌────────▼────────┐
│   Compositor    │
│  - Rendering    │
│  - GPU Accel    │
│  - VSync        │
└─────────────────┘
```

## Features

### Window Management
- **Floating Windows**: Free-form positioning and resizing
- **Tiling Modes**: Horizontal, vertical, grid, and master-stack layouts
- **Workspaces**: Multiple virtual desktops (4 by default)
- **Window Rules**: Per-application placement and behavior

### Animations
All window operations are smoothly animated:
- **Window Open**: Fade in + scale from 0.8x to 1.0x (300ms)
- **Window Close**: Fade out + scale to 0.8x (300ms)
- **Minimize**: Scale down to dock position
- **Maximize**: Expand to fullscreen with easing
- **Workspace Switch**: Slide transition between workspaces

### Easing Functions
12 easing functions available:
- Linear
- Ease In/Out/InOut
- Quad In/Out/InOut
- Cubic In/Out/InOut
- Bounce
- Elastic

## Service Configuration

Location: `/etc/services/window-manager.service`

```ini
[Service]
Description=Window Manager with Animations
Type=simple
ExecStart=/usr/bin/window-manager

Requires=compositor
After=compositor

Restart=always
RestartDelay=2

CPUQuota=50
MemoryLimit=256M
```

## Building

```bash
cd userspace/wm
make clean
make
sudo make install
```

## Running

The window manager is started automatically by the service manager:

```bash
# Start window manager
servicectl start window-manager

# Check status
servicectl status window-manager

# View logs
tail -f /var/log/services/window-manager.log
```

## IPC Protocol

The window manager communicates with the compositor via Unix socket at `/run/compositor.sock`.

### Message Format

```c
struct wm_message {
    uint32_t type;      // Message type
    uint32_t length;    // Payload length
    uint8_t data[];     // Payload
};
```

### Message Types

| Type | Name | Direction | Description |
|------|------|-----------|-------------|
| 0x01 | WINDOW_CREATE | WM → Comp | Create new window |
| 0x02 | WINDOW_DESTROY | WM → Comp | Destroy window |
| 0x03 | WINDOW_MAP | WM → Comp | Show window |
| 0x04 | WINDOW_UNMAP | WM → Comp | Hide window |
| 0x05 | WINDOW_MOVE | WM → Comp | Update position |
| 0x06 | WINDOW_RESIZE | WM → Comp | Update size |
| 0x10 | INPUT_EVENT | Comp → WM | Keyboard/mouse input |
| 0x11 | DAMAGE_NOTIFY | Comp → WM | Window needs redraw |

## Window Operations

### Creating a Window

```c
window_t *win = wm_create_window(wm, 
    WINDOW_NORMAL,     // Type
    800, 600,          // Size
    "My Application"   // Title
);
wm_map_window(wm, win);
```

### Moving a Window

```c
wm_move_window(wm, win, 100, 100);
```

### Animating a Window

```c
// Close with animation
window->animation = animation_window_close();
animation_start(window->animation, 1.0f, 0.0f);
```

## Window Types

- `WINDOW_NORMAL`: Regular application window (with decorations)
- `WINDOW_DIALOG`: Modal dialog
- `WINDOW_UTILITY`: Floating utility window
- `WINDOW_TOOLBAR`: Toolbar/panel
- `WINDOW_MENU`: Popup menu
- `WINDOW_SPLASH`: Splash screen
- `WINDOW_DESKTOP`: Desktop background
- `WINDOW_DOCK`: System dock/taskbar

## Tiling Modes

### Horizontal Tiling
```
┌────────┬────────┬────────┐
│Window 1│Window 2│Window 3│
│        │        │        │
└────────┴────────┴────────┘
```

### Vertical Tiling
```
┌────────────────────────┐
│      Window 1          │
├────────────────────────┤
│      Window 2          │
├────────────────────────┤
│      Window 3          │
└────────────────────────┘
```

### Grid Tiling
```
┌────────────┬────────────┐
│  Window 1  │  Window 2  │
├────────────┼────────────┤
│  Window 3  │  Window 4  │
└────────────┴────────────┘
```

## Window Rules

Define per-application behavior:

```c
window_rule_t rule = {
    .app_name = "terminal",
    .type = WINDOW_NORMAL,
    .placement = PLACEMENT_FLOATING,
    .decorations = true,
    .workspace = 1  // Start on workspace 1
};
wm_add_rule(wm, &rule);
```

## Performance

- **Target**: 60 FPS
- **Frame Time**: 16.67ms
- **Memory**: ~50MB base + ~5MB per window
- **CPU Usage**: 5-15% during animations, <1% idle

## Keyboard Shortcuts (Planned)

| Shortcut | Action |
|----------|--------|
| Super+Enter | Open terminal |
| Super+Q | Close window |
| Super+F | Toggle fullscreen |
| Super+M | Toggle maximize |
| Super+H | Minimize window |
| Super+1-4 | Switch workspace |
| Super+Shift+1-4 | Move window to workspace |
| Super+Tab | Next window |
| Super+Shift+Tab | Previous window |
| Super+T | Toggle tiling mode |

## Configuration

Location: `/etc/window-manager.conf` (future)

```ini
[Decorations]
Enabled=true
Height=32
BorderWidth=1

[Animations]
Enabled=true
Duration=300
Easing=cubic-out

[Tiling]
GapSize=8
DefaultMode=floating

[Workspaces]
Count=4
Names=Desktop,Development,Web,Communication
```

## Integration Points

### Compositor
- Receives window position updates
- Sends input events to WM
- Handles actual rendering

### Desktop Shell
- Creates desktop background window
- Provides dock/panel windows
- Manages system menu

### Applications
- Request window creation
- Update window surfaces
- Respond to close requests

## Code Statistics

- **window_manager.c**: 625 LOC (core logic)
- **window_manager.h**: 139 LOC (API definitions)
- **main.c**: 280 LOC (entry point and main loop)
- **Total**: ~1044 LOC

## Future Enhancements

- [ ] Multi-monitor support
- [ ] Window snapping to edges
- [ ] Window thumbnails for workspace switcher
- [ ] Picture-in-Picture mode
- [ ] Window grouping/tabbing
- [ ] Custom window shapes (non-rectangular)
- [ ] Window blur effects
- [ ] Smart window placement algorithms
- [ ] Touch gesture support
- [ ] Voice control integration

## Dependencies

- **Compositor**: Display rendering
- **Animation System**: Smooth transitions
- **Service Manager**: Process lifecycle
- **Input System**: Keyboard/mouse events

## Testing

```bash
# Run compositor
servicectl start compositor

# Run window manager
servicectl start window-manager

# Create test window
./demo_simple_window
```

## Debugging

Enable debug output:
```bash
export WM_DEBUG=1
window-manager
```

View real-time logs:
```bash
journalctl -u window-manager -f
```

## License

Part of AutomationOS - See main LICENSE file.
