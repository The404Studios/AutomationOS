# Font API Quick Reference

**AutomationOS Font Rendering Library - Cheat Sheet**

## Core Font Library API

### Initialization

```c
#include "font.h"

// Initialize font system (call once at startup)
bool font_init(size_t cache_size);
// Example: font_init(2000);

// Shutdown font system
void font_shutdown(void);
```

### Font Loading

```c
// Load font from file
font_t* font_load(const char* path);
// Example: font_t* font = font_load("/fonts/DejaVuSans.ttf");

// Load font from memory buffer
font_t* font_load_memory(const void* data, size_t size);

// Free font
void font_free(font_t* font);
```

### Font Configuration

```c
// Set font size (in points, at 72 DPI)
void font_set_size(font_t* font, float size_pt);
// Example: font_set_size(font, 12.0f);

// Set rendering quality
void font_set_quality(font_t* font, font_quality_t quality);
// Options: FONT_QUALITY_LOW, MEDIUM, HIGH, ULTRA
// Example: font_set_quality(font, FONT_QUALITY_HIGH);
```

### Text Rendering

```c
// Render text to framebuffer
int font_render_text(font_t* font, uint32_t* fb,
                     uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                     int32_t x, int32_t y, const char* text,
                     const font_render_opts_t* opts);
// Returns: number of characters rendered, or -1 on failure

// Render single glyph
int font_render_glyph(font_t* font, uint32_t* fb,
                      uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                      int32_t x, int32_t y, uint32_t codepoint, uint32_t color);
// Returns: horizontal advance in pixels, or -1 on failure
```

### Text Measurement

```c
// Measure text dimensions
bool font_measure_text(font_t* font, const char* text, font_metrics_t* metrics);

// Get font metrics
void font_get_metrics(font_t* font, font_metrics_t* metrics);

// Get single glyph
const font_glyph_t* font_get_glyph(font_t* font, uint32_t codepoint);
```

### Rendering Options

```c
typedef struct {
    uint32_t color;           // ARGB color (e.g., 0xFF000000 = black)
    font_align_t align;       // FONT_ALIGN_LEFT, CENTER, or RIGHT
    uint32_t wrap_width;      // Line wrap width (0 = no wrap)
    uint32_t line_spacing;    // Extra line spacing in pixels
    uint8_t style;            // FONT_STYLE_NORMAL, BOLD, ITALIC, UNDERLINE
} font_render_opts_t;

// Default options
font_render_opts_t opts = {
    .color = 0xFF000000,
    .align = FONT_ALIGN_LEFT,
    .wrap_width = 0,
    .line_spacing = 0,
    .style = FONT_STYLE_NORMAL
};
```

### Cache Management

```c
// Clear glyph cache
void font_cache_clear(font_t* font);  // Specific font
void font_cache_clear(NULL);          // All fonts

// Get cache statistics
size_t hits, misses, size;
font_cache_stats(&hits, &misses, &size);
```

---

## Compositor Font API

```c
#include "font_integration.h"

// Initialize compositor fonts
compositor_font_pool_t* pool = compositor_font_init(2000);

// Render window title
compositor_render_title(pool, fb, width, height, pitch,
                        x, y, title_width, 32, "Window Title", focused);

// Render menu item
compositor_render_menu_item(pool, fb, width, height, pitch,
                             x, y, "File", selected);

// Measure text
font_metrics_t metrics;
compositor_measure_text(pool, "Text", 0, &metrics);  // font_type: 0=title

// Warm cache
compositor_warm_cache(pool);

// Cleanup
compositor_font_shutdown(pool);
```

---

## Desktop Shell Font API

```c
#include "font_integration.h"

// Initialize desktop fonts
desktop_font_pool_t* pool = desktop_font_init();

// Render panel clock
time_t now = time(NULL);
struct tm* tm = localtime(&now);
char time_str[32];
strftime(time_str, sizeof(time_str), "%H:%M", tm);
desktop_render_clock(pool, fb, width, height, pitch, x, y, time_str);

// Render dock label
desktop_render_dock_label(pool, fb, width, height, pitch,
                           icon_center_x, icon_bottom_y, "Terminal");

// Render menu item
desktop_render_menu_item(pool, fb, width, height, pitch,
                          x, y, 28, "System Settings", hovered);

// Render notification
desktop_render_notification(pool, fb, width, height, pitch,
                             x, y, 300, "Title", "Body text here");

// Cleanup
desktop_font_shutdown(pool);
```

---

## Terminal Font API

```c
#include "font_integration.h"

// Initialize terminal font
terminal_font_ctx_t* ctx = terminal_font_init(12.0f);

// Calculate terminal dimensions
uint32_t cols, rows;
terminal_calculate_dimensions(ctx, 800, 600, &cols, &rows);

// Render single cell
terminal_render_cell(ctx, fb, width, height, pitch,
                     col, row, 'A', 0xFFFFFFFF, 0xFF000000, false, false);

// Render full line (faster)
terminal_render_line(ctx, fb, width, height, pitch,
                     row, "Hello, world!", 0xFFFFFFFF, 0xFF000000);

// Render cursor
terminal_render_cursor(ctx, fb, width, height, pitch,
                        col, row, 0xFFFFFFFF, 0);  // style: 0=block

// Change font size
terminal_set_font_size(ctx, 14.0f);

// Get cell dimensions
uint32_t cell_w = ctx->cell_width;
uint32_t cell_h = ctx->cell_height;

// Cleanup
terminal_font_shutdown(ctx);
```

---

## File Explorer Font API

```c
#include "font_integration.h"

// Initialize explorer fonts
explorer_font_ctx_t* ctx = explorer_font_init();

// Render file name
explorer_render_filename(ctx, fb, width, height, pitch,
                          x, y, 24, "document.txt", selected);

// Render file metadata
explorer_render_metadata(ctx, fb, width, height, pitch,
                          x, y, 24, "1.5 MB", selected);

// Render breadcrumb (returns width for next segment)
uint32_t width = explorer_render_breadcrumb(ctx, fb, width, height, pitch,
                                             x, y, "Home", hovered);

// Render status bar
explorer_render_status(ctx, fb, width, height, pitch,
                        x, y, 20, "42 items, 12 selected");

// Render column header
explorer_render_column_header(ctx, fb, width, height, pitch,
                               x, y, 200, 28, "Name", 1);  // 1=ascending

// Cleanup
explorer_font_shutdown(ctx);
```

---

## Common Patterns

### Vertical Text Centering

```c
font_metrics_t metrics;
font_get_metrics(font, &metrics);

// For element of height H:
int32_t text_y = y + (H / 2) + (metrics.ascent / 2);

font_render_text(font, fb, width, height, pitch, x, text_y, text, &opts);
```

### Right-Aligned Text

```c
font_render_opts_t opts = {
    .color = 0xFFFFFFFF,
    .align = FONT_ALIGN_RIGHT,
    .wrap_width = 0,
    .line_spacing = 0,
    .style = FONT_STYLE_NORMAL
};

font_render_text(font, fb, width, height, pitch,
                 right_edge_x, y, "Right", &opts);
```

### Center-Aligned Text

```c
font_metrics_t metrics;
font_measure_text(font, text, &metrics);

int32_t centered_x = x + (width / 2) - (metrics.width / 2);

font_render_text(font, fb, width, height, pitch,
                 centered_x, y, text, NULL);
```

### Text with Shadow

```c
// Render shadow (offset by 1px)
font_render_opts_t shadow = { .color = 0x80000000 };  // Semi-transparent
font_render_text(font, fb, width, height, pitch, x+1, y+1, text, &shadow);

// Render main text
font_render_opts_t main = { .color = 0xFFFFFFFF };
font_render_text(font, fb, width, height, pitch, x, y, text, &main);
```

### Bold Text

```c
font_render_opts_t opts = {
    .color = 0xFF000000,
    .align = FONT_ALIGN_LEFT,
    .wrap_width = 0,
    .line_spacing = 0,
    .style = FONT_STYLE_BOLD
};

font_render_text(font, fb, width, height, pitch, x, y, "Bold", &opts);
```

### Wrapped Text

```c
font_render_opts_t opts = {
    .color = 0xFF000000,
    .align = FONT_ALIGN_LEFT,
    .wrap_width = 300,      // Wrap at 300 pixels
    .line_spacing = 4,      // 4 pixels between lines
    .style = FONT_STYLE_NORMAL
};

font_render_text(font, fb, width, height, pitch, x, y, long_text, &opts);
```

---

## Color Reference

### Standard Colors (ARGB)

```c
#define COLOR_BLACK       0xFF000000
#define COLOR_WHITE       0xFFFFFFFF
#define COLOR_RED         0xFFFF0000
#define COLOR_GREEN       0xFF00FF00
#define COLOR_BLUE        0xFF0000FF
#define COLOR_YELLOW      0xFFFFFF00
#define COLOR_CYAN        0xFF00FFFF
#define COLOR_MAGENTA     0xFFFF00FF
#define COLOR_GRAY        0xFF808080
#define COLOR_DARK_GRAY   0xFF404040
#define COLOR_LIGHT_GRAY  0xFFC0C0C0
```

### macOS-Inspired Colors

```c
#define COLOR_ACCENT_BLUE     0xFF007AFF  // macOS blue
#define COLOR_TEXT_PRIMARY    0xFF000000  // Black
#define COLOR_TEXT_SECONDARY  0xFF666666  // Gray
#define COLOR_TEXT_TERTIARY   0xFF999999  // Light gray
#define COLOR_BG_PRIMARY      0xFFFFFFFF  // White
#define COLOR_BG_SECONDARY    0xFFF5F5F5  // Off-white
```

### Semi-Transparent Colors

```c
#define COLOR_SHADOW      0x80000000  // 50% black
#define COLOR_OVERLAY     0x40000000  // 25% black
#define COLOR_HIGHLIGHT   0x400078D7  // 25% blue
```

---

## Font Paths

```c
#define FONT_SANS       "/fonts/DejaVuSans.ttf"
#define FONT_MONO       "/fonts/DejaVuSansMono.ttf"
```

---

## Typical Font Sizes

| Use Case | Size (pt) | Example |
|----------|-----------|---------|
| Small UI text | 10 | Status bar, dock labels |
| Body text | 11-12 | File lists, menu items |
| Headings | 14-16 | Column headers, titles |
| Large headings | 18-24 | Section titles |
| Terminal | 12 | Monospace terminal text |

---

## Performance Tips

1. **Warm cache on startup:**
   ```c
   const char* common = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
   for (const char* p = common; *p; p++) {
       font_get_glyph(font, *p);
   }
   ```

2. **Reuse font instances:**
   ```c
   static font_t* g_ui_font = NULL;  // Load once, use many times
   ```

3. **Use batch rendering:**
   ```c
   font_render_text(font, fb, ..., "Full string", &opts);  // Fast
   // vs
   for (char c in text) font_render_glyph(...);  // Slow
   ```

4. **Avoid font size changes:**
   ```c
   // Slow:
   font_set_size(font, 12.0f);
   font_render_text(...);
   font_set_size(font, 14.0f);
   font_render_text(...);
   
   // Fast:
   font_t* font12 = font_load(...); font_set_size(font12, 12.0f);
   font_t* font14 = font_load(...); font_set_size(font14, 14.0f);
   ```

---

## Troubleshooting

| Problem | Cause | Solution |
|---------|-------|----------|
| Text not visible | Wrong baseline | Add `metrics.ascent` to Y |
| Text cut off | Wrong FB bounds | Check fb_width, fb_height |
| Blurry text | Low quality | Use FONT_QUALITY_HIGH |
| Slow rendering | Cache misses | Warm cache on startup |
| Link error | Missing lib | Add `-lfont -lm` to LDFLAGS |
| Font not found | Missing file | Check `/fonts/` in initrd |

---

**Quick Start: 30-Second Integration**

```c
#include "font.h"

int main(void) {
    // 1. Init
    font_init(1000);
    font_t* font = font_load("/fonts/DejaVuSans.ttf");
    font_set_size(font, 12.0f);
    font_set_quality(font, FONT_QUALITY_HIGH);
    
    // 2. Render
    font_metrics_t m;
    font_get_metrics(font, &m);
    font_render_text(font, fb, w, h, p, 100, 100 + m.ascent, "Hello!", NULL);
    
    // 3. Cleanup
    font_free(font);
    font_shutdown();
}
```

---

**End of Font API Reference**
