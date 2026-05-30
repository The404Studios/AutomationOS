/**
 * AutomationOS Window Manager Animations
 *
 * Window lifecycle animations:
 * - Open, close, minimize, maximize, fullscreen
 * - Workspace switching, tiling animations
 * - Focus transitions
 */

#ifndef WINDOW_ANIMATIONS_H
#define WINDOW_ANIMATIONS_H

#include "animator.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * Window animation state
 */
typedef struct {
    rect_f_t geometry;          // Current window position/size
    float opacity;
    float scale;
    float rotation;             // For 3D effects
    float blur_amount;
    bool visible;

    // Shadow properties
    float shadow_opacity;
    float shadow_blur;

    // For minimize/maximize tracking
    rect_f_t icon_geometry;     // Dock/taskbar icon position
} window_anim_t;

// ============================================================================
// WINDOW LIFECYCLE ANIMATIONS
// ============================================================================

/**
 * Window open animation
 * Fade in (0 → 1) + scale (0.9 → 1.0)
 * Duration: 200ms
 */
void wm_window_open(animator_t *animator, window_anim_t *window);

/**
 * Window close animation
 * Fade out + scale (1.0 → 0.95)
 * Duration: 150ms
 */
void wm_window_close(animator_t *animator, window_anim_t *window);

/**
 * Window minimize animation
 * Scale down + move to dock icon (genie effect optional)
 * Duration: 300ms
 */
void wm_window_minimize(animator_t *animator, window_anim_t *window, bool genie_effect);

/**
 * Window unminimize (restore)
 * Reverse of minimize
 * Duration: 300ms
 */
void wm_window_unminimize(animator_t *animator, window_anim_t *window);

/**
 * Window maximize animation
 * Smooth expansion to fullscreen
 * Duration: 200ms
 */
void wm_window_maximize(animator_t *animator, window_anim_t *window, rect_f_t fullscreen_rect);

/**
 * Window unmaximize (restore)
 * Return to original size
 * Duration: 200ms
 */
void wm_window_unmaximize(animator_t *animator, window_anim_t *window, rect_f_t restore_rect);

/**
 * Window fullscreen transition
 * Similar to maximize but hides decorations
 * Duration: 250ms
 */
void wm_window_fullscreen(animator_t *animator, window_anim_t *window, rect_f_t fullscreen_rect);

/**
 * Exit fullscreen
 * Duration: 250ms
 */
void wm_window_unfullscreen(animator_t *animator, window_anim_t *window, rect_f_t restore_rect);

// ============================================================================
// WINDOW MOVEMENT ANIMATIONS
// ============================================================================

/**
 * Window move animation
 * Smooth position transition
 * Duration: 250ms
 */
void wm_window_move(animator_t *animator, window_anim_t *window, int32_t target_x, int32_t target_y);

/**
 * Window resize animation
 * Smooth size transition
 * Duration: 200ms
 */
void wm_window_resize(animator_t *animator, window_anim_t *window, int32_t target_w, int32_t target_h);

/**
 * Window snap animation (to screen edge)
 * Quick snap with slight bounce
 * Duration: 150ms
 */
void wm_window_snap(animator_t *animator, window_anim_t *window, rect_f_t snap_rect);

// ============================================================================
// FOCUS ANIMATIONS
// ============================================================================

/**
 * Window focus animation
 * Slight scale up + shadow increase
 * Duration: 150ms
 */
void wm_window_focus(animator_t *animator, window_anim_t *window);

/**
 * Window unfocus animation
 * Reduce shadow, dim slightly
 * Duration: 150ms
 */
void wm_window_unfocus(animator_t *animator, window_anim_t *window);

/**
 * Attention animation (urgent window)
 * Pulsing glow or bounce
 * Duration: infinite loop
 */
animation_t *wm_window_attention(animator_t *animator, window_anim_t *window);

// ============================================================================
// WORKSPACE ANIMATIONS
// ============================================================================

/**
 * Workspace slide transition
 * All windows slide left/right together
 * Duration: 350ms
 */
void wm_workspace_slide(animator_t *animator, window_anim_t *windows, uint32_t count, bool slide_left);

/**
 * Workspace fade transition
 * Crossfade between workspaces
 * Duration: 250ms
 */
void wm_workspace_fade(animator_t *animator,
                       window_anim_t *old_windows, uint32_t old_count,
                       window_anim_t *new_windows, uint32_t new_count);

/**
 * Workspace cube rotation (3D effect)
 * Rotate cube to show different face
 * Duration: 400ms
 */
void wm_workspace_cube(animator_t *animator, window_anim_t *windows, uint32_t count, float rotation_angle);

/**
 * Workspace zoom (expo view)
 * Zoom out to show all workspaces
 * Duration: 300ms
 */
void wm_workspace_zoom_out(animator_t *animator, window_anim_t *windows, uint32_t count);

/**
 * Workspace zoom in (from expo)
 * Zoom back to selected workspace
 * Duration: 300ms
 */
void wm_workspace_zoom_in(animator_t *animator, window_anim_t *windows, uint32_t count);

// ============================================================================
// TILING ANIMATIONS
// ============================================================================

/**
 * Window tile animation
 * Smooth transition to tiled position
 * Duration: 250ms
 */
void wm_window_tile(animator_t *animator, window_anim_t *window, rect_f_t tile_rect);

/**
 * Re-tile animation (when layout changes)
 * All tiled windows smoothly move to new positions
 * Duration: 300ms
 */
void wm_retile_all(animator_t *animator, window_anim_t *windows, rect_f_t *target_rects, uint32_t count);

/**
 * Master-stack swap animation
 * Swap positions of two windows
 * Duration: 250ms
 */
void wm_window_swap(animator_t *animator, window_anim_t *window1, window_anim_t *window2);

// ============================================================================
// SPECIAL EFFECTS
// ============================================================================

/**
 * Window shake animation (error/notification)
 * Shake horizontally
 * Duration: 300ms
 */
void wm_window_shake(animator_t *animator, window_anim_t *window);

/**
 * Window flash (highlight)
 * Brief opacity/color flash
 * Duration: 200ms
 */
void wm_window_flash(animator_t *animator, window_anim_t *window);

/**
 * Window burn effect (on close)
 * Fire/dissolve effect when closing
 * Duration: 400ms
 */
void wm_window_burn(animator_t *animator, window_anim_t *window);

/**
 * Window wobbly effect (physics)
 * Spring-like jelly effect during movement
 * Duration: varies (spring-based)
 */
void wm_window_wobbly_start(animator_t *animator, window_anim_t *window);
void wm_window_wobbly_stop(animator_t *animator, window_anim_t *window);

// ============================================================================
// COMPOSITOR EFFECTS
// ============================================================================

/**
 * Screen flash (screenshot feedback)
 * Brief white flash
 * Duration: 150ms
 */
void wm_screen_flash(animator_t *animator, float *flash_opacity);

/**
 * Screen shake (system notification)
 * Shake entire screen
 * Duration: 200ms
 */
void wm_screen_shake(animator_t *animator, float *shake_offset_x, float *shake_offset_y);

/**
 * Zoom effect (accessibility)
 * Smooth zoom in/out
 * Duration: 250ms
 */
void wm_screen_zoom(animator_t *animator, float *zoom_level, float target_zoom);

/**
 * Dim/brighten screen
 * For idle dimming or power management
 * Duration: 500ms
 */
void wm_screen_dim(animator_t *animator, float *brightness, float target);

// ============================================================================
// ALT+TAB SWITCHER ANIMATIONS
// ============================================================================

/**
 * Switcher appear
 * Fade + scale in
 * Duration: 150ms
 */
void wm_switcher_show(animator_t *animator, window_anim_t *switcher);

/**
 * Switcher hide
 * Fade + scale out
 * Duration: 100ms
 */
void wm_switcher_hide(animator_t *animator, window_anim_t *switcher);

/**
 * Switcher scroll
 * Smooth scroll to next/prev window
 * Duration: 150ms
 */
void wm_switcher_scroll(animator_t *animator, float *scroll_offset, float target_offset);

// ============================================================================
// DRAG & DROP WINDOW ANIMATIONS
// ============================================================================

/**
 * Window drag start
 * Lift window (scale + shadow)
 * Duration: 100ms
 */
void wm_window_drag_start(animator_t *animator, window_anim_t *window);

/**
 * Window drag end
 * Drop window (scale + shadow back)
 * Duration: 150ms
 */
void wm_window_drag_end(animator_t *animator, window_anim_t *window);

/**
 * Drop zone highlight
 * Highlight area where window can be dropped
 * Duration: infinite (pulsing)
 */
animation_t *wm_drop_zone_highlight(animator_t *animator, window_anim_t *zone);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * Genie effect (for minimize)
 * Complex path animation from window to icon
 */
void wm_genie_effect(animator_t *animator, window_anim_t *window);

/**
 * Create animation group for all windows
 * Animate multiple windows simultaneously
 */
animation_group_t *wm_animate_all_windows(animator_t *animator,
                                          window_anim_t *windows,
                                          uint32_t count,
                                          void (*anim_func)(animator_t*, window_anim_t*));

/**
 * Cancel all window animations
 */
void wm_cancel_all_animations(window_anim_t *windows, uint32_t count);

/**
 * Check if any window is animating
 */
bool wm_any_window_animating(const window_anim_t *windows, uint32_t count);

// ============================================================================
// PRESET ANIMATION PROFILES
// ============================================================================

typedef enum {
    WM_ANIM_PROFILE_NONE,       // No animations
    WM_ANIM_PROFILE_MINIMAL,    // Fast, simple animations
    WM_ANIM_PROFILE_NORMAL,     // Balanced
    WM_ANIM_PROFILE_FANCY,      // Full effects
} wm_anim_profile_t;

/**
 * Set global animation profile
 */
void wm_set_animation_profile(wm_anim_profile_t profile);
wm_anim_profile_t wm_get_animation_profile(void);

#endif // WINDOW_ANIMATIONS_H
