# Font Library Integration Guide

This document explains how to integrate the font rendering library into the AutomationOS desktop stack.

## Quick Start

### 1. Download Dependencies

```bash
cd userspace/lib/font

# Download stb_truetype.h
make download-stb

# Download DejaVu Sans font
bash DOWNLOAD_FONT.sh
```

### 2. Build Library

```bash
make
make test
```

### 3. Install to Userspace

```bash
make install
```

This copies:
- `font.h` → `userspace/include/font.h`
- `libfont.a` → `userspace/lib/libfont.a`

## Integration with Window Manager

### Window Title Bars

```c
#include "font.h"

// In window manager initialization:
font_t* title_font = font_load("/fonts/DejaVuSans.ttf");
font_set_size(title_font, 12.0f);

// When rendering window:
void render_title_bar(window_t* win) {
    // Draw title bar background
    fill_rect(win->x, win->y, win->width, 24, 0xFFE0E0E0);
    
    // Render title text
    font_render_opts_t opts = {
        .color = 0xFF000000,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = win->width - 100,  // Leave room for buttons
        .line_spacing = 0
    };
    
    font_metrics_t metrics;
    font_get_metrics(title_font, &metrics);
    
    font_render_text(title_font, framebuffer, fb_width, fb_height, fb_pitch,
                     win->x + 8, win->y + 12 + (metrics.ascent / 2),
                     win->title, &opts);
}
```

## Integration with Terminal Emulator

### Monospace Text Rendering

```c
#include "font.h"

// In terminal initialization:
font_t* term_font = font_load("/fonts/DejaVuSansMono.ttf");
font_set_size(term_font, 14.0f);

// Calculate cell size
font_metrics_t metrics;
font_get_metrics(term_font, &metrics);
const font_glyph_t* glyph = font_get_glyph(term_font, 'M');

int cell_width = glyph->advance;
int cell_height = metrics.height;

// Render terminal cell
void render_cell(int col, int row, char ch, uint32_t fg, uint32_t bg) {
    int x = col * cell_width;
    int y = row * cell_height;
    
    // Draw background
    fill_rect(x, y, cell_width, cell_height, bg);
    
    // Draw character
    font_render_glyph(term_font, framebuffer, fb_width, fb_height, fb_pitch,
                      x, y + metrics.ascent, ch, fg);
}
```

## Integration with Desktop Shell

### Panel Clock

```c
#include "font.h"
#include <time.h>

font_t* clock_font = font_load("/fonts/DejaVuSans.ttf");
font_set_size(clock_font, 14.0f);

void render_clock(int x, int y) {
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", tm);
    
    font_render_opts_t opts = {
        .color = 0xFFFFFFFF,  // White
        .align = FONT_ALIGN_RIGHT,
        .wrap_width = 0,
        .line_spacing = 0
    };
    
    font_render_text(clock_font, framebuffer, fb_width, fb_height, fb_pitch,
                     x, y, time_str, &opts);
}
```

### Dock App Labels

```c
font_t* dock_font = font_load("/fonts/DejaVuSans.ttf");
font_set_size(dock_font, 10.0f);

void render_app_icon(const char* name, int x, int y) {
    // Draw icon (64x64)
    draw_icon(x, y);
    
    // Measure text for centering
    font_metrics_t metrics;
    font_measure_text(dock_font, name, &metrics);
    
    // Draw label below icon
    font_render_opts_t opts = {
        .color = 0xFFFFFFFF,
        .align = FONT_ALIGN_CENTER,
        .wrap_width = 0,
        .line_spacing = 0
    };
    
    font_get_metrics(dock_font, &metrics);
    font_render_text(dock_font, framebuffer, fb_width, fb_height, fb_pitch,
                     x + 32, y + 64 + 4 + metrics.ascent, name, &opts);
}
```

## Integration with File Manager

### File List Rendering

```c
font_t* file_font = font_load("/fonts/DejaVuSans.ttf");
font_set_size(file_font, 12.0f);

void render_file_list(file_entry_t* files, int count, int scroll_y) {
    font_metrics_t metrics;
    font_get_metrics(file_font, &metrics);
    int line_height = metrics.height + 4;
    
    int y = 50 - scroll_y;
    
    for (int i = 0; i < count; i++) {
        if (y > 0 && y < window_height) {
            // Highlight selected
            if (i == selected_index) {
                fill_rect(20, y, window_width - 40, line_height, 0xFF0078D7);
            }
            
            // Render file name
            font_render_opts_t opts = {
                .color = (i == selected_index) ? 0xFFFFFFFF : 0xFF000000,
                .align = FONT_ALIGN_LEFT,
                .wrap_width = 0,
                .line_spacing = 0
            };
            
            font_render_text(file_font, framebuffer, fb_width, fb_height, fb_pitch,
                             50, y + metrics.ascent + 2, files[i].name, &opts);
        }
        
        y += line_height;
    }
}
```

## Compositor Integration

### Damage Tracking

The compositor should track text regions as dirty:

```c
void track_text_damage(int x, int y, const char* text, font_t* font) {
    font_metrics_t metrics;
    font_measure_text(font, text, &metrics);
    
    // Mark region as damaged
    compositor_damage_rect(x, y - metrics.ascent,
                           metrics.width, metrics.height);
}
```

### Off-Screen Rendering

For window buffers, render text to window surface then composite:

```c
// Window has its own ARGB buffer
uint32_t* window_buffer = window->buffer;

// Render text to window buffer
font_render_text(font, window_buffer, window->width, window->height,
                 window->width * 4, 10, 20, "Window Text", NULL);

// Compositor blits window buffer to framebuffer
composite_window(window);
```

## libgui Integration

Add to `userspace/lib/ui/Makefile`:

```makefile
FONT_OBJS = ../font/ttf_parser.o ../font/rasterizer.o ../font/cache.o
LIBGUI_OBJS += $(FONT_OBJS)

CFLAGS += -I../font

libgui.a: $(LIBGUI_OBJS)
	ar rcs $@ $^
```

Create `userspace/lib/ui/text.c` wrapper:

```c
#include "gui.h"
#include "font.h"

static font_t* default_font = NULL;

void gui_init_text(void) {
    font_init(1000);
    default_font = font_load("/fonts/DejaVuSans.ttf");
    font_set_size(default_font, 12.0f);
}

void gui_draw_text(window_t* win, int x, int y, const char* text, uint32_t color) {
    font_render_opts_t opts = {
        .color = color,
        .align = FONT_ALIGN_LEFT,
        .wrap_width = 0,
        .line_spacing = 0
    };
    
    font_metrics_t metrics;
    font_get_metrics(default_font, &metrics);
    
    font_render_text(default_font, win->buffer, win->width, win->height,
                     win->width * 4, x, y + metrics.ascent, text, &opts);
}
```

## Performance Considerations

### Cache Warming

Pre-render common glyphs on startup:

```c
void warm_font_cache(font_t* font) {
    const char* common = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,!?";
    
    for (const char* p = common; *p; p++) {
        font_get_glyph(font, *p);
    }
}
```

### Font Pooling

Reuse font instances:

```c
typedef struct {
    font_t* ui_font;       // 12pt for UI elements
    font_t* title_font;    // 14pt for window titles
    font_t* mono_font;     // 12pt monospace for terminal
    font_t* icon_font;     // 10pt for dock labels
} font_pool_t;

font_pool_t* create_font_pool(void) {
    font_pool_t* pool = calloc(1, sizeof(font_pool_t));
    
    pool->ui_font = font_load("/fonts/DejaVuSans.ttf");
    font_set_size(pool->ui_font, 12.0f);
    
    pool->title_font = font_load("/fonts/DejaVuSans.ttf");
    font_set_size(pool->title_font, 14.0f);
    
    pool->mono_font = font_load("/fonts/DejaVuSansMono.ttf");
    font_set_size(pool->mono_font, 12.0f);
    
    pool->icon_font = font_load("/fonts/DejaVuSans.ttf");
    font_set_size(pool->icon_font, 10.0f);
    
    return pool;
}
```

## Memory Usage

Estimated memory consumption:

- **Font data:** ~300KB per TTF file (DejaVu Sans)
- **Glyph cache:** ~500 bytes per glyph × 1000 = ~500KB
- **stb_truetype internal:** ~100KB
- **Total:** ~900KB per font

For desktop with 3 fonts: ~2.7MB

## Thread Safety

**The font library is NOT thread-safe by default.**

For multi-threaded rendering:

1. Use one font instance per thread, OR
2. Add mutex locks around `font_render_text()` calls

```c
pthread_mutex_t font_lock = PTHREAD_MUTEX_INITIALIZER;

void thread_safe_render(font_t* font, ...) {
    pthread_mutex_lock(&font_lock);
    font_render_text(font, ...);
    pthread_mutex_unlock(&font_lock);
}
```

## Debugging

### Enable Debug Output

Add to `ttf_parser.c`:

```c
#define FONT_DEBUG 1

#if FONT_DEBUG
#define FONT_LOG(fmt, ...) fprintf(stderr, "[FONT] " fmt "\n", ##__VA_ARGS__)
#else
#define FONT_LOG(fmt, ...) ((void)0)
#endif
```

### Cache Statistics

```c
void print_cache_stats(void) {
    size_t hits, misses, size;
    font_cache_stats(&hits, &misses, &size);
    
    float hit_rate = (hits + misses) > 0 ? (float)hits / (hits + misses) * 100.0f : 0.0f;
    
    printf("Font cache: %zu entries, %.1f%% hit rate (%zu hits, %zu misses)\n",
           size, hit_rate, hits, misses);
}
```

## Testing in QEMU

1. Build font library and test program
2. Create test initrd with fonts:

```bash
mkdir -p initrd_root/fonts
cp fonts/DejaVuSans.ttf initrd_root/fonts/
cp test_font initrd_root/bin/

# Create initrd
cd initrd_root
find . | cpio -o -H newc | gzip > ../initrd.img
```

3. Run in QEMU:

```bash
qemu-system-x86_64 \
    -kernel kernel.bin \
    -initrd initrd.img \
    -vga std \
    -m 512M \
    -serial stdio
```

4. In shell:

```
# /bin/test_font
```

## Troubleshooting

### "stb_truetype.h not found"
Run `make download-stb` in `userspace/lib/font/`

### "Failed to load font"
- Check font path: `/fonts/DejaVuSans.ttf` in initrd
- Verify font file exists and is readable
- Check file size > 0

### Glyphs appear blurry
- Increase quality: `font_set_quality(font, FONT_QUALITY_ULTRA)`
- Ensure alpha blending is enabled in compositor

### Poor cache hit rate
- Increase cache size: `font_init(2000)`
- Check if font size changes frequently (invalidates cache)

### Text rendering slow
- Profile with `perf` to identify bottleneck
- Enable cache warming for common glyphs
- Consider using bitmap fonts for small sizes (<10pt)

## Future Enhancements

- [ ] Subpixel rendering (RGB LCD optimization)
- [ ] Font hinting support
- [ ] Emoji rendering (color fonts)
- [ ] Text shaping (HarfBuzz integration)
- [ ] Right-to-left languages (Arabic, Hebrew)
- [ ] Ligatures support
- [ ] Variable fonts (OpenType 1.8)
- [ ] GPU-accelerated glyph rasterization
