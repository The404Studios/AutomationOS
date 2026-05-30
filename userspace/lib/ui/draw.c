/**
 * Drawing Utilities - Implementation
 */

#include "draw.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Helper to check bounds
static inline bool in_bounds(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    return x >= 0 && y >= 0 && (uint32_t)x < width && (uint32_t)y < height;
}

void draw_pixel(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                int32_t x, int32_t y, uint32_t color) {
    if (!fb || !in_bounds(x, y, fb_width, fb_height)) return;

    uint32_t idx = (uint32_t)y * fb_width + (uint32_t)x;

    // Alpha blend if source has transparency
    uint8_t alpha = ALPHA(color);
    if (alpha == 255) {
        fb[idx] = color;
    } else if (alpha > 0) {
        fb[idx] = draw_blend(color, fb[idx]);
    }
}

uint32_t draw_get_pixel(const uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                        int32_t x, int32_t y) {
    if (!fb || !in_bounds(x, y, fb_width, fb_height)) return 0;
    return fb[(uint32_t)y * fb_width + (uint32_t)x];
}

uint32_t draw_blend(uint32_t src, uint32_t dst) {
    uint8_t src_a = ALPHA(src);
    if (src_a == 0) return dst;
    if (src_a == 255) return src;

    uint8_t dst_a = ALPHA(dst);
    uint8_t src_r = RED(src);
    uint8_t src_g = GREEN(src);
    uint8_t src_b = BLUE(src);
    uint8_t dst_r = RED(dst);
    uint8_t dst_g = GREEN(dst);
    uint8_t dst_b = BLUE(dst);

    // Alpha compositing (over operation)
    uint8_t out_a = src_a + dst_a * (255 - src_a) / 255;
    uint8_t out_r = (src_r * src_a + dst_r * dst_a * (255 - src_a) / 255) / (out_a ? out_a : 1);
    uint8_t out_g = (src_g * src_a + dst_g * dst_a * (255 - src_a) / 255) / (out_a ? out_a : 1);
    uint8_t out_b = (src_b * src_a + dst_b * dst_a * (255 - src_a) / 255) / (out_a ? out_a : 1);

    return ARGB(out_a, out_r, out_g, out_b);
}

void draw_clear(uint32_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t color) {
    if (!fb) return;

    size_t count = fb_width * fb_height;
    for (size_t i = 0; i < count; i++) {
        fb[i] = color;
    }
}

void draw_fill_rect(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                    int32_t x, int32_t y, uint32_t width, uint32_t height,
                    uint32_t color) {
    if (!fb) return;

    // Clip to framebuffer
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if ((uint32_t)x >= fb_width || (uint32_t)y >= fb_height) return;
    if ((uint32_t)x + width > fb_width) width = fb_width - (uint32_t)x;
    if ((uint32_t)y + height > fb_height) height = fb_height - (uint32_t)y;

    uint8_t alpha = ALPHA(color);

    if (alpha == 255) {
        // Opaque fill - fast path
        for (uint32_t row = 0; row < height; row++) {
            uint32_t *line = &fb[((uint32_t)y + row) * fb_width + (uint32_t)x];
            for (uint32_t col = 0; col < width; col++) {
                line[col] = color;
            }
        }
    } else if (alpha > 0) {
        // Alpha blended fill
        for (uint32_t row = 0; row < height; row++) {
            for (uint32_t col = 0; col < width; col++) {
                draw_pixel(fb, fb_width, fb_height, x + col, y + row, color);
            }
        }
    }
}

void draw_rect(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
               int32_t x, int32_t y, uint32_t width, uint32_t height,
               uint32_t color, uint32_t thickness) {
    if (!fb || thickness == 0) return;

    // Top edge
    draw_fill_rect(fb, fb_width, fb_height, x, y, width, thickness, color);
    // Bottom edge
    draw_fill_rect(fb, fb_width, fb_height, x, y + height - thickness, width, thickness, color);
    // Left edge
    draw_fill_rect(fb, fb_width, fb_height, x, y + thickness, thickness, height - 2 * thickness, color);
    // Right edge
    draw_fill_rect(fb, fb_width, fb_height, x + width - thickness, y + thickness, thickness, height - 2 * thickness, color);
}

void draw_hline(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                int32_t x, int32_t y, uint32_t length, uint32_t color) {
    draw_fill_rect(fb, fb_width, fb_height, x, y, length, 1, color);
}

void draw_vline(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                int32_t x, int32_t y, uint32_t length, uint32_t color) {
    draw_fill_rect(fb, fb_width, fb_height, x, y, 1, length, color);
}

void draw_line(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
               int32_t x1, int32_t y1, int32_t x2, int32_t y2,
               uint32_t color, uint32_t thickness) {
    if (!fb) return;

    // Bresenham's line algorithm
    int32_t dx = abs(x2 - x1);
    int32_t dy = abs(y2 - y1);
    int32_t sx = x1 < x2 ? 1 : -1;
    int32_t sy = y1 < y2 ? 1 : -1;
    int32_t err = dx - dy;

    while (1) {
        // Draw thick line by drawing circle at each point
        if (thickness == 1) {
            draw_pixel(fb, fb_width, fb_height, x1, y1, color);
        } else {
            draw_circle(fb, fb_width, fb_height, x1, y1, thickness / 2, color);
        }

        if (x1 == x2 && y1 == y2) break;

        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

void draw_circle(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                 int32_t cx, int32_t cy, uint32_t radius, uint32_t color) {
    if (!fb || radius == 0) return;

    // Midpoint circle algorithm
    int32_t r = (int32_t)radius;
    for (int32_t y = -r; y <= r; y++) {
        for (int32_t x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                draw_pixel(fb, fb_width, fb_height, cx + x, cy + y, color);
            }
        }
    }
}

void draw_rounded_rect(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                       int32_t x, int32_t y, uint32_t width, uint32_t height,
                       uint32_t radius, uint32_t color) {
    if (!fb) return;

    if (radius == 0 || radius * 2 > width || radius * 2 > height) {
        // Fall back to regular rectangle
        draw_fill_rect(fb, fb_width, fb_height, x, y, width, height, color);
        return;
    }

    // Draw main rectangles
    draw_fill_rect(fb, fb_width, fb_height, x + radius, y, width - 2 * radius, height, color);
    draw_fill_rect(fb, fb_width, fb_height, x, y + radius, radius, height - 2 * radius, color);
    draw_fill_rect(fb, fb_width, fb_height, x + width - radius, y + radius, radius, height - 2 * radius, color);

    // Draw corners
    int32_t r = (int32_t)radius;
    for (int32_t dy = 0; dy < r; dy++) {
        for (int32_t dx = 0; dx < r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                // Top-left
                draw_pixel(fb, fb_width, fb_height, x + radius - dx - 1, y + radius - dy - 1, color);
                // Top-right
                draw_pixel(fb, fb_width, fb_height, x + width - radius + dx, y + radius - dy - 1, color);
                // Bottom-left
                draw_pixel(fb, fb_width, fb_height, x + radius - dx - 1, y + height - radius + dy, color);
                // Bottom-right
                draw_pixel(fb, fb_width, fb_height, x + width - radius + dx, y + height - radius + dy, color);
            }
        }
    }
}

void draw_blur_region(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                      int32_t x, int32_t y, uint32_t width, uint32_t height,
                      uint32_t radius) {
    if (!fb || radius == 0) return;

    // Simple box blur (approximation of Gaussian blur)
    // For performance, we only do one pass

    uint32_t *temp = malloc(width * height * sizeof(uint32_t));
    if (!temp) return;

    // Clip to framebuffer
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if ((uint32_t)x >= fb_width || (uint32_t)y >= fb_height) {
        free(temp);
        return;
    }
    if ((uint32_t)x + width > fb_width) width = fb_width - (uint32_t)x;
    if ((uint32_t)y + height > fb_height) height = fb_height - (uint32_t)y;

    int32_t r = (int32_t)radius;

    // Horizontal pass
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t col = 0; col < width; col++) {
            uint32_t sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
            uint32_t count = 0;

            for (int32_t dx = -r; dx <= r; dx++) {
                int32_t px = x + col + dx;
                int32_t py = y + row;
                if (in_bounds(px, py, fb_width, fb_height)) {
                    uint32_t pixel = fb[py * fb_width + px];
                    sum_a += ALPHA(pixel);
                    sum_r += RED(pixel);
                    sum_g += GREEN(pixel);
                    sum_b += BLUE(pixel);
                    count++;
                }
            }

            if (count > 0) {
                temp[row * width + col] = ARGB(sum_a / count, sum_r / count, sum_g / count, sum_b / count);
            }
        }
    }

    // Write back
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t col = 0; col < width; col++) {
            fb[(y + row) * fb_width + (x + col)] = temp[row * width + col];
        }
    }

    free(temp);
}

void draw_shadow(uint32_t *fb, uint32_t fb_width, uint32_t fb_height,
                 int32_t x, int32_t y, uint32_t width, uint32_t height,
                 uint32_t blur, uint8_t opacity) {
    if (!fb) return;

    // Draw shadow as semi-transparent black rectangle with blur
    uint32_t shadow_color = ARGB(opacity, 0, 0, 0);

    // Offset shadow slightly
    int32_t offset = blur / 2;
    draw_fill_rect(fb, fb_width, fb_height, x + offset, y + offset, width, height, shadow_color);

    if (blur > 0) {
        draw_blur_region(fb, fb_width, fb_height, x + offset, y + offset, width, height, blur);
    }
}
