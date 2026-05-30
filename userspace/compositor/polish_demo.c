/**
 * AutomationOS Graphics & Animation Polish Demo
 *
 * Demonstrates all polish features:
 * - 60 FPS performance monitoring
 * - Enhanced shadow system (5 levels)
 * - Background blur effects
 * - Smooth scrolling with kinetic physics
 * - Window animations
 */

#include "compositor.h"
#include "animations.h"
#include "performance.h"
#include "shadow_system.h"
#include "blur_effects.h"
#include "../lib/ui/smooth_scroll.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

// Demo configuration
#define DEMO_DURATION_SECONDS 30
#define WINDOW_COUNT 5

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Create demo window
 */
static window_t *create_demo_window(uint32_t id, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    rect_t geometry = { x, y, (int32_t)w, (int32_t)h };
    window_t *window = window_create(id, WINDOW_NORMAL, &geometry);
    if (!window) return NULL;

    char title[64];
    snprintf(title, sizeof(title), "Demo Window %u", id);
    window_set_title(window, title);

    window->mapped = true;
    window->focused = (id == 0);  // First window is focused

    return window;
}

/**
 * Animate window open
 */
static void animate_window_open(window_t *window) {
    printf("[Demo] Animating window %u open\n", window->id);

    // Create scale animation (0.85 -> 1.0)
    animation_t *scale_anim = animation_create(ANIM_SCALE, 250000, EASING_EASE_OUT_CUBIC);
    animation_start(scale_anim, 0.85f, 1.0f);

    // Create fade animation (0 -> 1.0)
    animation_t *fade_anim = animation_create(ANIM_FADE, 250000, EASING_EASE_OUT_CUBIC);
    animation_start(fade_anim, 0.0f, 1.0f);

    window->animation = scale_anim;  // Attach to window
}

/**
 * Demo shadow levels
 */
static void demo_shadow_levels(gpu_context_t *gpu) {
    printf("\n=== SHADOW LEVELS DEMO ===\n");

    rect_t rect = { 100, 100, 200, 150 };

    for (int level = SHADOW_SM; level <= SHADOW_XXL; level++) {
        const char *level_names[] = { "SM", "MD", "LG", "XL", "XXL" };
        const shadow_spec_t *spec = shadow_get_spec(level);

        printf("Shadow Level %s:\n", level_names[level]);
        printf("  Key: offset_y=%d, blur=%.1f, opacity=%.2f\n",
               spec->key_offset_y, spec->key_blur, spec->key_opacity);
        printf("  Ambient: offset_y=%d, blur=%.1f, opacity=%.2f\n",
               spec->ambient_offset_y, spec->ambient_blur, spec->ambient_opacity);

        // Draw shadow
        shadow_draw_layered(gpu, &rect, level, false);

        rect.x += 250;  // Next position
    }
}

/**
 * Demo blur effects
 */
static void demo_blur_effects(blur_context_t *blur_ctx) {
    printf("\n=== BLUR EFFECTS DEMO ===\n");

    rect_t panel_rect = { 0, 0, 1920, 32 };
    printf("Blurring panel background (20px radius)...\n");
    blur_region(blur_ctx, &panel_rect, &BLUR_PANEL_BACKGROUND);

    rect_t dock_rect = { 500, 1050, 920, 70 };
    printf("Blurring dock background (30px radius)...\n");
    blur_region(blur_ctx, &dock_rect, &BLUR_DOCK_BACKGROUND);

    rect_t dialog_rect = { 600, 300, 720, 480 };
    printf("Blurring dialog background (25px radius)...\n");
    blur_region(blur_ctx, &dialog_rect, &BLUR_DIALOG_BACKGROUND);
}

/**
 * Demo smooth scrolling
 */
static void demo_smooth_scrolling(void) {
    printf("\n=== SMOOTH SCROLLING DEMO ===\n");

    scroll_state_t scroll;
    scroll_init(&scroll, 0.0f, 1000.0f);

    printf("Testing kinetic scroll:\n");

    // Simulate drag
    scroll_drag_start(&scroll, 100.0f);
    scroll_drag_update(&scroll, 150.0f);
    scroll_drag_update(&scroll, 250.0f);
    scroll_drag_end(&scroll);

    printf("  Initial velocity: %.2f px/frame\n", scroll_get_velocity(&scroll));

    // Simulate physics for 2 seconds at 60 FPS
    uint64_t start_time = get_time_us();
    int frame_count = 0;

    while (scroll_is_active(&scroll) && frame_count < 120) {
        uint64_t current_time = start_time + (frame_count * 16667);
        scroll_update(&scroll, current_time);

        if (frame_count % 20 == 0) {
            printf("  Frame %d: pos=%.1f, vel=%.2f\n",
                   frame_count,
                   scroll_get_position(&scroll),
                   scroll_get_velocity(&scroll));
        }

        frame_count++;
    }

    printf("  Final position: %.1f (settled in %d frames)\n",
           scroll_get_position(&scroll), frame_count);

    // Test rubber-band bounce
    printf("\nTesting rubber-band bounce:\n");
    scroll_init(&scroll, 0.0f, 500.0f);
    scroll_drag_start(&scroll, 100.0f);
    scroll_drag_update(&scroll, -50.0f);  // Drag past boundary
    scroll_drag_end(&scroll);

    frame_count = 0;
    start_time = get_time_us();

    while (scroll_is_active(&scroll) && frame_count < 120) {
        uint64_t current_time = start_time + (frame_count * 16667);
        scroll_update(&scroll, current_time);

        if (frame_count % 20 == 0) {
            printf("  Frame %d: pos=%.1f, vel=%.2f, bouncing=%s\n",
                   frame_count,
                   scroll_get_position(&scroll),
                   scroll_get_velocity(&scroll),
                   scroll.bouncing ? "YES" : "NO");
        }

        frame_count++;
    }
}

/**
 * Main demo
 */
int main(int argc, char *argv[]) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  AutomationOS Graphics & Animation Polish Demo          ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    // Initialize compositor
    printf("[Demo] Initializing compositor...\n");
    compositor_t *comp = compositor_init("/dev/dri/card0");
    if (!comp) {
        fprintf(stderr, "Failed to initialize compositor\n");
        return 1;
    }

    // Initialize performance monitoring
    perf_stats_t perf_stats;
    perf_init(&perf_stats);

    gpu_stats_t gpu_stats = {
        .gpu_usage_percent = 12.5f,
        .vram_used_mb = 256,
        .vram_total_mb = 4096,
        .texture_count = 10,
        .draw_calls = 45,
    };

    // Initialize blur effects
    blur_context_t *blur_ctx = blur_init(comp->gpu);
    if (!blur_ctx) {
        fprintf(stderr, "Failed to initialize blur effects\n");
        compositor_cleanup(comp);
        return 1;
    }

    // Add demo display
    display_t *display = display_create(0, 1920, 1080, 60);
    if (display) {
        display->primary = true;
        compositor_add_display(comp, display);
    }

    // Create demo windows
    printf("\n[Demo] Creating %d demo windows...\n", WINDOW_COUNT);
    for (uint32_t i = 0; i < WINDOW_COUNT; i++) {
        int32_t x = 100 + (i * 150);
        int32_t y = 100 + (i * 50);
        window_t *window = create_demo_window(i, x, y, 640, 480);
        if (window) {
            compositor_add_window(comp, window);
            animate_window_open(window);
        }
    }

    // Run demos
    demo_shadow_levels(comp->gpu);
    demo_blur_effects(blur_ctx);
    demo_smooth_scrolling();

    // Main render loop
    printf("\n[Demo] Starting render loop (30 seconds)...\n");
    uint64_t demo_start = get_time_us();
    uint64_t last_report = demo_start;
    uint32_t frame_number = 0;

    while ((get_time_us() - demo_start) < (DEMO_DURATION_SECONDS * 1000000)) {
        uint64_t frame_start = get_time_us();

        // Render frame
        compositor_frame(comp);

        // Record frame timing
        uint64_t frame_time = get_time_us() - frame_start;
        perf_record_frame(&perf_stats, frame_time);

        frame_number++;

        // Update FPS every second
        uint64_t now = get_time_us();
        if (now - last_report >= 1000000) {
            perf_update_fps(&perf_stats);
            last_report = now;

            // Check performance health
            if (!perf_check_health(&perf_stats)) {
                printf("[Demo] ⚠️  Performance degradation detected!\n");
            }
        }

        // Print detailed report every 10 seconds
        if (frame_number % 600 == 0) {
            perf_print_report(&perf_stats, &gpu_stats);
        }
    }

    // Final report
    printf("\n[Demo] Demo complete. Generating final report...\n");
    perf_print_report(&perf_stats, &gpu_stats);

    // Performance grade
    const char *grade = perf_get_grade(&perf_stats);
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  FINAL PERFORMANCE GRADE: %s%-35s ║\n", grade,
           perf_check_health(&perf_stats) ? " ✓ PASSED" : " ✗ NEEDS OPTIMIZATION");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    // Cleanup
    printf("\n[Demo] Cleaning up...\n");
    blur_cleanup(blur_ctx);
    compositor_cleanup(comp);

    printf("[Demo] Done!\n");
    return 0;
}
