# Image Library Quick Start

Get started with libimage in 5 minutes.

## 1. Build and Install

```bash
cd userspace/lib/image
chmod +x build.sh
./build.sh
```

## 2. Link Your Application

Add to your Makefile:
```makefile
CFLAGS += -I../../lib/image
LDFLAGS += -L../../../build/lib/image -limage -lm
```

## 3. Basic Usage

```c
#include "image.h"

int main(void) {
    // Load image
    image_t *img = image_load("/path/to/image.png", IMAGE_LOAD_RGBA);
    if (!img) {
        fprintf(stderr, "Error: %s\n", image_get_error());
        return 1;
    }
    
    printf("Loaded %ux%u image\n", img->width, img->height);
    
    // Use image...
    
    image_free(img);
    return 0;
}
```

## 4. Common Tasks

### Load Wallpaper
```c
image_t *wallpaper = image_load("/usr/share/wallpapers/default.png", IMAGE_LOAD_RGBA);
image_t *scaled = image_resize_bilinear(wallpaper, 1920, 1080);
// Display scaled...
image_free(wallpaper);
image_free(scaled);
```

### Load Icon
```c
icon_set_t *icons = icon_load_set("terminal");
image_t *icon = icon_get_best_size(icons, 48);
// Display icon...
icon_free_set(icons);
```

### Generate Thumbnail
```c
image_t *img = image_load(photo_path, IMAGE_LOAD_RGBA);
image_t *thumb = image_resize_bilinear(img, 256, 256);
// Display thumb...
image_free(img);
image_free(thumb);
```

## 5. Test It

```bash
# Build test program
make test

# Run tests
./build/lib/image/test_image

# Test with your image
./build/lib/image/test_image ~/Pictures/photo.jpg
```

## Done!

See `README.md` for full API documentation.

See `INTEGRATION_GUIDE.md` for more examples.
