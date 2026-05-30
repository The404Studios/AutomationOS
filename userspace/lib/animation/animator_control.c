/**
 * AutomationOS Animation Control & Update Logic
 *
 * This file contains animation control functions and the main update loop.
 */

#include "animator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern float easing_function(easing_t easing, float t);
extern float easing_spring(float t, const spring_params_t *params, float *velocity);
extern float easing_bezier(float t, const bezier_params_t *params);
extern color_t color_lerp(color_t from, color_t to, float t);
extern transform_t transform_lerp(transform_t from, transform_t to, float t);
extern rect_f_t rect_lerp(rect_f_t from, rect_f_t to, float t);

/**
 * Linear interpolation
 */
static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/**
 * Update single animation
 */
static void update_animation(animation_t *anim, uint64_t current_time) {
    if (!anim || anim->state != ANIM_STATE_RUNNING) return;

    // Calculate elapsed time
    uint64_t elapsed = current_time - anim->start_time;

    // Check if still in delay
    if (elapsed < anim->delay) {
        anim->state = ANIM_STATE_DELAYED;
        return;
    }

    // Adjust for delay
    elapsed -= anim->delay;

    // Normalized time [0, 1]
    float t = (anim->duration > 0) ? (float)elapsed / (float)anim->duration : 1.0f;

    // Handle repeating animations
    if (t >= 1.0f && anim->repeat_count != 0) {
        anim->current_iteration++;

        if (anim->repeat_count > 0 && anim->current_iteration >= (uint32_t)anim->repeat_count) {
            // Animation finished
            t = 1.0f;
            anim->state = ANIM_STATE_FINISHED;
        } else {
            // Restart animation
            anim->start_time = current_time;
            t = 0.0f;

            // Toggle reverse direction if auto-reverse
            if (anim->auto_reverse) {
                anim->reversing = !anim->reversing;
            }
        }
    } else if (t >= 1.0f) {
        t = 1.0f;
        anim->state = ANIM_STATE_FINISHED;
    }

    // Apply reverse if needed
    if (anim->reversing) {
        t = 1.0f - t;
    }

    // Apply easing
    float eased_t;
    if (anim->easing == EASE_SPRING) {
        eased_t = easing_spring(t, &anim->spring, &anim->spring_velocity);
    } else if (anim->easing == EASE_CUSTOM_BEZIER) {
        eased_t = easing_bezier(t, &anim->bezier);
    } else {
        eased_t = easing_function(anim->easing, t);
    }

    // Update values based on animation type
    switch (anim->type) {
        case ANIM_TYPE_VALUE:
            anim->current = lerp(anim->from, anim->to, eased_t);
            if (anim->target) {
                *anim->target = anim->current;
            }
            break;

        case ANIM_TYPE_COLOR:
            anim->current_color = color_lerp(anim->from_color, anim->to_color, eased_t);
            if (anim->target_color) {
                *anim->target_color = anim->current_color;
            }
            break;

        case ANIM_TYPE_TRANSFORM:
            anim->current_transform = transform_lerp(anim->from_transform, anim->to_transform, eased_t);
            if (anim->target_transform) {
                *anim->target_transform = anim->current_transform;
            }
            break;

        case ANIM_TYPE_RECT:
            anim->current_rect = rect_lerp(anim->from_rect, anim->to_rect, eased_t);
            if (anim->target_rect) {
                *anim->target_rect = anim->current_rect;
            }
            break;

        case ANIM_TYPE_KEYFRAME:
            // Find current keyframe segment
            if (anim->keyframe_count >= 2) {
                for (uint32_t i = 0; i < anim->keyframe_count - 1; i++) {
                    float t1 = anim->keyframes[i].time;
                    float t2 = anim->keyframes[i + 1].time;

                    if (t >= t1 && t <= t2) {
                        float segment_t = (t - t1) / (t2 - t1);
                        float segment_eased = easing_function(anim->keyframes[i].easing, segment_t);

                        float v1 = anim->keyframes[i].value;
                        float v2 = anim->keyframes[i + 1].value;
                        anim->current = lerp(v1, v2, segment_eased);

                        if (anim->target) {
                            *anim->target = anim->current;
                        }
                        break;
                    }
                }
            }
            break;

        default:
            break;
    }

    // Call update callback
    if (anim->on_update) {
        anim->on_update(anim->current, anim->user_data);
    }

    // Check if finished
    if (anim->state == ANIM_STATE_FINISHED && anim->on_complete) {
        anim->on_complete(anim->user_data);
    }
}

/**
 * Update all animations - call once per frame
 */
void animator_update(animator_t *animator) {
    if (!animator) return;

    animator->current_time = get_time_microseconds();
    uint64_t delta = animator->current_time - animator->last_update;
    animator->last_update = animator->current_time;

    // Update FPS counter
    static uint64_t fps_accumulator = 0;
    static uint32_t fps_frame_count = 0;
    fps_accumulator += delta;
    fps_frame_count++;
    if (fps_accumulator >= 1000000) {  // 1 second
        animator->fps = fps_frame_count;
        fps_accumulator = 0;
        fps_frame_count = 0;
    }

    // Update all animations
    animation_t *prev = NULL;
    animation_t *current = animator->animations;

    while (current) {
        animation_t *next = current->next;

        if (current->state == ANIM_STATE_RUNNING || current->state == ANIM_STATE_DELAYED) {
            update_animation(current, animator->current_time);
        }

        // Remove finished or canceled animations
        if (current->state == ANIM_STATE_FINISHED || current->state == ANIM_STATE_CANCELED) {
            if (prev) {
                prev->next = next;
            } else {
                animator->animations = next;
            }
            free(current);
            animator->animation_count--;
        } else {
            prev = current;
        }

        current = next;
    }
}

// ============================================================================
// ANIMATION CONTROL
// ============================================================================

/**
 * Start animation
 */
void anim_start(animation_t *anim) {
    if (!anim) return;

    anim->start_time = get_time_microseconds();
    anim->state = anim->delay > 0 ? ANIM_STATE_DELAYED : ANIM_STATE_RUNNING;
    anim->current_iteration = 0;
    anim->reversing = false;

    if (anim->on_start) {
        anim->on_start(anim->user_data);
    }
}

/**
 * Pause animation
 */
void anim_pause(animation_t *anim) {
    if (!anim || anim->state != ANIM_STATE_RUNNING) return;
    anim->state = ANIM_STATE_PAUSED;
}

/**
 * Resume animation
 */
void anim_resume(animation_t *anim) {
    if (!anim || anim->state != ANIM_STATE_PAUSED) return;
    anim->state = ANIM_STATE_RUNNING;
    // Adjust start time to account for pause duration
    uint64_t now = get_time_microseconds();
    anim->start_time = now - (anim->duration * anim_get_progress(anim));
}

/**
 * Stop animation (finish immediately)
 */
void anim_stop(animation_t *anim) {
    if (!anim) return;

    // Jump to end value
    switch (anim->type) {
        case ANIM_TYPE_VALUE:
            anim->current = anim->to;
            if (anim->target) *anim->target = anim->to;
            break;
        case ANIM_TYPE_COLOR:
            anim->current_color = anim->to_color;
            if (anim->target_color) *anim->target_color = anim->to_color;
            break;
        case ANIM_TYPE_TRANSFORM:
            anim->current_transform = anim->to_transform;
            if (anim->target_transform) *anim->target_transform = anim->to_transform;
            break;
        case ANIM_TYPE_RECT:
            anim->current_rect = anim->to_rect;
            if (anim->target_rect) *anim->target_rect = anim->to_rect;
            break;
        default:
            break;
    }

    anim->state = ANIM_STATE_FINISHED;

    if (anim->on_complete) {
        anim->on_complete(anim->user_data);
    }
}

/**
 * Cancel animation
 */
void anim_cancel(animation_t *anim) {
    if (!anim) return;

    anim->state = ANIM_STATE_CANCELED;

    if (anim->on_cancel) {
        anim->on_cancel(anim->user_data);
    }
}

/**
 * Seek to specific time in animation
 */
void anim_seek(animation_t *anim, float normalized_time) {
    if (!anim) return;

    if (normalized_time < 0.0f) normalized_time = 0.0f;
    if (normalized_time > 1.0f) normalized_time = 1.0f;

    // Update start time to reflect seek position
    uint64_t now = get_time_microseconds();
    anim->start_time = now - (uint64_t)(anim->duration * normalized_time);

    if (anim->state == ANIM_STATE_IDLE) {
        anim->state = ANIM_STATE_RUNNING;
    }
}

// ============================================================================
// ANIMATION CONFIGURATION
// ============================================================================

void anim_set_delay(animation_t *anim, uint64_t delay_us) {
    if (anim) anim->delay = delay_us;
}

void anim_set_repeat(animation_t *anim, int32_t count) {
    if (anim) anim->repeat_count = count;
}

void anim_set_auto_reverse(animation_t *anim, bool enabled) {
    if (anim) anim->auto_reverse = enabled;
}

void anim_set_target(animation_t *anim, float *target) {
    if (anim && anim->type == ANIM_TYPE_VALUE) {
        anim->target = target;
    }
}

void anim_set_target_color(animation_t *anim, color_t *target) {
    if (anim && anim->type == ANIM_TYPE_COLOR) {
        anim->target_color = target;
    }
}

void anim_set_target_transform(animation_t *anim, transform_t *target) {
    if (anim && anim->type == ANIM_TYPE_TRANSFORM) {
        anim->target_transform = target;
    }
}

void anim_set_target_rect(animation_t *anim, rect_f_t *target) {
    if (anim && anim->type == ANIM_TYPE_RECT) {
        anim->target_rect = target;
    }
}

void anim_set_bezier(animation_t *anim, float x1, float y1, float x2, float y2) {
    if (anim) {
        anim->easing = EASE_CUSTOM_BEZIER;
        anim->bezier = (bezier_params_t){ .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2 };
    }
}

// ============================================================================
// CALLBACKS
// ============================================================================

void anim_on_start(animation_t *anim, anim_callback_t callback, void *user_data) {
    if (anim) {
        anim->on_start = callback;
        anim->user_data = user_data;
    }
}

void anim_on_update(animation_t *anim, anim_update_callback_t callback, void *user_data) {
    if (anim) {
        anim->on_update = callback;
        anim->user_data = user_data;
    }
}

void anim_on_complete(animation_t *anim, anim_callback_t callback, void *user_data) {
    if (anim) {
        anim->on_complete = callback;
        anim->user_data = user_data;
    }
}

void anim_on_cancel(animation_t *anim, anim_callback_t callback, void *user_data) {
    if (anim) {
        anim->on_cancel = callback;
        anim->user_data = user_data;
    }
}

// ============================================================================
// QUERY
// ============================================================================

bool anim_is_running(const animation_t *anim) {
    return anim && (anim->state == ANIM_STATE_RUNNING || anim->state == ANIM_STATE_DELAYED);
}

bool anim_is_finished(const animation_t *anim) {
    return anim && anim->state == ANIM_STATE_FINISHED;
}

float anim_get_progress(const animation_t *anim) {
    if (!anim || anim->duration == 0) return 0.0f;

    uint64_t now = get_time_microseconds();
    uint64_t elapsed = (now > anim->start_time) ? (now - anim->start_time) : 0;

    if (elapsed < anim->delay) return 0.0f;

    elapsed -= anim->delay;
    float progress = (float)elapsed / (float)anim->duration;

    return (progress > 1.0f) ? 1.0f : progress;
}

float anim_get_current_value(const animation_t *anim) {
    return anim ? anim->current : 0.0f;
}

// ============================================================================
// ANIMATION GROUPS
// ============================================================================

animation_group_t *anim_group_create(void) {
    animation_group_t *group = calloc(1, sizeof(animation_group_t));
    if (!group) {
        fprintf(stderr, "Failed to allocate animation group\n");
        return NULL;
    }
    group->count = 0;
    group->wait_for_all = true;
    return group;
}

void anim_group_add(animation_group_t *group, animation_t *anim) {
    if (!group || !anim || group->count >= MAX_GROUP_ANIMATIONS) return;
    group->animations[group->count++] = anim;
}

void anim_group_start(animation_group_t *group) {
    if (!group) return;

    // Start all animations simultaneously
    for (uint32_t i = 0; i < group->count; i++) {
        anim_start(group->animations[i]);
    }
}

void anim_group_destroy(animation_group_t *group) {
    if (group) {
        free(group);
    }
}
