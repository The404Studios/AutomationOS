/**
 * Double-Buffering Visual Demo
 *
 * Demonstrates the difference between double-buffered rendering with VSync
 * and without VSync. Shows moving objects to make tearing visible.
 *
 * Usage:
 *   ./demo_double_buffer [vsync|no-vsync]
 */

#include "fb_compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#define DEMO_DURATION 30  // seconds

// Create a colorful gradient window
window_t *create_gradient_window(uint32_t id, int32_t x, int32_t y,
                                 uint32_t width, uint32_t height) {
    window_t *window = window_create(id, WINDOW_NORMAL, x, y, width, height);
    if (!window || !window->surface) {
        return NULL;
    }

    // Create horizontal gradient (rainbow)
    for (uint32_t py = 0; py < height; py++) {
        for (uint32_t px = 0; px < width; px++) {
            float t = (float)px / width;
            uint8_t r = (uint8_t)(255 * (0.5 + 0.5 * sin(t * 6.28)));
            uint8_t g = (uint8_t)(255 * (0.5 + 0.5 * sin(t * 6.28 + 2.09)));
            uint8_t b = (uint8_t)(255 * (0.5 + 0.5 * sin(t * 6.28 + 4.18)));
            uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;
            window->surface->pixels[py * width + px] = color;
        }
    }

    window->surface->dirty = true;
    return window;
}

// Create a checkerboard pattern window (good for detecting tearing)
window_t *create_checkerboard_window(uint32_t id, int32_t x, int32_t y,
                                     uint32_t width, uint32_t height) {
    window_t *window = window_create(id, WINDOW_NORMAL, x, y, width, height);
    if (!window || !window->surface) {
        return NULL;
    }

    const uint32_t square_size = 20;
    for (uint32_t py = 0; py < height; py++) {
        for (uint32_t px = 0; px < width; px++) {
            bool is_white = ((px / square_size) + (py / square_size)) % 2 == 0;
            uint32_t color = is_white ? 0xFFFFFFFF : 0xFF000000;
            window->surface->pixels[py * width + px] = color;
        }
    }

    window->surface->dirty = true;
    return window;
}

// Draw FPS counter on screen
void draw_fps_overlay(fb_compositor_t *comp) {
    char fps_text[64];
    snprintf(fps_text, sizeof(fps_text), "FPS: %u | Frame Time: %.2f ms",
             fb_compositor_get_fps(comp),
             fb_compositor_get_frame_time(comp) / 1000.0);
    printf("\r%s", fps_text);
    fflush(stdout);
}

int main(int argc, char **argv) {
    bool enable_vsync = true;

    if (argc > 1) {
        if (strcmp(argv[1], "no-vsync") == 0) {
            enable_vsync = false;
        }
    }

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║         Double-Buffering Visual Demo                        ║\n");
    printf("║                                                              ║\n");
    printf("║  VSync: %-8s                                          ║\n",
           enable_vsync ? "ENABLED" : "DISABLED");
    printf("║                                                              ║\n");
    printf("║  Watch the moving windows for tearing artifacts.            ║\n");
    printf("║  With VSync: smooth, no tearing                             ║\n");
    printf("║  Without VSync: tearing visible during movement             ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    // Initialize compositor
    fb_compositor_t *comp = fb_compositor_init();
    if (!comp) {
        fprintf(stderr, "Failed to initialize compositor\n");
        return 1;
    }

    fb_compositor_set_vsync(comp, enable_vsync);

    // Create test windows
    window_t *gradient_window = create_gradient_window(1, 100, 50, 300, 200);
    window_t *checker_window = create_checkerboard_window(2, 150, 300, 350, 250);

    if (!gradient_window || !checker_window) {
        fprintf(stderr, "Failed to create test windows\n");
        fb_compositor_cleanup(comp);
        return 1;
    }

    fb_compositor_add_window(comp, gradient_window);
    fb_compositor_add_window(comp, checker_window);

    // Animation parameters
    int32_t gradient_vel_x = 3;
    int32_t checker_vel_x = -4;
    int32_t checker_vel_y = 2;

    printf("Running demo for %d seconds...\n\n", DEMO_DURATION);

    uint64_t start_time = time(NULL);
    uint64_t last_stats_time = start_time;

    // Main loop
    while ((time(NULL) - start_time) < DEMO_DURATION) {
        // Update gradient window position (horizontal motion)
        gradient_window->geometry.x += gradient_vel_x;
        if (gradient_window->geometry.x <= 0 ||
            gradient_window->geometry.x >= 500) {
            gradient_vel_x = -gradient_vel_x;
        }

        // Update checkerboard window position (diagonal motion)
        checker_window->geometry.x += checker_vel_x;
        checker_window->geometry.y += checker_vel_y;

        // Bounce off edges
        if (checker_window->geometry.x <= 0 ||
            checker_window->geometry.x >= 350) {
            checker_vel_x = -checker_vel_x;
        }
        if (checker_window->geometry.y <= 0 ||
            checker_window->geometry.y >= 400) {
            checker_vel_y = -checker_vel_y;
        }

        // Mark windows as damaged
        damage_add_region(&comp->damage, &gradient_window->geometry);
        damage_add_region(&comp->damage, &checker_window->geometry);

        // Render frame
        fb_compositor_frame(comp);

        // Show FPS every frame
        draw_fps_overlay(comp);

        // Print detailed stats every 5 seconds
        if (time(NULL) - last_stats_time >= 5) {
            printf("\n[Stats] FPS: %u | Frame Time: %.2f ms ± variance\n",
                   fb_compositor_get_fps(comp),
                   fb_compositor_get_frame_time(comp) / 1000.0);
            last_stats_time = time(NULL);
        }
    }

    printf("\n\nDemo complete!\n");
    printf("Final FPS: %u\n", fb_compositor_get_fps(comp));
    printf("Average Frame Time: %.2f ms\n",
           fb_compositor_get_frame_time(comp) / 1000.0);

    if (enable_vsync) {
        printf("\nExpected: ~60 FPS, stable frame times, NO tearing\n");
    } else {
        printf("\nExpected: Higher FPS, variable frame times, VISIBLE tearing\n");
    }

    // Cleanup
    fb_compositor_remove_window(comp, 1);
    fb_compositor_remove_window(comp, 2);
    fb_compositor_cleanup(comp);

    return 0;
}
