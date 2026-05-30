/**
 * Animation Test Demo
 *
 * Showcases animation system with various easing functions
 */

#include "compositor.h"
#include "animations.h"
#include "../wm/window_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile bool g_running = true;

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

/**
 * Draw animated window content
 */
void draw_animated_content(window_t *window, float progress) {
    if (!window || !window->surface) return;

    uint32_t *pixels = window->surface->pixels;
    uint32_t width = window->surface->width;
    uint32_t height = window->surface->height;

    // Animated gradient based on progress
    uint8_t color_shift = (uint8_t)(progress * 255);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r = (x * 255) / width;
            uint8_t g = (y * 255) / height;
            uint8_t b = color_shift;
            uint8_t a = 255;

            pixels[y * width + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    window->surface->dirty = true;
}

/**
 * Test all easing functions
 */
void test_easing_functions(void) {
    printf("\n=== Testing Easing Functions ===\n");

    const char *easing_names[] = {
        "LINEAR", "EASE_IN", "EASE_OUT", "EASE_IN_OUT",
        "EASE_IN_QUAD", "EASE_OUT_QUAD", "EASE_IN_OUT_QUAD",
        "EASE_IN_CUBIC", "EASE_OUT_CUBIC", "EASE_IN_OUT_CUBIC",
        "BOUNCE", "ELASTIC"
    };

    for (int i = 0; i < 12; i++) {
        printf("%-20s: ", easing_names[i]);
        for (float t = 0.0f; t <= 1.0f; t += 0.1f) {
            float value = easing_apply((easing_t)i, t);
            printf("%.2f ", value);
        }
        printf("\n");
    }
    printf("\n");
}

int main(int argc, char *argv[]) {
    printf("=== AutomationOS Animation Demo ===\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Test easing functions
    test_easing_functions();

    // Initialize compositor and window manager
    compositor_t *comp = compositor_init("/dev/dri/card0");
    if (!comp) {
        fprintf(stderr, "Failed to initialize compositor\n");
        return 1;
    }

    display_t *display = display_create(0, 1920, 1080, 60);
    if (!display) {
        compositor_cleanup(comp);
        return 1;
    }
    display->primary = true;
    compositor_add_display(comp, display);

    window_manager_t *wm = wm_init(comp);
    if (!wm) {
        compositor_cleanup(comp);
        return 1;
    }

    // Create test windows with different animations
    printf("[Demo] Creating animated windows...\n");

    // Window 1: Fade in animation
    window_t *fade_window = wm_create_window(wm, WINDOW_NORMAL, 600, 400, "Fade Animation");
    if (fade_window) {
        fade_window->geometry.x = 100;
        fade_window->geometry.y = 100;
        fade_window->animation = animation_create(ANIM_FADE, 2000000, EASING_EASE_IN_OUT);
        animation_start(fade_window->animation, 0.0f, 1.0f);
        draw_animated_content(fade_window, 0.0f);
        wm_map_window(wm, fade_window);
    }

    // Window 2: Scale animation with bounce
    window_t *scale_window = wm_create_window(wm, WINDOW_NORMAL, 600, 400, "Scale Animation (Bounce)");
    if (scale_window) {
        scale_window->geometry.x = 700;
        scale_window->geometry.y = 100;
        scale_window->animation = animation_create(ANIM_SCALE, 1500000, EASING_BOUNCE);
        animation_start(scale_window->animation, 0.5f, 1.0f);
        draw_animated_content(scale_window, 0.0f);
        wm_map_window(wm, scale_window);
    }

    // Window 3: Slide animation
    window_t *slide_window = wm_create_window(wm, WINDOW_NORMAL, 600, 400, "Slide Animation");
    if (slide_window) {
        slide_window->geometry.x = 400;
        slide_window->geometry.y = 500;
        slide_window->animation = animation_create(ANIM_SLIDE, 2000000, EASING_EASE_IN_OUT_CUBIC);
        animation_start(slide_window->animation, 0.0f, 1.0f);
        draw_animated_content(slide_window, 0.0f);
        wm_map_window(wm, slide_window);
    }

    printf("[Demo] Animations running... Press Ctrl+C to exit\n\n");

    // Animation loop
    uint32_t frame = 0;
    while (g_running) {
        // Update animations
        if (fade_window && fade_window->animation) {
            animation_update(fade_window->animation);
            if (animation_is_finished(fade_window->animation)) {
                // Restart animation (loop)
                animation_start(fade_window->animation,
                              fade_window->animation->to,
                              fade_window->animation->from);
            }
            draw_animated_content(fade_window, fade_window->animation->current);
        }

        if (scale_window && scale_window->animation) {
            animation_update(scale_window->animation);
            if (animation_is_finished(scale_window->animation)) {
                animation_start(scale_window->animation,
                              scale_window->animation->to,
                              scale_window->animation->from);
            }
            draw_animated_content(scale_window, scale_window->animation->current);
        }

        if (slide_window && slide_window->animation) {
            animation_update(slide_window->animation);
            if (animation_is_finished(slide_window->animation)) {
                animation_start(slide_window->animation,
                              slide_window->animation->to,
                              slide_window->animation->from);
            }
            draw_animated_content(slide_window, slide_window->animation->current);
        }

        // Update window manager
        wm_update(wm);

        // Render frame
        compositor_frame(comp);

        // Log progress
        frame++;
        if (frame % 60 == 0) {
            printf("[Demo] Frame %u - FPS: %u\n", frame, compositor_get_fps(comp));
            if (fade_window && fade_window->animation) {
                printf("  Fade: %.2f%% complete\n", fade_window->animation->current * 100);
            }
        }

        usleep(16667);  // ~60 FPS
    }

    // Cleanup
    printf("\n[Demo] Cleaning up...\n");
    wm_cleanup(wm);
    compositor_cleanup(comp);

    printf("[Demo] Animation demo complete!\n");
    return 0;
}
