# Image Library Integration Guide

Quick reference for integrating libimage into AutomationOS applications.

## Installation

```bash
cd userspace/lib/image
chmod +x build.sh
./build.sh
```

This will:
1. Build libimage.a and libimage.so
2. Generate default icons (requires Python + Pillow)
3. Install to /usr/lib and /usr/include
4. Install icons to /usr/share/icons
5. Install wallpaper to /usr/share/wallpapers

## Linking Your Application

### Makefile

```makefile
CC = gcc
CFLAGS = -Wall -O2 -I../../lib/image
LDFLAGS = -L../../../build/lib/image -limage -lm

my_app: main.o
	$(CC) main.o -o my_app $(LDFLAGS)

main.o: main.c
	$(CC) $(CFLAGS) -c main.c
```

### Example: Desktop Application

```c
#include "image.h"

// Load wallpaper
image_t *wallpaper = image_load("/usr/share/wallpapers/default.png", IMAGE_LOAD_RGBA);

// Resize to screen size
image_t *scaled = image_resize_bilinear(wallpaper, screen_width, screen_height);

// Blit to framebuffer
image_blit_to_framebuffer(framebuffer, fb_width, fb_height, scaled, 0, 0);

// Cleanup
image_free(wallpaper);
image_free(scaled);
```

### Example: Icon Loading

```c
// Load icon set
icon_set_t *icons = icon_load_set("terminal");

// Get best icon for target size
image_t *icon = icon_get_best_size(icons, 48);

// Use icon...
image_blit_to_framebuffer(fb, fb_w, fb_h, icon, x, y);

// Cleanup
icon_free_set(icons);
```

### Example: Image Preview

```c
// Load image for preview
image_t *preview = image_load(file_path, IMAGE_LOAD_RGBA);

if (preview) {
    printf("Image: %ux%u pixels\n", preview->width, preview->height);
    
    // Generate thumbnail
    image_t *thumb = image_resize_bilinear(preview, 256, 256);
    
    // Display...
    
    image_free(thumb);
    image_free(preview);
}
```

## Common Use Cases

### 1. Load and Display Wallpaper

```c
void set_desktop_wallpaper(const char *path) {
    image_t *img = image_load(path, IMAGE_LOAD_RGBA);
    if (!img) {
        fprintf(stderr, "Failed to load wallpaper: %s\n", image_get_error());
        return;
    }
    
    // Resize to screen
    image_t *scaled = image_resize_bilinear(img, 1920, 1080);
    
    // Blit to framebuffer or texture
    // ...
    
    image_free(img);
    image_free(scaled);
}
```

### 2. Load Dock Icon

```c
image_t *load_app_icon(const char *app_name, uint32_t size) {
    icon_set_t *icons = icon_load_set(app_name);
    if (!icons) {
        return NULL;  // Use default icon
    }
    
    image_t *icon = icon_get_best_size(icons, size);
    image_t *clone = image_clone(icon);  // Clone before freeing set
    
    icon_free_set(icons);
    return clone;
}
```

### 3. Generate Thumbnail

```c
image_t *generate_thumbnail(const char *image_path, uint32_t max_size) {
    image_t *img = image_load(image_path, IMAGE_LOAD_RGBA);
    if (!img) return NULL;
    
    // Calculate thumbnail dimensions (preserve aspect ratio)
    uint32_t thumb_w, thumb_h;
    if (img->width > img->height) {
        thumb_w = max_size;
        thumb_h = (img->height * max_size) / img->width;
    } else {
        thumb_h = max_size;
        thumb_w = (img->width * max_size) / img->height;
    }
    
    image_t *thumb = image_resize_bilinear(img, thumb_w, thumb_h);
    
    image_free(img);
    return thumb;
}
```

### 4. Composite Images with Alpha

```c
void draw_icon_with_badge(image_t *dest, image_t *icon, image_t *badge, 
                          int32_t x, int32_t y) {
    // Draw main icon
    image_blit(dest, icon, x, y);
    
    // Draw badge in top-right corner
    int32_t badge_x = x + icon->width - badge->width - 2;
    int32_t badge_y = y + 2;
    image_blit(dest, badge, badge_x, badge_y);
}
```

### 5. Create Icon with Text

```c
image_t *create_text_icon(const char *text, uint32_t size) {
    // Create blank icon
    image_t *icon = image_create(size, size, 4);
    image_fill(icon, 0xFF3498DB);  // Blue background
    
    // TODO: Render text (requires font library)
    // For now, just return blue square
    
    return icon;
}
```

## Error Handling

Always check return values and handle errors:

```c
image_t *img = image_load(path, IMAGE_LOAD_RGBA);
if (!img) {
    const char *error = image_get_error();
    fprintf(stderr, "Image load failed: %s\n", error);
    // Handle error...
    return;
}

// Use image...

image_free(img);
```

## Performance Tips

1. **Cache loaded images**: Don't reload the same image repeatedly
2. **Reuse resized versions**: Cache common sizes (48x48, 64x64, etc.)
3. **Use bilinear for quality**: Use nearest-neighbor only for speed
4. **Free unused images**: Call image_free() to avoid memory leaks
5. **Batch operations**: Load multiple images before rendering

## Icon Directory Structure

Icons should be placed in `/usr/share/icons/`:

```
/usr/share/icons/
├── terminal/
│   ├── 16x16.png
│   ├── 24x24.png
│   ├── 32x32.png
│   ├── 48x48.png
│   ├── 64x64.png
│   ├── 96x96.png
│   ├── 128x128.png
│   └── 256x256.png
├── folder/
│   └── ...
└── myapp/
    └── ...
```

## Testing

```bash
# Build test program
cd userspace/lib/image
make test

# Run tests
./build/lib/image/test_image

# Test with specific image
./build/lib/image/test_image /path/to/image.png
```

## Supported Formats

| Format | Extension | Alpha | Notes |
|--------|-----------|-------|-------|
| PNG | .png | Yes | Recommended for icons |
| JPEG | .jpg, .jpeg | No | Recommended for photos |
| BMP | .bmp | Partial | Windows bitmap |
| GIF | .gif | Yes | Animated not supported |
| TGA | .tga | Yes | Targa format |
| PSD | .psd | Yes | Photoshop (composited only) |
| HDR | .hdr | No | High dynamic range |

## Troubleshooting

### "Failed to load image: no decode found"

The file format is not supported or the file is corrupted.

### "Out of memory"

The image is too large or system is low on RAM. Try:
- Resizing the source image
- Increasing available memory
- Loading at lower resolution

### Icons not loading

Check:
1. Icon directory exists: `/usr/share/icons/`
2. PNG files have correct names: `16x16.png`, `48x48.png`, etc.
3. Icons are readable: `chmod 644 /usr/share/icons/*/`

### Blurry icons

Use `icon_get_best_size()` to get the closest resolution, not `image_resize()`.

## API Quick Reference

```c
// Load/Create
image_t *image_load(const char *path, image_load_mode_t mode);
image_t *image_create(uint32_t w, uint32_t h, uint32_t channels);
image_t *image_clone(const image_t *img);
void image_free(image_t *img);

// Pixel access
uint32_t image_get_pixel(const image_t *img, uint32_t x, uint32_t y);
void image_set_pixel(image_t *img, uint32_t x, uint32_t y, uint32_t rgba);

// Manipulation
image_t *image_resize_bilinear(const image_t *img, uint32_t w, uint32_t h);
image_t *image_crop(const image_t *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void image_flip_vertical(image_t *img);

// Blending
void image_blit(image_t *dest, const image_t *src, int32_t x, int32_t y);
void image_blit_to_framebuffer(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                                const image_t *img, int32_t x, int32_t y);

// Icons
icon_set_t *icon_load_set(const char *name);
image_t *icon_get_best_size(const icon_set_t *set, uint32_t size);
void icon_free_set(icon_set_t *set);

// Utilities
void image_fill(image_t *img, uint32_t rgba);
const char *image_get_error(void);
```

## Next Steps

1. See `README.md` for comprehensive API documentation
2. See `test_image.c` for usage examples
3. See `desktop.c` for desktop integration example
4. See `preview.c` for file manager integration example

## Support

For issues or questions, see:
- `AGENT8_IMAGE_INTEGRATION_REPORT.md` - Full integration report
- `README.md` - Complete API reference
- `image.h` - API header with documentation
