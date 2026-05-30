/**
 * AutomationOS Animation System Demo
 *
 * Demonstrates all animation features and capabilities.
 */

#include "animator.h"
#include "ui_animations.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile bool running = true;

void signal_handler(int sig) {
    (void)sig;
    running = false;
}

// Demo 1: Basic value animation
void demo_value_animation(animator_t *animator) {
    printf("\n=== Demo 1: Value Animation ===\n");

    float value = 0.0f;
    animation_t *anim = anim_from_to(animator, 0.0f, 100.0f, DURATION_SLOW, EASE_OUT_CUBIC);
    anim_set_target(anim, &value);
    anim_start(anim);

    // Simulate update loop
    for (int i = 0; i < 60; i++) {
        animator_update(animator);
        printf("Frame %d: value = %.2f\n", i, value);
        usleep(16667);  // ~60 FPS

        if (anim_is_finished(anim)) break;
    }

    printf("Final value: %.2f\n", value);
}

// Demo 2: Color animation
void demo_color_animation(animator_t *animator) {
    printf("\n=== Demo 2: Color Animation ===\n");

    color_t red = { .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f };
    color_t blue = { .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 1.0f };
    color_t current = red;

    animation_t *anim = anim_color(animator, red, blue, DURATION_NORMAL, EASE_LINEAR);
    anim_set_target_color(anim, &current);
    anim_start(anim);

    for (int i = 0; i < 30; i++) {
        animator_update(animator);
        printf("Frame %d: RGB(%.2f, %.2f, %.2f)\n", i, current.r, current.g, current.b);
        usleep(16667);

        if (anim_is_finished(anim)) break;
    }
}

// Demo 3: Spring physics
void demo_spring_animation(animator_t *animator) {
    printf("\n=== Demo 3: Spring Physics ===\n");

    float value = 0.0f;
    spring_params_t spring = SPRING_BOUNCY;

    animation_t *anim = anim_spring(animator, 0.0f, 100.0f, &spring);
    anim_set_target(anim, &value);
    anim_start(anim);

    printf("Bouncy spring animation:\n");
    for (int i = 0; i < 100; i++) {
        animator_update(animator);

        if (i % 5 == 0) {  // Print every 5th frame
            printf("Frame %d: value = %.2f\n", i, value);
        }

        usleep(16667);

        // Spring animations don't have fixed duration
        if (fabs(value - 100.0f) < 0.1f && i > 50) break;
    }
}

// Demo 4: Keyframe animation
void demo_keyframe_animation(animator_t *animator) {
    printf("\n=== Demo 4: Keyframe Animation ===\n");

    float value = 0.0f;
    animation_t *anim = anim_keyframe(animator);

    // Complex motion path
    anim_add_keyframe(anim, 0.0f, 0.0f, EASE_LINEAR);
    anim_add_keyframe(anim, 0.25f, 50.0f, EASE_OUT_CUBIC);
    anim_add_keyframe(anim, 0.5f, 25.0f, EASE_IN_CUBIC);
    anim_add_keyframe(anim, 0.75f, 75.0f, EASE_OUT_BOUNCE);
    anim_add_keyframe(anim, 1.0f, 100.0f, EASE_LINEAR);

    anim->duration = 1000000;  // 1 second
    anim_set_target(anim, &value);
    anim_start(anim);

    for (int i = 0; i < 60; i++) {
        animator_update(animator);

        if (i % 3 == 0) {
            printf("Frame %d: value = %.2f (progress: %.1f%%)\n",
                   i, value, anim_get_progress(anim) * 100.0f);
        }

        usleep(16667);

        if (anim_is_finished(anim)) break;
    }
}

// Demo 5: Animation with callbacks
void on_animation_start(void *user_data) {
    printf("Animation started! (user_data: %s)\n", (char*)user_data);
}

void on_animation_update(float value, void *user_data) {
    // Called every frame (would be too noisy to print)
}

void on_animation_complete(void *user_data) {
    printf("Animation completed! (user_data: %s)\n", (char*)user_data);
}

void demo_callbacks(animator_t *animator) {
    printf("\n=== Demo 5: Animation Callbacks ===\n");

    float value = 0.0f;
    animation_t *anim = anim_from_to(animator, 0.0f, 100.0f, DURATION_NORMAL, EASE_OUT_QUAD);
    anim_set_target(anim, &value);

    // Set callbacks
    char *user_data = "My Animation";
    anim_on_start(anim, on_animation_start, user_data);
    anim_on_update(anim, on_animation_update, user_data);
    anim_on_complete(anim, on_animation_complete, user_data);

    anim_start(anim);

    while (!anim_is_finished(anim)) {
        animator_update(animator);
        usleep(16667);
    }
}

// Demo 6: Repeating animation
void demo_repeat_animation(animator_t *animator) {
    printf("\n=== Demo 6: Repeating Animation ===\n");

    float value = 0.0f;
    animation_t *anim = anim_from_to(animator, 0.0f, 100.0f, 300000, EASE_LINEAR);
    anim_set_target(anim, &value);
    anim_set_repeat(anim, 3);  // Repeat 3 times
    anim_set_auto_reverse(anim, true);  // Ping-pong
    anim_start(anim);

    int iteration = 0;
    float last_value = 0.0f;

    for (int i = 0; i < 200; i++) {
        animator_update(animator);

        // Detect direction changes
        if ((value < last_value && last_value > 90.0f) || (value > last_value && last_value < 10.0f)) {
            iteration++;
            printf("Iteration %d: direction changed\n", iteration);
        }

        last_value = value;
        usleep(16667);

        if (anim_is_finished(anim)) {
            printf("Animation finished after %d iterations\n", iteration);
            break;
        }
    }
}

// Demo 7: Animation groups
void demo_animation_groups(animator_t *animator) {
    printf("\n=== Demo 7: Animation Groups ===\n");

    float value1 = 0.0f;
    float value2 = 0.0f;
    float value3 = 0.0f;

    animation_t *anim1 = anim_from_to(animator, 0.0f, 100.0f, DURATION_FAST, EASE_OUT_QUAD);
    anim_set_target(anim1, &value1);

    animation_t *anim2 = anim_from_to(animator, 0.0f, 50.0f, DURATION_NORMAL, EASE_OUT_CUBIC);
    anim_set_target(anim2, &value2);

    animation_t *anim3 = anim_from_to(animator, 0.0f, 75.0f, DURATION_SLOW, EASE_OUT_BACK);
    anim_set_target(anim3, &value3);

    // Create group
    animation_group_t *group = anim_group_create();
    anim_group_add(group, anim1);
    anim_group_add(group, anim2);
    anim_group_add(group, anim3);

    printf("Starting 3 animations simultaneously...\n");
    anim_group_start(group);

    bool all_finished = false;
    for (int i = 0; i < 60 && !all_finished; i++) {
        animator_update(animator);

        if (i % 5 == 0) {
            printf("Frame %d: v1=%.1f v2=%.1f v3=%.1f\n", i, value1, value2, value3);
        }

        usleep(16667);

        all_finished = anim_is_finished(anim1) && anim_is_finished(anim2) && anim_is_finished(anim3);
    }

    printf("Final: v1=%.1f v2=%.1f v3=%.1f\n", value1, value2, value3);
    anim_group_destroy(group);
}

// Demo 8: Easing comparison
void demo_easing_functions(animator_t *animator) {
    printf("\n=== Demo 8: Easing Function Comparison ===\n");

    easing_t easings[] = {
        EASE_LINEAR,
        EASE_OUT_QUAD,
        EASE_OUT_CUBIC,
        EASE_OUT_BACK,
        EASE_OUT_ELASTIC,
        EASE_OUT_BOUNCE
    };

    const char *names[] = {
        "LINEAR",
        "OUT_QUAD",
        "OUT_CUBIC",
        "OUT_BACK",
        "OUT_ELASTIC",
        "OUT_BOUNCE"
    };

    for (int e = 0; e < 6; e++) {
        printf("\nTesting %s:\n", names[e]);

        float value = 0.0f;
        animation_t *anim = anim_from_to(animator, 0.0f, 100.0f, DURATION_NORMAL, easings[e]);
        anim_set_target(anim, &value);
        anim_start(anim);

        // Sample at specific points
        float samples[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
        int sample_idx = 0;

        for (int i = 0; i < 30; i++) {
            animator_update(animator);

            float progress = anim_get_progress(anim);
            if (sample_idx < 5 && progress >= samples[sample_idx]) {
                printf("  %.0f%% → value: %.2f\n", samples[sample_idx] * 100.0f, value);
                sample_idx++;
            }

            usleep(16667);

            if (anim_is_finished(anim)) break;
        }
    }
}

// Demo 9: UI animations
void demo_ui_animations(animator_t *animator) {
    printf("\n=== Demo 9: UI Widget Animations ===\n");

    widget_anim_state_t button = {
        .scale = 1.0f,
        .opacity = 1.0f,
        .offset_x = 0.0f,
        .offset_y = 0.0f,
        .bg_color = { .r = 0.2f, .g = 0.6f, .b = 1.0f, .a = 1.0f },
        .visible = true
    };

    printf("Simulating button hover...\n");
    color_t hover_color = { .r = 0.3f, .g = 0.7f, .b = 1.0f, .a = 1.0f };
    ui_button_hover(animator, &button, hover_color);

    for (int i = 0; i < 15; i++) {
        animator_update(animator);
        printf("  Scale: %.3f, Color: (%.2f, %.2f, %.2f)\n",
               button.scale, button.bg_color.r, button.bg_color.g, button.bg_color.b);
        usleep(16667);
    }

    printf("\nSimulating button click...\n");
    ui_button_click(animator, &button);

    for (int i = 0; i < 20; i++) {
        animator_update(animator);

        if (i % 3 == 0) {
            printf("  Scale: %.3f\n", button.scale);
        }

        usleep(16667);
    }
}

// Demo 10: Performance test
void demo_performance_test(animator_t *animator) {
    printf("\n=== Demo 10: Performance Test ===\n");

    const int NUM_ANIMATIONS = 100;
    float values[NUM_ANIMATIONS] = {0};
    animation_t *animations[NUM_ANIMATIONS];

    printf("Creating %d simultaneous animations...\n", NUM_ANIMATIONS);

    for (int i = 0; i < NUM_ANIMATIONS; i++) {
        animations[i] = anim_from_to(animator, 0.0f, 100.0f,
                                     DURATION_NORMAL + (i * 1000),
                                     EASE_OUT_CUBIC);
        anim_set_target(animations[i], &values[i]);
        anim_start(animations[i]);
    }

    printf("Animator has %u active animations\n", animator->animation_count);

    // Run for 60 frames
    uint64_t start_time = get_time_microseconds();

    for (int frame = 0; frame < 60; frame++) {
        uint64_t frame_start = get_time_microseconds();

        animator_update(animator);

        uint64_t frame_time = get_time_microseconds() - frame_start;

        if (frame % 10 == 0) {
            printf("Frame %d: update took %lu μs, FPS: %u, active: %u\n",
                   frame, frame_time, animator->fps, animator->animation_count);
        }

        usleep(16667);
    }

    uint64_t total_time = get_time_microseconds() - start_time;
    printf("Total time: %.2f ms\n", total_time / 1000.0f);
    printf("Average frame time: %.2f ms\n", (total_time / 1000.0f) / 60.0f);
}

int main(int argc, char **argv) {
    printf("AutomationOS Animation System Demo\n");
    printf("===================================\n");

    // Setup signal handler for clean exit
    signal(SIGINT, signal_handler);

    // Create animator
    animator_t *animator = animator_create();
    if (!animator) {
        fprintf(stderr, "Failed to create animator\n");
        return 1;
    }

    printf("Animator initialized\n");
    printf("Global speed: %.1fx\n", animator->global_speed);
    printf("Reduce motion: %s\n", animator->reduce_motion ? "enabled" : "disabled");

    // Run demos
    bool run_all = (argc == 1);
    int demo = (argc > 1) ? atoi(argv[1]) : 0;

    if (run_all || demo == 1) demo_value_animation(animator);
    if (run_all || demo == 2) demo_color_animation(animator);
    if (run_all || demo == 3) demo_spring_animation(animator);
    if (run_all || demo == 4) demo_keyframe_animation(animator);
    if (run_all || demo == 5) demo_callbacks(animator);
    if (run_all || demo == 6) demo_repeat_animation(animator);
    if (run_all || demo == 7) demo_animation_groups(animator);
    if (run_all || demo == 8) demo_easing_functions(animator);
    if (run_all || demo == 9) demo_ui_animations(animator);
    if (run_all || demo == 10) demo_performance_test(animator);

    // Cleanup
    animator_destroy(animator);

    printf("\n=== Demo Complete ===\n");
    printf("All animations finished successfully!\n");

    return 0;
}
