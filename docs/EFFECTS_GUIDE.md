# AutomationOS Effects Guide

## Overview

The AutomationOS compositor includes a powerful effects engine that provides beautiful visual enhancements with minimal performance impact. All effects are GPU-accelerated and can be configured or disabled via `compositor.conf`.

## Available Effects

### 1. Drop Shadows

Soft shadows behind windows for depth perception.

**Configuration:**

```ini
[effects]
shadows = true
shadow_offset_x = 0
shadow_offset_y = 4
shadow_blur_radius = 12
shadow_opacity = 0.6
```

**Parameters:**

```c
typedef struct {
    uint32_t color;         // RGBA color (default: 0x80000000)
    int32_t offset_x;       // Horizontal offset in pixels
    int32_t offset_y;       // Vertical offset in pixels
    float blur_radius;      // Blur kernel radius
    float opacity;          // Shadow opacity (0.0 - 1.0)
} shadow_params_t;
```

**Implementation:**

Shadows use a two-pass Gaussian blur:

```glsl
// Shadow Fragment Shader
varying vec2 v_texcoord;
uniform sampler2D texture;
uniform vec4 shadow_color;
uniform float blur_radius;

void main() {
    vec4 color = texture2D(texture, v_texcoord);
    gl_FragColor = shadow_color * color.a;
}
```

**Performance:**
- GPU: ~1-2% overhead per window
- Memory: ~2MB per 1920x1080 shadow texture

**Use Cases:**
- Normal windows (depth perception)
- Floating dialogs
- Menus and popups

### 2. Background Blur

Real-time Gaussian blur for transparent windows.

**Configuration:**

```ini
[effects]
blur = true
blur_radius = 8.0
blur_passes = 2
```

**Parameters:**

```c
typedef struct {
    float radius;       // Blur kernel radius
    int passes;         // Number of blur iterations
} blur_params_t;
```

**Algorithm:**

Two-pass separable Gaussian blur:

1. **Horizontal Pass:**
   ```glsl
   uniform vec2 direction = vec2(1.0, 0.0);
   ```

2. **Vertical Pass:**
   ```glsl
   uniform vec2 direction = vec2(0.0, 1.0);
   ```

**Gaussian Kernel:**

```c
void effect_gaussian_kernel(float *kernel, int size, float sigma) {
    float sum = 0.0f;
    int half = size / 2;
    
    for (int i = 0; i < size; i++) {
        int x = i - half;
        float value = expf(-(x * x) / (2.0f * sigma * sigma));
        kernel[i] = value;
        sum += value;
    }
    
    // Normalize
    for (int i = 0; i < size; i++) {
        kernel[i] /= sum;
    }
}
```

**Shader Implementation:**

```glsl
// Blur Fragment Shader
varying vec2 v_texcoord;
uniform sampler2D texture;
uniform vec2 direction;
uniform float blur_radius;

void main() {
    vec4 color = vec4(0.0);
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 
                               0.054054, 0.016216);
    vec2 tex_offset = 1.0 / textureSize(texture, 0) * direction * blur_radius;
    
    // Center sample
    color += texture2D(texture, v_texcoord) * weights[0];
    
    // Surrounding samples
    for(int i = 1; i < 5; i++) {
        color += texture2D(texture, v_texcoord + tex_offset * float(i)) * weights[i];
        color += texture2D(texture, v_texcoord - tex_offset * float(i)) * weights[i];
    }
    
    gl_FragColor = color;
}
```

**Performance:**
- GPU: ~3-5% overhead per blurred window
- Two render passes per window
- Best for transparent dialogs and overlays

**Use Cases:**
- Modal dialogs (blur background)
- Dropdown menus
- Notification popups
- Lock screen

### 3. Dim Inactive Windows

Subtle dimming to highlight focused window.

**Configuration:**

```ini
[effects]
dim_inactive = true
dim_factor = 0.15
```

**Implementation:**

```c
void effect_apply_dim(gpu_context_t *gpu, const rect_t *rect, float factor) {
    // Draw semi-transparent black overlay
    uint32_t dim_color = (uint32_t)(factor * 255.0f) << 24;
    gpu_draw_rect(gpu, rect, dim_color);
}
```

**Shader:**

```glsl
uniform vec4 dim_color = vec4(0.0, 0.0, 0.0, 0.15);

void main() {
    vec4 color = texture2D(texture, v_texcoord);
    gl_FragColor = mix(color, dim_color, dim_color.a);
}
```

**Performance:**
- Negligible (<0.5% CPU)
- Simple alpha blending

**Use Cases:**
- Focus indication
- Reduce visual clutter
- Improve multitasking awareness

### 4. Wobbly Windows (Optional)

Physics-based spring animation on window move/resize.

**Configuration:**

```ini
[effects]
wobbly = false  # Disabled by default (novelty effect)
```

**Physics Model:**

Mass-spring-damper system:

```c
typedef struct {
    float mass;         // Vertex mass
    float spring_k;     // Spring constant
    float damping;      // Damping factor
    vec2 position;
    vec2 velocity;
    vec2 target;
} vertex_t;

void update_vertex(vertex_t *v, float dt) {
    // Spring force: F = -k * (x - x0)
    vec2 spring_force = scale(sub(v->target, v->position), v->spring_k);
    
    // Damping force: F = -c * v
    vec2 damping_force = scale(v->velocity, -v->damping);
    
    // F = ma
    vec2 acceleration = scale(add(spring_force, damping_force), 1.0f / v->mass);
    
    // Integrate
    v->velocity = add(v->velocity, scale(acceleration, dt));
    v->position = add(v->position, scale(v->velocity, dt));
}
```

**Implementation:**

```c
void effect_wobbly_update(window_t *window, float delta_time) {
    // Update each vertex based on spring forces
    for (int i = 0; i < VERTEX_COUNT; i++) {
        update_vertex(&window->vertices[i], delta_time);
    }
    
    // Rebuild window geometry from vertex positions
    update_window_mesh(window);
}
```

**Performance:**
- CPU: ~2-3% during wobble
- Simulation runs at 60 FPS
- Effect dampens over 0.5-1 second

## Animation System

### Easing Functions

All animations use configurable easing functions for natural motion.

**Available Easings:**

```c
typedef enum {
    EASING_LINEAR,              // Constant speed
    EASING_EASE_IN,             // Slow start
    EASING_EASE_OUT,            // Slow end
    EASING_EASE_IN_OUT,         // Slow start and end
    EASING_EASE_IN_QUAD,        // Quadratic ease in
    EASING_EASE_OUT_QUAD,       // Quadratic ease out
    EASING_EASE_IN_OUT_QUAD,    // Quadratic ease in-out
    EASING_EASE_IN_CUBIC,       // Cubic ease in
    EASING_EASE_OUT_CUBIC,      // Cubic ease out
    EASING_EASE_IN_OUT_CUBIC,   // Cubic ease in-out
    EASING_BOUNCE,              // Bouncy effect
    EASING_ELASTIC,             // Elastic overshoot
} easing_t;
```

**Mathematical Definitions:**

```c
float easing_apply(easing_t easing, float t) {
    t = clamp01(t);
    
    switch (easing) {
        case EASING_LINEAR:
            return t;
            
        case EASING_EASE_IN_QUAD:
            return t * t;
            
        case EASING_EASE_OUT_QUAD:
            return 1.0f - (1.0f - t) * (1.0f - t);
            
        case EASING_EASE_IN_OUT_CUBIC: {
            float t2 = t * 2.0f;
            if (t2 < 1.0f) return 0.5f * t2 * t2 * t2;
            t2 -= 2.0f;
            return 0.5f * (t2 * t2 * t2 + 2.0f);
        }
        
        case EASING_BOUNCE: {
            const float n1 = 7.5625f;
            const float d1 = 2.75f;
            if (t < 1.0f / d1) {
                return n1 * t * t;
            } else if (t < 2.0f / d1) {
                t -= 1.5f / d1;
                return n1 * t * t + 0.75f;
            } else if (t < 2.5f / d1) {
                t -= 2.25f / d1;
                return n1 * t * t + 0.9375f;
            } else {
                t -= 2.625f / d1;
                return n1 * t * t + 0.984375f;
            }
        }
        
        case EASING_ELASTIC: {
            const float c4 = (2.0f * M_PI) / 3.0f;
            if (t == 0.0f) return 0.0f;
            if (t == 1.0f) return 1.0f;
            return -powf(2.0f, 10.0f * t - 10.0f) * 
                    sinf((t * 10.0f - 10.75f) * c4);
        }
    }
}
```

**Visual Comparison:**

```
LINEAR:         ──────────────────────▶
EASE_IN_QUAD:   ───────────────────────▶
EASE_OUT_QUAD:  ─▶─────────────────────
EASE_IN_OUT:    ───────▶───────────────
BOUNCE:         ───▲─▼─▲─▼─────────────
ELASTIC:        ─────▲───▼─▲────────────
```

### Preset Animations

#### Window Open

```c
animation_t *animation_window_open(void) {
    // Scale from 0.8 to 1.0 + fade in
    animation_t *anim = animation_create(ANIM_SCALE, 200000, 
                                        EASING_EASE_OUT_CUBIC);
    return anim;
}
```

**Duration:** 200ms  
**Easing:** Ease Out Cubic  
**Effect:** Window scales from 80% to 100% while fading in

#### Window Close

```c
animation_t *animation_window_close(void) {
    // Scale to 0.8 + fade out
    animation_t *anim = animation_create(ANIM_SCALE, 200000, 
                                        EASING_EASE_IN_CUBIC);
    return anim;
}
```

**Duration:** 200ms  
**Easing:** Ease In Cubic  
**Effect:** Window scales to 80% while fading out

#### Minimize

```c
animation_t *animation_minimize(void) {
    // Slide to taskbar
    animation_t *anim = animation_create(ANIM_SLIDE, 300000, 
                                        EASING_EASE_IN_OUT_QUAD);
    return anim;
}
```

**Duration:** 300ms  
**Easing:** Ease In-Out Quad  
**Effect:** Window slides and scales to taskbar position

#### Maximize

```c
animation_t *animation_maximize(void) {
    // Expand to fullscreen
    animation_t *anim = animation_create(ANIM_SCALE, 250000, 
                                        EASING_EASE_OUT_QUAD);
    return anim;
}
```

**Duration:** 250ms  
**Easing:** Ease Out Quad  
**Effect:** Window expands to fill screen

#### Workspace Switch

```c
animation_t *animation_workspace_switch(void) {
    // Slide transition
    animation_t *anim = animation_create(ANIM_SLIDE, 350000, 
                                        EASING_EASE_IN_OUT_CUBIC);
    return anim;
}
```

**Duration:** 350ms  
**Easing:** Ease In-Out Cubic  
**Effect:** Windows slide horizontally between workspaces

## Creating Custom Animations

### Basic Animation

```c
// Create animation
animation_t *anim = animation_create(ANIM_FADE, 500000, EASING_EASE_IN_OUT);

// Start animation (from 0% to 100%)
animation_start(anim, 0.0f, 1.0f);

// Update each frame
while (!animation_is_finished(anim)) {
    animation_update(anim);
    
    // Use current value
    float alpha = anim->current;
    
    // Apply to window
    window->alpha = alpha;
    
    // Render frame
    compositor_frame(comp);
}

// Cleanup
animation_destroy(anim);
```

### Chained Animations

```c
// Animation 1: Fade in
animation_t *fade = animation_create(ANIM_FADE, 200000, EASING_LINEAR);
fade->on_complete = start_scale_animation;
fade->user_data = window;
animation_start(fade, 0.0f, 1.0f);

void start_scale_animation(void *user_data) {
    window_t *window = user_data;
    
    // Animation 2: Scale up
    animation_t *scale = animation_create(ANIM_SCALE, 300000, EASING_BOUNCE);
    animation_start(scale, 0.5f, 1.0f);
    window->animation = scale;
}
```

### Looping Animation

```c
animation_t *anim = animation_create(ANIM_FADE, 1000000, EASING_LINEAR);

while (running) {
    animation_update(anim);
    
    if (animation_is_finished(anim)) {
        // Restart in reverse
        animation_start(anim, anim->to, anim->from);
    }
    
    // Use animation value
    apply_effect(anim->current);
}
```

## Performance Tuning

### Effect Performance Impact

| Effect | GPU Usage | CPU Usage | Memory | Recommended |
|--------|-----------|-----------|--------|-------------|
| Shadows | 1-2% | <1% | 2MB/window | Yes |
| Blur | 3-5% | <1% | 4MB/window | Dialogs only |
| Dim | <1% | <1% | Negligible | Yes |
| Wobbly | 1-2% | 2-3% | Negligible | No (novelty) |
| Animations | <1% | <1% | Negligible | Yes |

### Optimization Tips

1. **Disable effects on low-end hardware:**
   ```ini
   [effects]
   shadows = false
   blur = false
   ```

2. **Reduce blur quality:**
   ```ini
   blur_radius = 4.0      # Lower = faster
   blur_passes = 1        # Fewer = faster
   ```

3. **Disable wobble:**
   ```ini
   wobbly = false
   ```

4. **Shorter animations:**
   ```ini
   open_duration = 100    # Faster animations
   close_duration = 100
   ```

## Troubleshooting

### Effects not working

1. **Check GPU support:**
   ```bash
   glxinfo | grep "OpenGL"
   ```

2. **Verify configuration:**
   ```bash
   cat /etc/compositor.conf
   ```

3. **Check logs:**
   ```bash
   journalctl -u compositor
   ```

### Poor performance

1. **Monitor FPS:**
   ```c
   uint32_t fps = compositor_get_fps(comp);
   printf("FPS: %u\n", fps);
   ```

2. **Profile GPU usage:**
   ```bash
   nvidia-smi  # NVIDIA
   intel_gpu_top  # Intel
   ```

3. **Disable effects incrementally**

### Visual glitches

1. **Check VSync:**
   ```ini
   [compositor]
   vsync = true
   ```

2. **Verify triple buffering:**
   ```ini
   [performance]
   triple_buffering = true
   ```

3. **Update GPU drivers**

## See Also

- [COMPOSITOR_ARCHITECTURE.md](COMPOSITOR_ARCHITECTURE.md) - Compositor design
- [WINDOW_MANAGER_API.md](WINDOW_MANAGER_API.md) - Window management
- [compositor.conf](../userspace/compositor/compositor.conf) - Full configuration reference
