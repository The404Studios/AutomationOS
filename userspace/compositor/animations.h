/**
 * AutomationOS Animation System
 *
 * Smooth animations for window transitions
 */

#ifndef ANIMATIONS_H
#define ANIMATIONS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Animation types
 */
typedef enum {
    ANIM_FADE,          // Opacity transition
    ANIM_SLIDE,         // Position transition
    ANIM_SCALE,         // Size transition
    ANIM_BLUR,          // Blur amount transition
    ANIM_WOBBLE,        // Physics-based wobble
} anim_type_t;

/**
 * Easing functions
 */
typedef enum {
    EASING_LINEAR,
    EASING_EASE_IN,
    EASING_EASE_OUT,
    EASING_EASE_IN_OUT,
    EASING_EASE_IN_QUAD,
    EASING_EASE_OUT_QUAD,
    EASING_EASE_IN_OUT_QUAD,
    EASING_EASE_IN_CUBIC,
    EASING_EASE_OUT_CUBIC,
    EASING_EASE_IN_OUT_CUBIC,
    EASING_BOUNCE,
    EASING_ELASTIC,
} easing_t;

/**
 * Animation structure
 */
typedef struct animation {
    anim_type_t type;
    uint64_t start_time;    // Microseconds
    uint64_t duration;      // Microseconds
    easing_t easing;

    float from;
    float to;
    float current;

    bool finished;
    void (*on_complete)(void *user_data);
    void *user_data;
} animation_t;

// Animation creation
animation_t *animation_create(anim_type_t type, uint64_t duration, easing_t easing);
void animation_destroy(animation_t *anim);

// Animation control
void animation_start(animation_t *anim, float from, float to);
void animation_update(animation_t *anim);
void animation_stop(animation_t *anim);
bool animation_is_finished(animation_t *anim);

// Easing functions
float easing_apply(easing_t easing, float t);

// Preset animations
animation_t *animation_window_open(void);     // Scale from 0.8 to 1.0 + fade in
animation_t *animation_window_close(void);    // Scale to 0.8 + fade out
animation_t *animation_minimize(void);        // Slide to taskbar
animation_t *animation_maximize(void);        // Expand to fullscreen
animation_t *animation_workspace_switch(void); // Slide transition

#endif // ANIMATIONS_H
