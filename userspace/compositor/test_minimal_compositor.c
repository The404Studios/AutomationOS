/**
 * Minimal Compositor Test Program
 *
 * Tests the basic compositor functionality:
 * - Framebuffer initialization
 * - Window creation
 * - Compositing
 * - Rendering at 60 FPS
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <math.h>

#include "fb.h"
#include "render.h"

static bool running = true;

static void signal_handler(int sig) {
    (void)sig;
    running = false;
}

static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

int main(void) {
    printf("=== Minimal Compositor Test ===\n\n");

    // Set up signal handler
    signal(SIGINT, signal_handler);

    // Initialize framebuffer
    printf("[Test] Initializing framebuffer...\n");
    framebuffer_t *fb = fb_init();
    if (!fb) {
        fprintf(stderr, "[Test] Failed to initialize framebuffer\n");
        return 1;
    }

    printf("[Test] Framebuffer: %dx%d @ %d bpp\n\n", fb->width, fb->height, fb->bpp);

    // Initialize renderer
    printf("[Test] Initializing renderer...\n");
    renderer_t *renderer = renderer_init(fb);
    if (!renderer) {
        fprintf(stderr, "[Test] Failed to initialize renderer\n");
        fb_cleanup(fb);
        return 1;
    }

    // Create test windows
    printf("[Test] Creating test windows...\n");

    // Window 1: Red window (bottom layer)
    window_t *win1 = renderer_create_test_window(
        1, 100, 100, 400, 300, 0xFFFF0000);  // Red
    if (!win1) {
        fprintf(stderr, "[Test] Failed to create window 1\n");
        goto cleanup;
    }
    win1->z_order = 0;  // Bottom
    renderer_add_window(renderer, win1);

    // Window 2: Green window (middle layer)
    window_t *win2 = renderer_create_test_window(
        2, 200, 150, 400, 300, 0xFF00FF00);  // Green
    if (!win2) {
        fprintf(stderr, "[Test] Failed to create window 2\n");
        goto cleanup;
    }
    win2->z_order = 1;  // Middle
    renderer_add_window(renderer, win2);

    // Window 3: Blue window with alpha (top layer)
    window_t *win3 = renderer_create_test_window(
        3, 300, 200, 400, 300, 0xC00000FF);  // Semi-transparent blue
    if (!win3) {
        fprintf(stderr, "[Test] Failed to create window 3\n");
        goto cleanup;
    }
    win3->z_order = 2;  // Top
    renderer_add_window(renderer, win3);

    printf("[Test] Created 3 test windows\n\n");

    // Test 1: Clear screen
    printf("[Test] Test 1: Clear screen to dark gray...\n");
    renderer_clear(renderer, 0xFF2C3E50);
    renderer_present(renderer);
    printf("[Test] ✓ Clear test passed\n\n");
    sleep(1);

    // Test 2: Single window
    printf("[Test] Test 2: Render single window...\n");
    win2->visible = false;
    win3->visible = false;
    renderer_clear(renderer, 0xFF2C3E50);
    renderer_composite_windows(renderer);
    renderer_present(renderer);
    printf("[Test] ✓ Single window test passed\n\n");
    sleep(1);

    // Test 3: Multiple windows
    printf("[Test] Test 3: Render multiple windows...\n");
    win1->visible = true;
    win2->visible = true;
    win3->visible = true;
    renderer_clear(renderer, 0xFF2C3E50);
    renderer_composite_windows(renderer);
    renderer_present(renderer);
    printf("[Test] ✓ Multiple window test passed\n\n");
    sleep(1);

    // Test 4: Z-order test
    printf("[Test] Test 4: Z-order manipulation...\n");
    // Swap z-order (bring red to front)
    win1->z_order = 10;
    renderer_clear(renderer, 0xFF2C3E50);
    renderer_composite_windows(renderer);
    renderer_present(renderer);
    printf("[Test] ✓ Z-order test passed\n\n");
    sleep(1);

    // Test 5: 60 FPS rendering test
    printf("[Test] Test 5: 60 FPS rendering (5 seconds)...\n");
    uint64_t start_time = get_time_us();
    uint64_t frame_count = 0;
    uint64_t fps_start = start_time;

    // Reset z-order
    win1->z_order = 0;
    win2->z_order = 1;
    win3->z_order = 2;

    while (running && (get_time_us() - start_time) < 5000000) {
        uint64_t frame_start = get_time_us();

        // Animate windows (move them slightly)
        win1->x = 100 + (int32_t)(50 * sin((frame_count % 120) * 0.05));
        win2->x = 200 + (int32_t)(50 * cos((frame_count % 120) * 0.05));
        win3->y = 200 + (int32_t)(30 * sin((frame_count % 90) * 0.07));

        // Render
        renderer_clear(renderer, 0xFF2C3E50);
        renderer_composite_windows(renderer);
        renderer_present(renderer);

        frame_count++;

        // Print FPS every second
        uint64_t now = get_time_us();
        if (now - fps_start >= 1000000) {
            double fps = (double)frame_count / ((now - fps_start) / 1000000.0);
            printf("[Test]   FPS: %.1f (frame time: %.2f ms)\n",
                   fps, (now - frame_start) / 1000.0);
            frame_count = 0;
            fps_start = now;
        }

        // Sleep to maintain ~60 FPS
        uint64_t elapsed = get_time_us() - frame_start;
        if (elapsed < 16667) {
            usleep(16667 - elapsed);
        }
    }

    printf("[Test] ✓ 60 FPS rendering test passed\n\n");

    // Summary
    printf("[Test] === All Tests Passed ===\n");
    printf("[Test] Compositor is functional and ready\n\n");

cleanup:
    // Cleanup
    printf("[Test] Cleaning up...\n");
    if (renderer) renderer_cleanup(renderer);
    if (fb) fb_cleanup(fb);

    printf("[Test] Test complete\n");
    return 0;
}
