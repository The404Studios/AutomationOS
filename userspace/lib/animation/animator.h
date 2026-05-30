/**
 * AutomationOS Advanced Animation Engine
 *
 * Full-featured animation system with easing, spring physics,
 * bezier curves, keyframe animation, and animation groups.
 */

#ifndef ANIMATOR_H
#define ANIMATOR_H

#include <stdint.h>
#include <stdbool.h>

// Maximum animation limits
#define MAX_ANIMATIONS 512
#define MAX_KEYFRAMES 32
#define MAX_GROUP_ANIMATIONS 16
#define MAX_ANIMATION_CALLBACKS 8

/**
 * Animation types
 */
typedef enum {
    ANIM_TYPE_VALUE,        // Animate single float value
    ANIM_TYPE_COLOR,        // RGBA color interpolation
    ANIM_TYPE_RECT,         // Rectangle (x, y, w, h)
    ANIM_TYPE_TRANSFORM,    // 2D transform (translation, rotation, scale)
    ANIM_TYPE_KEYFRAME,     // Multi-keyframe animation
    ANIM_TYPE_SPRING,       // Spring physics simulation
    ANIM_TYPE_BEZIER,       // Custom cubic bezier curve
} anim_type_t;

/**
 * Easing functions - comprehensive set
 */
typedef enum {
    // Basic easing
    EASE_LINEAR,

    // Quadratic
    EASE_IN_QUAD,
    EASE_OUT_QUAD,
    EASE_IN_OUT_QUAD,

    // Cubic
    EASE_IN_CUBIC,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_CUBIC,

    // Quartic
    EASE_IN_QUART,
    EASE_OUT_QUART,
    EASE_IN_OUT_QUART,

    // Quintic
    EASE_IN_QUINT,
    EASE_OUT_QUINT,
    EASE_IN_OUT_QUINT,

    // Sinusoidal
    EASE_IN_SINE,
    EASE_OUT_SINE,
    EASE_IN_OUT_SINE,

    // Exponential
    EASE_IN_EXPO,
    EASE_OUT_EXPO,
    EASE_IN_OUT_EXPO,

    // Circular
    EASE_IN_CIRC,
    EASE_OUT_CIRC,
    EASE_IN_OUT_CIRC,

    // Back (overshoot)
    EASE_IN_BACK,
    EASE_OUT_BACK,
    EASE_IN_OUT_BACK,

    // Elastic
    EASE_IN_ELASTIC,
    EASE_OUT_ELASTIC,
    EASE_IN_OUT_ELASTIC,

    // Bounce
    EASE_IN_BOUNCE,
    EASE_OUT_BOUNCE,
    EASE_IN_OUT_BOUNCE,

    // Custom
    EASE_SPRING,            // Spring physics
    EASE_CUSTOM_BEZIER,     // User-defined bezier curve
} easing_t;

/**
 * Animation state
 */
typedef enum {
    ANIM_STATE_IDLE,        // Not started
    ANIM_STATE_DELAYED,     // Waiting for delay
    ANIM_STATE_RUNNING,     // Currently animating
    ANIM_STATE_PAUSED,      // Paused
    ANIM_STATE_FINISHED,    // Completed
    ANIM_STATE_CANCELED,    // Canceled before completion
} anim_state_t;

/**
 * Color structure (RGBA)
 */
typedef struct {
    float r, g, b, a;
} color_t;

/**
 * 2D Transform
 */
typedef struct {
    float tx, ty;           // Translation
    float sx, sy;           // Scale
    float rotation;         // Rotation in radians
    float anchor_x, anchor_y; // Transform origin
} transform_t;

/**
 * Rectangle
 */
typedef struct {
    float x, y, w, h;
} rect_f_t;

/**
 * Keyframe
 */
typedef struct {
    float time;             // Normalized time [0, 1]
    float value;
    easing_t easing;        // Easing to next keyframe
} keyframe_t;

/**
 * Spring parameters (for spring physics)
 */
typedef struct {
    float stiffness;        // Spring constant (100-500)
    float damping;          // Damping ratio (0.5-1.0)
    float mass;             // Mass (1.0 default)
    float velocity;         // Initial velocity
} spring_params_t;

/**
 * Bezier curve parameters
 */
typedef struct {
    float x1, y1;           // First control point
    float x2, y2;           // Second control point
} bezier_params_t;

/**
 * Animation callbacks
 */
typedef void (*anim_callback_t)(void *user_data);
typedef void (*anim_update_callback_t)(float value, void *user_data);

/**
 * Core animation structure
 */
typedef struct animation {
    uint32_t id;
    anim_type_t type;
    anim_state_t state;

    // Timing
    uint64_t start_time;        // Microseconds
    uint64_t duration;          // Microseconds
    uint64_t delay;             // Delay before starting
    int32_t repeat_count;       // -1 for infinite, 0 for once, >0 for N times
    bool auto_reverse;          // Reverse on alternate iterations

    // Easing
    easing_t easing;
    spring_params_t spring;     // For EASE_SPRING
    bezier_params_t bezier;     // For EASE_CUSTOM_BEZIER

    // Value animation
    float from;
    float to;
    float current;
    float *target;              // Optional pointer to auto-update

    // Color animation
    color_t from_color;
    color_t to_color;
    color_t current_color;
    color_t *target_color;

    // Transform animation
    transform_t from_transform;
    transform_t to_transform;
    transform_t current_transform;
    transform_t *target_transform;

    // Rectangle animation
    rect_f_t from_rect;
    rect_f_t to_rect;
    rect_f_t current_rect;
    rect_f_t *target_rect;

    // Keyframe animation
    keyframe_t keyframes[MAX_KEYFRAMES];
    uint32_t keyframe_count;
    uint32_t current_keyframe;

    // Callbacks
    anim_callback_t on_start;
    anim_update_callback_t on_update;
    anim_callback_t on_complete;
    anim_callback_t on_cancel;
    void *user_data;

    // Internal state
    uint32_t current_iteration;
    bool reversing;
    float spring_velocity;      // For spring physics

    struct animation *next;     // Linked list
} animation_t;

/**
 * Animation group - synchronized animations
 */
typedef struct {
    animation_t *animations[MAX_GROUP_ANIMATIONS];
    uint32_t count;
    bool wait_for_all;          // true = wait for all, false = wait for first
    anim_callback_t on_complete;
    void *user_data;
} animation_group_t;

/**
 * Animator - manages all animations
 */
typedef struct {
    animation_t *animations;    // Linked list
    uint32_t animation_count;

    bool reduce_motion;         // Accessibility: reduce animations
    float global_speed;         // Speed multiplier (1.0 = normal)

    uint64_t current_time;      // Updated each frame
    uint64_t last_update;
    uint32_t fps;
} animator_t;

// Animator lifecycle
animator_t *animator_create(void);
void animator_destroy(animator_t *animator);
void animator_update(animator_t *animator);  // Call once per frame
void animator_set_reduce_motion(animator_t *animator, bool enabled);
void animator_set_global_speed(animator_t *animator, float speed);

// Animation creation - value
animation_t *anim_create(animator_t *animator, uint64_t duration, easing_t easing);
animation_t *anim_from_to(animator_t *animator, float from, float to, uint64_t duration, easing_t easing);
animation_t *anim_spring(animator_t *animator, float from, float to, const spring_params_t *params);

// Animation creation - color
animation_t *anim_color(animator_t *animator, color_t from, color_t to, uint64_t duration, easing_t easing);

// Animation creation - transform
animation_t *anim_transform(animator_t *animator, transform_t from, transform_t to, uint64_t duration, easing_t easing);

// Animation creation - rectangle
animation_t *anim_rect(animator_t *animator, rect_f_t from, rect_f_t to, uint64_t duration, easing_t easing);

// Animation creation - keyframes
animation_t *anim_keyframe(animator_t *animator);
void anim_add_keyframe(animation_t *anim, float time, float value, easing_t easing);

// Animation control
void anim_start(animation_t *anim);
void anim_pause(animation_t *anim);
void anim_resume(animation_t *anim);
void anim_stop(animation_t *anim);
void anim_cancel(animation_t *anim);
void anim_seek(animation_t *anim, float normalized_time);  // 0.0 - 1.0

// Animation configuration
void anim_set_delay(animation_t *anim, uint64_t delay_us);
void anim_set_repeat(animation_t *anim, int32_t count);
void anim_set_auto_reverse(animation_t *anim, bool enabled);
void anim_set_target(animation_t *anim, float *target);
void anim_set_target_color(animation_t *anim, color_t *target);
void anim_set_target_transform(animation_t *anim, transform_t *target);
void anim_set_target_rect(animation_t *anim, rect_f_t *target);
void anim_set_bezier(animation_t *anim, float x1, float y1, float x2, float y2);

// Callbacks
void anim_on_start(animation_t *anim, anim_callback_t callback, void *user_data);
void anim_on_update(animation_t *anim, anim_update_callback_t callback, void *user_data);
void anim_on_complete(animation_t *anim, anim_callback_t callback, void *user_data);
void anim_on_cancel(animation_t *anim, anim_callback_t callback, void *user_data);

// Query
bool anim_is_running(const animation_t *anim);
bool anim_is_finished(const animation_t *anim);
float anim_get_progress(const animation_t *anim);
float anim_get_current_value(const animation_t *anim);

// Animation groups
animation_group_t *anim_group_create(void);
void anim_group_add(animation_group_t *group, animation_t *anim);
void anim_group_start(animation_group_t *group);
void anim_group_destroy(animation_group_t *group);

// Easing functions
float easing_function(easing_t easing, float t);
float easing_spring(float t, const spring_params_t *params, float *velocity);
float easing_bezier(float t, const bezier_params_t *params);

// Utilities
color_t color_lerp(color_t from, color_t to, float t);
transform_t transform_lerp(transform_t from, transform_t to, float t);
rect_f_t rect_lerp(rect_f_t from, rect_f_t to, float t);
uint64_t get_time_microseconds(void);

// Preset animations (durations in milliseconds)
#define DURATION_INSTANT    0
#define DURATION_FAST       100000      // 100ms
#define DURATION_NORMAL     200000      // 200ms
#define DURATION_SLOW       300000      // 300ms
#define DURATION_SLOWER     500000      // 500ms

// Preset spring configurations
extern const spring_params_t SPRING_SMOOTH;    // Smooth, no bounce
extern const spring_params_t SPRING_BOUNCY;    // Bouncy spring
extern const spring_params_t SPRING_WOBBLY;    // Very bouncy
extern const spring_params_t SPRING_STIFF;     // Fast, stiff spring

#endif // ANIMATOR_H
