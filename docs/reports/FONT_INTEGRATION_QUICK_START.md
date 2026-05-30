# Font Integration Quick Start

**5-minute guide to integrating font rendering into AutomationOS applications.**

## Prerequisites

- Font library built and installed (see Setup below)
- DejaVu fonts downloaded
- Application has framebuffer access

## 1. Setup (One-time)

### Automated Setup

```bash
cd scripts
bash setup-fonts.sh
```

This will:
- Download stb_truetype.h
- Download DejaVu Sans and DejaVu Sans Mono fonts
- Build libfont.a
- Install to userspace/include and userspace/lib

### Manual Setup

```bash
cd userspace/lib/font

# Download dependencies
make download-stb
bash DOWNLOAD_FONT.sh

# Build and install
make
make install
```

## 2. Update Your Makefile

Add three things to your application's Makefile:

```makefile
# 1. Add include path
CFLAGS += -I../../../include

# 2. Add font integration source
SOURCES += font_integration.c

# 3. Link with libfont
LDFLAGS += -L../../../lib -lfont -lm
```

## 3. Add Font Integration Module

Copy the appropriate font integration files to your app:

**For compositor:**
```bash
cp userspace/compositor/font_integration.{c,h} your_compositor/
```

**For desktop shell:**
```bash
cp userspace/shell/desktop/font_integration.{c,h} your_shell/
```

**For terminal:**
```bash
cp userspace/apps/terminal/font_integration.{c,h} your_terminal/
```

**For file manager:**
```bash
cp userspace/apps/files/font_integration.{c,h} your_filemanager/
```

## 4. Use in Your Code

### Compositor Example

```c
#include "font_integration.h"

// Global font pool
static compositor_font_pool_t* g_fonts = NULL;

int main(void) {
    // Initialize font system
    g_fonts = compositor_font_init(2000);
    if (!g_fonts) {
        fprintf(stderr, "Failed to init fonts\n");
        return 1;
    }
    
    // Warm cache for better performance
    compositor_warm_cache(g_fonts);
    
    // Main loop
    while (running) {
        // Render window decorations
        for (int i = 0; i < num_windows; i++) {
            compositor_render_title(
                g_fonts,
                framebuffer, fb_width, fb_height, fb_pitch,
                windows[i].x, windows[i].y,
                windows[i].width, 32,
                windows[i].title,
                windows[i].focused
            );
        }
    }
    
    // Cleanup
    compositor_font_shutdown(g_fonts);
    return 0;
}
```

### Terminal Example

```c
#include "font_integration.h"

static terminal_font_ctx_t* g_font = NULL;

int main(void) {
    // Initialize font (12pt monospace)
    g_font = terminal_font_init(12.0f);
    if (!g_font) {
        fprintf(stderr, "Failed to init terminal font\n");
        return 1;
    }
    
    // Calculate terminal size
    uint32_t cols, rows;
    terminal_calculate_dimensions(g_font, 800, 600, &cols, &rows);
    printf("Terminal: %dx%d\n", cols, rows);
    
    // Render text
    for (uint32_t row = 0; row < rows; row++) {
        terminal_render_line(
            g_font,
            framebuffer, fb_width, fb_height, fb_pitch,
            row,
            buffer[row],  // Line text
            0xFFFFFFFF,   // White foreground
            0xFF000000    // Black background
        );
    }
    
    // Cleanup
    terminal_font_shutdown(g_font);
    return 0;
}
```

### Desktop Shell Example

```c
#include "font_integration.h"
#include <time.h>

static desktop_font_pool_t* g_fonts = NULL;

void render_panel(void) {
    // Get current time
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", tm);
    
    // Render clock in top-right corner
    desktop_render_clock(
        g_fonts,
        framebuffer, fb_width, fb_height, fb_pitch,
        fb_width - 80,  // Right-aligned
        0,              // Top of panel
        time_str
    );
}

void render_dock(void) {
    const char* apps[] = {"Files", "Terminal", "Settings"};
    int x = 100;
    
    for (int i = 0; i < 3; i++) {
        // Draw icon at (x, dock_y)
        draw_icon(x, dock_y, app_icons[i]);
        
        // Draw label below icon
        desktop_render_dock_label(
            g_fonts,
            framebuffer, fb_width, fb_height, fb_pitch,
            x + 32,  // Center of 64px icon
            dock_y + 64 + 4,
            apps[i]
        );
        
        x += 80;  // Spacing
    }
}

int main(void) {
    g_fonts = desktop_font_init();
    
    while (running) {
        render_panel();
        render_dock();
    }
    
    desktop_font_shutdown(g_fonts);
    return 0;
}
```

### File Explorer Example

```c
#include "font_integration.h"

static explorer_font_ctx_t* g_fonts = NULL;

void render_file_list(file_entry_t* files, int count, int selected) {
    int y = 100;  // Start after toolbar
    
    for (int i = 0; i < count; i++) {
        bool is_selected = (i == selected);
        
        // Render filename
        explorer_render_filename(
            g_fonts,
            framebuffer, fb_width, fb_height, fb_pitch,
            20, y, 24,
            files[i].name,
            is_selected
        );
        
        // Render size
        char size_str[32];
        format_size(size_str, files[i].size);
        explorer_render_metadata(
            g_fonts,
            framebuffer, fb_width, fb_height, fb_pitch,
            400, y, 24,
            size_str,
            is_selected
        );
        
        y += 24;
    }
}

int main(void) {
    g_fonts = explorer_font_init();
    
    while (running) {
        render_file_list(files, file_count, selected_index);
    }
    
    explorer_font_shutdown(g_fonts);
    return 0;
}
```

## 5. Bundle Fonts in Initrd

Update your initrd build script:

```bash
#!/bin/bash
# build-initrd.sh

# Create initrd structure
mkdir -p initrd_root/fonts
mkdir -p initrd_root/bin

# Copy fonts
cp userspace/lib/font/fonts/DejaVuSans.ttf initrd_root/fonts/
cp userspace/lib/font/fonts/DejaVuSansMono.ttf initrd_root/fonts/

# Copy binaries
cp build/userspace/compositor/compositor initrd_root/bin/
cp build/userspace/apps/terminal/terminal initrd_root/bin/
# ... other binaries

# Create initrd
cd initrd_root
find . | cpio -o -H newc | gzip > ../initrd.img
cd ..

echo "Initrd created: initrd.img (includes fonts)"
```

## 6. Test

### Build Everything

```bash
# Build font library
cd userspace/lib/font
make

# Build your application
cd ../../apps/your_app
make
```

### Run in QEMU

```bash
qemu-system-x86_64 \
    -kernel build/kernel.bin \
    -initrd initrd.img \
    -vga std \
    -m 512M \
    -serial stdio
```

### Expected Results

✓ Text renders clearly  
✓ Window titles visible  
✓ Panel clock updates  
✓ Dock labels show below icons  
✓ Terminal text is monospace  
✓ File names readable in explorer  

## Troubleshooting

### "Font not found" error

**Problem:** Font files not in initrd

**Solution:**
```bash
ls initrd_root/fonts/
# Should show DejaVuSans.ttf and DejaVuSansMono.ttf
```

### Blurry text

**Problem:** Quality too low

**Solution:**
```c
font_set_quality(font, FONT_QUALITY_HIGH);
```

### Build errors: "undefined reference to font_*"

**Problem:** Not linking with libfont

**Solution:** Add to Makefile:
```makefile
LDFLAGS += -L../../../lib -lfont -lm
```

### Misaligned text

**Problem:** Incorrect baseline calculation

**Solution:** Use metrics:
```c
font_metrics_t metrics;
font_get_metrics(font, &metrics);
int baseline_y = y + metrics.ascent;  // Correct
```

## API Reference

### Common Functions

```c
// Initialize font system (call once at startup)
bool font_init(size_t cache_size);

// Load font from file
font_t* font_load(const char* path);

// Set font size
void font_set_size(font_t* font, float size_pt);

// Set rendering quality
void font_set_quality(font_t* font, font_quality_t quality);

// Render text to framebuffer
int font_render_text(font_t* font, uint32_t* fb, 
                     uint32_t fb_width, uint32_t fb_height, uint32_t fb_pitch,
                     int32_t x, int32_t y, const char* text,
                     const font_render_opts_t* opts);

// Measure text dimensions
bool font_measure_text(font_t* font, const char* text, font_metrics_t* metrics);

// Free font
void font_free(font_t* font);

// Shutdown font system
void font_shutdown(void);
```

### Integration Wrappers

See full API in:
- `userspace/compositor/font_integration.h`
- `userspace/shell/desktop/font_integration.h`
- `userspace/apps/terminal/font_integration.h`
- `userspace/apps/files/font_integration.h`

## Performance Tips

1. **Cache warming:** Pre-render common glyphs on startup
   ```c
   compositor_warm_cache(fonts);
   ```

2. **Reuse font instances:** Don't load same font multiple times
   ```c
   static font_t* g_ui_font = NULL;  // Load once, use everywhere
   ```

3. **Batch rendering:** Use `font_render_text()` instead of per-glyph
   ```c
   font_render_text(font, fb, ..., "Hello World", &opts);
   ```

4. **Damage tracking:** Only redraw changed text regions
   ```c
   if (title_changed) {
       compositor_render_title(...);
   }
   ```

## Next Steps

- ✅ Font library integrated
- ⏭️ Use in widget library (buttons, labels, text boxes)
- ⏭️ Add text editing support (cursor positioning, selection)
- ⏭️ Implement text shaping for complex scripts (HarfBuzz)
- ⏭️ Add emoji support (color fonts)

---

**You're done! Text rendering is now integrated into your application.**

For detailed documentation, see:
- `FONT_INTEGRATION_DELIVERABLES.md` - Complete integration guide
- `userspace/lib/font/README.md` - Font library documentation
- `userspace/lib/font/INTEGRATION.md` - Integration examples
