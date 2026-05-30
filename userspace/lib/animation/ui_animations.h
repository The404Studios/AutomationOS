/**
 * AutomationOS UI Widget Animations
 *
 * Pre-built animations for common UI elements:
 * - Buttons, menus, tooltips, lists, progress bars, etc.
 */

#ifndef UI_ANIMATIONS_H
#define UI_ANIMATIONS_H

#include "animator.h"
#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct widget widget_t;
typedef struct button button_t;
typedef struct menu menu_t;
typedef struct tooltip tooltip_t;
typedef struct list list_t;
typedef struct progress_bar progress_bar_t;

/**
 * Widget animation state
 */
typedef struct {
    float scale;
    float opacity;
    float offset_x, offset_y;
    color_t bg_color;
    color_t text_color;
    bool visible;
} widget_anim_state_t;

// ============================================================================
// BUTTON ANIMATIONS
// ============================================================================

/**
 * Button hover animation
 * Scale: 1.0 → 1.05 (100ms)
 * Background color transition
 */
void ui_button_hover(animator_t *animator, widget_anim_state_t *state, color_t hover_color);

/**
 * Button unhover animation
 * Scale: current → 1.0 (100ms)
 */
void ui_button_unhover(animator_t *animator, widget_anim_state_t *state, color_t normal_color);

/**
 * Button click animation
 * Scale: 1.05 → 0.95 → 1.0 (150ms total)
 */
void ui_button_click(animator_t *animator, widget_anim_state_t *state);

/**
 * Button press/release (for long press)
 */
void ui_button_press(animator_t *animator, widget_anim_state_t *state);
void ui_button_release(animator_t *animator, widget_anim_state_t *state);

/**
 * Button pulse animation (attention grabber)
 * Scale oscillates: 1.0 → 1.1 → 1.0 (infinite)
 */
animation_t *ui_button_pulse(animator_t *animator, widget_anim_state_t *state);

// ============================================================================
// MENU ANIMATIONS
// ============================================================================

/**
 * Menu open animation
 * Fade + slide from top (150ms)
 */
void ui_menu_open(animator_t *animator, widget_anim_state_t *state);

/**
 * Menu close animation
 * Fade out + slide up (120ms)
 */
void ui_menu_close(animator_t *animator, widget_anim_state_t *state);

/**
 * Menu item cascade animation
 * Each item fades in with staggered delay (50ms delay per item)
 */
void ui_menu_cascade(animator_t *animator, widget_anim_state_t *items, uint32_t count);

/**
 * Context menu popup
 * Scale from small + fade in (100ms)
 */
void ui_context_menu_popup(animator_t *animator, widget_anim_state_t *state, int32_t x, int32_t y);

// ============================================================================
// TOOLTIP ANIMATIONS
// ============================================================================

/**
 * Tooltip show animation
 * Fade in with 500ms delay (100ms fade)
 */
animation_t *ui_tooltip_show(animator_t *animator, widget_anim_state_t *state);

/**
 * Tooltip hide animation
 * Fade out immediately (80ms)
 */
void ui_tooltip_hide(animator_t *animator, widget_anim_state_t *state);

// ============================================================================
// LIST ANIMATIONS
// ============================================================================

/**
 * List item insertion
 * Slide in + fade (200ms)
 */
void ui_list_item_insert(animator_t *animator, widget_anim_state_t *state, int32_t position);

/**
 * List item removal
 * Fade out + collapse height (200ms)
 */
void ui_list_item_remove(animator_t *animator, widget_anim_state_t *state);

/**
 * List item reorder animation
 * Smooth position transition (250ms)
 */
void ui_list_item_reorder(animator_t *animator, widget_anim_state_t *state, int32_t from_y, int32_t to_y);

/**
 * List scroll momentum
 * Kinetic scrolling with deceleration
 */
typedef struct {
    float velocity;
    float position;
    float friction;
} scroll_state_t;

animation_t *ui_list_scroll_momentum(animator_t *animator, scroll_state_t *scroll);

// ============================================================================
// PROGRESS BAR ANIMATIONS
// ============================================================================

/**
 * Progress bar value transition
 * Smooth value change (300ms)
 */
void ui_progress_animate(animator_t *animator, float *current_value, float target_value);

/**
 * Indeterminate progress animation
 * Animated shimmer effect (infinite)
 */
animation_t *ui_progress_indeterminate(animator_t *animator, float *offset);

/**
 * Progress complete animation
 * Flash green + scale (200ms)
 */
void ui_progress_complete(animator_t *animator, widget_anim_state_t *state);

// ============================================================================
// DIALOG/WINDOW ANIMATIONS
// ============================================================================

/**
 * Dialog appear animation
 * Fade + scale from 0.95 (200ms)
 * Background dim fades in
 */
void ui_dialog_show(animator_t *animator, widget_anim_state_t *dialog, widget_anim_state_t *backdrop);

/**
 * Dialog disappear animation
 * Fade + scale to 0.95 (150ms)
 */
void ui_dialog_hide(animator_t *animator, widget_anim_state_t *dialog, widget_anim_state_t *backdrop);

/**
 * Dialog shake animation (error feedback)
 * Shake horizontally (300ms)
 */
void ui_dialog_shake(animator_t *animator, widget_anim_state_t *state);

// ============================================================================
// NOTIFICATION/TOAST ANIMATIONS
// ============================================================================

/**
 * Toast slide in from side
 * Slide + fade in (200ms)
 */
void ui_toast_slide_in(animator_t *animator, widget_anim_state_t *state, bool from_right);

/**
 * Toast slide out
 * Slide + fade out (150ms)
 */
void ui_toast_slide_out(animator_t *animator, widget_anim_state_t *state, bool to_right);

/**
 * Toast auto-dismiss timer
 * Stays for duration, then slides out
 */
animation_t *ui_toast_auto_dismiss(animator_t *animator, widget_anim_state_t *state, uint64_t duration_ms);

// ============================================================================
// TAB ANIMATIONS
// ============================================================================

/**
 * Tab switch animation
 * Slide content left/right (200ms)
 */
void ui_tab_switch(animator_t *animator, widget_anim_state_t *from_tab, widget_anim_state_t *to_tab, bool left);

/**
 * Tab indicator animation
 * Smooth position + width transition (250ms)
 */
void ui_tab_indicator_move(animator_t *animator, rect_f_t *indicator, rect_f_t target);

// ============================================================================
// SWITCH/CHECKBOX ANIMATIONS
// ============================================================================

/**
 * Toggle switch animation
 * Slide knob + background color (200ms)
 */
void ui_switch_toggle(animator_t *animator, widget_anim_state_t *knob, color_t *bg_color, bool checked);

/**
 * Checkbox check animation
 * Checkmark draw animation (150ms)
 */
void ui_checkbox_check(animator_t *animator, float *progress);

/**
 * Checkbox uncheck animation
 */
void ui_checkbox_uncheck(animator_t *animator, float *progress);

// ============================================================================
// SCROLL ANIMATIONS
// ============================================================================

/**
 * Smooth scroll to position
 * Eased scroll (400ms)
 */
void ui_scroll_to(animator_t *animator, float *scroll_pos, float target_pos);

/**
 * Rubber band bounce effect
 * When scrolling past boundaries
 */
void ui_scroll_rubber_band(animator_t *animator, float *scroll_pos, float boundary);

// ============================================================================
// COLLAPSE/EXPAND ANIMATIONS
// ============================================================================

/**
 * Expand animation
 * Height: 0 → full (250ms)
 */
void ui_expand(animator_t *animator, float *height, float target_height);

/**
 * Collapse animation
 * Height: full → 0 (250ms)
 */
void ui_collapse(animator_t *animator, float *height);

/**
 * Accordion animation
 * One expands, others collapse
 */
void ui_accordion(animator_t *animator, widget_anim_state_t *items, uint32_t count, uint32_t active_index);

// ============================================================================
// LOADING ANIMATIONS
// ============================================================================

/**
 * Spinner rotation
 * Infinite rotation (1s per revolution)
 */
animation_t *ui_loading_spinner(animator_t *animator, float *rotation);

/**
 * Pulsing dots
 * Three dots pulse in sequence
 */
void ui_loading_dots(animator_t *animator, float *dot_scales, uint32_t count);

/**
 * Loading bar sweep
 * Bar sweeps across (infinite)
 */
animation_t *ui_loading_bar_sweep(animator_t *animator, float *position);

// ============================================================================
// DRAG & DROP ANIMATIONS
// ============================================================================

/**
 * Drag start animation
 * Lift effect: scale up + shadow (150ms)
 */
void ui_drag_start(animator_t *animator, widget_anim_state_t *state);

/**
 * Drag hover over drop zone
 * Drop zone highlight pulse
 */
animation_t *ui_drop_zone_highlight(animator_t *animator, widget_anim_state_t *state);

/**
 * Drop animation
 * Scale back + snap to position (200ms)
 */
void ui_drop(animator_t *animator, widget_anim_state_t *state, int32_t target_x, int32_t target_y);

/**
 * Drag cancel animation (snap back)
 * Return to original position (250ms)
 */
void ui_drag_cancel(animator_t *animator, widget_anim_state_t *state, int32_t origin_x, int32_t origin_y);

// ============================================================================
// RIPPLE EFFECT (Material Design)
// ============================================================================

typedef struct {
    float x, y;             // Ripple center
    float radius;           // Current radius
    float opacity;          // Current opacity
    bool active;
} ripple_t;

/**
 * Start ripple effect from click position
 */
void ui_ripple_start(animator_t *animator, ripple_t *ripple, int32_t x, int32_t y);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

/**
 * Create fade animation
 */
animation_t *ui_fade(animator_t *animator, float *opacity, float from, float to, uint64_t duration);

/**
 * Create slide animation
 */
animation_t *ui_slide(animator_t *animator, float *position, float from, float to, uint64_t duration);

/**
 * Create scale animation
 */
animation_t *ui_scale(animator_t *animator, float *scale, float from, float to, uint64_t duration);

/**
 * Chain two animations (second starts when first finishes)
 */
void ui_chain_animations(animation_t *first, animation_t *second);

/**
 * Parallel animations (start multiple simultaneously)
 */
animation_group_t *ui_parallel_animations(animation_t **animations, uint32_t count);

#endif // UI_ANIMATIONS_H
