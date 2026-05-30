# AutomationOS Theme API

**Version:** 1.0  
**Date:** 2026-05-26

---

## Overview

The AutomationOS Theme API provides a comprehensive system for customizing the visual appearance of the desktop shell. Themes support light/dark modes, custom colors, blur effects, shadows, and typography.

---

## Theme Structure

### Complete Theme Definition

```c
typedef struct {
    theme_mode_t mode;          // THEME_LIGHT, THEME_DARK, THEME_AUTO

    // Primary colors
    color_t primary;            // #007AFF - Main accent color
    color_t secondary;          // #5856D6 - Secondary accent
    color_t success;            // #34C759 - Success states
    color_t warning;            // #FF9500 - Warning states
    color_t error;              // #FF3B30 - Error states

    // Background colors
    color_t bg_primary;         // Main background
    color_t bg_secondary;       // Secondary background
    color_t bg_tertiary;        // Tertiary background

    // Text colors
    color_t text_primary;       // Primary text
    color_t text_secondary;     // Secondary text
    color_t text_tertiary;      // Tertiary text

    // UI element colors
    color_t panel_bg;           // Panel background (with alpha)
    color_t dock_bg;            // Dock background (with alpha)
    color_t window_bg;          // Window background
    color_t separator;          // Separator lines

    // Effects
    uint8_t blur_radius;        // Blur effect strength (0-255)
    uint8_t shadow_opacity;     // Shadow opacity (0-255)
    uint8_t corner_radius;      // Rounded corners (pixels)

    // Fonts
    char font_system[64];       // System font name
    uint32_t font_size_small;   // Small text (11px)
    uint32_t font_size_body;    // Body text (13px)
    uint32_t font_size_heading; // Heading text (16px)
} theme_t;
```

---

## Built-in Themes

### Light Theme

**Colors:**
- **Primary:** #007AFF (Blue)
- **Background:** #FFFFFF (White)
- **Text:** #000000 (Black)
- **Panel:** rgba(240, 240, 240, 0.9)
- **Dock:** rgba(255, 255, 255, 0.78)

**Effects:**
- Blur radius: 20px
- Shadow opacity: 30
- Corner radius: 8px

**Usage:**
```c
theme_t theme;
theme_init_light(&theme);
```

### Dark Theme

**Colors:**
- **Primary:** #007AFF (Blue)
- **Background:** #1E1E1E (Dark gray)
- **Text:** #FFFFFF (White)
- **Panel:** rgba(45, 45, 45, 0.9)
- **Dock:** rgba(30, 30, 30, 0.78)

**Effects:**
- Blur radius: 20px
- Shadow opacity: 50
- Corner radius: 8px

**Usage:**
```c
theme_t theme;
theme_init_dark(&theme);
```

### Auto Theme

Automatically switches between light and dark based on time:
- **6:00 AM - 6:00 PM:** Light theme
- **6:00 PM - 6:00 AM:** Dark theme

**Usage:**
```c
theme_apply(shell, THEME_AUTO);
```

---

## Color Utilities

### Creating Colors

```c
// RGB (opaque)
color_t blue = color_rgb(0, 122, 255);

// RGBA (with transparency)
color_t semi_transparent = color_rgba(255, 255, 255, 128);

// From hex code
color_t primary = color_hex(0x007AFF);
```

### Color Structure

```c
typedef struct {
    uint8_t r;  // Red (0-255)
    uint8_t g;  // Green (0-255)
    uint8_t b;  // Blue (0-255)
    uint8_t a;  // Alpha (0-255, 255 = opaque)
} color_t;
```

---

## Creating Custom Themes

### Example: Solarized Theme

```c
theme_t solarized_light;
theme_init_light(&solarized_light);

// Base colors (Solarized Light)
solarized_light.bg_primary = color_hex(0xFDF6E3);      // base3
solarized_light.bg_secondary = color_hex(0xEEE8D5);    // base2
solarized_light.bg_tertiary = color_hex(0x93A1A1);     // base1

solarized_light.text_primary = color_hex(0x657B83);    // base00
solarized_light.text_secondary = color_hex(0x839496);  // base0
solarized_light.text_tertiary = color_hex(0x93A1A1);   // base1

// Accent colors
solarized_light.primary = color_hex(0x268BD2);         // blue
solarized_light.success = color_hex(0x859900);         // green
solarized_light.warning = color_hex(0xCB4B16);         // orange
solarized_light.error = color_hex(0xDC322F);           // red

// Apply to shell
shell->theme = solarized_light;
```

### Example: Dracula Theme

```c
theme_t dracula;
theme_init_dark(&dracula);

// Dracula colors
dracula.bg_primary = color_hex(0x282A36);      // Background
dracula.bg_secondary = color_hex(0x44475A);    // Current Line
dracula.bg_tertiary = color_hex(0x6272A4);     // Comment

dracula.text_primary = color_hex(0xF8F8F2);    // Foreground
dracula.text_secondary = color_hex(0xF8F8F2);  // Foreground
dracula.text_tertiary = color_hex(0x6272A4);   // Comment

dracula.primary = color_hex(0xBD93F9);         // Purple
dracula.success = color_hex(0x50FA7B);         // Green
dracula.warning = color_hex(0xFFB86C);         // Orange
dracula.error = color_hex(0xFF5555);           // Red

shell->theme = dracula;
```

---

## Theme Application

### Applying Themes

```c
// Apply light theme
theme_apply(shell, THEME_LIGHT);

// Apply dark theme
theme_apply(shell, THEME_DARK);

// Auto theme (time-based)
theme_apply(shell, THEME_AUTO);
```

### Dynamic Theme Switching

```c
// Switch theme at runtime
void toggle_theme(desktop_shell_t *shell) {
    if (shell->theme.mode == THEME_LIGHT) {
        theme_apply(shell, THEME_DARK);
    } else {
        theme_apply(shell, THEME_LIGHT);
    }
}
```

### Saving Theme Preferences

```c
// Save theme to config file
void save_theme_preference(theme_mode_t mode) {
    FILE *f = fopen("~/.config/autoshell/shell.conf", "w");
    fprintf(f, "[theme]\n");
    fprintf(f, "mode = %s\n",
            mode == THEME_LIGHT ? "light" :
            mode == THEME_DARK ? "dark" : "auto");
    fclose(f);
}
```

---

## Component-Specific Theming

### Panel Theming

```c
// Access theme from panel
panel_t *panel = ...;
theme_t *theme = panel->theme;

// Use theme colors
color_t bg_color = theme->panel_bg;
color_t text_color = theme->text_primary;
uint8_t blur = theme->blur_radius;
```

### Dock Theming

```c
// Dock uses theme colors
dock_t *dock = ...;
color_t bg_color = dock->theme->dock_bg;
uint8_t corner_radius = dock->theme->corner_radius;
```

### Window Theming

```c
// Windows use theme colors
window_t *window = ...;
color_t bg_color = window->theme->window_bg;
color_t border_color = window->theme->separator;
```

---

## Typography

### Font Configuration

```c
// Set system font
strcpy(theme.font_system, "Inter");

// Font sizes
theme.font_size_small = 11;      // Captions, labels
theme.font_size_body = 13;       // Body text, buttons
theme.font_size_heading = 16;    // Headings, titles
```

### Font Rendering

```c
// Render text with theme font
void render_text(const char *text, theme_t *theme, font_size_t size) {
    const char *font_name = theme->font_system;
    uint32_t font_size;

    switch (size) {
        case FONT_SMALL:
            font_size = theme->font_size_small;
            break;
        case FONT_BODY:
            font_size = theme->font_size_body;
            break;
        case FONT_HEADING:
            font_size = theme->font_size_heading;
            break;
    }

    // TODO: Render text with font_name and font_size
}
```

---

## Visual Effects

### Blur Effect

```c
// Apply blur to panel/dock backgrounds
void apply_blur(rect_t *rect, uint8_t blur_radius) {
    // Gaussian blur implementation
    // blur_radius from theme->blur_radius (typically 20)
}
```

### Shadow Effect

```c
// Draw shadow under elements
void draw_shadow(rect_t *rect, uint8_t shadow_opacity) {
    // Offset shadow (4px down, 2px right)
    rect_t shadow_rect = {
        .x = rect->x + 2,
        .y = rect->y + 4,
        .width = rect->width,
        .height = rect->height
    };

    color_t shadow_color = color_rgba(0, 0, 0, shadow_opacity);
    // Apply blur to shadow
    // Draw shadow_rect with shadow_color
}
```

### Rounded Corners

```c
// Draw rounded rectangle
void draw_rounded_rect(rect_t *rect, uint8_t corner_radius, color_t color) {
    // corner_radius from theme->corner_radius (typically 8)
    // Use bezier curves for corners
}
```

---

## Animation & Transitions

### Theme Transition

Smooth transition between themes:

```c
void animate_theme_transition(desktop_shell_t *shell, theme_t *new_theme, uint32_t duration_ms) {
    theme_t *old_theme = &shell->theme;

    // Interpolate colors over duration_ms
    for (uint32_t t = 0; t < duration_ms; t += 16) {  // 60 FPS
        float progress = (float)t / (float)duration_ms;

        // Interpolate each color
        shell->theme.bg_primary = interpolate_color(
            old_theme->bg_primary,
            new_theme->bg_primary,
            progress
        );

        // Redraw shell
        desktop_shell_render(shell);

        // Wait 16ms (60 FPS)
        usleep(16000);
    }

    shell->theme = *new_theme;
}

color_t interpolate_color(color_t a, color_t b, float t) {
    return (color_t){
        .r = (uint8_t)(a.r + (b.r - a.r) * t),
        .g = (uint8_t)(a.g + (b.g - a.g) * t),
        .b = (uint8_t)(a.b + (b.b - a.b) * t),
        .a = (uint8_t)(a.a + (b.a - a.a) * t)
    };
}
```

---

## Theme File Format

### JSON Theme File

```json
{
  "name": "Ocean Breeze",
  "mode": "light",
  "colors": {
    "primary": "#0077BE",
    "secondary": "#00A8E8",
    "success": "#00D9A3",
    "warning": "#FFA500",
    "error": "#FF4444",
    "bg_primary": "#F0F8FF",
    "bg_secondary": "#E6F3FF",
    "bg_tertiary": "#CCE7FF",
    "text_primary": "#1A3A52",
    "text_secondary": "#4A6A82",
    "text_tertiary": "#7A8A9A"
  },
  "effects": {
    "blur_radius": 20,
    "shadow_opacity": 30,
    "corner_radius": 10
  },
  "fonts": {
    "system": "Roboto",
    "size_small": 11,
    "size_body": 13,
    "size_heading": 16
  }
}
```

### Loading Custom Themes

```c
theme_t load_theme_from_file(const char *path) {
    theme_t theme;
    theme_init_light(&theme);  // Start with defaults

    FILE *f = fopen(path, "r");
    if (!f) return theme;

    // Parse JSON
    // TODO: JSON parser implementation

    fclose(f);
    return theme;
}
```

---

## Best Practices

### Color Contrast

Ensure sufficient contrast for accessibility:

```c
// Check contrast ratio (WCAG AA: 4.5:1 minimum)
float get_contrast_ratio(color_t fg, color_t bg) {
    float l1 = get_relative_luminance(fg);
    float l2 = get_relative_luminance(bg);
    return (max(l1, l2) + 0.05) / (min(l1, l2) + 0.05);
}
```

### Theme Testing

Test themes in different lighting conditions:
- Bright sunlight
- Indoor office lighting
- Low light / evening
- Complete darkness

### Performance

Minimize redraws when changing themes:
- Cache rendered elements
- Use dirty regions
- Batch updates

---

## Examples

### Complete Custom Theme

```c
#include "desktop_shell.h"

theme_t create_gruvbox_theme(void) {
    theme_t theme;
    theme.mode = THEME_DARK;

    // Gruvbox Dark colors
    theme.bg_primary = color_hex(0x282828);      // bg0
    theme.bg_secondary = color_hex(0x3C3836);    // bg1
    theme.bg_tertiary = color_hex(0x504945);     // bg2

    theme.text_primary = color_hex(0xEBDBB2);    // fg0
    theme.text_secondary = color_hex(0xD5C4A1);  // fg1
    theme.text_tertiary = color_hex(0xBDAE93);   // fg2

    theme.primary = color_hex(0x83A598);         // blue
    theme.secondary = color_hex(0xB16286);       // purple
    theme.success = color_hex(0xB8BB26);         // green
    theme.warning = color_hex(0xFABD2F);         // yellow
    theme.error = color_hex(0xFB4934);           // red

    theme.panel_bg = color_rgba(40, 40, 40, 230);
    theme.dock_bg = color_rgba(40, 40, 40, 200);
    theme.window_bg = theme.bg_primary;
    theme.separator = color_hex(0x504945);

    theme.blur_radius = 20;
    theme.shadow_opacity = 50;
    theme.corner_radius = 8;

    strcpy(theme.font_system, "JetBrains Mono");
    theme.font_size_small = 11;
    theme.font_size_body = 13;
    theme.font_size_heading = 16;

    return theme;
}

int main(void) {
    desktop_shell_t *shell = desktop_shell_create(1920, 1080);

    // Apply Gruvbox theme
    theme_t gruvbox = create_gruvbox_theme();
    shell->theme = gruvbox;

    desktop_shell_run(shell);
    desktop_shell_destroy(shell);
    return 0;
}
```

---

## API Reference

### Functions

```c
// Initialize built-in themes
void theme_init_light(theme_t *theme);
void theme_init_dark(theme_t *theme);

// Apply theme to shell
void theme_apply(desktop_shell_t *shell, theme_mode_t mode);

// Color utilities
color_t color_rgb(uint8_t r, uint8_t g, uint8_t b);
color_t color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
color_t color_hex(uint32_t hex);
```

---

**Build beautiful, accessible interfaces with the Theme API.**
