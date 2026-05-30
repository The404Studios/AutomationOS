/**
 * Rendering Helpers - Implementation
 *
 * Simple framebuffer drawing functions
 */

#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

// For Linux framebuffer on real hardware
#ifdef __linux__
#include <linux/fb.h>
#endif

// ============================================================================
// FRAMEBUFFER INITIALIZATION
// ============================================================================

framebuffer_t *fb_init(void) {
    framebuffer_t *fb = calloc(1, sizeof(framebuffer_t));
    if (!fb) {
        fprintf(stderr, "[FB] Failed to allocate framebuffer structure\n");
        return NULL;
    }

#ifdef __linux__
    // Try to open real framebuffer device
    fb->fd = open("/dev/fb0", O_RDWR);
    if (fb->fd >= 0) {
        struct fb_var_screeninfo vinfo;
        struct fb_fix_screeninfo finfo;

        if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
            ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) == 0) {

            fb->width = vinfo.xres;
            fb->height = vinfo.yres;
            fb->bpp = vinfo.bits_per_pixel;
            fb->pitch = finfo.line_length;
            fb->size = fb->pitch * fb->height;

            // Map framebuffer memory
            fb->mapped_addr = mmap(NULL, fb->size, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fb->fd, 0);
            if (fb->mapped_addr == MAP_FAILED) {
                fprintf(stderr, "[FB] Failed to mmap framebuffer\n");
                close(fb->fd);
                free(fb);
                return NULL;
            }

            fb->pixels = (uint32_t *)fb->mapped_addr;

            printf("[FB] Hardware framebuffer: %ux%u @ %u bpp\n",
                   fb->width, fb->height, fb->bpp);

            return fb;
        }

        close(fb->fd);
    }
#endif

    // Fallback to simulated framebuffer
    fprintf(stderr, "[FB] Hardware framebuffer not available, using simulation\n");

    fb->width = 1920;
    fb->height = 1080;
    fb->bpp = 32;
    fb->pitch = fb->width * 4;
    fb->size = fb->pitch * fb->height;
    fb->fd = -1;

    // Allocate simulated framebuffer
    fb->mapped_addr = calloc(1, fb->size);
    if (!fb->mapped_addr) {
        fprintf(stderr, "[FB] Failed to allocate simulated framebuffer\n");
        free(fb);
        return NULL;
    }

    fb->pixels = (uint32_t *)fb->mapped_addr;

    printf("[FB] Simulated framebuffer: %ux%u\n", fb->width, fb->height);

    return fb;
}

// ============================================================================
// CLEANUP
// ============================================================================

void fb_cleanup(framebuffer_t *fb) {
    if (!fb) {
        return;
    }

    if (fb->mapped_addr) {
        if (fb->fd >= 0) {
            // Unmap hardware framebuffer
            munmap(fb->mapped_addr, fb->size);
        } else {
            // Free simulated framebuffer
            free(fb->mapped_addr);
        }
    }

    if (fb->fd >= 0) {
        close(fb->fd);
    }

    free(fb);
}

// ============================================================================
// BASIC DRAWING
// ============================================================================

void fb_clear(framebuffer_t *fb, uint32_t color) {
    if (!fb || !fb->pixels) {
        return;
    }

    uint32_t pixel_count = fb->width * fb->height;
    for (uint32_t i = 0; i < pixel_count; i++) {
        fb->pixels[i] = color;
    }
}

void fb_put_pixel(framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t color) {
    if (!fb || !fb->pixels) {
        return;
    }

    if (x >= fb->width || y >= fb->height) {
        return;
    }

    uint32_t offset = y * (fb->pitch / 4) + x;
    fb->pixels[offset] = color;
}

void fb_fill_rect(framebuffer_t *fb, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t color) {
    if (!fb || !fb->pixels) {
        return;
    }

    // Clip to screen bounds
    if (x >= fb->width || y >= fb->height) {
        return;
    }

    if (x + width > fb->width) {
        width = fb->width - x;
    }

    if (y + height > fb->height) {
        height = fb->height - y;
    }

    // Draw rectangle
    for (uint32_t dy = 0; dy < height; dy++) {
        uint32_t offset = (y + dy) * (fb->pitch / 4) + x;
        for (uint32_t dx = 0; dx < width; dx++) {
            fb->pixels[offset + dx] = color;
        }
    }
}

void fb_draw_rect(framebuffer_t *fb, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t color) {
    if (!fb || !fb->pixels || width == 0 || height == 0) {
        return;
    }

    // Draw top and bottom edges
    fb_fill_rect(fb, x, y, width, 1, color);
    fb_fill_rect(fb, x, y + height - 1, width, 1, color);

    // Draw left and right edges
    fb_fill_rect(fb, x, y, 1, height, color);
    fb_fill_rect(fb, x + width - 1, y, 1, height, color);
}

// ============================================================================
// TEXT RENDERING (STUB)
// ============================================================================

void draw_text(framebuffer_t *fb, uint32_t x, uint32_t y,
               const char *text, uint32_t color) {
    // TODO: Implement basic bitmap font rendering
    // For now, this is a stub

    (void)fb;
    (void)x;
    (void)y;
    (void)text;
    (void)color;
}

// ============================================================================
// LINE DRAWING
// ============================================================================

void draw_line(framebuffer_t *fb, int32_t x0, int32_t y0,
               int32_t x1, int32_t y1, uint32_t color) {
    if (!fb) {
        return;
    }

    // Bresenham's line algorithm
    int32_t dx = abs(x1 - x0);
    int32_t dy = abs(y1 - y0);
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;

    while (true) {
        if (x0 >= 0 && x0 < (int32_t)fb->width &&
            y0 >= 0 && y0 < (int32_t)fb->height) {
            fb_put_pixel(fb, x0, y0, color);
        }

        if (x0 == x1 && y0 == y1) {
            break;
        }

        int32_t e2 = 2 * err;

        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }

        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}
