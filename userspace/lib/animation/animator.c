/**
 * AutomationOS Advanced Animation Engine Implementation
 */

#include "animator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

// π constant
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Preset spring configurations
const spring_params_t SPRING_SMOOTH = {
    .stiffness = 200.0f,
    .damping = 20.0f,
    .mass = 1.0f,
    .velocity = 0.0f
};

const spring_params_t SPRING_BOUNCY = {
    .stiffness = 300.0f,
    .damping = 10.0f,
    .mass = 1.0f,
    .velocity = 0.0f
};

const spring_params_t SPRING_WOBBLY = {
    .stiffness = 180.0f,
    .damping = 7.0f,
    .mass = 1.0f,
    .velocity = 0.0f
};

const spring_params_t SPRING_STIFF = {
    .stiffness = 500.0f,
    .damping = 30.0f,
    .mass = 1.0f,
    .velocity = 0.0f
};

/**
 * Get current time in microseconds
 */
uint64_t get_time_microseconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/**
 * Clamp value between min and max
 */
static float clamp(float value, float min, float max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

/**
 * Linear interpolation
 */
static float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

/**
 * Power function for easing
 */
static float pow_easing(float base, float exponent) {
    return powf(base, exponent);
}

// ============================================================================
// EASING FUNCTIONS
// ============================================================================

/**
 * Quadratic easing
 */
static float ease_in_quad(float t) {
    return t * t;
}

static float ease_out_quad(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t);
}

static float ease_in_out_quad(float t) {
    return t < 0.5f ? 2.0f * t * t : 1.0f - pow_easing(-2.0f * t + 2.0f, 2.0f) / 2.0f;
}

/**
 * Cubic easing
 */
static float ease_in_cubic(float t) {
    return t * t * t;
}

static float ease_out_cubic(float t) {
    float t1 = 1.0f - t;
    return 1.0f - t1 * t1 * t1;
}

static float ease_in_out_cubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - pow_easing(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

/**
 * Quartic easing
 */
static float ease_in_quart(float t) {
    return t * t * t * t;
}

static float ease_out_quart(float t) {
    float t1 = 1.0f - t;
    return 1.0f - t1 * t1 * t1 * t1;
}

static float ease_in_out_quart(float t) {
    return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - pow_easing(-2.0f * t + 2.0f, 4.0f) / 2.0f;
}

/**
 * Quintic easing
 */
static float ease_in_quint(float t) {
    return t * t * t * t * t;
}

static float ease_out_quint(float t) {
    float t1 = 1.0f - t;
    return 1.0f - t1 * t1 * t1 * t1 * t1;
}

static float ease_in_out_quint(float t) {
    return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - pow_easing(-2.0f * t + 2.0f, 5.0f) / 2.0f;
}

/**
 * Sinusoidal easing
 */
static float ease_in_sine(float t) {
    return 1.0f - cosf(t * M_PI / 2.0f);
}

static float ease_out_sine(float t) {
    return sinf(t * M_PI / 2.0f);
}

static float ease_in_out_sine(float t) {
    return -(cosf(M_PI * t) - 1.0f) / 2.0f;
}

/**
 * Exponential easing
 */
static float ease_in_expo(float t) {
    return t == 0.0f ? 0.0f : pow_easing(2.0f, 10.0f * t - 10.0f);
}

static float ease_out_expo(float t) {
    return t == 1.0f ? 1.0f : 1.0f - pow_easing(2.0f, -10.0f * t);
}

static float ease_in_out_expo(float t) {
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return t < 0.5f ? pow_easing(2.0f, 20.0f * t - 10.0f) / 2.0f
                    : (2.0f - pow_easing(2.0f, -20.0f * t + 10.0f)) / 2.0f;
}

/**
 * Circular easing
 */
static float ease_in_circ(float t) {
    return 1.0f - sqrtf(1.0f - t * t);
}

static float ease_out_circ(float t) {
    return sqrtf(1.0f - (t - 1.0f) * (t - 1.0f));
}

static float ease_in_out_circ(float t) {
    return t < 0.5f ? (1.0f - sqrtf(1.0f - 4.0f * t * t)) / 2.0f
                    : (sqrtf(1.0f - pow_easing(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;
}

/**
 * Back easing (overshoot)
 */
static float ease_in_back(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}

static float ease_out_back(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * pow_easing(t - 1.0f, 3.0f) + c1 * pow_easing(t - 1.0f, 2.0f);
}

static float ease_in_out_back(float t) {
    const float c1 = 1.70158f;
    const float c2 = c1 * 1.525f;
    return t < 0.5f
        ? (pow_easing(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) / 2.0f
        : (pow_easing(2.0f * t - 2.0f, 2.0f) * ((c2 + 1.0f) * (t * 2.0f - 2.0f) + c2) + 2.0f) / 2.0f;
}

/**
 * Elastic easing
 */
static float ease_in_elastic(float t) {
    const float c4 = (2.0f * M_PI) / 3.0f;
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return -pow_easing(2.0f, 10.0f * t - 10.0f) * sinf((t * 10.0f - 10.75f) * c4);
}

static float ease_out_elastic(float t) {
    const float c4 = (2.0f * M_PI) / 3.0f;
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return pow_easing(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
}

static float ease_in_out_elastic(float t) {
    const float c5 = (2.0f * M_PI) / 4.5f;
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    return t < 0.5f
        ? -(pow_easing(2.0f, 20.0f * t - 10.0f) * sinf((20.0f * t - 11.125f) * c5)) / 2.0f
        : (pow_easing(2.0f, -20.0f * t + 10.0f) * sinf((20.0f * t - 11.125f) * c5)) / 2.0f + 1.0f;
}

/**
 * Bounce easing
 */
static float ease_out_bounce(float t) {
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

static float ease_in_bounce(float t) {
    return 1.0f - ease_out_bounce(1.0f - t);
}

static float ease_in_out_bounce(float t) {
    return t < 0.5f
        ? (1.0f - ease_out_bounce(1.0f - 2.0f * t)) / 2.0f
        : (1.0f + ease_out_bounce(2.0f * t - 1.0f)) / 2.0f;
}

/**
 * Main easing function dispatcher
 */
float easing_function(easing_t easing, float t) {
    t = clamp(t, 0.0f, 1.0f);

    switch (easing) {
        case EASE_LINEAR:           return t;
        case EASE_IN_QUAD:          return ease_in_quad(t);
        case EASE_OUT_QUAD:         return ease_out_quad(t);
        case EASE_IN_OUT_QUAD:      return ease_in_out_quad(t);
        case EASE_IN_CUBIC:         return ease_in_cubic(t);
        case EASE_OUT_CUBIC:        return ease_out_cubic(t);
        case EASE_IN_OUT_CUBIC:     return ease_in_out_cubic(t);
        case EASE_IN_QUART:         return ease_in_quart(t);
        case EASE_OUT_QUART:        return ease_out_quart(t);
        case EASE_IN_OUT_QUART:     return ease_in_out_quart(t);
        case EASE_IN_QUINT:         return ease_in_quint(t);
        case EASE_OUT_QUINT:        return ease_out_quint(t);
        case EASE_IN_OUT_QUINT:     return ease_in_out_quint(t);
        case EASE_IN_SINE:          return ease_in_sine(t);
        case EASE_OUT_SINE:         return ease_out_sine(t);
        case EASE_IN_OUT_SINE:      return ease_in_out_sine(t);
        case EASE_IN_EXPO:          return ease_in_expo(t);
        case EASE_OUT_EXPO:         return ease_out_expo(t);
        case EASE_IN_OUT_EXPO:      return ease_in_out_expo(t);
        case EASE_IN_CIRC:          return ease_in_circ(t);
        case EASE_OUT_CIRC:         return ease_out_circ(t);
        case EASE_IN_OUT_CIRC:      return ease_in_out_circ(t);
        case EASE_IN_BACK:          return ease_in_back(t);
        case EASE_OUT_BACK:         return ease_out_back(t);
        case EASE_IN_OUT_BACK:      return ease_in_out_back(t);
        case EASE_IN_ELASTIC:       return ease_in_elastic(t);
        case EASE_OUT_ELASTIC:      return ease_out_elastic(t);
        case EASE_IN_OUT_ELASTIC:   return ease_in_out_elastic(t);
        case EASE_IN_BOUNCE:        return ease_in_bounce(t);
        case EASE_OUT_BOUNCE:       return ease_out_bounce(t);
        case EASE_IN_OUT_BOUNCE:    return ease_in_out_bounce(t);
        default:                    return t;
    }
}

/**
 * Spring physics easing
 */
float easing_spring(float t, const spring_params_t *params, float *velocity) {
    if (!params) return t;

    // Spring-damper system simulation
    float displacement = 1.0f - t;
    float spring_force = -params->stiffness * displacement;
    float damping_force = -params->damping * (*velocity);
    float acceleration = (spring_force + damping_force) / params->mass;

    *velocity += acceleration * 0.016f;  // Assume 60fps delta
    displacement += (*velocity) * 0.016f;

    return 1.0f - displacement;
}

/**
 * Cubic bezier curve easing
 */
float easing_bezier(float t, const bezier_params_t *params) {
    if (!params) return t;

    // Use Newton-Raphson method to solve for t
    float x1 = params->x1;
    float y1 = params->y1;
    float x2 = params->x2;
    float y2 = params->y2;

    // Simplified cubic bezier evaluation
    float u = 1.0f - t;
    float tt = t * t;
    float uu = u * u;
    float uuu = uu * u;
    float ttt = tt * t;

    float y = uuu * 0.0f +           // P0
              3.0f * uu * t * y1 +   // P1
              3.0f * u * tt * y2 +   // P2
              ttt * 1.0f;            // P3

    return y;
}

// ============================================================================
// COLOR INTERPOLATION
// ============================================================================

color_t color_lerp(color_t from, color_t to, float t) {
    return (color_t){
        .r = lerp(from.r, to.r, t),
        .g = lerp(from.g, to.g, t),
        .b = lerp(from.b, to.b, t),
        .a = lerp(from.a, to.a, t)
    };
}

// ============================================================================
// TRANSFORM INTERPOLATION
// ============================================================================

transform_t transform_lerp(transform_t from, transform_t to, float t) {
    return (transform_t){
        .tx = lerp(from.tx, to.tx, t),
        .ty = lerp(from.ty, to.ty, t),
        .sx = lerp(from.sx, to.sx, t),
        .sy = lerp(from.sy, to.sy, t),
        .rotation = lerp(from.rotation, to.rotation, t),
        .anchor_x = lerp(from.anchor_x, to.anchor_x, t),
        .anchor_y = lerp(from.anchor_y, to.anchor_y, t)
    };
}

// ============================================================================
// RECTANGLE INTERPOLATION
// ============================================================================

rect_f_t rect_lerp(rect_f_t from, rect_f_t to, float t) {
    return (rect_f_t){
        .x = lerp(from.x, to.x, t),
        .y = lerp(from.y, to.y, t),
        .w = lerp(from.w, to.w, t),
        .h = lerp(from.h, to.h, t)
    };
}

// ============================================================================
// ANIMATOR
// ============================================================================

/**
 * Create animator
 */
animator_t *animator_create(void) {
    animator_t *animator = calloc(1, sizeof(animator_t));
    if (!animator) {
        fprintf(stderr, "Failed to allocate animator\n");
        return NULL;
    }

    animator->animations = NULL;
    animator->animation_count = 0;
    animator->reduce_motion = false;
    animator->global_speed = 1.0f;
    animator->current_time = get_time_microseconds();
    animator->last_update = animator->current_time;
    animator->fps = 60;

    return animator;
}

/**
 * Destroy animator and all animations
 */
void animator_destroy(animator_t *animator) {
    if (!animator) return;

    animation_t *current = animator->animations;
    while (current) {
        animation_t *next = current->next;
        free(current);
        current = next;
    }

    free(animator);
}

/**
 * Set reduce motion mode (accessibility)
 */
void animator_set_reduce_motion(animator_t *animator, bool enabled) {
    if (animator) {
        animator->reduce_motion = enabled;
    }
}

/**
 * Set global animation speed multiplier
 */
void animator_set_global_speed(animator_t *animator, float speed) {
    if (animator) {
        animator->global_speed = clamp(speed, 0.1f, 10.0f);
    }
}

// ============================================================================
// ANIMATION CREATION
// ============================================================================

static uint32_t next_animation_id = 1;

/**
 * Create basic animation
 */
animation_t *anim_create(animator_t *animator, uint64_t duration, easing_t easing) {
    if (!animator) return NULL;

    animation_t *anim = calloc(1, sizeof(animation_t));
    if (!anim) {
        fprintf(stderr, "Failed to allocate animation\n");
        return NULL;
    }

    anim->id = next_animation_id++;
    anim->type = ANIM_TYPE_VALUE;
    anim->state = ANIM_STATE_IDLE;
    anim->duration = duration;
    anim->easing = easing;
    anim->repeat_count = 0;
    anim->auto_reverse = false;

    // Add to linked list
    anim->next = animator->animations;
    animator->animations = anim;
    animator->animation_count++;

    return anim;
}

/**
 * Create value animation with from/to
 */
animation_t *anim_from_to(animator_t *animator, float from, float to, uint64_t duration, easing_t easing) {
    animation_t *anim = anim_create(animator, duration, easing);
    if (anim) {
        anim->from = from;
        anim->to = to;
        anim->current = from;
    }
    return anim;
}

/**
 * Create spring animation
 */
animation_t *anim_spring(animator_t *animator, float from, float to, const spring_params_t *params) {
    animation_t *anim = anim_create(animator, 1000000, EASE_SPRING);  // 1 second default
    if (anim && params) {
        anim->from = from;
        anim->to = to;
        anim->current = from;
        anim->spring = *params;
        anim->spring_velocity = params->velocity;
    }
    return anim;
}

/**
 * Create color animation
 */
animation_t *anim_color(animator_t *animator, color_t from, color_t to, uint64_t duration, easing_t easing) {
    animation_t *anim = anim_create(animator, duration, easing);
    if (anim) {
        anim->type = ANIM_TYPE_COLOR;
        anim->from_color = from;
        anim->to_color = to;
        anim->current_color = from;
    }
    return anim;
}

/**
 * Create transform animation
 */
animation_t *anim_transform(animator_t *animator, transform_t from, transform_t to, uint64_t duration, easing_t easing) {
    animation_t *anim = anim_create(animator, duration, easing);
    if (anim) {
        anim->type = ANIM_TYPE_TRANSFORM;
        anim->from_transform = from;
        anim->to_transform = to;
        anim->current_transform = from;
    }
    return anim;
}

/**
 * Create rectangle animation
 */
animation_t *anim_rect(animator_t *animator, rect_f_t from, rect_f_t to, uint64_t duration, easing_t easing) {
    animation_t *anim = anim_create(animator, duration, easing);
    if (anim) {
        anim->type = ANIM_TYPE_RECT;
        anim->from_rect = from;
        anim->to_rect = to;
        anim->current_rect = from;
    }
    return anim;
}

/**
 * Create keyframe animation
 */
animation_t *anim_keyframe(animator_t *animator) {
    animation_t *anim = anim_create(animator, 1000000, EASE_LINEAR);
    if (anim) {
        anim->type = ANIM_TYPE_KEYFRAME;
        anim->keyframe_count = 0;
        anim->current_keyframe = 0;
    }
    return anim;
}

/**
 * Add keyframe to animation
 */
void anim_add_keyframe(animation_t *anim, float time, float value, easing_t easing) {
    if (!anim || anim->type != ANIM_TYPE_KEYFRAME) return;
    if (anim->keyframe_count >= MAX_KEYFRAMES) return;

    anim->keyframes[anim->keyframe_count] = (keyframe_t){
        .time = clamp(time, 0.0f, 1.0f),
        .value = value,
        .easing = easing
    };
    anim->keyframe_count++;
}

// Continued in next part due to length...
