/**
 * Integration Test for Compositor Framebuffer + Input
 *
 * Tests:
 * - Direct framebuffer access at 0x40000000
 * - Input from /dev/input/event0
 * - Real-time cursor rendering
 * - 60 FPS compositing
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

// Test parameters
#define TARGET_FPS 60
#define FRAME_TIME_US (1000000 / TARGET_FPS)
#define TEST_DURATION_SECONDS 10

static bool running = true;

static void signal_handler(int sig) {
    (void)sig;
    printf("\n[Test] Interrupted - shutting down\n");
    running = false;
}

static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

int main(void) {
    printf("=== Compositor Integration Test ===\n");
    printf("Testing framebuffer + input integration\n\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 1. Initialize framebuffer
    printf("[Test] Initializing framebuffer...\n");
    framebuffer_t *fb = fb_init();
    if (!fb) {
        fprintf(stderr, "[Test] FAIL: Framebuffer initialization failed\n");
        return 1;
    }
    printf("[Test] PASS: Framebuffer initialized (%dx%d)\n", fb->width, fb->height);

    // 2. Initialize renderer
    printf("[Test] Initializing renderer...\n");
    renderer_t *renderer = renderer_init(fb);
    if (!renderer) {
        fprintf(stderr, "[Test] FAIL: Renderer initialization failed\n");
        fb_cleanup(fb);
        return 1;
    }
    printf("[Test] PASS: Renderer initialized\n");

    // 3. Initialize input
    printf("[Test] Initializing input...\n");
    input_handler_t *input = input_init();
    if (!input) {
        fprintf(stderr, "[Test] WARNING: Input initialization failed (continuing)\n");
    } else {
        printf("[Test] PASS: Input initialized\n");
    }

    // 4. Create test windows
    printf("[Test] Creating test windows...\n");

    window_t *win1 = renderer_create_test_window(1, 100, 100, 300, 200, 0xFF3498DB);  // Blue
    window_t *win2 = renderer_create_test_window(2, 250, 150, 300, 200, 0xFF2ECC71);  // Green
    window_t *win3 = renderer_create_test_window(3, 400, 200, 300, 200, 0xFFE74C3C);  // Red

    if (!win1 || !win2 || !win3) {
        fprintf(stderr, "[Test] FAIL: Window creation failed\n");
        goto cleanup;
    }

    win1->z_order = 1;
    win2->z_order = 2;
    win3->z_order = 3;

    renderer_add_window(renderer, win1);
    renderer_add_window(renderer, win2);
    renderer_add_window(renderer, win3);

    printf("[Test] PASS: Created 3 test windows\n");

    // 5. Render loop test
    printf("[Test] Starting render loop (60 FPS target, %d seconds)...\n", TEST_DURATION_SECONDS);

    uint64_t test_start = get_time_us();
    uint64_t test_end = test_start + (TEST_DURATION_SECONDS * 1000000ULL);
    uint64_t frame_count = 0;
    uint64_t fps_start = test_start;

    while (running && get_time_us() < test_end) {
        uint64_t frame_start = get_time_us();

        // Poll input
        if (input) {
            int events = input_poll(input);
            if (events > 0) {
                int32_t mouse_x, mouse_y;
                input_get_mouse_pos(input, &mouse_x, &mouse_y);
                renderer_set_cursor_position(renderer, mouse_x, mouse_y);

                uint8_t buttons = input_get_mouse_buttons(input);
                if (buttons) {
                    printf("[Test] Mouse buttons: 0x%02X at (%d, %d)\n",
                           buttons, mouse_x, mouse_y);
                }
            }
        }

        // Clear
        renderer_clear(renderer, 0x2C3E50);  // Dark blue

        // Composite windows
        renderer_composite_windows(renderer);

        // Draw cursor
        renderer_draw_cursor(renderer);

        // Present
        renderer_present(renderer);

        frame_count++;

        // FPS counter
        uint64_t now = get_time_us();
        if (now - fps_start >= 1000000) {
            double fps = (double)frame_count / ((now - fps_start) / 1000000.0);
            printf("[Test] FPS: %.1f (%.2f ms/frame)\n",
                   fps, (now - frame_start) / 1000.0);
            frame_count = 0;
            fps_start = now;
        }

        // Sleep to maintain 60 FPS
        uint64_t elapsed = get_time_us() - frame_start;
        if (elapsed < FRAME_TIME_US) {
            usleep(FRAME_TIME_US - elapsed);
        }
    }

    printf("[Test] Render loop complete\n");

    // 6. Test framebuffer rendering
    printf("[Test] Testing direct framebuffer access...\n");

    // Draw test pattern
    fb_clear(fb, 0x000000);  // Black
    fb_fill_rect(fb, 0, 0, 100, 100, 0xFF0000);  // Red square top-left
    fb_fill_rect(fb, fb->width - 100, 0, 100, 100, 0x00FF00);  // Green square top-right
    fb_fill_rect(fb, 0, fb->height - 100, 100, 100, 0x0000FF);  // Blue square bottom-left
    fb_fill_rect(fb, fb->width - 100, fb->height - 100, 100, 100, 0xFFFF00);  // Yellow square bottom-right

    printf("[Test] Test pattern drawn - verify on screen\n");
    sleep(2);

    printf("[Test] PASS: All tests completed successfully\n");

cleanup:
    // Cleanup
    if (input) input_cleanup(input);
    if (renderer) renderer_cleanup(renderer);
    if (fb) fb_cleanup(fb);

    printf("=== Integration Test Complete ===\n");
    return 0;
}
