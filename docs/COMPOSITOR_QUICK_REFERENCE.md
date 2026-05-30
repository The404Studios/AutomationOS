# Compositor Quick Reference

## One-Minute Overview

**What:** GPU-accelerated desktop compositor with beautiful animations  
**Performance:** 60+ FPS, < 1% CPU idle, tear-free rendering  
**Features:** Shadows, blur, animations, tiling, multi-workspace  
**Code:** 7,750 lines across 25 files  

## Essential API

### Initialize

```c
// 1. Create compositor
compositor_t *comp = compositor_init("/dev/dri/card0");

// 2. Add display
display_t *display = display_create(0, 1920, 1080, 60);
compositor_add_display(comp, display);

// 3. Create window manager
window_manager_t *wm = wm_init(comp);
```

### Create Window

```c
window_t *window = wm_create_window(wm, WINDOW_NORMAL, 800, 600, "My App");
wm_center_window(wm, window);
wm_map_window(wm, window);
```

### Main Loop

```c
while (running) {
    wm_update(wm);          // Update window manager
    compositor_frame(comp); // Render at 60 FPS
}
```

### Cleanup

```c
wm_cleanup(wm);
compositor_cleanup(comp);
```

## Window Operations

| Function | Description |
|----------|-------------|
| `wm_map_window()` | Show window |
| `wm_unmap_window()` | Hide window |
| `wm_focus_window()` | Give keyboard focus |
| `wm_raise_window()` | Bring to front |
| `wm_minimize_window()` | Minimize to taskbar |
| `wm_maximize_window()` | Maximize to screen |
| `wm_fullscreen_window()` | Toggle fullscreen |
| `wm_close_window()` | Request close |
| `wm_destroy_window()` | Destroy window |

## Window Types

```c
WINDOW_NORMAL    // Regular app window (with decorations)
WINDOW_DIALOG    // Modal dialog
WINDOW_UTILITY   // Floating utility (always on top)
WINDOW_TOOLBAR   // Toolbar/panel
WINDOW_MENU      // Popup menu
WINDOW_SPLASH    // Splash screen (no decorations)
WINDOW_DESKTOP   // Desktop background
WINDOW_DOCK      // System dock/taskbar
```

## Animations

```c
// Create animation
animation_t *anim = animation_create(ANIM_FADE, 200000, EASING_EASE_IN_OUT);

// Start (from 0% to 100%)
animation_start(anim, 0.0f, 1.0f);

// Update each frame
animation_update(anim);

// Check completion
if (animation_is_finished(anim)) {
    // Done
}

// Cleanup
animation_destroy(anim);
```

### Presets

```c
animation_window_open()         // 200ms, scale + fade in
animation_window_close()        // 200ms, scale + fade out
animation_minimize()            // 300ms, slide to taskbar
animation_maximize()            // 250ms, expand to screen
animation_workspace_switch()    // 350ms, slide transition
```

### Easing Functions

```
LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT
EASE_IN_QUAD, EASE_OUT_QUAD, EASE_IN_OUT_QUAD
EASE_IN_CUBIC, EASE_OUT_CUBIC, EASE_IN_OUT_CUBIC
BOUNCE, ELASTIC
```

## Workspaces

```c
// Create workspace
workspace_t *ws = wm_create_workspace(wm, "Development");

// Switch workspace
wm_switch_workspace(wm, 1);

// Move window to workspace
wm_move_window_to_workspace(wm, window, 2);
```

## Tiling

```c
// Set tiling mode
wm_set_tiling_mode(wm, TILING_HORIZONTAL);

// Modes: TILING_NONE, TILING_HORIZONTAL, TILING_VERTICAL, 
//        TILING_GRID, TILING_MASTER_STACK

// Manually tile windows
wm_tile_windows(wm, workspace);
```

## Configuration

**File:** `compositor.conf`

### Enable/Disable Features

```ini
[compositor]
vsync = true
effects = true
animations = true

[effects]
shadows = true
blur = true
dim_inactive = true
wobbly = false

[animations]
open_duration = 200
close_duration = 200
default_easing = ease-in-out-cubic
```

### Performance Tuning

```ini
[performance]
damage_tracking = true
triple_buffering = true
idle_cpu_target = 1

[debug]
show_fps = true
```

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `Alt + Tab` | Switch windows |
| `Alt + F4` | Close window |
| `Super + ↑` | Maximize |
| `Super + ↓` | Minimize |
| `F11` | Fullscreen |
| `Alt + Drag` | Move window |
| `Alt + Shift + Drag` | Resize |
| `Super + 1-4` | Switch workspace |
| `Super + H/V/G` | Tiling mode |

## Performance Targets

| Metric | Target | Typical |
|--------|--------|---------|
| FPS | 60+ | 60-120 |
| Frame Time | < 16.67ms | 10-15ms |
| Input Latency | < 10ms | 5-8ms |
| CPU (Idle) | < 1% | 0.5% |
| CPU (Active) | < 5% | 2-3% |

## Effects

### Shadows

```c
shadow_params_t shadow = {
    .color = 0x80000000,    // Semi-transparent black
    .offset_x = 0,
    .offset_y = 4,          // 4px below
    .blur_radius = 12.0f,
    .opacity = 0.6f,
};
```

### Blur

```c
blur_params_t blur = {
    .radius = 8.0f,
    .passes = 2,
};
```

## File Structure

```
userspace/compositor/
├── compositor.c/h         # Core compositor (1,500 LOC)
├── gpu.c/h               # GPU backend (700 LOC)
├── animations.c/h        # Animations (500 LOC)
├── effects.c/h           # Effects (500 LOC)
├── utils.c               # Utilities (300 LOC)
├── shaders/              # GLSL shaders (120 LOC)
├── demo_*.c              # Demos (600 LOC)
├── Makefile              # Build system
└── compositor.conf       # Configuration

userspace/wm/
├── window_manager.c/h    # Window manager (1,400 LOC)
```

## Build

```bash
cd userspace/compositor
make                  # Build library and demos
make test            # Run tests
make install         # Install system-wide
```

## Common Patterns

### Window with Animation

```c
window_t *w = wm_create_window(wm, WINDOW_NORMAL, 800, 600, "App");
w->animation = animation_window_open();
animation_start(w->animation, 0.8f, 1.0f);
wm_map_window(wm, w);
```

### Centered Dialog

```c
window_t *dialog = wm_create_window(wm, WINDOW_DIALOG, 400, 300, "Alert");
wm_center_window(wm, dialog);
wm_map_window(wm, dialog);
```

### Tiled Workspace

```c
wm_set_tiling_mode(wm, TILING_HORIZONTAL);
// All windows now tile automatically
```

## Troubleshooting

### Black Screen
```bash
glxinfo | grep "OpenGL"  # Check GPU support
```

### Low FPS
```ini
[effects]
blur = false
shadows = false
```

### Screen Tearing
```ini
[compositor]
vsync = true
```

## Documentation

- **COMPOSITOR_ARCHITECTURE.md** - Detailed design
- **WINDOW_MANAGER_API.md** - Complete API reference
- **EFFECTS_GUIDE.md** - Effects and animations
- **README.md** - Getting started

## Support

- Check logs: `journalctl -u compositor`
- Enable debug: `debug = true` in compositor.conf
- Monitor FPS: `compositor_get_fps(comp)`

---

**Quick Start:** See `demo_simple_window.c` for a working example!
