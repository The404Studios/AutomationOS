/**
 * AutomationOS Minimal Compositor - Main Event Loop
 *
 * A simple userspace compositor that:
 * - Maps the framebuffer into userspace
 * - Receives window events from the window manager
 * - Composites all windows at 60 FPS
 * - Handles double buffering to prevent tearing
 *
 * This is a minimal reference implementation without GPU acceleration.
 * For production use, see the GPU-accelerated compositor in compositor.c
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>

#include "fb.h"
#include "render.h"
#include "input.h"

// Timing constants
#define TARGET_FPS 60
#define FRAME_TIME_US (1000000 / TARGET_FPS)  // 16667 microseconds

// Global state
static bool running = true;
static framebuffer_t *fb = NULL;
static renderer_t *renderer = NULL;
static input_handler_t *input = NULL;

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int sig) {
    (void)sig;
    printf("\n[Compositor] Shutting down gracefully...\n");
    running = false;
}

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Sleep for remaining frame time to maintain target FPS
 */
static void frame_sleep(uint64_t frame_start_us) {
    uint64_t now = get_time_us();
    uint64_t elapsed = now - frame_start_us;

    if (elapsed < FRAME_TIME_US) {
        uint64_t sleep_us = FRAME_TIME_US - elapsed;
        sleep(1);// TODO: use nanosleep
    }
}

/**
 * Main rendering loop - runs at 60 FPS
 */
static void main_loop(void) {
    uint64_t frame_count = 0;
    uint64_t fps_start_time = get_time_us();

    printf("[Compositor] Entering main loop (target: %d FPS)\n", TARGET_FPS);

    while (running) {
        uint64_t frame_start = get_time_us();

        // 1. Poll input events
        if (input) {
            int events = input_poll(input);
            if (events > 0) {
                // Update cursor position from input
                int32_t mouse_x, mouse_y;
                input_get_mouse_pos(input, &mouse_x, &mouse_y);
                renderer_set_cursor_position(renderer, mouse_x, mouse_y);
            }
        }

        // 2. Clear back buffer
        renderer_clear(renderer, 0x2C3E50);  // Dark blue background

        // 3. Composite all windows (painter's algorithm)
        renderer_composite_windows(renderer);

        // 4. Draw cursor
        renderer_draw_cursor(renderer);

        // 5. Flip/present to framebuffer
        renderer_present(renderer);

        frame_count++;

        // Calculate and print FPS every second
        uint64_t now = get_time_us();
        if (now - fps_start_time >= 1000000) {
            double fps = (double)frame_count / ((now - fps_start_time) / 1000000.0);
            printf("[Compositor] FPS: %.1f (frame time: %.2f ms)\n",
                   fps, (now - frame_start) / 1000.0);
            frame_count = 0;
            fps_start_time = now;
        }

        // 4. Sleep to maintain 60 FPS
        frame_sleep(frame_start);
    }

    printf("[Compositor] Main loop exited\n");
}

/**
 * Initialize compositor subsystems
 */
static bool init(void) {
    printf("[Compositor] Initializing minimal compositor...\n");

    // Initialize framebuffer
    fb = fb_init();
    if (!fb) {
        fprintf(stderr, "[Compositor] Failed to initialize framebuffer\n");
        return false;
    }

    printf("[Compositor] Framebuffer: %dx%d @ %d bpp\n",
           fb->width, fb->height, fb->bpp);

    // Initialize renderer
    renderer = renderer_init(fb);
    if (!renderer) {
        fprintf(stderr, "[Compositor] Failed to initialize renderer\n");
        fb_cleanup(fb);
        return false;
    }

    printf("[Compositor] Renderer initialized (double buffering)\n");

    // Initialize input handler
    input = input_init();
    if (!input) {
        fprintf(stderr, "[Compositor] Failed to initialize input (continuing anyway)\n");
        // Not fatal - compositor can run without input
    } else {
        printf("[Compositor] Input handler initialized\n");
    }

    return true;
}

/**
 * Cleanup compositor resources
 */
static void cleanup(void) {
    printf("[Compositor] Cleaning up...\n");

    if (input) {
        input_cleanup(input);
        input = NULL;
    }

    if (renderer) {
        renderer_cleanup(renderer);
        renderer = NULL;
    }

    if (fb) {
        fb_cleanup(fb);
        fb = NULL;
    }

    printf("[Compositor] Cleanup complete\n");
}

/**
 * Entry point
 */
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("=== AutomationOS Minimal Compositor v1.0 ===\n");
    printf("A simple software compositor with 60 FPS rendering\n\n");

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize
    if (!init()) {
        fprintf(stderr, "Initialization failed\n");
        return 1;
    }

    // Run main loop
    main_loop();

    // Cleanup
    cleanup();

    printf("=== Compositor exited successfully ===\n");
    return 0;
}
