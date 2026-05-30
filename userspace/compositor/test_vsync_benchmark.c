/**
 * VSync and Double-Buffering Benchmark
 *
 * Tests:
 * 1. Frame time variance (should be stable at 16.67ms with VSync)
 * 2. Visual tearing test (moving window across screen)
 * 3. FPS measurement with/without VSync
 *
 * Expected Results:
 * - With VSync: stable 60 FPS, frame time variance < 1ms, no tearing
 * - Without VSync: higher FPS but tearing visible, unstable frame times
 */

#include "fb_compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <math.h>

#define TEST_DURATION_SEC 10
#define MAX_SAMPLES 1000

// Get current time in microseconds
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

// Calculate standard deviation
static double calculate_stddev(uint64_t *samples, int count) {
    if (count == 0) return 0.0;

    // Calculate mean
    double mean = 0.0;
    for (int i = 0; i < count; i++) {
        mean += samples[i];
    }
    mean /= count;

    // Calculate variance
    double variance = 0.0;
    for (int i = 0; i < count; i++) {
        double diff = samples[i] - mean;
        variance += diff * diff;
    }
    variance /= count;

    return sqrt(variance);
}

/**
 * Test 1: Frame time variance measurement
 */
void test_frame_time_variance(fb_compositor_t *comp, bool vsync_enabled) {
    printf("\n=== Test 1: Frame Time Variance ===\n");
    printf("VSync: %s\n", vsync_enabled ? "enabled" : "disabled");

    fb_compositor_set_vsync(comp, vsync_enabled);

    uint64_t frame_times[MAX_SAMPLES];
    int sample_count = 0;
    uint64_t start_time = get_time_us();

    // Run for 5 seconds
    while ((get_time_us() - start_time) < 5000000 && sample_count < MAX_SAMPLES) {
        fb_compositor_frame(comp);
        frame_times[sample_count++] = fb_compositor_get_frame_time(comp);
    }

    // Calculate statistics
    double mean = 0.0;
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;

    for (int i = 0; i < sample_count; i++) {
        mean += frame_times[i];
        if (frame_times[i] < min) min = frame_times[i];
        if (frame_times[i] > max) max = frame_times[i];
    }
    mean /= sample_count;

    double stddev = calculate_stddev(frame_times, sample_count);

    printf("Samples: %d\n", sample_count);
    printf("Mean frame time: %.2f us (%.2f ms)\n", mean, mean / 1000.0);
    printf("Std deviation: %.2f us (%.2f ms)\n", stddev, stddev / 1000.0);
    printf("Min frame time: %lu us (%.2f ms)\n", min, min / 1000.0);
    printf("Max frame time: %lu us (%.2f ms)\n", max, max / 1000.0);
    printf("FPS: %u\n", fb_compositor_get_fps(comp));

    // Verdict
    if (vsync_enabled) {
        if (stddev < 1000.0) {  // < 1ms variance
            printf("✓ PASS: Frame time variance is stable (< 1ms)\n");
        } else {
            printf("✗ FAIL: Frame time variance too high (%.2f ms)\n", stddev / 1000.0);
        }

        double expected_fps = 60.0;
        double fps_tolerance = 2.0;
        if (fabs(fb_compositor_get_fps(comp) - expected_fps) < fps_tolerance) {
            printf("✓ PASS: FPS close to 60 (%u)\n", fb_compositor_get_fps(comp));
        } else {
            printf("✗ FAIL: FPS not at 60 (%u)\n", fb_compositor_get_fps(comp));
        }
    }
}

/**
 * Test 2: Moving window tearing test
 * Creates a high-contrast window that moves horizontally across the screen.
 * Tearing should be visible without VSync, invisible with VSync.
 */
void test_moving_window_tearing(fb_compositor_t *comp, bool vsync_enabled) {
    printf("\n=== Test 2: Moving Window Tearing Test ===\n");
    printf("VSync: %s\n", vsync_enabled ? "enabled" : "disabled");
    printf("Watch for horizontal tearing artifacts during window movement...\n");

    fb_compositor_set_vsync(comp, vsync_enabled);

    // Create a high-contrast test window (vertical stripes)
    const uint32_t window_width = 400;
    const uint32_t window_height = 300;
    window_t *test_window = window_create(1, WINDOW_NORMAL, 0, 100, window_width, window_height);

    if (!test_window || !test_window->surface) {
        printf("Failed to create test window\n");
        return;
    }

    // Fill with vertical stripes (black and white alternating)
    for (uint32_t y = 0; y < window_height; y++) {
        for (uint32_t x = 0; x < window_width; x++) {
            uint32_t color = ((x / 20) % 2 == 0) ? 0xFFFFFFFF : 0xFF000000;
            test_window->surface->pixels[y * window_width + x] = color;
        }
    }
    test_window->surface->dirty = true;

    fb_compositor_add_window(comp, test_window);

    // Move window horizontally across screen
    int32_t velocity = 5;  // pixels per frame
    uint64_t start_time = get_time_us();

    while ((get_time_us() - start_time) < 5000000) {  // 5 seconds
        // Update window position
        test_window->geometry.x += velocity;

        // Bounce off edges
        if (test_window->geometry.x <= 0 ||
            test_window->geometry.x >= (int32_t)(800 - window_width)) {
            velocity = -velocity;
        }

        // Mark as damaged
        damage_add_region(&comp->damage, &test_window->geometry);

        // Render frame
        fb_compositor_frame(comp);
    }

    printf("Test completed. Visual inspection required.\n");
    printf("Expected: %s tearing with VSync %s\n",
           vsync_enabled ? "NO" : "VISIBLE",
           vsync_enabled ? "enabled" : "disabled");

    fb_compositor_remove_window(comp, 1);
}

/**
 * Test 3: FPS comparison
 */
void test_fps_comparison(void) {
    printf("\n=== Test 3: FPS Comparison ===\n");

    fb_compositor_t *comp = fb_compositor_init();
    if (!comp) {
        printf("Failed to initialize compositor\n");
        return;
    }

    // Test with VSync enabled
    printf("\nTesting with VSync enabled...\n");
    fb_compositor_set_vsync(comp, true);
    uint64_t start = get_time_us();
    int frames = 0;
    while ((get_time_us() - start) < 3000000) {  // 3 seconds
        fb_compositor_frame(comp);
        frames++;
    }
    uint32_t fps_with_vsync = fb_compositor_get_fps(comp);
    printf("FPS with VSync: %u\n", fps_with_vsync);

    // Test with VSync disabled
    printf("\nTesting with VSync disabled...\n");
    fb_compositor_set_vsync(comp, false);
    start = get_time_us();
    frames = 0;
    while ((get_time_us() - start) < 3000000) {  // 3 seconds
        fb_compositor_frame(comp);
        frames++;
    }
    uint32_t fps_without_vsync = fb_compositor_get_fps(comp);
    printf("FPS without VSync: %u\n", fps_without_vsync);

    printf("\nResults:\n");
    printf("  With VSync: %u FPS (should be ~60)\n", fps_with_vsync);
    printf("  Without VSync: %u FPS (should be higher)\n", fps_without_vsync);

    if (fps_with_vsync >= 58 && fps_with_vsync <= 62) {
        printf("✓ PASS: VSync correctly limits to 60 FPS\n");
    } else {
        printf("✗ FAIL: VSync not working correctly\n");
    }

    fb_compositor_cleanup(comp);
}

int main(int argc, char **argv) {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  VSync and Double-Buffering Benchmark for AutomationOS      ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    fb_compositor_t *comp = fb_compositor_init();
    if (!comp) {
        fprintf(stderr, "Failed to initialize compositor\n");
        return 1;
    }

    // Run all tests
    test_frame_time_variance(comp, true);   // With VSync
    test_frame_time_variance(comp, false);  // Without VSync
    test_moving_window_tearing(comp, true);  // With VSync (no tearing)
    test_moving_window_tearing(comp, false); // Without VSync (tearing visible)

    fb_compositor_cleanup(comp);

    // Run FPS comparison in fresh compositor instance
    test_fps_comparison();

    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Benchmark Complete                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    return 0;
}
