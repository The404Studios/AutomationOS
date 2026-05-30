# Desktop Shell API Reference

Quick reference for the desktop shell integration libraries.

## Compositor Client API

### Initialization
```c
#include "compositor_client.h"

// Connect to compositor
comp_client_t *client = comp_client_init("/run/compositor.sock");

// Cleanup when done
comp_client_cleanup(client);
```

### Window Management
```c
// Create window
comp_window_t *window = comp_create_window(
    client,
    COMP_WINDOW_NORMAL,  // or COMP_WINDOW_DOCK, COMP_WINDOW_DESKTOP
    100, 100,            // x, y position
    800, 600,            // width, height
    "My Window"          // title
);

// Show window
comp_map_window(client, window);

// Hide window
comp_unmap_window(client, window);

// Get pixel buffer for drawing
uint32_t *pixels = comp_get_pixels(window);

// After drawing, notify compositor
comp_update_surface(client, window);

// Move window
comp_move_window(client, window, 200, 150);

// Change title
comp_set_title(client, window, "New Title");

// Raise to top
comp_raise_window(client, window);

// Destroy window
comp_destroy_window(client, window);
```

## Drawing API

### Basic Shapes
```c
#include "draw.h"

uint32_t *framebuffer = comp_get_pixels(window);
uint32_t width = window->width;
uint32_t height = window->height;

// Clear screen
draw_clear(framebuffer, width, height, RGB(255, 255, 255));

// Fill rectangle
draw_fill_rect(framebuffer, width, height,
               10, 10,          // x, y
               100, 50,         // width, height
               RGB(0, 128, 255)); // blue

// Rectangle outline
draw_rect(framebuffer, width, height,
          10, 10, 100, 50,
          RGB(0, 0, 0),     // black
          2);               // 2px thick

// Rounded rectangle
draw_rounded_rect(framebuffer, width, height,
                  10, 70, 100, 50,
                  8,              // 8px corner radius
                  RGB(255, 0, 0)); // red

// Circle
draw_circle(framebuffer, width, height,
            60, 200,        // center x, y
            30,             // radius
            RGB(0, 255, 0)); // green

// Line
draw_line(framebuffer, width, height,
          10, 250, 110, 300,
          RGB(128, 0, 128),  // purple
          3);                // 3px thick
```

### Colors
```c
// Opaque colors
uint32_t red = RGB(255, 0, 0);
uint32_t green = RGB(0, 255, 0);
uint32_t blue = RGB(0, 0, 255);

// Transparent colors (alpha, red, green, blue)
uint32_t semi_transparent = ARGB(128, 255, 0, 0);  // 50% red

// Extract color components
uint8_t alpha = ALPHA(color);
uint8_t red_component = RED(color);
uint8_t green_component = GREEN(color);
uint8_t blue_component = BLUE(color);

// Common colors
COLOR_BLACK
COLOR_WHITE
COLOR_RED
COLOR_GREEN
COLOR_BLUE
COLOR_GRAY
COLOR_LIGHT_GRAY
COLOR_DARK_GRAY
COLOR_TRANSPARENT
```

### Effects
```c
// Drop shadow
draw_shadow(framebuffer, width, height,
            50, 50, 200, 100,  // rect bounds
            10,                // blur radius
            80);               // opacity (0-255)

// Blur region
draw_blur_region(framebuffer, width, height,
                 100, 100, 200, 150,  // region
                 5);                  // radius
```

### Pixel Operations
```c
// Set individual pixel
draw_pixel(framebuffer, width, height, x, y, color);

// Get pixel color
uint32_t pixel_color = draw_get_pixel(framebuffer, width, height, x, y);

// Alpha blend two colors
uint32_t result = draw_blend(source_color, dest_color);
```

## Launcher API

### Launch Applications
```c
// Launch by app ID
int pid = launcher_launch_app("com.automationos.terminal");

// Launch by path
int pid = launcher_launch_path("/usr/bin/terminal");

// Check if running
if (launcher_is_running("com.automationos.terminal")) {
    printf("Terminal is running\n");
}

// Get window count
uint32_t windows = launcher_get_window_count("com.automationos.terminal");

// Update launcher state (call periodically to reap zombies)
launcher_update();
```

## Desktop Shell Structures

### Theme Colors
```c
desktop_shell_t *shell = desktop_shell_create(1920, 1080);

// Access theme
theme_t *theme = &shell->theme;

// Primary colors
theme->primary     // #007AFF (blue)
theme->secondary   // #5856D6 (purple)
theme->success     // #34C759 (green)
theme->warning     // #FF9500 (orange)
theme->error       // #FF3B30 (red)

// Background colors
theme->bg_primary
theme->bg_secondary
theme->bg_tertiary

// Text colors
theme->text_primary
theme->text_secondary
theme->text_tertiary

// UI colors
theme->panel_bg
theme->dock_bg
theme->window_bg
theme->separator

// Convert to ARGB
uint32_t color = ARGB(theme->primary.a,
                      theme->primary.r,
                      theme->primary.g,
                      theme->primary.b);
```

## Example: Complete Window with UI

```c
#include "compositor_client.h"
#include "draw.h"

int main() {
    // Connect to compositor
    comp_client_t *client = comp_client_init(NULL);
    if (!client) {
        fprintf(stderr, "Failed to connect to compositor\n");
        return 1;
    }

    // Create window
    comp_window_t *window = comp_create_window(
        client,
        COMP_WINDOW_NORMAL,
        100, 100,
        400, 300,
        "My App"
    );

    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        comp_client_cleanup(client);
        return 1;
    }

    // Get pixel buffer
    uint32_t *pixels = comp_get_pixels(window);
    uint32_t width = window->width;
    uint32_t height = window->height;

    // Draw UI
    draw_clear(pixels, width, height, RGB(240, 240, 240));

    // Title bar
    draw_fill_rect(pixels, width, height,
                   0, 0, width, 32,
                   RGB(70, 130, 180));

    // Content area with rounded corners
    draw_rounded_rect(pixels, width, height,
                     20, 50, width - 40, height - 70,
                     8,
                     RGB(255, 255, 255));

    // Button
    draw_rounded_rect(pixels, width, height,
                     150, 220, 100, 40,
                     4,
                     RGB(0, 120, 215));

    // Update compositor
    comp_update_surface(client, window);

    // Show window
    comp_map_window(client, window);

    // Main loop
    printf("Window created. Press Enter to exit.\n");
    getchar();

    // Cleanup
    comp_destroy_window(client, window);
    comp_client_cleanup(client);

    return 0;
}
```

Compile with:
```bash
gcc -o myapp myapp.c \
    -I../../lib/compositor_client \
    -I../../lib/ui \
    ../../lib/compositor_client/compositor_client.c \
    ../../lib/ui/draw.c \
    -lm
```

## Example: Dock Click Handler

```c
void handle_dock_click(dock_t *dock, int32_t mouse_x, int32_t mouse_y) {
    // Check each dock item
    for (uint32_t i = 0; i < dock->count; i++) {
        dock_item_t *item = dock->items[i];
        
        // Check if click is within item bounds
        if (rect_contains(&item->bounds, mouse_x, mouse_y)) {
            printf("Clicked on %s\n", item->name);
            
            // Launch if not running
            if (!launcher_is_running(item->app_id)) {
                launcher_launch_app(item->app_id);
            } else {
                // Focus existing window
                // TODO: Send focus request to window manager
            }
            
            break;
        }
    }
}
```

## Example: Panel Rendering

```c
void render_panel(panel_t *panel) {
    if (!panel->comp_window) return;
    
    uint32_t *pixels = comp_get_pixels(panel->comp_window);
    uint32_t width = panel->comp_window->width;
    uint32_t height = panel->height;
    
    // Semi-transparent background
    uint32_t bg = ARGB(230,
                       panel->theme->panel_bg.r,
                       panel->theme->panel_bg.g,
                       panel->theme->panel_bg.b);
    draw_fill_rect(pixels, width, height, 0, 0, width, height, bg);
    
    // Activities button
    if (panel->activities->hovered) {
        draw_rounded_rect(pixels, width, height,
                         panel->activities->bounds.x,
                         panel->activities->bounds.y,
                         panel->activities->bounds.width,
                         panel->activities->bounds.height,
                         4,
                         ARGB(150, 200, 200, 200));
    }
    
    // System tray icons
    for (uint32_t i = 0; i < panel->system_tray->count; i++) {
        tray_icon_t *icon = &panel->system_tray->items[i];
        draw_circle(pixels, width, height,
                   icon->bounds.x + 8,
                   icon->bounds.y + 8,
                   6,
                   RGB(100, 100, 100));
    }
    
    // Update compositor
    comp_update_surface(panel->comp_client, panel->comp_window);
}
```

## Performance Tips

1. **Minimize Updates:** Only call `comp_update_surface()` when pixels actually change
2. **Dirty Rectangles:** Track which regions changed and only redraw those
3. **Alpha Blending:** Use opaque colors (alpha=255) when possible for faster rendering
4. **Batch Drawing:** Draw everything, then update compositor once
5. **Clip Regions:** The drawing API automatically clips to bounds - no need to check yourself

## Debugging

```c
// Enable compositor client debug messages
#define DEBUG_COMP_CLIENT 1

// Check if window creation succeeded
if (!window) {
    fprintf(stderr, "Window creation failed: %s\n", strerror(errno));
}

// Verify compositor connection
if (client->socket_fd < 0) {
    fprintf(stderr, "Not connected to compositor\n");
}

// Check pixel buffer
if (!pixels) {
    fprintf(stderr, "Invalid pixel buffer\n");
}
```

## Common Patterns

### Double Buffering Pattern
```c
// Render to back buffer (window pixels)
uint32_t *pixels = comp_get_pixels(window);
draw_clear(pixels, width, height, COLOR_WHITE);
// ... draw your UI ...

// Flip (update compositor)
comp_update_surface(client, window);
```

### Event Loop Pattern
```c
while (running) {
    // Handle events
    // TODO: Receive input events from compositor
    
    // Update state
    launcher_update();
    
    // Render
    render_window(window);
    
    // Sleep to target 60 FPS
    usleep(16666);  // ~16.6ms
}
```

### Resource Cleanup Pattern
```c
void cleanup() {
    if (window) comp_destroy_window(client, window);
    if (client) comp_client_cleanup(client);
}

// Register cleanup on exit
atexit(cleanup);
```

---
**API Version:** 1.0  
**Last Updated:** 2026-05-27
