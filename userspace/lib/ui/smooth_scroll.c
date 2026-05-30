/**
 * AutomationOS Smooth Scrolling Implementation
 */

#include "smooth_scroll.h"
#include <stdio.h>
#include <math.h>
#include <sys/time.h>

/**
 * Physics presets
 */
const scroll_physics_t SCROLL_PHYSICS_DEFAULT = {
    .friction = 0.92f,              // 8% deceleration per frame
    .bounce_stiffness = 0.15f,
    .bounce_damping = 0.7f,
    .velocity_threshold = 0.5f,     // Stop at 0.5 px/frame
    .overscroll_distance = 50.0f,
};

const scroll_physics_t SCROLL_PHYSICS_BOUNCY = {
    .friction = 0.95f,              // Less friction = longer momentum
    .bounce_stiffness = 0.2f,       // Bouncier
    .bounce_damping = 0.6f,
    .velocity_threshold = 0.5f,
    .overscroll_distance = 80.0f,
};

const scroll_physics_t SCROLL_PHYSICS_STIFF = {
    .friction = 0.88f,              // More friction = quick stop
    .bounce_stiffness = 0.25f,      // Stiffer bounce
    .bounce_damping = 0.8f,
    .velocity_threshold = 1.0f,
    .overscroll_distance = 30.0f,
};

const scroll_physics_t SCROLL_PHYSICS_SMOOTH = {
    .friction = 0.94f,
    .bounce_stiffness = 0.12f,      // Very smooth bounce
    .bounce_damping = 0.75f,
    .velocity_threshold = 0.3f,
    .overscroll_distance = 60.0f,
};

/**
 * Get current time in microseconds
 */
static uint64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
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
 * Initialize scroll state
 */
void scroll_init(scroll_state_t *scroll, float min, float max) {
    scroll_init_custom(scroll, min, max, &SCROLL_PHYSICS_DEFAULT);
}

/**
 * Initialize with custom physics
 */
void scroll_init_custom(scroll_state_t *scroll,
                        float min,
                        float max,
                        const scroll_physics_t *physics) {
    if (!scroll) return;

    scroll->position = min;
    scroll->velocity = 0.0f;
    scroll->target_position = min;

    scroll->dragging = false;
    scroll->animating = false;
    scroll->bouncing = false;

    scroll->drag_start_pos = 0.0f;
    scroll->last_drag_pos = 0.0f;
    scroll->last_drag_time = 0;

    scroll->physics = physics ? *physics : SCROLL_PHYSICS_DEFAULT;
    scroll->physics.min_position = min;
    scroll->physics.max_position = max;

    scroll->last_update_time = get_time_us();
}

/**
 * Start drag
 */
void scroll_drag_start(scroll_state_t *scroll, float position) {
    if (!scroll) return;

    scroll->dragging = true;
    scroll->animating = false;
    scroll->velocity = 0.0f;

    scroll->drag_start_pos = position;
    scroll->last_drag_pos = position;
    scroll->last_drag_time = get_time_us();
}

/**
 * Update drag
 */
void scroll_drag_update(scroll_state_t *scroll, float position) {
    if (!scroll || !scroll->dragging) return;

    float delta = position - scroll->last_drag_pos;

    // Apply resistance when dragging past boundaries
    float new_pos = scroll->position + delta;
    if (new_pos < scroll->physics.min_position) {
        // Past top boundary - apply resistance
        float overshoot = scroll->physics.min_position - new_pos;
        delta *= 1.0f / (1.0f + overshoot / 100.0f);  // Exponential resistance
    } else if (new_pos > scroll->physics.max_position) {
        // Past bottom boundary - apply resistance
        float overshoot = new_pos - scroll->physics.max_position;
        delta *= 1.0f / (1.0f + overshoot / 100.0f);
    }

    scroll->position += delta;
    scroll->last_drag_pos = position;
    scroll->last_drag_time = get_time_us();
}

/**
 * End drag and calculate velocity
 */
void scroll_drag_end(scroll_state_t *scroll) {
    if (!scroll || !scroll->dragging) return;

    scroll->dragging = false;

    // Calculate velocity from recent drag movement
    uint64_t now = get_time_us();
    uint64_t time_delta = now - scroll->last_drag_time;

    if (time_delta > 0 && time_delta < 100000) {  // Within 100ms
        float distance = scroll->position - scroll->drag_start_pos;
        // Convert to pixels per frame (at 60 FPS)
        scroll->velocity = (distance * 60.0f * 1000000.0f) / (float)time_delta;

        // Clamp velocity
        float max_velocity = 50.0f;  // Max 50 px/frame
        scroll->velocity = clamp(scroll->velocity, -max_velocity, max_velocity);

        scroll->animating = true;
    }

    // If past boundaries, trigger bounce-back
    if (scroll->position < scroll->physics.min_position ||
        scroll->position > scroll->physics.max_position) {
        scroll->bouncing = true;
        scroll->animating = true;
    }
}

/**
 * Update scroll physics
 */
bool scroll_update(scroll_state_t *scroll, uint64_t current_time_us) {
    if (!scroll) return false;
    if (!scroll->animating && !scroll->bouncing) return false;

    // Calculate delta time
    float dt = (float)(current_time_us - scroll->last_update_time) / 1000000.0f;
    scroll->last_update_time = current_time_us;

    // Clamp dt to avoid huge jumps
    if (dt > 0.1f) dt = 0.1f;  // Max 100ms

    bool still_active = false;

    // Rubber-band bounce physics
    if (scroll->bouncing) {
        float target;
        if (scroll->position < scroll->physics.min_position) {
            target = scroll->physics.min_position;
        } else if (scroll->position > scroll->physics.max_position) {
            target = scroll->physics.max_position;
        } else {
            scroll->bouncing = false;
            return scroll->animating;
        }

        // Spring force: F = -k * x
        float displacement = scroll->position - target;
        float spring_force = -scroll->physics.bounce_stiffness * displacement;

        // Apply spring force to velocity
        scroll->velocity += spring_force;

        // Apply damping
        scroll->velocity *= scroll->physics.bounce_damping;

        // Update position
        scroll->position += scroll->velocity;

        // Check if settled
        if (fabsf(displacement) < 1.0f && fabsf(scroll->velocity) < 0.5f) {
            scroll->position = target;
            scroll->velocity = 0.0f;
            scroll->bouncing = false;
        } else {
            still_active = true;
        }
    }

    // Momentum/friction physics
    if (scroll->animating && !scroll->bouncing) {
        // Apply friction
        scroll->velocity *= scroll->physics.friction;

        // Update position
        scroll->position += scroll->velocity * dt * 60.0f;  // Normalize to 60 FPS

        // Check boundaries
        if (scroll->position < scroll->physics.min_position - scroll->physics.overscroll_distance) {
            scroll->position = scroll->physics.min_position - scroll->physics.overscroll_distance;
            scroll->velocity = 0.0f;
            scroll->bouncing = true;
        } else if (scroll->position > scroll->physics.max_position + scroll->physics.overscroll_distance) {
            scroll->position = scroll->physics.max_position + scroll->physics.overscroll_distance;
            scroll->velocity = 0.0f;
            scroll->bouncing = true;
        }

        // Start bounce-back if past boundaries
        if (scroll->position < scroll->physics.min_position ||
            scroll->position > scroll->physics.max_position) {
            scroll->bouncing = true;
        }

        // Stop if velocity too low
        if (fabsf(scroll->velocity) < scroll->physics.velocity_threshold) {
            scroll->velocity = 0.0f;
            scroll->animating = false;
        } else {
            still_active = true;
        }
    }

    return still_active;
}

/**
 * Scroll to position (animated)
 */
void scroll_to(scroll_state_t *scroll, float target_position, bool smooth) {
    if (!scroll) return;

    target_position = clamp(target_position,
                            scroll->physics.min_position,
                            scroll->physics.max_position);

    if (smooth) {
        // Calculate velocity needed to reach target
        float distance = target_position - scroll->position;
        scroll->velocity = distance * 0.2f;  // Ease towards target
        scroll->target_position = target_position;
        scroll->animating = true;
    } else {
        // Instant
        scroll->position = target_position;
        scroll->velocity = 0.0f;
        scroll->animating = false;
    }
}

/**
 * Scroll by delta
 */
void scroll_by(scroll_state_t *scroll, float delta) {
    if (!scroll) return;

    // Add to velocity for smooth scroll wheel
    scroll->velocity += delta * 0.5f;
    scroll->animating = true;
}

/**
 * Stop scrolling
 */
void scroll_stop(scroll_state_t *scroll) {
    if (!scroll) return;

    scroll->velocity = 0.0f;
    scroll->animating = false;
    scroll->bouncing = false;
    scroll->dragging = false;

    // Clamp to boundaries
    scroll->position = clamp(scroll->position,
                             scroll->physics.min_position,
                             scroll->physics.max_position);
}

/**
 * Get current position
 */
float scroll_get_position(const scroll_state_t *scroll) {
    return scroll ? scroll->position : 0.0f;
}

/**
 * Get current velocity
 */
float scroll_get_velocity(const scroll_state_t *scroll) {
    return scroll ? scroll->velocity : 0.0f;
}

/**
 * Check if active
 */
bool scroll_is_active(const scroll_state_t *scroll) {
    return scroll && (scroll->animating || scroll->bouncing || scroll->dragging);
}

/**
 * Check boundaries
 */
bool scroll_at_min(const scroll_state_t *scroll) {
    return scroll && scroll->position <= scroll->physics.min_position;
}

bool scroll_at_max(const scroll_state_t *scroll) {
    return scroll && scroll->position >= scroll->physics.max_position;
}

/**
 * Set boundaries
 */
void scroll_set_boundaries(scroll_state_t *scroll, float min, float max) {
    if (!scroll) return;

    scroll->physics.min_position = min;
    scroll->physics.max_position = max;

    // Clamp current position to new boundaries
    if (scroll->position < min || scroll->position > max) {
        scroll->position = clamp(scroll->position, min, max);
    }
}
