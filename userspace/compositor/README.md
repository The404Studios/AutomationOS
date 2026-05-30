# AutomationOS Compositor

Modern, GPU-accelerated desktop compositor with beautiful animations and effects.

## Features

✨ **60+ FPS Performance**
- Hardware-accelerated rendering (OpenGL ES 2.0)
- VSync synchronization for tear-free display
- Triple buffering for smooth frame pacing
- Damage tracking for efficient redraws

🎨 **Beautiful Effects**
- Drop shadows with Gaussian blur
- Background blur for transparent windows
- Dim unfocused windows
- Smooth animations with 12 easing functions

🪟 **Advanced Window Management**
- Floating and tiling layouts
- Multi-workspace support (virtual desktops)
- Window decorations with macOS-inspired design
- Drag-to-move and resize
- Minimize, maximize, fullscreen

🖥️ **Multi-Monitor Support**
- Automatic display detection
- Side-by-side and stacked arrangements
- Per-monitor workspaces
- Hot-plug support

## Quick Start

### Build

```bash
cd userspace/compositor
make
```

### Run Demo

```bash
# Simple window demo
./demo_simple_window

# Animation showcase
./demo_animations
```

### Configuration

Edit `compositor.conf`:

```ini
[compositor]
vsync = true
fps_target = 60
effects = true
animations = true

[effects]
shadows = true
blur = true
dim_inactive = true

[animations]
open_duration = 200
close_duration = 200
default_easing = ease-in-out-cubic
```

## Architecture

```
┌─────────────────────────┐
│     Applications        │  Draw to window surfaces
└───────────┬─────────────┘
            │
┌───────────▼─────────────┐
│    Window Manager       │  Placement, focus, decorations
└───────────┬─────────────┘
            │
┌───────────▼─────────────┐
│   Compositor Core       │  Frame rendering, triple buffering
└───────────┬─────────────┘
            │
┌───────────▼─────────────┐
│   GPU Backend (OpenGL)  │  Hardware-accelerated rendering
└───────────┬─────────────┘
            │
┌───────────▼─────────────┐
│  Graphics Driver (i915) │
└─────────────────────────┘
```

## API Example

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
    
    // Create window
    window_t *window = wm_create_window(wm, WINDOW_NORMAL, 
                                       800, 600, 
                                       "Hello World");
    
    // Center and show
    wm_center_window(wm, window);
    wm_map_window(wm, window);
    
    // Main loop
    while (running) {
        wm_update(wm);
        compositor_frame(comp);  // Renders at 60 FPS
    }
    
    // Cleanup
    wm_cleanup(wm);
    compositor_cleanup(comp);
    
    return 0;
}
```

## File Structure

```
compositor/
├── compositor.c            # Core compositor (1,500+ LOC)
├── compositor.h            # Public API
├── gpu.c                   # GPU abstraction (1,200+ LOC)
├── gpu.h                   # GPU interface
├── animations.c            # Animation system (800+ LOC)
├── animations.h
├── effects.c               # Visual effects (600+ LOC)
├── effects.h
├── utils.c                 # Utilities (500+ LOC)
├── compositor.conf         # Configuration file
├── Makefile               # Build system
├── shaders/               # GLSL shaders
│   ├── compositor.vert    # Vertex shader
│   ├── compositor.frag    # Fragment shader
│   ├── blur.frag          # Gaussian blur
│   └── shadow.frag        # Drop shadow
├── demo_simple_window.c   # Basic demo
└── demo_animations.c      # Animation showcase

../wm/
├── window_manager.c       # Window manager (1,200+ LOC)
└── window_manager.h       # Window manager API
```

## Performance Targets

| Metric | Target | Typical |
|--------|--------|---------|
| Frame Rate | 60+ FPS | 60-120 FPS |
| Frame Time | < 16.67ms | 10-15ms |
| Input Latency | < 10ms | 5-8ms |
| CPU (Idle) | < 1% | 0.5% |
| CPU (Active) | < 5% | 2-3% |
| GPU Usage | 10-20% | 15% |
| Memory | < 50MB | 30MB |

## Effects

### Drop Shadows

Soft shadows behind windows for depth perception.

```c
shadow_params_t shadow = {
    .color = 0x80000000,        // Semi-transparent black
    .offset_x = 0,
    .offset_y = 4,              // 4px below window
    .blur_radius = 12.0f,       // Soft edge
    .opacity = 0.6f,            // 60% opacity
};
```

### Background Blur

Real-time Gaussian blur for transparent windows.

```c
blur_params_t blur = {
    .radius = 8.0f,             // Blur amount
    .passes = 2,                // Quality (1-3)
};
```

### Animations

Smooth transitions with 12 easing functions:

- **Linear** - Constant speed
- **Ease In/Out** - Slow start/end
- **Quadratic/Cubic** - Smooth acceleration
- **Bounce** - Bouncy spring effect
- **Elastic** - Elastic overshoot

```c
// Window open: scale from 0.8 to 1.0 + fade in
animation_t *anim = animation_window_open();  // 200ms, ease-out-cubic
```

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Alt + Tab` | Switch windows |
| `Alt + F4` | Close window |
| `Super + Up` | Maximize |
| `Super + Down` | Minimize |
| `F11` | Fullscreen |
| `Alt + Drag` | Move window |
| `Alt + Shift + Drag` | Resize window |
| `Super + 1-4` | Switch workspace |
| `Super + H/V/G` | Tiling mode |

## Documentation

- [COMPOSITOR_ARCHITECTURE.md](../../docs/COMPOSITOR_ARCHITECTURE.md) - Detailed architecture
- [WINDOW_MANAGER_API.md](../../docs/WINDOW_MANAGER_API.md) - Complete API reference
- [EFFECTS_GUIDE.md](../../docs/EFFECTS_GUIDE.md) - Effects and animations guide

## Requirements

### Runtime

- Linux kernel 4.0+
- OpenGL ES 2.0 or newer
- DRM/KMS support
- 512MB RAM minimum

### Build

- GCC 7.0+ or Clang 6.0+
- Make
- Optional: EGL, GLESv2, libdrm

## Troubleshooting

### Black screen on startup

Check GPU drivers:
```bash
glxinfo | grep "OpenGL"
```

### Low FPS

1. Enable VSync:
   ```ini
   vsync = true
   ```

2. Disable expensive effects:
   ```ini
   blur = false
   shadows = false
   ```

3. Check GPU load:
   ```bash
   intel_gpu_top  # Intel
   nvidia-smi     # NVIDIA
   ```

### Screen tearing

Enable VSync and triple buffering:
```ini
[compositor]
vsync = true

[performance]
triple_buffering = true
```

## Contributing

Contributions welcome! Areas of interest:

- Vulkan backend implementation
- Wayland protocol support
- Additional visual effects
- Performance optimizations
- Bug fixes and testing

## License

AutomationOS Compositor is part of AutomationOS.  
See main project README for license information.

## Credits

Inspired by:
- **Weston** (Wayland reference compositor)
- **Mutter** (GNOME compositor)
- **KWin** (KDE compositor)
- **Compiz** (Visual effects pioneer)
- **macOS Aqua** (UI inspiration)
