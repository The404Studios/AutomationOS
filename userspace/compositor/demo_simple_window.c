/**
 * Simple Window Demo
 *
 * Demonstrates basic window creation and rendering
 */

#include "compositor.h"
#include "gpu.h"
#include "../wm/window_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile bool g_running = true;

/**
 * Signal handler for graceful shutdown
 */
void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

/**
 * Draw simple content to window surface
 */
void draw_window_content(window_t *window) {
    if (!window || !window->surface) return;

    uint32_t *pixels = window->surface->pixels;
    uint32_t width = window->surface->width;
    uint32_t height = window->surface->height;

    // Fill with gradient
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r = (x * 255) / width;
            uint8_t g = (y * 255) / height;
            uint8_t b = 128;
            uint8_t a = 255;

            pixels[y * width + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    window->surface->dirty = true;
}

int main(int argc, char *argv[]) {
    printf("=== AutomationOS Simple Window Demo ===\n\n");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize compositor
    printf("[Demo] Initializing compositor...\n");
    compositor_t *comp = compositor_init("/dev/dri/card0");
    if (!comp) {
        fprintf(stderr, "Failed to initialize compositor\n");
        return 1;
    }

    // Add display
    display_t *display = display_create(0, 1920, 1080, 60);
    if (!display) {
        fprintf(stderr, "Failed to create display\n");
        compositor_cleanup(comp);
        return 1;
    }
    display->primary = true;

    if (compositor_add_display(comp, display) < 0) {
        fprintf(stderr, "Failed to add display to compositor\n");
        display_cleanup(display);
        compositor_cleanup(comp);
        return 1;
    }

    // Initialize window manager
    printf("[Demo] Initializing window manager...\n");
    window_manager_t *wm = wm_init(comp);
    if (!wm) {
        fprintf(stderr, "Failed to initialize window manager\n");
        compositor_cleanup(comp);
        return 1;
    }

    // Create windows
    printf("[Demo] Creating windows...\n");

    window_t *window1 = wm_create_window(wm, WINDOW_NORMAL, 800, 600, "Demo Window 1");
    if (window1) {
        draw_window_content(window1);
        wm_map_window(wm, window1);
    }

    window_t *window2 = wm_create_window(wm, WINDOW_NORMAL, 640, 480, "Demo Window 2");
    if (window2) {
        window2->geometry.x = 200;
        window2->geometry.y = 150;
        draw_window_content(window2);
        wm_map_window(wm, window2);
    }

    window_t *window3 = wm_create_window(wm, WINDOW_UTILITY, 400, 300, "Utility Window");
    if (window3) {
        window3->geometry.x = 300;
        window3->geometry.y = 250;
        draw_window_content(window3);
        wm_map_window(wm, window3);
    }

    printf("[Demo] Starting render loop...\n");
    printf("[Demo] Press Ctrl+C to exit\n\n");

    // Main loop
    uint32_t frame_count = 0;
    while (g_running) {
        // Update window manager
        wm_update(wm);

        // Render frame
        compositor_frame(comp);

        frame_count++;

        // Print FPS every 60 frames
        if (frame_count % 60 == 0) {
            uint32_t fps = compositor_get_fps(comp);
            printf("[Demo] FPS: %u\n", fps);
        }

        // Sleep to cap frame rate (demo only - real compositor doesn't need this)
        usleep(16667);  // ~60 FPS
    }

    // Cleanup
    printf("\n[Demo] Cleaning up...\n");
    wm_cleanup(wm);
    compositor_cleanup(comp);

    printf("[Demo] Done!\n");
    return 0;
}
