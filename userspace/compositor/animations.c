/**
 * AutomationOS Animation System Implementation
 */

#include "animations.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>

// π constant
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Linear interpolation
 */
static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/**
 * Clamp value between 0 and 1
 */
static float clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

/**
 * Apply easing function to normalized time [0, 1]
 */
float easing_apply(easing_t easing, float t) {
    t = clamp01(t);

    switch (easing) {
        case EASING_LINEAR:
            return t;

        case EASING_EASE_IN:
            return t * t;

        case EASING_EASE_OUT:
            return t * (2.0f - t);

        case EASING_EASE_IN_OUT:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;

        case EASING_EASE_IN_QUAD:
            return t * t;

        case EASING_EASE_OUT_QUAD:
            return 1.0f - (1.0f - t) * (1.0f - t);

        case EASING_EASE_IN_OUT_QUAD: {
            float t2 = t * 2.0f;
            if (t2 < 1.0f) return 0.5f * t2 * t2;
            t2 -= 1.0f;
            return -0.5f * (t2 * (t2 - 2.0f) - 1.0f);
        }

        case EASING_EASE_IN_CUBIC:
            return t * t * t;

        case EASING_EASE_OUT_CUBIC: {
            float t1 = t - 1.0f;
            return t1 * t1 * t1 + 1.0f;
        }

        case EASING_EASE_IN_OUT_CUBIC: {
            float t2 = t * 2.0f;
            if (t2 < 1.0f) return 0.5f * t2 * t2 * t2;
            t2 -= 2.0f;
            return 0.5f * (t2 * t2 * t2 + 2.0f);
        }

        case EASING_BOUNCE: {
            const float n1 = 7.5625f;
            const float d1 = 2.75f;
            if (t < 1.0f / d1) {
                return n1 * t * t;
            } else if (t < 2.0f / d1) {
                t -= 1.5f / d1;
                return n1 * t * t + 0.75f;
            } else if (t < 2.5f / d1) {
                t -= 2.25f / d1;
                return n1 * t * t + 0.9375f;
            } else {
                t -= 2.625f / d1;
                return n1 * t * t + 0.984375f;
            }
        }

        case EASING_ELASTIC: {
            const float c4 = (2.0f * M_PI) / 3.0f;
            if (t == 0.0f) return 0.0f;
            if (t == 1.0f) return 1.0f;
            return -powf(2.0f, 10.0f * t - 10.0f) * sinf((t * 10.0f - 10.75f) * c4);
        }

        default:
            return t;
    }
}

/**
 * Create animation
 */
animation_t *animation_create(anim_type_t type, uint64_t duration, easing_t easing) {
    animation_t *anim = calloc(1, sizeof(animation_t));
    if (!anim) {
        fprintf(stderr, "Failed to allocate animation\n");
        return NULL;
    }

    anim->type = type;
    anim->duration = duration;
    anim->easing = easing;
    anim->finished = false;
    anim->on_complete = NULL;
    anim->user_data = NULL;

    return anim;
}

/**
 * Destroy animation
 */
void animation_destroy(animation_t *anim) {
    if (anim) {
        free(anim);
    }
}

/**
 * Start animation
 */
void animation_start(animation_t *anim, float from, float to) {
    if (!anim) return;

    anim->start_time = get_time_us();
    anim->from = from;
    anim->to = to;
    anim->current = from;
    anim->finished = false;
}

/**
 * Update animation - call once per frame
 */
void animation_update(animation_t *anim) {
    if (!anim || anim->finished) return;

    uint64_t now = get_time_us();
    uint64_t elapsed = now - anim->start_time;

    if (elapsed >= anim->duration) {
        // Animation finished
        anim->current = anim->to;
        anim->finished = true;

        if (anim->on_complete) {
            anim->on_complete(anim->user_data);
        }
    } else {
        // Compute normalized time [0, 1]
        float t = (float)elapsed / (float)anim->duration;

        // Apply easing
        float eased_t = easing_apply(anim->easing, t);

        // Interpolate
        anim->current = lerp(anim->from, anim->to, eased_t);
    }
}

/**
 * Stop animation
 */
void animation_stop(animation_t *anim) {
    if (!anim) return;
    anim->finished = true;
}

/**
 * Check if animation is finished
 */
bool animation_is_finished(animation_t *anim) {
    return anim ? anim->finished : true;
}

/**
 * Preset: Window open animation
 * Scale from 0.8 to 1.0 + fade in (200ms)
 */
animation_t *animation_window_open(void) {
    return animation_create(ANIM_SCALE, 200000, EASING_EASE_OUT_CUBIC);
}

/**
 * Preset: Window close animation
 * Scale to 0.8 + fade out (200ms)
 */
animation_t *animation_window_close(void) {
    return animation_create(ANIM_SCALE, 200000, EASING_EASE_IN_CUBIC);
}

/**
 * Preset: Minimize animation
 * Slide to taskbar (300ms)
 */
animation_t *animation_minimize(void) {
    return animation_create(ANIM_SLIDE, 300000, EASING_EASE_IN_OUT_QUAD);
}

/**
 * Preset: Maximize animation
 * Expand to fullscreen (250ms)
 */
animation_t *animation_maximize(void) {
    return animation_create(ANIM_SCALE, 250000, EASING_EASE_OUT_QUAD);
}

/**
 * Preset: Workspace switch animation
 * Slide transition (350ms)
 */
animation_t *animation_workspace_switch(void) {
    return animation_create(ANIM_SLIDE, 350000, EASING_EASE_IN_OUT_CUBIC);
}
