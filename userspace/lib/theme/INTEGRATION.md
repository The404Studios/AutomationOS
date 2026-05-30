# Theme Engine Integration Guide

## Overview

This guide shows how to integrate the theme engine with existing AutomationOS components.

## Window Manager Integration

### Step 1: Include Theme Engine

Edit `userspace/wm/window_manager.c`:

```c
#include "../lib/theme/theme.h"

// Add theme engine to window manager structure
typedef struct window_manager {
    compositor_t *compositor;
    theme_engine_t *theme_engine;  // NEW
    
    // ... existing fields
} window_manager_t;
```

### Step 2: Initialize Theme Engine

In `wm_init()`:

```c
window_manager_t *wm_init(compositor_t *comp) {
    // ... existing initialization

    // Initialize theme engine
    wm->theme_engine = theme_engine_init();
    if (!wm->theme_engine) {
        fprintf(stderr, "[WM] Failed to initialize theme engine\n");
        free(wm);
        return NULL;
    }

    // Load all themes
    theme_load_all(wm->theme_engine);

    // Set default theme (light mode by default)
    theme_set_active(wm->theme_engine, "Default Light");

    // Register callback for theme changes
    theme_register_callback(wm->theme_engine, wm_on_theme_changed, wm);

    // Apply initial theme settings
    const theme_t *theme = theme_get_active(wm->theme_engine);
    wm->decoration_height = theme->window.titlebar_height;
    wm->border_width = theme->window.border_width;
    wm->corner_radius = theme->window.corner_radius;

    printf("[WM] Theme engine initialized: %s\n", theme->meta.name);

    return wm;
}
```

### Step 3: Theme Change Callback

Add callback function:

```c
static void wm_on_theme_changed(const theme_t *new_theme, void *user_data) {
    window_manager_t *wm = (window_manager_t *)user_data;
    
    printf("[WM] Theme changed to: %s\n", new_theme->meta.name);

    // Update window manager settings
    wm->decoration_height = new_theme->window.titlebar_height;
    wm->border_width = new_theme->window.border_width;
    wm->corner_radius = new_theme->window.corner_radius;

    // Redraw all window decorations
    for (uint32_t i = 0; i < wm->compositor->window_count; i++) {
        window_t *win = wm->compositor->windows[i];
        if (win && win->type == WINDOW_NORMAL) {
            wm_draw_decorations(wm, win);
        }
    }

    // Mark full redraw
    compositor_mark_full_redraw(wm->compositor);
}
```

### Step 4: Update `wm_draw_decorations()`

```c
void wm_draw_decorations(window_manager_t *wm, window_t *window) {
    if (!wm || !window || !wm->decorations_enabled) return;
    if (window->type != WINDOW_NORMAL) return;

    const theme_t *theme = theme_get_active(wm->theme_engine);
    if (!theme) return;

    // Get theme colors
    color_rgba_t bg_color = theme->colors.panel_bg;
    color_rgba_t border_color = theme->colors.border;
    color_rgba_t text_color = theme->colors.text_primary;

    rect_t titlebar = {
        .x = window->frame_geometry.x,
        .y = window->frame_geometry.y,
        .w = window->frame_geometry.w,
        .h = wm->decoration_height
    };

    // Draw titlebar background
    draw_rounded_rect_corners(wm->compositor->gpu, &titlebar,
                              wm->corner_radius, wm->corner_radius,
                              0, 0, bg_color);

    // Draw traffic light buttons (if enabled)
    if (theme->window.show_traffic_lights) {
        bool close_hover = false;  // TODO: Track hover state
        bool minimize_hover = false;
        bool maximize_hover = false;

        draw_traffic_lights(wm->compositor->gpu,
                           titlebar.x + 12,
                           titlebar.y + (wm->decoration_height - WINDOW_TRAFFIC_LIGHT) / 2,
                           close_hover, minimize_hover, maximize_hover);
    }

    // Draw window title (if enabled)
    if (theme->window.show_title_text && window->title[0]) {
        text_style_t title_style = {
            .font_family = theme->typography.family,
            .font_size = theme->typography.base_size,
            .font_weight = 600,
            .italic = false,
            .underline = false
        };

        int32_t title_x = titlebar.x + titlebar.w / 2;
        int32_t title_y = titlebar.y + (wm->decoration_height / 2);

        draw_text_centered(wm->compositor->gpu, window->title,
                          &titlebar, title_style, text_color);
    }

    // Draw border
    if (wm->border_width > 0) {
        draw_rect_outline(wm->compositor->gpu, &window->frame_geometry,
                         wm->border_width, border_color);
    }
}
```

### Step 5: Add Theme Switching API

Add to `window_manager.h`:

```c
/**
 * Set window manager theme
 */
void wm_set_theme(window_manager_t *wm, const char *theme_name);

/**
 * Set theme mode (light/dark/auto)
 */
void wm_set_theme_mode(window_manager_t *wm, theme_mode_t mode);

/**
 * Get current theme
 */
const theme_t *wm_get_theme(window_manager_t *wm);
```

Implementation in `window_manager.c`:

```c
void wm_set_theme(window_manager_t *wm, const char *theme_name) {
    if (!wm || !wm->theme_engine) return;
    theme_set_active(wm->theme_engine, theme_name);
}

void wm_set_theme_mode(window_manager_t *wm, theme_mode_t mode) {
    if (!wm || !wm->theme_engine) return;
    theme_set_mode(wm->theme_engine, mode);
}

const theme_t *wm_get_theme(window_manager_t *wm) {
    if (!wm || !wm->theme_engine) return NULL;
    return theme_get_active(wm->theme_engine);
}
```

## Panel Integration

Edit `userspace/shell/desktop/panel.c`:

```c
#include "../../lib/theme/theme.h"

typedef struct {
    theme_engine_t *theme_engine;
    uint32_t height;
    uint32_t padding;
    uint32_t icon_size;
    // ... existing fields
} panel_t;

panel_t *panel_init(void) {
    panel_t *panel = calloc(1, sizeof(panel_t));
    
    // Initialize theme engine
    panel->theme_engine = theme_engine_init();
    theme_load_all(panel->theme_engine);
    theme_set_active(panel->theme_engine, "Default Light");

    // Get panel settings from theme
    theme_get_panel_settings(panel->theme_engine,
                            &panel->height,
                            &panel->padding,
                            &panel->icon_size,
                            NULL);

    theme_register_callback(panel->theme_engine, panel_on_theme_changed, panel);

    return panel;
}

static void panel_on_theme_changed(const theme_t *new_theme, void *user_data) {
    panel_t *panel = (panel_t *)user_data;
    
    // Update panel dimensions
    panel->height = new_theme->panel.height;
    panel->padding = new_theme->panel.padding;
    panel->icon_size = new_theme->panel.icon_size;

    // Redraw panel
    panel_redraw(panel);
}

void panel_render(panel_t *panel) {
    const theme_t *theme = theme_get_active(panel->theme_engine);
    
    rect_t panel_rect = {
        .x = 0,
        .y = 0,
        .w = screen_width,
        .h = panel->height
    };

    // Draw panel background
    draw_rounded_rect(&panel_rect, RADIUS_SUBTLE, theme->colors.panel_bg);

    // Draw panel shadow
    draw_shadow(&panel_rect, theme->panel.shadow, RGB(0, 0, 0));

    // Draw panel contents with theme colors
    // ... render clock, system tray, etc.
}
```

## Dock Integration

Edit `userspace/shell/desktop/dock.c`:

```c
typedef struct {
    theme_engine_t *theme_engine;
    uint32_t icon_size;
    uint32_t padding;
    uint32_t margin;
    bool magnification_enabled;
    // ... existing fields
} dock_t;

dock_t *dock_init(void) {
    dock_t *dock = calloc(1, sizeof(dock_t));
    
    // Initialize theme engine
    dock->theme_engine = theme_engine_init();
    theme_load_all(dock->theme_engine);
    theme_set_active(dock->theme_engine, "Default Light");

    // Get dock settings from theme
    theme_get_dock_settings(dock->theme_engine,
                           &dock->icon_size,
                           &dock->padding,
                           &dock->margin,
                           NULL,
                           &dock->magnification_enabled);

    theme_register_callback(dock->theme_engine, dock_on_theme_changed, dock);

    return dock;
}

static void dock_on_theme_changed(const theme_t *new_theme, void *user_data) {
    dock_t *dock = (dock_t *)user_data;
    
    dock->icon_size = new_theme->dock.icon_size;
    dock->padding = new_theme->dock.padding;
    dock->margin = new_theme->dock.margin;
    dock->magnification_enabled = new_theme->dock.magnification_enabled;

    dock_layout(dock);
    dock_redraw(dock);
}
```

## Settings App Integration

Create theme settings panel in `userspace/apps/settings/panels.c`:

```c
void settings_panel_theme(settings_t *settings) {
    theme_engine_t *engine = settings->theme_engine;
    const theme_t *current = theme_get_active(engine);

    // Theme mode selector
    ui_label("Theme Mode:");
    if (ui_radio_button("Light", current->meta.mode == THEME_MODE_LIGHT)) {
        theme_set_mode(engine, THEME_MODE_LIGHT);
    }
    if (ui_radio_button("Dark", current->meta.mode == THEME_MODE_DARK)) {
        theme_set_mode(engine, THEME_MODE_DARK);
    }
    if (ui_radio_button("Auto", current->meta.mode == THEME_MODE_AUTO)) {
        theme_set_mode(engine, THEME_MODE_AUTO);
    }

    ui_separator();

    // Theme selector (dropdown showing all loaded themes)
    ui_label("Theme:");
    for (uint32_t i = 0; i < engine->theme_count; i++) {
        theme_t *theme = engine->themes[i];
        bool is_current = (theme == current);
        
        if (ui_selectable(theme->meta.name, is_current)) {
            theme_set_active(engine, theme->meta.name);
        }
    }

    ui_separator();

    // Accessibility options
    ui_label("Accessibility:");
    
    bool high_contrast = current->accessibility.high_contrast;
    if (ui_checkbox("High Contrast", &high_contrast)) {
        // Would need to create modified theme or toggle setting
    }

    bool reduce_motion = current->accessibility.reduce_motion;
    if (ui_checkbox("Reduce Motion", &reduce_motion)) {
        // Toggle animation system
    }

    float text_scale = current->accessibility.text_scale;
    if (ui_slider("Text Scale", &text_scale, 0.8f, 2.0f)) {
        // Apply text scaling
    }
}
```

## Auto Mode Daemon

Create background service to update auto mode:

```c
// userspace/system/services/theme_daemon.c

#include "../../lib/theme/theme.h"
#include <unistd.h>
#include <time.h>

int main(void) {
    theme_engine_t *engine = theme_engine_init();
    if (!engine) {
        fprintf(stderr, "Failed to initialize theme engine\n");
        return 1;
    }

    // Load themes
    theme_load_all(engine);

    // Set auto mode
    theme_set_mode(engine, THEME_MODE_AUTO);
    theme_set_auto_schedule(engine, 7, 19);  // 7am-7pm light

    printf("[Theme Daemon] Started with auto mode\n");

    // Update loop (check every minute)
    while (1) {
        sleep(60);  // 1 minute
        theme_auto_mode_update(engine);
    }

    theme_engine_cleanup(engine);
    return 0;
}
```

## Build Integration

Update `userspace/wm/Makefile`:

```makefile
CFLAGS += -I../lib/theme
LDFLAGS += -L../lib/theme -ltheme -lm

$(TARGET): $(OBJS) ../lib/theme/libtheme.a
	$(CC) $(OBJS) $(LDFLAGS) -o $@

../lib/theme/libtheme.a:
	$(MAKE) -C ../lib/theme
```

## IPC Integration (Optional)

For system-wide theme synchronization, add IPC messages:

```c
// Theme change message
struct theme_change_msg {
    uint32_t type;  // MSG_THEME_CHANGE
    char theme_name[64];
    theme_mode_t mode;
};

// Broadcast theme change to all clients
void wm_broadcast_theme_change(window_manager_t *wm) {
    const theme_t *theme = wm_get_theme(wm);
    
    struct theme_change_msg msg = {
        .type = MSG_THEME_CHANGE,
        .mode = theme->meta.mode
    };
    strncpy(msg.theme_name, theme->meta.name, sizeof(msg.theme_name));

    // Broadcast to all connected clients
    ipc_broadcast(wm->ipc_server, &msg, sizeof(msg));
}
```

## Testing Integration

```bash
# Build theme library
cd userspace/lib/theme
make clean && make

# Build window manager with theme support
cd userspace/wm
make clean && make

# Test theme switching
./window-manager &
# In another terminal:
echo "switch_theme Default Dark" > /tmp/wm_control
```

## Troubleshooting

### Theme Not Loading

```c
// Add debug logging
if (!theme_load(engine, path)) {
    const char *error = theme_get_last_error();
    fprintf(stderr, "Theme load failed: %s\n", error);
    
    char parse_errors[1024];
    theme_get_parse_errors(parse_errors, sizeof(parse_errors));
    fprintf(stderr, "Parse errors:\n%s", parse_errors);
}
```

### Colors Not Updating

Ensure you're calling `compositor_mark_full_redraw()` after theme changes.

### Memory Leaks

Use valgrind to check for leaks:

```bash
valgrind --leak-check=full ./window-manager
```

## Performance Optimization

```c
// Cache theme values to avoid repeated lookups
typedef struct {
    color_rgba_t cached_primary;
    color_rgba_t cached_background;
    uint32_t cached_titlebar_height;
    // ... other frequently accessed values
} theme_cache_t;

void update_theme_cache(theme_cache_t *cache, const theme_t *theme) {
    cache->cached_primary = theme->colors.primary;
    cache->cached_background = theme->colors.bg_primary;
    cache->cached_titlebar_height = theme->window.titlebar_height;
}
```

---

**Integration Status**: Ready for Agent 8, 11, and applications
