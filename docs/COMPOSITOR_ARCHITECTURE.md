# AutomationOS Compositor Architecture

## Overview

The AutomationOS compositor is a modern, hardware-accelerated desktop compositing system designed for smooth, tear-free rendering with beautiful visual effects. It achieves 60+ FPS performance with minimal CPU usage through GPU acceleration and intelligent damage tracking.

## Architecture

### High-Level Stack

```
┌─────────────────────────────────────┐
│       Applications                   │
│  (draw to window surfaces)          │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│      Window Manager                  │
│  - Window placement & focus         │
│  - Decorations & interactions       │
│  - Workspace management             │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│        Compositor Core               │
│  - Frame rendering loop             │
│  - Window texture management        │
│  - Damage tracking                  │
│  - Triple buffering                 │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│      GPU Backend (OpenGL/DRM)       │
│  - Hardware-accelerated rendering   │
│  - Texture upload/update            │
│  - VSync synchronization            │
└─────────────┬───────────────────────┘
              │
┌─────────────▼───────────────────────┐
│    Graphics Driver (i915/GPU)       │
└─────────────────────────────────────┘
```

## Core Components

### 1. Compositor Core (`compositor.c`)

**Responsibilities:**
- Main rendering loop (60+ FPS)
- Window texture management
- Damage tracking for efficient redraws
- Triple buffering for smooth frame pacing
- Multi-monitor support
- Effects pipeline coordination

**Key Functions:**

```c
compositor_t *compositor_init(const char *gpu_device);
void compositor_frame(compositor_t *comp);  // Called once per frame
void compositor_add_window(compositor_t *comp, window_t *window);
void compositor_add_damage(compositor_t *comp, const rect_t *rect);
```

**Frame Rendering Pipeline:**

```
1. Collect Damage Regions
   └─> Which areas of the screen need redrawing?
   
2. Update Window Textures
   └─> Upload dirty window surfaces to GPU
   
3. Composite Windows
   └─> Blend all windows to framebuffer (GPU)
   
4. Apply Effects
   └─> Shadows, blur, animations (GPU shaders)
   
5. Present with VSync
   └─> Swap buffers, tear-free display
   
6. Track Performance
   └─> FPS monitoring, frame time logging
```

### 2. GPU Backend (`gpu.c`, `gpu.h`)

**Backend Support:**
- **OpenGL ES 2.0** - Primary backend, maximum compatibility
- **Vulkan** - Future support for maximum performance
- **DRM/KMS** - Direct rendering for embedded systems

**GPU Context:**

```c
typedef struct gpu_context {
    gpu_backend_t backend;      // OpenGL, Vulkan, or DRM
    void *backend_data;          // Backend-specific data
    uint32_t width, height;
    bool initialized;
} gpu_context_t;
```

**Key Operations:**

```c
// Initialization
gpu_context_t *gpu_init(const char *device);

// Texture management
texture_t *gpu_upload_texture(gpu_context_t *gpu, 
                              const uint32_t *pixels, 
                              uint32_t width, uint32_t height);
void gpu_update_texture(gpu_context_t *gpu, texture_t *texture, ...);

// Rendering
void gpu_begin_frame(gpu_context_t *gpu, framebuffer_t *fb);
void gpu_draw_textured_quad(gpu_context_t *gpu, texture_t *texture, 
                           const rect_t *src, const rect_t *dst, 
                           float alpha);
void gpu_end_frame(gpu_context_t *gpu);
void gpu_present(gpu_context_t *gpu, framebuffer_t *fb, bool vsync);
```

**OpenGL Shader Pipeline:**

```glsl
// Vertex Shader
attribute vec2 position;
attribute vec2 texcoord;
varying vec2 v_texcoord;
uniform mat4 projection;

void main() {
    gl_Position = projection * vec4(position, 0.0, 1.0);
    v_texcoord = texcoord;
}

// Fragment Shader (with effects)
varying vec2 v_texcoord;
uniform sampler2D texture;
uniform float alpha;
uniform vec4 tint_color;

void main() {
    vec4 color = texture2D(texture, v_texcoord);
    color.a *= alpha;
    gl_FragColor = color * tint_color;
}
```

### 3. Animation System (`animations.c`, `animations.h`)

**Animation Types:**
- `ANIM_FADE` - Opacity transitions (window open/close)
- `ANIM_SLIDE` - Position transitions (minimize, workspace switch)
- `ANIM_SCALE` - Size transitions (maximize)
- `ANIM_BLUR` - Blur amount transitions
- `ANIM_WOBBLE` - Physics-based wobble effect

**Easing Functions:**

```c
typedef enum {
    EASING_LINEAR,
    EASING_EASE_IN,
    EASING_EASE_OUT,
    EASING_EASE_IN_OUT,
    EASING_EASE_IN_QUAD,
    EASING_EASE_OUT_QUAD,
    EASING_EASE_IN_OUT_QUAD,
    EASING_EASE_IN_CUBIC,
    EASING_EASE_OUT_CUBIC,
    EASING_EASE_IN_OUT_CUBIC,
    EASING_BOUNCE,          // Bouncy spring effect
    EASING_ELASTIC,         // Elastic overshoot
} easing_t;
```

**Animation Lifecycle:**

```c
// Create animation
animation_t *anim = animation_create(ANIM_FADE, 200000, EASING_EASE_OUT);

// Start animation
animation_start(anim, 0.0f, 1.0f);  // from 0% to 100%

// Update each frame
animation_update(anim);  // Updates current value based on time

// Check completion
if (animation_is_finished(anim)) {
    // Animation complete
}
```

**Preset Animations:**

```c
animation_t *animation_window_open(void);     // 200ms, scale + fade
animation_t *animation_window_close(void);    // 200ms, scale + fade
animation_t *animation_minimize(void);        // 300ms, slide
animation_t *animation_maximize(void);        // 250ms, scale
animation_t *animation_workspace_switch(void); // 350ms, slide
```

### 4. Effects Engine (`effects.c`, `effects.h`)

**Visual Effects:**

1. **Drop Shadows**
   - Soft shadows behind windows
   - GPU-accelerated Gaussian blur
   - Configurable offset, radius, opacity

2. **Background Blur**
   - Real-time blur for transparent windows
   - Two-pass Gaussian blur (horizontal + vertical)
   - Efficient shader implementation

3. **Dim Inactive Windows**
   - Subtle dimming of unfocused windows
   - Helps with focus awareness
   - Configurable dim factor

4. **Wobbly Windows** (optional)
   - Physics-based spring simulation
   - Wobble on move/resize
   - Mass-spring-damper system

**Effect Parameters:**

```c
typedef struct {
    bool shadows_enabled;
    bool blur_enabled;
    bool dim_inactive_enabled;
    bool wobbly_enabled;
    
    shadow_params_t shadow;
    blur_params_t blur;
    float dim_factor;
} effect_settings_t;
```

**Shader-Based Effects:**

```glsl
// Gaussian Blur Shader
uniform sampler2D texture;
uniform vec2 direction;  // (1,0) for horizontal, (0,1) for vertical
uniform float blur_radius;

void main() {
    vec4 color = vec4(0.0);
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 
                               0.054054, 0.016216);
    vec2 tex_offset = 1.0 / textureSize(texture, 0) * direction * blur_radius;
    
    color += texture2D(texture, v_texcoord) * weights[0];
    for(int i = 1; i < 5; i++) {
        color += texture2D(texture, v_texcoord + tex_offset * float(i)) * weights[i];
        color += texture2D(texture, v_texcoord - tex_offset * float(i)) * weights[i];
    }
    
    gl_FragColor = color;
}
```

### 5. Window Manager (`window_manager.c`, `window_manager.h`)

**Responsibilities:**
- Window creation, placement, and destruction
- Focus management
- Window decorations (title bar, buttons, borders)
- Workspace management (virtual desktops)
- Tiling window management
- Input handling (mouse, keyboard)

**Window Types:**

```c
typedef enum {
    WINDOW_NORMAL,      // Regular app window (with decorations)
    WINDOW_DIALOG,      // Modal dialog
    WINDOW_UTILITY,     // Floating utility (always on top)
    WINDOW_TOOLBAR,     // Toolbar/panel
    WINDOW_MENU,        // Popup menu
    WINDOW_SPLASH,      // Splash screen (no decorations)
    WINDOW_DESKTOP,     // Desktop background
    WINDOW_DOCK,        // System dock/taskbar
} window_type_t;
```

**Placement Modes:**

```c
typedef enum {
    PLACEMENT_FLOATING,     // Free-form positioning
    PLACEMENT_TILING,       // Automatic tiling
    PLACEMENT_MAXIMIZED,    // Maximized
    PLACEMENT_FULLSCREEN,   // Fullscreen
} placement_mode_t;
```

**Tiling Modes:**

```
TILING_HORIZONTAL      TILING_VERTICAL       TILING_GRID
┌─────┬─────┬─────┐   ┌────────────────┐   ┌──────┬──────┐
│  1  │  2  │  3  │   │       1        │   │  1   │  2   │
│     │     │     │   ├────────────────┤   ├──────┼──────┤
│     │     │     │   │       2        │   │  3   │  4   │
│     │     │     │   ├────────────────┤   └──────┴──────┘
│     │     │     │   │       3        │
└─────┴─────┴─────┘   └────────────────┘
```

## Performance Optimizations

### 1. Damage Tracking

Only redraw regions that have changed:

```c
typedef struct {
    rect_t damage_regions[MAX_DAMAGE_REGIONS];
    uint32_t damage_region_count;
    bool full_redraw;
} compositor_t;

// Add damage when window changes
compositor_add_damage(comp, &window->geometry);

// Skip rendering unchanged windows
if (!is_region_damaged(comp, &window->geometry)) {
    continue;  // No redraw needed
}
```

**Benefits:**
- Reduces GPU load by 70-90% when idle
- Lower power consumption
- Smoother performance on low-end hardware

### 2. Triple Buffering

Eliminates tearing and stuttering:

```
┌──────────┐     ┌──────────┐     ┌──────────┐
│  Buffer  │────▶│  Buffer  │────▶│  Buffer  │
│    0     │     │    1     │     │    2     │
│ (render) │     │ (ready)  │     │(display) │
└──────────┘     └──────────┘     └──────────┘

Frame N: Render to buffer 0, display buffer 2
Frame N+1: Render to buffer 1, display buffer 0
Frame N+2: Render to buffer 2, display buffer 1
```

**Benefits:**
- No tearing artifacts
- Consistent frame pacing
- Smooth 60 FPS rendering

### 3. VSync Synchronization

Synchronize with display refresh rate:

```c
void gpu_present(gpu_context_t *gpu, framebuffer_t *fb, bool vsync) {
    if (vsync) {
        eglSwapInterval(display, 1);  // Wait for VBlank
    }
    eglSwapBuffers(display, surface);
}
```

**Benefits:**
- Tear-free rendering
- Smooth motion
- No screen tearing during fast motion

### 4. Texture Caching

Avoid redundant GPU uploads:

```c
// Only update if surface is dirty
if (window->surface && window->surface->dirty && window->texture) {
    gpu_update_texture(gpu, window->texture, 
                      window->surface->pixels,
                      window->surface->width,
                      window->surface->height);
    window->surface->dirty = false;
}
```

## Multi-Monitor Support

### Display Management

```c
typedef struct display {
    uint32_t id;
    int32_t x, y;            // Position in desktop space
    uint32_t width, height;
    uint32_t refresh_rate;
    char name[64];
    bool primary;
} display_t;
```

### Arrangement Modes

**Side-by-Side:**
```
┌────────────┐┌────────────┐
│  Display 1 ││  Display 2 │
│  1920x1080 ││  1920x1080 │
└────────────┘└────────────┘
```

**Stacked:**
```
┌────────────┐
│  Display 1 │
│  1920x1080 │
├────────────┤
│  Display 2 │
│  1920x1080 │
└────────────┘
```

## Configuration

All settings are configurable via `compositor.conf`:

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
wobbly = false

[animations]
open_duration = 200
close_duration = 200
default_easing = ease-in-out-cubic
```

## Performance Metrics

### Targets

- **Frame Rate:** 60+ FPS sustained
- **Frame Time:** < 16.67ms (1000ms / 60)
- **Input Latency:** < 10ms (mouse click to screen update)
- **CPU Usage (Idle):** < 1%
- **CPU Usage (Active):** < 5%
- **Memory Overhead:** < 50MB per compositor instance

### Monitoring

```c
uint32_t fps = compositor_get_fps(comp);
printf("FPS: %u\n", fps);
```

## Future Enhancements

1. **Vulkan Backend** - Lower overhead, better multi-threading
2. **HDR Support** - High dynamic range rendering
3. **Variable Refresh Rate** - FreeSync/G-Sync support
4. **Screen Recording** - Built-in capture pipeline
5. **Remote Display** - Network transparency (RDP/VNC protocol)
6. **Touchscreen Gestures** - Multi-touch support
7. **Fractional Scaling** - HiDPI display support

## References

- [Wayland Protocol](https://wayland.freedesktop.org/)
- [Weston Compositor](https://gitlab.freedesktop.org/wayland/weston)
- [Mutter (GNOME)](https://gitlab.gnome.org/GNOME/mutter)
- [KWin (KDE)](https://invent.kde.org/plasma/kwin)
- [OpenGL ES 2.0 Specification](https://www.khronos.org/opengles/)
