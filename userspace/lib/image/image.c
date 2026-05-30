/**
 * Image Loading Library - Implementation
 *
 * Uses stb_image for decoding PNG, JPEG, BMP, GIF
 */

#include "image.h"

/*
 * ============================================================================
 * Dependency wiring (de-POSIX'd, dual-mode)
 * ============================================================================
 *
 * Hosted build (default): use the system libc/stdio as before so the existing
 * consumers (files/preview.c, desktop/desktop.c) keep working unchanged.
 *
 * Freestanding build (-DIMAGE_FREESTANDING): NO stdio, NO host headers. We
 * pull malloc/realloc/free + mem/str ops from the project libc (stdlib.c,
 * string.c) by extern declaration and the math stb_image needs (ldexp / pow)
 * from the project libm (math.c). stb_image is compiled with STBI_NO_STDIO so
 * no fopen/fread path is referenced, and its allocator/math hooks are wired to
 * our libc/libm BEFORE the implementation is included so no host <math.h>,
 * <stdlib.h> or <string.h> leaks in. Decoding is driven exclusively by
 * stbi_load_from_memory() over a SYS_MAP_FILE'd initrd buffer.
 */
#ifdef IMAGE_FREESTANDING

/* libc (stdlib.c / string.c) -- resolved at link time. */
extern void *malloc(size_t size);
extern void *calloc(size_t count, size_t size);
extern void *realloc(void *ptr, size_t size);
extern void  free(void *ptr);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *dest, int val, size_t n);
extern unsigned long strlen(const char *s);

/*
 * NOTE: with STBI_ONLY_PNG/JPEG + STBI_NO_HDR/LINEAR the decoder needs no
 * libm symbols (pow/ldexp live only in the HDR path), so we deliberately do
 * not pull anything from math.c here. libm still gets linked for stb_image's
 * sake-free build only if a future format needs it.
 */

/*
 * Wire stb_image to our freestanding world.
 *
 *  - STBI_NO_STDIO    : drop the fopen/fread/FILE* path entirely.
 *  - STBI_ONLY_PNG/JPEG: compile ONLY the PNG and JPEG decoders. This trims
 *    out the TGA decoder (which calls abs()), the HDR decoder (pow/ldexp/
 *    strcmp/strncmp/strtol), and BMP/GIF/PSD/PIC/PNM, so the only host-libc
 *    symbols left to resolve are malloc/realloc/free + memcpy/memmove/memset.
 *  - STBI_NO_HDR/LINEAR: belt-and-braces so no float gamma/HDR math is pulled.
 *  - STBI_ASSERT no-op : never reference host assert().
 *  - allocator + memmove hooks wired to our libc.
 *
 * stb_image.h still #includes <stdlib.h>/<string.h>/<limits.h>/<stddef.h>/
 * <stdarg.h> unconditionally; under the WSL hosted gcc those headers merely
 * provide declarations/typedefs (no code), and every function stb actually
 * *calls* is one we provide via libc at link time.
 */
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x)        ((void)0)
#define STBI_MALLOC(sz)       malloc(sz)
#define STBI_REALLOC(p, sz)   realloc((p), (sz))
#define STBI_FREE(p)          free(p)
#define STBI_MEMMOVE(d, s, n) image_memmove((d), (s), (n))

/* memmove may not exist in libc string.c; provide a local one for stb. */
static void *image_memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else       { for (size_t i = n; i != 0; i--) d[i - 1] = s[i - 1]; }
    return dst;
}

#else  /* hosted */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define STBI_MALLOC  malloc
#define STBI_REALLOC realloc
#define STBI_FREE    free
#define STBI_NO_THREAD_LOCALS

#endif /* IMAGE_FREESTANDING */

/* stb_image implementation (after all STBI_* config above). */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ============================================================================
// ERROR HANDLING
// ============================================================================

#ifdef IMAGE_FREESTANDING

/* No varargs/printf in freestanding mode: store a plain (non-formatted) msg. */
static char g_error_msg[256] = {0};

const char *image_get_error(void) {
    return g_error_msg;
}

static void set_error(const char *msg) {
    unsigned long i = 0;
    if (msg) {
        for (; msg[i] && i < sizeof(g_error_msg) - 1; i++) g_error_msg[i] = msg[i];
    }
    g_error_msg[i] = '\0';
}

#else  /* hosted */

static __thread char g_error_msg[256] = {0};

const char *image_get_error(void) {
    return g_error_msg;
}

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error_msg, sizeof(g_error_msg), fmt, args);
    va_end(args);
}

#endif /* IMAGE_FREESTANDING */

// ============================================================================
// CORE API
// ============================================================================

#ifndef IMAGE_FREESTANDING   /* fopen-backed loader -- hosted builds only */
image_t *image_load(const char *path, image_load_mode_t mode) {
    if (!path) {
        set_error("NULL path provided");
        return NULL;
    }

    image_t *img = calloc(1, sizeof(image_t));
    if (!img) {
        set_error("Out of memory");
        return NULL;
    }

    int width, height, channels;
    int desired_channels = (int)mode;

    // Load image using stb_image
    img->data = stbi_load(path, &width, &height, &channels, desired_channels);

    if (!img->data) {
        set_error("Failed to load image: %s", stbi_failure_reason());
        free(img);
        return NULL;
    }

    img->width = (uint32_t)width;
    img->height = (uint32_t)height;
    img->channels = (desired_channels > 0) ? (uint32_t)desired_channels : (uint32_t)channels;
    img->data_size = img->width * img->height * img->channels;
    img->owns_data = true;

    return img;
}
#endif /* !IMAGE_FREESTANDING */

image_t *image_load_from_memory(const void *buffer, size_t size, image_load_mode_t mode) {
    if (!buffer || size == 0) {
        set_error("Invalid buffer or size");
        return NULL;
    }

    image_t *img = calloc(1, sizeof(image_t));
    if (!img) {
        set_error("Out of memory");
        return NULL;
    }

    int width, height, channels;
    int desired_channels = (int)mode;

    // Load from memory buffer
    img->data = stbi_load_from_memory((const stbi_uc *)buffer, (int)size,
                                      &width, &height, &channels, desired_channels);

    if (!img->data) {
#ifdef IMAGE_FREESTANDING
        const char *why = stbi_failure_reason();
        set_error(why ? why : "Failed to decode image from memory");
#else
        set_error("Failed to load image from memory: %s", stbi_failure_reason());
#endif
        free(img);
        return NULL;
    }

    img->width = (uint32_t)width;
    img->height = (uint32_t)height;
    img->channels = (desired_channels > 0) ? (uint32_t)desired_channels : (uint32_t)channels;
    img->data_size = img->width * img->height * img->channels;
    img->owns_data = true;

    return img;
}

#ifndef IMAGE_FREESTANDING   /* requires fopen/fread -- hosted builds only */
image_format_t image_detect_format(const char *path) {
    if (!path) return IMAGE_FORMAT_UNKNOWN;

    FILE *f = fopen(path, "rb");
    if (!f) return IMAGE_FORMAT_UNKNOWN;

    unsigned char header[16];
    size_t read = fread(header, 1, sizeof(header), f);
    fclose(f);

    if (read < 4) return IMAGE_FORMAT_UNKNOWN;

    // PNG: 89 50 4E 47
    if (header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
        return IMAGE_FORMAT_PNG;
    }

    // JPEG: FF D8 FF
    if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
        return IMAGE_FORMAT_JPEG;
    }

    // BMP: 42 4D
    if (header[0] == 0x42 && header[1] == 0x4D) {
        return IMAGE_FORMAT_BMP;
    }

    // GIF: 47 49 46 38
    if (header[0] == 0x47 && header[1] == 0x49 && header[2] == 0x46 && header[3] == 0x38) {
        return IMAGE_FORMAT_GIF;
    }

    return IMAGE_FORMAT_UNKNOWN;
}
#endif /* !IMAGE_FREESTANDING */

void image_free(image_t *image) {
    if (!image) return;

    if (image->data && image->owns_data) {
        stbi_image_free(image->data);
    }

    free(image);
}

uint32_t image_get_pixel(const image_t *image, uint32_t x, uint32_t y) {
    if (!image || !image->data || x >= image->width || y >= image->height) {
        return 0;
    }

    size_t offset = (y * image->width + x) * image->channels;
    const uint8_t *pixel = &image->data[offset];

    uint32_t r = 0, g = 0, b = 0, a = 255;

    switch (image->channels) {
        case 1: // Grayscale
            r = g = b = pixel[0];
            break;
        case 2: // Grayscale + Alpha
            r = g = b = pixel[0];
            a = pixel[1];
            break;
        case 3: // RGB
            r = pixel[0];
            g = pixel[1];
            b = pixel[2];
            break;
        case 4: // RGBA
            r = pixel[0];
            g = pixel[1];
            b = pixel[2];
            a = pixel[3];
            break;
    }

    return (a << 24) | (r << 16) | (g << 8) | b;
}

void image_set_pixel(image_t *image, uint32_t x, uint32_t y, uint32_t rgba) {
    if (!image || !image->data || x >= image->width || y >= image->height) {
        return;
    }

    size_t offset = (y * image->width + x) * image->channels;
    uint8_t *pixel = &image->data[offset];

    uint8_t r = (rgba >> 16) & 0xFF;
    uint8_t g = (rgba >> 8) & 0xFF;
    uint8_t b = rgba & 0xFF;
    uint8_t a = (rgba >> 24) & 0xFF;

    switch (image->channels) {
        case 1: // Grayscale
            pixel[0] = (uint8_t)((r + g + b) / 3);
            break;
        case 2: // Grayscale + Alpha
            pixel[0] = (uint8_t)((r + g + b) / 3);
            pixel[1] = a;
            break;
        case 3: // RGB
            pixel[0] = r;
            pixel[1] = g;
            pixel[2] = b;
            break;
        case 4: // RGBA
            pixel[0] = r;
            pixel[1] = g;
            pixel[2] = b;
            pixel[3] = a;
            break;
    }
}

const uint8_t *image_get_pixels(const image_t *image) {
    return image ? image->data : NULL;
}

size_t image_get_stride(const image_t *image) {
    return image ? (image->width * image->channels) : 0;
}

// ============================================================================
// IMAGE MANIPULATION
// ============================================================================

image_t *image_convert_to_rgba(image_t *image, bool in_place) {
    if (!image) return NULL;
    if (image->channels == 4) return in_place ? image : image_clone(image);

    image_t *result = in_place ? image : image_create(image->width, image->height, 4);
    if (!result) return NULL;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint32_t rgba = image_get_pixel(image, x, y);
            image_set_pixel(result, x, y, rgba);
        }
    }

    if (in_place) {
        // Free old data and update
        if (image->owns_data && image->data) {
            stbi_image_free(image->data);
        }
        image->data = result->data;
        image->channels = 4;
        image->data_size = image->width * image->height * 4;
    }

    return result;
}

image_t *image_resize(const image_t *image, uint32_t new_width, uint32_t new_height) {
    if (!image || new_width == 0 || new_height == 0) return NULL;

    image_t *result = image_create(new_width, new_height, image->channels);
    if (!result) return NULL;

    // Nearest-neighbor scaling
    float x_ratio = (float)image->width / (float)new_width;
    float y_ratio = (float)image->height / (float)new_height;

    for (uint32_t y = 0; y < new_height; y++) {
        for (uint32_t x = 0; x < new_width; x++) {
            uint32_t src_x = (uint32_t)(x * x_ratio);
            uint32_t src_y = (uint32_t)(y * y_ratio);
            uint32_t pixel = image_get_pixel(image, src_x, src_y);
            image_set_pixel(result, x, y, pixel);
        }
    }

    return result;
}

image_t *image_resize_bilinear(const image_t *image, uint32_t new_width, uint32_t new_height) {
    if (!image || new_width == 0 || new_height == 0) return NULL;

    image_t *result = image_create(new_width, new_height, image->channels);
    if (!result) return NULL;

    float x_ratio = (float)(image->width - 1) / (float)new_width;
    float y_ratio = (float)(image->height - 1) / (float)new_height;

    for (uint32_t y = 0; y < new_height; y++) {
        for (uint32_t x = 0; x < new_width; x++) {
            float src_x = x * x_ratio;
            float src_y = y * y_ratio;

            uint32_t x1 = (uint32_t)src_x;
            uint32_t y1 = (uint32_t)src_y;
            uint32_t x2 = x1 + 1 < image->width ? x1 + 1 : x1;
            uint32_t y2 = y1 + 1 < image->height ? y1 + 1 : y1;

            float dx = src_x - x1;
            float dy = src_y - y1;

            uint32_t p11 = image_get_pixel(image, x1, y1);
            uint32_t p12 = image_get_pixel(image, x1, y2);
            uint32_t p21 = image_get_pixel(image, x2, y1);
            uint32_t p22 = image_get_pixel(image, x2, y2);

            // Bilinear interpolation for each channel
            uint8_t r = (uint8_t)(
                ((p11 >> 16) & 0xFF) * (1 - dx) * (1 - dy) +
                ((p21 >> 16) & 0xFF) * dx * (1 - dy) +
                ((p12 >> 16) & 0xFF) * (1 - dx) * dy +
                ((p22 >> 16) & 0xFF) * dx * dy
            );
            uint8_t g = (uint8_t)(
                ((p11 >> 8) & 0xFF) * (1 - dx) * (1 - dy) +
                ((p21 >> 8) & 0xFF) * dx * (1 - dy) +
                ((p12 >> 8) & 0xFF) * (1 - dx) * dy +
                ((p22 >> 8) & 0xFF) * dx * dy
            );
            uint8_t b = (uint8_t)(
                (p11 & 0xFF) * (1 - dx) * (1 - dy) +
                (p21 & 0xFF) * dx * (1 - dy) +
                (p12 & 0xFF) * (1 - dx) * dy +
                (p22 & 0xFF) * dx * dy
            );
            uint8_t a = (uint8_t)(
                ((p11 >> 24) & 0xFF) * (1 - dx) * (1 - dy) +
                ((p21 >> 24) & 0xFF) * dx * (1 - dy) +
                ((p12 >> 24) & 0xFF) * (1 - dx) * dy +
                ((p22 >> 24) & 0xFF) * dx * dy
            );

            uint32_t rgba = (a << 24) | (r << 16) | (g << 8) | b;
            image_set_pixel(result, x, y, rgba);
        }
    }

    return result;
}

image_t *image_crop(const image_t *image, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!image || x + width > image->width || y + height > image->height) {
        return NULL;
    }

    image_t *result = image_create(width, height, image->channels);
    if (!result) return NULL;

    for (uint32_t dy = 0; dy < height; dy++) {
        for (uint32_t dx = 0; dx < width; dx++) {
            uint32_t pixel = image_get_pixel(image, x + dx, y + dy);
            image_set_pixel(result, dx, dy, pixel);
        }
    }

    return result;
}

void image_flip_vertical(image_t *image) {
    if (!image || !image->data) return;

    size_t stride = image_get_stride(image);
    uint8_t *temp = malloc(stride);
    if (!temp) return;

    for (uint32_t y = 0; y < image->height / 2; y++) {
        uint8_t *row1 = &image->data[y * stride];
        uint8_t *row2 = &image->data[(image->height - 1 - y) * stride];

        memcpy(temp, row1, stride);
        memcpy(row1, row2, stride);
        memcpy(row2, temp, stride);
    }

    free(temp);
}

void image_flip_horizontal(image_t *image) {
    if (!image || !image->data) return;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width / 2; x++) {
            uint32_t x2 = image->width - 1 - x;
            uint32_t p1 = image_get_pixel(image, x, y);
            uint32_t p2 = image_get_pixel(image, x2, y);
            image_set_pixel(image, x, y, p2);
            image_set_pixel(image, x2, y, p1);
        }
    }
}

image_t *image_rotate_90(const image_t *image) {
    if (!image) return NULL;

    image_t *result = image_create(image->height, image->width, image->channels);
    if (!result) return NULL;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            uint32_t pixel = image_get_pixel(image, x, y);
            image_set_pixel(result, image->height - 1 - y, x, pixel);
        }
    }

    return result;
}

void image_premultiply_alpha(image_t *image) {
    if (!image || !image->data || image->channels != 4) return;

    for (uint32_t i = 0; i < image->width * image->height; i++) {
        uint8_t *pixel = &image->data[i * 4];
        uint8_t a = pixel[3];
        pixel[0] = (pixel[0] * a) / 255;
        pixel[1] = (pixel[1] * a) / 255;
        pixel[2] = (pixel[2] * a) / 255;
    }
}

// ============================================================================
// BLENDING & COMPOSITING
// ============================================================================

void image_blit(image_t *dest, const image_t *src, int32_t dest_x, int32_t dest_y) {
    if (!dest || !src) return;

    for (uint32_t y = 0; y < src->height; y++) {
        for (uint32_t x = 0; x < src->width; x++) {
            int32_t dx = dest_x + (int32_t)x;
            int32_t dy = dest_y + (int32_t)y;

            if (dx < 0 || dy < 0 || (uint32_t)dx >= dest->width || (uint32_t)dy >= dest->height) {
                continue;
            }

            uint32_t src_pixel = image_get_pixel(src, x, y);
            uint32_t dest_pixel = image_get_pixel(dest, (uint32_t)dx, (uint32_t)dy);

            uint8_t src_a = (src_pixel >> 24) & 0xFF;
            if (src_a == 255) {
                image_set_pixel(dest, (uint32_t)dx, (uint32_t)dy, src_pixel);
            } else if (src_a > 0) {
                // Alpha blend
                uint8_t dest_r = (dest_pixel >> 16) & 0xFF;
                uint8_t dest_g = (dest_pixel >> 8) & 0xFF;
                uint8_t dest_b = dest_pixel & 0xFF;

                uint8_t src_r = (src_pixel >> 16) & 0xFF;
                uint8_t src_g = (src_pixel >> 8) & 0xFF;
                uint8_t src_b = src_pixel & 0xFF;

                uint8_t r = (src_r * src_a + dest_r * (255 - src_a)) / 255;
                uint8_t g = (src_g * src_a + dest_g * (255 - src_a)) / 255;
                uint8_t b = (src_b * src_a + dest_b * (255 - src_a)) / 255;

                uint32_t blended = (255 << 24) | (r << 16) | (g << 8) | b;
                image_set_pixel(dest, (uint32_t)dx, (uint32_t)dy, blended);
            }
        }
    }
}

void image_blit_alpha(image_t *dest, const image_t *src, int32_t dest_x, int32_t dest_y, float opacity) {
    if (!dest || !src || opacity <= 0.0f) return;
    if (opacity >= 1.0f) {
        image_blit(dest, src, dest_x, dest_y);
        return;
    }

    uint8_t opacity_255 = (uint8_t)(opacity * 255.0f);

    for (uint32_t y = 0; y < src->height; y++) {
        for (uint32_t x = 0; x < src->width; x++) {
            int32_t dx = dest_x + (int32_t)x;
            int32_t dy = dest_y + (int32_t)y;

            if (dx < 0 || dy < 0 || (uint32_t)dx >= dest->width || (uint32_t)dy >= dest->height) {
                continue;
            }

            uint32_t src_pixel = image_get_pixel(src, x, y);
            uint32_t dest_pixel = image_get_pixel(dest, (uint32_t)dx, (uint32_t)dy);

            uint8_t src_a = ((src_pixel >> 24) & 0xFF);
            src_a = (src_a * opacity_255) / 255;

            if (src_a > 0) {
                uint8_t dest_r = (dest_pixel >> 16) & 0xFF;
                uint8_t dest_g = (dest_pixel >> 8) & 0xFF;
                uint8_t dest_b = dest_pixel & 0xFF;

                uint8_t src_r = (src_pixel >> 16) & 0xFF;
                uint8_t src_g = (src_pixel >> 8) & 0xFF;
                uint8_t src_b = src_pixel & 0xFF;

                uint8_t r = (src_r * src_a + dest_r * (255 - src_a)) / 255;
                uint8_t g = (src_g * src_a + dest_g * (255 - src_a)) / 255;
                uint8_t b = (src_b * src_a + dest_b * (255 - src_a)) / 255;

                uint32_t blended = (255 << 24) | (r << 16) | (g << 8) | b;
                image_set_pixel(dest, (uint32_t)dx, (uint32_t)dy, blended);
            }
        }
    }
}

void image_blit_scaled(image_t *dest, const image_t *src, int32_t dest_x, int32_t dest_y,
                       uint32_t dest_width, uint32_t dest_height) {
    if (!dest || !src) return;

    image_t *scaled = image_resize_bilinear(src, dest_width, dest_height);
    if (!scaled) return;

    image_blit(dest, scaled, dest_x, dest_y);
    image_free(scaled);
}

// ============================================================================
// ICON LOADING  (hosted only -- needs fopen via image_load, snprintf, strdup)
// ============================================================================
#ifndef IMAGE_FREESTANDING

icon_set_t *icon_load_set(const char *icon_name) {
    if (!icon_name) return NULL;

    icon_set_t *set = calloc(1, sizeof(icon_set_t));
    if (!set) return NULL;

    set->name = strdup(icon_name);
    set->images = calloc(8, sizeof(image_t *));
    set->count = 0;

    // Try to load common icon sizes
    const uint32_t sizes[] = {16, 24, 32, 48, 64, 96, 128, 256};
    char path[512];

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(path, sizeof(path), "/usr/share/icons/%s/%ux%u.png",
                 icon_name, sizes[i], sizes[i]);

        image_t *img = image_load(path, IMAGE_LOAD_RGBA);
        if (img) {
            set->images[set->count++] = img;
        }
    }

    if (set->count == 0) {
        free((char *)set->name);
        free(set->images);
        free(set);
        return NULL;
    }

    return set;
}

image_t *icon_get_best_size(const icon_set_t *set, uint32_t target_size) {
    if (!set || set->count == 0) return NULL;

    // Find exact match first
    for (uint32_t i = 0; i < set->count; i++) {
        if (set->images[i]->width == target_size) {
            return set->images[i];
        }
    }

    // Find closest size (prefer larger)
    image_t *best = set->images[0];
    uint32_t best_diff = abs((int)set->images[0]->width - (int)target_size);

    for (uint32_t i = 1; i < set->count; i++) {
        uint32_t diff = abs((int)set->images[i]->width - (int)target_size);
        if (diff < best_diff ||
            (diff == best_diff && set->images[i]->width > best->width)) {
            best = set->images[i];
            best_diff = diff;
        }
    }

    return best;
}

void icon_free_set(icon_set_t *set) {
    if (!set) return;

    for (uint32_t i = 0; i < set->count; i++) {
        image_free(set->images[i]);
    }

    free((char *)set->name);
    free(set->images);
    free(set);
}
#endif /* !IMAGE_FREESTANDING */

// ============================================================================
// UTILITIES
// ============================================================================

image_t *image_create(uint32_t width, uint32_t height, uint32_t channels) {
    if (width == 0 || height == 0 || channels == 0 || channels > 4) {
        set_error("Invalid dimensions or channel count");
        return NULL;
    }

    image_t *img = calloc(1, sizeof(image_t));
    if (!img) {
        set_error("Out of memory");
        return NULL;
    }

    img->width = width;
    img->height = height;
    img->channels = channels;
    img->data_size = width * height * channels;
    img->data = calloc(1, img->data_size);
    img->owns_data = true;

    if (!img->data) {
        free(img);
        set_error("Out of memory");
        return NULL;
    }

    return img;
}

image_t *image_clone(const image_t *image) {
    if (!image) return NULL;

    image_t *clone = image_create(image->width, image->height, image->channels);
    if (!clone) return NULL;

    memcpy(clone->data, image->data, image->data_size);

    return clone;
}

void image_fill(image_t *image, uint32_t rgba) {
    if (!image || !image->data) return;

    for (uint32_t y = 0; y < image->height; y++) {
        for (uint32_t x = 0; x < image->width; x++) {
            image_set_pixel(image, x, y, rgba);
        }
    }
}

const char *image_format_name(image_format_t format) {
    switch (format) {
        case IMAGE_FORMAT_PNG: return "PNG";
        case IMAGE_FORMAT_JPEG: return "JPEG";
        case IMAGE_FORMAT_BMP: return "BMP";
        case IMAGE_FORMAT_GIF: return "GIF";
        case IMAGE_FORMAT_TGA: return "TGA";
        case IMAGE_FORMAT_PSD: return "PSD";
        case IMAGE_FORMAT_HDR: return "HDR";
        default: return "Unknown";
    }
}

// ============================================================================
// FRAMEBUFFER UTILITIES
// ============================================================================

void image_blit_to_framebuffer(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                                const image_t *image, int32_t x, int32_t y) {
    if (!fb || !image) return;

    for (uint32_t iy = 0; iy < image->height; iy++) {
        for (uint32_t ix = 0; ix < image->width; ix++) {
            int32_t fx = x + (int32_t)ix;
            int32_t fy = y + (int32_t)iy;

            if (fx < 0 || fy < 0 || (uint32_t)fx >= fb_width || (uint32_t)fy >= fb_height) {
                continue;
            }

            uint32_t pixel = image_get_pixel(image, ix, iy);
            uint8_t alpha = (pixel >> 24) & 0xFF;

            if (alpha == 255) {
                fb[fy * fb_width + fx] = pixel;
            } else if (alpha > 0) {
                // Alpha blend with existing framebuffer pixel
                uint32_t dest = fb[fy * fb_width + fx];
                uint8_t dest_r = (dest >> 16) & 0xFF;
                uint8_t dest_g = (dest >> 8) & 0xFF;
                uint8_t dest_b = dest & 0xFF;

                uint8_t src_r = (pixel >> 16) & 0xFF;
                uint8_t src_g = (pixel >> 8) & 0xFF;
                uint8_t src_b = pixel & 0xFF;

                uint8_t r = (src_r * alpha + dest_r * (255 - alpha)) / 255;
                uint8_t g = (src_g * alpha + dest_g * (255 - alpha)) / 255;
                uint8_t b = (src_b * alpha + dest_b * (255 - alpha)) / 255;

                fb[fy * fb_width + fx] = (255 << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

void image_blit_to_framebuffer_scaled(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                                      const image_t *image, int32_t x, int32_t y,
                                      uint32_t dest_width, uint32_t dest_height) {
    if (!fb || !image) return;

    image_t *scaled = image_resize_bilinear(image, dest_width, dest_height);
    if (!scaled) return;

    image_blit_to_framebuffer(fb, fb_width, fb_height, scaled, x, y);
    image_free(scaled);
}
