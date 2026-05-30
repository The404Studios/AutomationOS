/**
 * Framebuffer Access Module - Implementation
 *
 * Provides userspace access to the kernel framebuffer via memory mapping.
 */

#include "fb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

/**
 * Initialize and map the framebuffer
 */
framebuffer_t *fb_init(void) {
    framebuffer_t *fb = calloc(1, sizeof(framebuffer_t));
    if (!fb) {
        fprintf(stderr, "[FB] Failed to allocate framebuffer structure\n");
        return NULL;
    }

    // Try direct physical framebuffer access first (bare metal)
    // Kernel framebuffer is mapped at 0x40000000 (1024x768x32)
    fb->mapped_addr = (void*)0x40000000;
    fb->width = 1024;
    fb->height = 768;
    fb->bpp = 32;
    fb->pitch = 4096;  // 1024 * 4 bytes
    fb->size = fb->pitch * fb->height;
    fb->pixels = (uint32_t *)fb->mapped_addr;
    fb->fd = -2;  // Mark as direct physical access

    printf("[FB] Direct framebuffer initialized: %dx%d at 0x%p\n",
           fb->width, fb->height, fb->mapped_addr);
    return fb;

    // Open framebuffer device (Linux-style, if kernel has /dev/fb0)
    fb->fd = open("/dev/fb0", O_RDWR);
    if (fb->fd < 0) {
        fprintf(stderr, "[FB] Failed to open /dev/fb0\n");
        fprintf(stderr, "[FB] Falling back to simulated framebuffer (1024x768)\n");

        // Fallback to simulated framebuffer for testing
        fb->width = 1024;
        fb->height = 768;
        fb->bpp = 32;
        fb->pitch = fb->width * 4;
        fb->size = fb->pitch * fb->height;

        // Allocate memory for simulated framebuffer
        fb->mapped_addr = calloc(1, fb->size);
        if (!fb->mapped_addr) {
            fprintf(stderr, "[FB] Failed to allocate simulated framebuffer\n");
            free(fb);
            return NULL;
        }

        fb->pixels = (uint32_t *)fb->mapped_addr;
        fb->fd = -1;  // Mark as simulated

        printf("[FB] Simulated framebuffer initialized: %dx%d\n", fb->width, fb->height);
        return fb;
    }

    // Get fixed screen info
    struct fb_fix_screeninfo fix_info;
    if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix_info) < 0) {
        fprintf(stderr, "[FB] Failed to get fixed screen info\n");
        close(fb->fd);
        free(fb);
        return NULL;
    }

    // Get variable screen info
    struct fb_var_screeninfo var_info;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var_info) < 0) {
        fprintf(stderr, "[FB] Failed to get variable screen info\n");
        close(fb->fd);
        free(fb);
        return NULL;
    }

    // Extract framebuffer parameters
    fb->width = var_info.xres;
    fb->height = var_info.yres;
    fb->bpp = var_info.bits_per_pixel;
    fb->pitch = fix_info.line_length;
    fb->size = fb->pitch * fb->height;

    // Validate that we support this framebuffer format
    if (fb->bpp != 32) {
        fprintf(stderr, "[FB] Unsupported color depth: %d bpp (need 32 bpp)\n", fb->bpp);
        close(fb->fd);
        free(fb);
        return NULL;
    }

    // Memory map the framebuffer
    fb->mapped_addr = mmap(NULL, fb->size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fb->fd, 0);
    if (fb->mapped_addr == MAP_FAILED) {
        fprintf(stderr, "[FB] Failed to mmap framebuffer\n");
        close(fb->fd);
        free(fb);
        return NULL;
    }

    fb->pixels = (uint32_t *)fb->mapped_addr;

    printf("[FB] Framebuffer mapped successfully:\n");
    printf("[FB]   Resolution: %dx%d\n", fb->width, fb->height);
    printf("[FB]   BPP: %d\n", fb->bpp);
    printf("[FB]   Pitch: %d bytes\n", fb->pitch);
    printf("[FB]   Size: %zu bytes (%.2f MB)\n", fb->size, fb->size / (1024.0 * 1024.0));

    return fb;
}

/**
 * Cleanup and unmap framebuffer
 */
void fb_cleanup(framebuffer_t *fb) {
    if (!fb) return;

    if (fb->fd >= 0) {
        // Real framebuffer - unmap
        if (fb->mapped_addr != MAP_FAILED) {
            munmap(fb->mapped_addr, fb->size);
        }
        close(fb->fd);
    } else if (fb->fd == -1) {
        // Simulated framebuffer - free memory
        free(fb->mapped_addr);
    } else if (fb->fd == -2) {
        // Direct physical framebuffer - no cleanup needed
        printf("[FB] Direct physical framebuffer detached\n");
    }

    free(fb);
    printf("[FB] Framebuffer unmapped and cleaned up\n");
}

/**
 * Clear framebuffer to solid color
 */
void fb_clear(framebuffer_t *fb, uint32_t color) {
    if (!fb || !fb->pixels) return;

    // Fast memset for the first line
    uint32_t pixels_per_line = fb->pitch / 4;
    for (uint32_t x = 0; x < fb->width; x++) {
        fb->pixels[x] = color;
    }

    // Copy first line to remaining lines for speed
    for (uint32_t y = 1; y < fb->height; y++) {
        memcpy(&fb->pixels[y * pixels_per_line],
               &fb->pixels[0],
               fb->width * 4);
    }
}

/**
 * Fill a rectangular region
 */
void fb_fill_rect(framebuffer_t *fb, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t color) {
    if (!fb || !fb->pixels) return;

    // Clip to framebuffer bounds
    if (x >= fb->width || y >= fb->height) return;

    uint32_t x_end = (x + width > fb->width) ? fb->width : x + width;
    uint32_t y_end = (y + height > fb->height) ? fb->height : y + height;

    uint32_t pixels_per_line = fb->pitch / 4;

    for (uint32_t row = y; row < y_end; row++) {
        for (uint32_t col = x; col < x_end; col++) {
            fb->pixels[row * pixels_per_line + col] = color;
        }
    }
}

/**
 * Blit (copy) pixel data to framebuffer
 */
void fb_blit(framebuffer_t *fb, uint32_t dst_x, uint32_t dst_y,
             const uint32_t *src_pixels, uint32_t src_width,
             uint32_t src_height, uint32_t src_pitch) {
    if (!fb || !fb->pixels || !src_pixels) return;

    // Clip to framebuffer bounds
    if (dst_x >= fb->width || dst_y >= fb->height) return;

    uint32_t copy_width = (dst_x + src_width > fb->width) ?
                          fb->width - dst_x : src_width;
    uint32_t copy_height = (dst_y + src_height > fb->height) ?
                           fb->height - dst_y : src_height;

    uint32_t dst_pixels_per_line = fb->pitch / 4;
    uint32_t src_pixels_per_line = src_pitch / 4;

    for (uint32_t y = 0; y < copy_height; y++) {
        memcpy(&fb->pixels[(dst_y + y) * dst_pixels_per_line + dst_x],
               &src_pixels[y * src_pixels_per_line],
               copy_width * 4);
    }
}
