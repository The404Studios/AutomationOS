# AutomationOS Animation Library

Advanced animation engine for AutomationOS with 30+ easing functions, spring physics, keyframe animation, and comprehensive UI widget animations.

## Features

- **High Performance**: 60 FPS rendering with GPU acceleration
- **30+ Easing Functions**: Quadratic, cubic, elastic, bounce, spring physics, custom bezier
- **Multiple Animation Types**: Value, color, transform, rectangle, keyframe
- **Spring Physics**: Natural motion with mass-spring-damper simulation
- **Animation Groups**: Synchronize multiple animations
- **Accessibility**: Reduce motion mode for users with motion sensitivity
- **Auto-Targeting**: Animations automatically update target variables
- **Callbacks**: on_start, on_update, on_complete, on_cancel
- **Repeating/Reversing**: Infinite loops, ping-pong animations
- **UI Presets**: Pre-built animations for buttons, menus, dialogs, etc.

## Quick Start

### Building

```bash
make                  # Build library
make example          # Build demo program
make install          # Install to system
```

### Basic Usage

```c
#include "animator.h"

// Create animator
animator_t *animator = animator_create();

// Create animation: fade from 0 to 1 over 200ms
float opacity = 0.0f;
animation_t *anim = anim_from_to(animator, 0.0f, 1.0f, 200000, EASE_OUT_CUBIC);
anim_set_target(anim, &opacity);  // Auto-update opacity
anim_start(anim);

// In your render loop (60 FPS)
while (running) {
    animator_update(animator);  // Update all animations
    render_frame();
}

// Cleanup
animator_destroy(animator);
```

## File Structure

```
userspace/lib/animation/
├── animator.h                 - Core animation engine header
├── animator.c                 - Easing functions, interpolation
├── animator_control.c         - Animation control & update logic
├── ui_animations.h            - UI widget animations header
├── ui_animations.c            - Pre-built UI animations
├── window_animations.h        - Window manager animations
├── ANIMATION_GUIDE.md         - Complete guide (1000+ lines)
├── example_demo.c             - Demo program
├── Makefile                   - Build system
└── README.md                  - This file
```

## Core Concepts

### Animation Types

1. **Value** - Animate single float value
2. **Color** - RGBA color interpolation
3. **Transform** - 2D transform (translation, rotation, scale)
4. **Rectangle** - Position and size (x, y, w, h)
5. **Keyframe** - Multi-point animation with different easing
6. **Spring** - Physics-based spring animation

### Easing Functions

Choose from 30+ easing functions:

- **Linear**: Constant speed
- **Quad/Cubic/Quart/Quint**: Polynomial curves
- **Sine/Expo/Circ**: Mathematical curves
- **Back**: Overshoot effect (great for UI!)
- **Elastic**: Bouncy overshoot
- **Bounce**: Bouncing effect
- **Spring**: Physics simulation
- **Custom Bezier**: Define your own curve

### Animation Control

```c
anim_start(anim);           // Start animation
anim_pause(anim);           // Pause
anim_resume(anim);          // Resume from pause
anim_stop(anim);            // Jump to end immediately
anim_cancel(anim);          // Cancel without completing
anim_seek(anim, 0.5f);      // Jump to 50% progress
```

### Repeating Animations

```c
anim_set_repeat(anim, 3);          // Repeat 3 times
anim_set_repeat(anim, -1);         // Infinite loop
anim_set_auto_reverse(anim, true); // Ping-pong effect
```

## Examples

### Example 1: Button Hover

```c
#include "ui_animations.h"

widget_anim_state_t button = {
    .scale = 1.0f,
    .opacity = 1.0f,
    .bg_color = { .r = 0.2f, .g = 0.6f, .b = 1.0f, .a = 1.0f }
};

// On hover
color_t hover_color = { .r = 0.3f, .g = 0.7f, .b = 1.0f, .a = 1.0f };
ui_button_hover(animator, &button, hover_color);
// Scale animates to 1.05, color transitions
```

### Example 2: Window Open

```c
#include "window_animations.h"

window_anim_t window = {
    .geometry = { .x = 100, .y = 100, .w = 800, .h = 600 },
    .opacity = 0.0f,
    .scale = 0.9f
};

wm_window_open(animator, &window);
// Fades in + scales to 1.0 over 200ms
```

### Example 3: Spring Physics

```c
spring_params_t spring = {
    .stiffness = 200.0f,
    .damping = 20.0f,
    .mass = 1.0f,
    .velocity = 0.0f
};

float value = 0.0f;
animation_t *anim = anim_spring(animator, 0.0f, 100.0f, &spring);
anim_set_target(anim, &value);
anim_start(anim);
// Natural bouncy motion
```

### Example 4: Keyframe Animation

```c
animation_t *anim = anim_keyframe(animator);
anim_add_keyframe(anim, 0.0f, 0.0f, EASE_LINEAR);
anim_add_keyframe(anim, 0.3f, 100.0f, EASE_OUT_CUBIC);
anim_add_keyframe(anim, 0.7f, 50.0f, EASE_IN_CUBIC);
anim_add_keyframe(anim, 1.0f, 100.0f, EASE_OUT_BOUNCE);
anim->duration = 1000000;  // 1 second
anim_start(anim);
```

### Example 5: Animation Callbacks

```c
void on_complete(void *user_data) {
    printf("Animation finished!\n");
}

animation_t *anim = anim_from_to(animator, 0.0f, 1.0f, 200000, EASE_OUT_QUAD);
anim_on_complete(anim, on_complete, NULL);
anim_start(anim);
```

### Example 6: Animation Groups

```c
animation_group_t *group = anim_group_create();
anim_group_add(group, anim1);
anim_group_add(group, anim2);
anim_group_add(group, anim3);
anim_group_start(group);  // Start all simultaneously
```

## UI Widget Animations

Pre-built animations for common UI elements:

### Buttons
- `ui_button_hover()` - Scale + color on hover
- `ui_button_click()` - Squeeze effect on click
- `ui_button_pulse()` - Attention animation

### Menus
- `ui_menu_open()` - Fade + slide from top
- `ui_menu_cascade()` - Staggered item appearance

### Dialogs
- `ui_dialog_show()` - Scale + fade with backdrop
- `ui_dialog_shake()` - Error shake

### Toasts
- `ui_toast_slide_in()` - Slide from edge
- `ui_toast_auto_dismiss()` - Auto-hide after delay

### Progress Bars
- `ui_progress_animate()` - Smooth value transition
- `ui_progress_indeterminate()` - Shimmer effect

### Lists
- `ui_list_item_insert()` - Slide in new item
- `ui_list_item_remove()` - Fade out + collapse

## Window Manager Animations

### Lifecycle
- `wm_window_open()` - Fade + scale in
- `wm_window_close()` - Fade + scale out
- `wm_window_minimize()` - Scale down to icon (genie effect)
- `wm_window_maximize()` - Expand to fullscreen

### Workspace Switching
- `wm_workspace_slide()` - Slide transition
- `wm_workspace_cube()` - 3D cube rotation
- `wm_workspace_zoom_out()` - Expo view

### Focus
- `wm_window_focus()` - Highlight focused window
- `wm_window_attention()` - Urgent window pulse

## Performance

### Benchmarks

- **100 simultaneous animations**: ~0.5ms update time
- **500 animations**: ~2ms update time
- **Target**: < 1ms for typical workload

### Optimization Tips

1. Use GPU acceleration for visual effects
2. Batch animations together
3. Enable reduce motion for accessibility
4. Use simple easing for large numbers of animations
5. Avoid memory allocation during animation

### Reduce Motion Mode

```c
animator_set_reduce_motion(animator, true);
```

In reduce motion mode:
- Animations are instant or very fast (50ms)
- No elastic/bounce/spring effects
- Crossfades instead of slides
- Essential feedback only

## API Reference

See `ANIMATION_GUIDE.md` for complete API documentation (1000+ lines).

### Quick Reference

**Animator:**
- `animator_create()` / `animator_destroy()`
- `animator_update()` - Call per frame
- `animator_set_reduce_motion()`
- `animator_set_global_speed()`

**Animation Creation:**
- `anim_create()` / `anim_from_to()`
- `anim_spring()` / `anim_color()`
- `anim_transform()` / `anim_rect()`
- `anim_keyframe()` / `anim_add_keyframe()`

**Control:**
- `anim_start()` / `anim_pause()` / `anim_resume()`
- `anim_stop()` / `anim_cancel()`

**Configuration:**
- `anim_set_delay()` / `anim_set_repeat()`
- `anim_set_auto_reverse()` / `anim_set_target()`
- `anim_set_bezier()`

**Callbacks:**
- `anim_on_start()` / `anim_on_update()`
- `anim_on_complete()` / `anim_on_cancel()`

**Query:**
- `anim_is_running()` / `anim_is_finished()`
- `anim_get_progress()` / `anim_get_current_value()`

## Demo Program

Build and run the demo:

```bash
make example
./animation_demo          # Run all demos
./animation_demo 5        # Run specific demo (1-10)
```

Demos:
1. Basic value animation
2. Color animation
3. Spring physics
4. Keyframe animation
5. Callbacks
6. Repeating/reversing
7. Animation groups
8. Easing comparison
9. UI animations
10. Performance test

## Integration

### With Compositor

```c
#include "animator.h"
#include "compositor.h"

compositor_t *comp = compositor_init("/dev/dri/card0");
animator_t *animator = animator_create();

// Main loop
while (running) {
    // Update animations
    animator_update(animator);
    
    // Render frame
    compositor_frame(comp);
}
```

### With Window Manager

```c
#include "animator.h"
#include "window_animations.h"

// Window open event
void on_window_open(window_t *window) {
    window_anim_t anim = {
        .geometry = window->geometry,
        .opacity = 0.0f,
        .scale = 0.9f
    };
    
    wm_window_open(global_animator, &anim);
    
    // Copy animated values back
    window->opacity = anim.opacity;
    window->scale = anim.scale;
}
```

## Testing

```bash
# Unit tests (if available)
make test

# Performance test
./animation_demo 10

# Visual test
./animation_demo 9
```

## License

Part of AutomationOS. See root LICENSE file.

## Contributing

Contributions welcome! Please follow AutomationOS coding style.

## Documentation

- `ANIMATION_GUIDE.md` - Complete guide with examples
- `animator.h` - API documentation in comments
- `example_demo.c` - Working examples

## Support

For bugs or questions:
- Check `ANIMATION_GUIDE.md` for detailed documentation
- Review `example_demo.c` for code examples
- Consult source code comments

---

**Make it feel like magic!** ✨
