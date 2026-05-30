/**
 * AutomationOS Smooth Scrolling System
 *
 * Features:
 * - Kinetic scrolling with momentum
 * - Smooth deceleration
 * - Rubber-band bounce at edges
 * - Touch-like feel on desktop
 */

#ifndef SMOOTH_SCROLL_H
#define SMOOTH_SCROLL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Scroll physics parameters
 */
typedef struct {
    // Physics constants
    float friction;              // Deceleration rate (0.0 - 1.0)
    float bounce_stiffness;      // Spring stiffness for rubber-band
    float bounce_damping;        // Damping for rubber-band
    float velocity_threshold;    // Stop when velocity below this

    // Scrolling limits
    float min_position;          // Top/left boundary
    float max_position;          // Bottom/right boundary
    float overscroll_distance;   // How far past boundary allowed
} scroll_physics_t;

/**
 * Scroll state
 */
typedef struct {
    // Current state
    float position;              // Current scroll position
    float velocity;              // Current velocity (pixels/second)
    float target_position;       // Target for programmatic scrolls

    // Tracking
    bool dragging;               // User is actively dragging
    bool animating;              // Momentum animation active
    bool bouncing;               // Rubber-band bounce active

    // Drag tracking
    float drag_start_pos;
    float last_drag_pos;
    uint64_t last_drag_time;

    // Physics
    scroll_physics_t physics;

    // Metrics
    uint64_t last_update_time;
} scroll_state_t;

/**
 * Initialize scroll state with default physics
 */
void scroll_init(scroll_state_t *scroll, float min, float max);

/**
 * Initialize scroll state with custom physics
 */
void scroll_init_custom(scroll_state_t *scroll,
                        float min,
                        float max,
                        const scroll_physics_t *physics);

/**
 * Start drag (user pressed mouse/touch)
 */
void scroll_drag_start(scroll_state_t *scroll, float position);

/**
 * Update drag (user moved mouse/touch)
 */
void scroll_drag_update(scroll_state_t *scroll, float position);

/**
 * End drag (user released mouse/touch)
 * Calculates velocity and starts momentum animation
 */
void scroll_drag_end(scroll_state_t *scroll);

/**
 * Update scroll physics (call every frame)
 * Returns true if still animating, false if settled
 */
bool scroll_update(scroll_state_t *scroll, uint64_t current_time_us);

/**
 * Programmatic scroll to position (animated)
 */
void scroll_to(scroll_state_t *scroll, float target_position, bool smooth);

/**
 * Scroll by delta (e.g., mouse wheel)
 */
void scroll_by(scroll_state_t *scroll, float delta);

/**
 * Stop all scrolling immediately
 */
void scroll_stop(scroll_state_t *scroll);

/**
 * Get current scroll position
 */
float scroll_get_position(const scroll_state_t *scroll);

/**
 * Get current velocity
 */
float scroll_get_velocity(const scroll_state_t *scroll);

/**
 * Check if scrolling is active
 */
bool scroll_is_active(const scroll_state_t *scroll);

/**
 * Check if at boundary
 */
bool scroll_at_min(const scroll_state_t *scroll);
bool scroll_at_max(const scroll_state_t *scroll);

/**
 * Set boundaries (e.g., content size changed)
 */
void scroll_set_boundaries(scroll_state_t *scroll, float min, float max);

/**
 * Default physics presets
 */
extern const scroll_physics_t SCROLL_PHYSICS_DEFAULT;
extern const scroll_physics_t SCROLL_PHYSICS_BOUNCY;
extern const scroll_physics_t SCROLL_PHYSICS_STIFF;
extern const scroll_physics_t SCROLL_PHYSICS_SMOOTH;

#endif // SMOOTH_SCROLL_H
