/**
 * AutomationOS UI Widget Animations Implementation
 */

#include "ui_animations.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// ============================================================================
// BUTTON ANIMATIONS
// ============================================================================

void ui_button_hover(animator_t *animator, widget_anim_state_t *state, color_t hover_color) {
    if (!animator || !state) return;

    // Scale animation: 1.0 → 1.05 (100ms)
    animation_t *scale_anim = anim_from_to(animator, state->scale, 1.05f, DURATION_FAST, EASE_OUT_QUAD);
    anim_set_target(scale_anim, &state->scale);
    anim_start(scale_anim);

    // Color transition
    animation_t *color_anim = anim_color(animator, state->bg_color, hover_color, DURATION_FAST, EASE_LINEAR);
    anim_set_target_color(color_anim, &state->bg_color);
    anim_start(color_anim);
}

void ui_button_unhover(animator_t *animator, widget_anim_state_t *state, color_t normal_color) {
    if (!animator || !state) return;

    animation_t *scale_anim = anim_from_to(animator, state->scale, 1.0f, DURATION_FAST, EASE_OUT_QUAD);
    anim_set_target(scale_anim, &state->scale);
    anim_start(scale_anim);

    animation_t *color_anim = anim_color(animator, state->bg_color, normal_color, DURATION_FAST, EASE_LINEAR);
    anim_set_target_color(color_anim, &state->bg_color);
    anim_start(color_anim);
}

void ui_button_click(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    // Keyframe animation: 1.05 → 0.95 → 1.0
    animation_t *anim = anim_keyframe(animator);
    anim_add_keyframe(anim, 0.0f, 1.05f, EASE_LINEAR);
    anim_add_keyframe(anim, 0.4f, 0.95f, EASE_OUT_QUAD);
    anim_add_keyframe(anim, 1.0f, 1.0f, EASE_OUT_QUAD);
    anim->duration = 150000;  // 150ms
    anim_set_target(anim, &state->scale);
    anim_start(anim);
}

void ui_button_press(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    animation_t *anim = anim_from_to(animator, state->scale, 0.95f, DURATION_FAST, EASE_OUT_QUAD);
    anim_set_target(anim, &state->scale);
    anim_start(anim);
}

void ui_button_release(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    animation_t *anim = anim_from_to(animator, state->scale, 1.0f, DURATION_FAST, EASE_OUT_BACK);
    anim_set_target(anim, &state->scale);
    anim_start(anim);
}

animation_t *ui_button_pulse(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return NULL;

    animation_t *anim = anim_from_to(animator, 1.0f, 1.1f, 600000, EASE_IN_OUT_SINE);
    anim_set_target(anim, &state->scale);
    anim_set_repeat(anim, -1);  // Infinite
    anim_set_auto_reverse(anim, true);
    anim_start(anim);

    return anim;
}

// ============================================================================
// MENU ANIMATIONS
// ============================================================================

void ui_menu_open(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    // Fade in
    animation_t *fade = anim_from_to(animator, 0.0f, 1.0f, 150000, EASE_OUT_CUBIC);
    anim_set_target(fade, &state->opacity);
    anim_start(fade);

    // Slide from top
    animation_t *slide = anim_from_to(animator, -20.0f, 0.0f, 150000, EASE_OUT_CUBIC);
    anim_set_target(slide, &state->offset_y);
    anim_start(slide);

    state->visible = true;
}

void ui_menu_close(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    animation_t *fade = anim_from_to(animator, state->opacity, 0.0f, 120000, EASE_IN_QUAD);
    anim_set_target(fade, &state->opacity);
    anim_start(fade);

    animation_t *slide = anim_from_to(animator, state->offset_y, -10.0f, 120000, EASE_IN_QUAD);
    anim_set_target(slide, &state->offset_y);
    anim_start(slide);
}

void ui_menu_cascade(animator_t *animator, widget_anim_state_t *items, uint32_t count) {
    if (!animator || !items) return;

    for (uint32_t i = 0; i < count; i++) {
        animation_t *fade = anim_from_to(animator, 0.0f, 1.0f, 150000, EASE_OUT_CUBIC);
        anim_set_delay(fade, i * 50000);  // 50ms stagger
        anim_set_target(fade, &items[i].opacity);
        anim_start(fade);

        // Slight slide
        animation_t *slide = anim_from_to(animator, -10.0f, 0.0f, 150000, EASE_OUT_CUBIC);
        anim_set_delay(slide, i * 50000);
        anim_set_target(slide, &items[i].offset_y);
        anim_start(slide);
    }
}

void ui_context_menu_popup(animator_t *animator, widget_anim_state_t *state, int32_t x, int32_t y) {
    if (!animator || !state) return;

    // Scale from 0.8
    animation_t *scale = anim_from_to(animator, 0.8f, 1.0f, DURATION_FAST, EASE_OUT_BACK);
    anim_set_target(scale, &state->scale);
    anim_start(scale);

    // Fade in
    animation_t *fade = anim_from_to(animator, 0.0f, 1.0f, DURATION_FAST, EASE_OUT_QUAD);
    anim_set_target(fade, &state->opacity);
    anim_start(fade);

    state->visible = true;
}

// ============================================================================
// TOOLTIP ANIMATIONS
// ============================================================================

animation_t *ui_tooltip_show(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return NULL;

    animation_t *anim = anim_from_to(animator, 0.0f, 1.0f, DURATION_FAST, EASE_OUT_QUAD);
    anim_set_delay(anim, 500000);  // 500ms delay
    anim_set_target(anim, &state->opacity);
    anim_start(anim);

    return anim;
}

void ui_tooltip_hide(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    animation_t *anim = anim_from_to(animator, state->opacity, 0.0f, 80000, EASE_IN_QUAD);
    anim_set_target(anim, &state->opacity);
    anim_start(anim);
}

// ============================================================================
// LIST ANIMATIONS
// ============================================================================

void ui_list_item_insert(animator_t *animator, widget_anim_state_t *state, int32_t position) {
    if (!animator || !state) return;

    // Slide in
    animation_t *slide = anim_from_to(animator, 50.0f, 0.0f, DURATION_NORMAL, EASE_OUT_CUBIC);
    anim_set_target(slide, &state->offset_x);
    anim_start(slide);

    // Fade in
    animation_t *fade = anim_from_to(animator, 0.0f, 1.0f, DURATION_NORMAL, EASE_OUT_QUAD);
    anim_set_target(fade, &state->opacity);
    anim_start(fade);
}

void ui_list_item_remove(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    animation_t *fade = anim_from_to(animator, state->opacity, 0.0f, DURATION_NORMAL, EASE_IN_QUAD);
    anim_set_target(fade, &state->opacity);
    anim_start(fade);

    // Slide out slightly
    animation_t *slide = anim_from_to(animator, 0.0f, -30.0f, DURATION_NORMAL, EASE_IN_QUAD);
    anim_set_target(slide, &state->offset_x);
    anim_start(slide);
}

void ui_list_item_reorder(animator_t *animator, widget_anim_state_t *state, int32_t from_y, int32_t to_y) {
    if (!animator || !state) return;

    animation_t *anim = anim_from_to(animator, (float)from_y, (float)to_y, 250000, EASE_IN_OUT_CUBIC);
    anim_set_target(anim, &state->offset_y);
    anim_start(anim);
}

animation_t *ui_list_scroll_momentum(animator_t *animator, scroll_state_t *scroll) {
    if (!animator || !scroll) return NULL;

    // Use spring physics for natural momentum
    spring_params_t spring = SPRING_SMOOTH;
    spring.velocity = scroll->velocity;
    spring.damping = scroll->friction;

    animation_t *anim = anim_spring(animator, scroll->position, scroll->position + scroll->velocity, &spring);
    anim_set_target(anim, &scroll->position);
    anim_start(anim);

    return anim;
}

// ============================================================================
// PROGRESS BAR ANIMATIONS
// ============================================================================

void ui_progress_animate(animator_t *animator, float *current_value, float target_value) {
    if (!animator || !current_value) return;

    animation_t *anim = anim_from_to(animator, *current_value, target_value, DURATION_SLOW, EASE_IN_OUT_CUBIC);
    anim_set_target(anim, current_value);
    anim_start(anim);
}

animation_t *ui_progress_indeterminate(animator_t *animator, float *offset) {
    if (!animator || !offset) return NULL;

    animation_t *anim = anim_from_to(animator, 0.0f, 1.0f, 1500000, EASE_IN_OUT_CUBIC);
    anim_set_repeat(anim, -1);  // Infinite
    anim_set_target(anim, offset);
    anim_start(anim);

    return anim;
}

void ui_progress_complete(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    // Flash effect with color
    color_t green = { .r = 0.2f, .g = 0.8f, .b = 0.2f, .a = 1.0f };
    animation_t *color_anim = anim_color(animator, state->bg_color, green, DURATION_FAST, EASE_OUT_QUAD);
    anim_set_target_color(color_anim, &state->bg_color);
    anim_start(color_anim);

    // Slight scale up
    animation_t *scale = anim_from_to(animator, 1.0f, 1.05f, DURATION_NORMAL, EASE_OUT_BACK);
    anim_set_target(scale, &state->scale);
    anim_set_auto_reverse(scale, true);
    anim_start(scale);
}

// ============================================================================
// DIALOG/WINDOW ANIMATIONS
// ============================================================================

void ui_dialog_show(animator_t *animator, widget_anim_state_t *dialog, widget_anim_state_t *backdrop) {
    if (!animator || !dialog) return;

    // Dialog fade + scale
    animation_t *fade = anim_from_to(animator, 0.0f, 1.0f, DURATION_NORMAL, EASE_OUT_CUBIC);
    anim_set_target(fade, &dialog->opacity);
    anim_start(fade);

    animation_t *scale = anim_from_to(animator, 0.95f, 1.0f, DURATION_NORMAL, EASE_OUT_CUBIC);
    anim_set_target(scale, &dialog->scale);
    anim_start(scale);

    // Backdrop dim
    if (backdrop) {
        animation_t *backdrop_fade = anim_from_to(animator, 0.0f, 0.5f, DURATION_NORMAL, EASE_OUT_QUAD);
        anim_set_target(backdrop_fade, &backdrop->opacity);
        anim_start(backdrop_fade);
    }

    dialog->visible = true;
}

void ui_dialog_hide(animator_t *animator, widget_anim_state_t *dialog, widget_anim_state_t *backdrop) {
    if (!animator || !dialog) return;

    animation_t *fade = anim_from_to(animator, dialog->opacity, 0.0f, 150000, EASE_IN_QUAD);
    anim_set_target(fade, &dialog->opacity);
    anim_start(fade);

    animation_t *scale = anim_from_to(animator, dialog->scale, 0.95f, 150000, EASE_IN_QUAD);
    anim_set_target(scale, &dialog->scale);
    anim_start(scale);

    if (backdrop) {
        animation_t *backdrop_fade = anim_from_to(animator, backdrop->opacity, 0.0f, 150000, EASE_IN_QUAD);
        anim_set_target(backdrop_fade, &backdrop->opacity);
        anim_start(backdrop_fade);
    }
}

void ui_dialog_shake(animator_t *animator, widget_anim_state_t *state) {
    if (!animator || !state) return;

    // Keyframe shake animation
    animation_t *shake = anim_keyframe(animator);
    anim_add_keyframe(shake, 0.0f, 0.0f, EASE_LINEAR);
    anim_add_keyframe(shake, 0.1f, -10.0f, EASE_LINEAR);
    anim_add_keyframe(shake, 0.3f, 10.0f, EASE_LINEAR);
    anim_add_keyframe(shake, 0.5f, -8.0f, EASE_LINEAR);
    anim_add_keyframe(shake, 0.7f, 8.0f, EASE_LINEAR);
    anim_add_keyframe(shake, 0.9f, -4.0f, EASE_LINEAR);
    anim_add_keyframe(shake, 1.0f, 0.0f, EASE_LINEAR);
    shake->duration = 300000;  // 300ms
    anim_set_target(shake, &state->offset_x);
    anim_start(shake);
}

// ============================================================================
// NOTIFICATION/TOAST ANIMATIONS
// ============================================================================

void ui_toast_slide_in(animator_t *animator, widget_anim_state_t *state, bool from_right) {
    if (!animator || !state) return;

    float start_x = from_right ? 300.0f : -300.0f;

    animation_t *slide = anim_from_to(animator, start_x, 0.0f, DURATION_NORMAL, EASE_OUT_CUBIC);
    anim_set_target(slide, &state->offset_x);
    anim_start(slide);

    animation_t *fade = anim_from_to(animator, 0.0f, 1.0f, DURATION_NORMAL, EASE_OUT_QUAD);
    anim_set_target(fade, &state->opacity);
    anim_start(fade);
}

void ui_toast_slide_out(animator_t *animator, widget_anim_state_t *state, bool to_right) {
    if (!animator || !state) return;

    float end_x = to_right ? 300.0f : -300.0f;

    animation_t *slide = anim_from_to(animator, state->offset_x, end_x, 150000, EASE_IN_CUBIC);
    anim_set_target(slide, &state->offset_x);
    anim_start(slide);

    animation_t *fade = anim_from_to(animator, state->opacity, 0.0f, 150000, EASE_IN_QUAD);
    anim_set_target(fade, &state->opacity);
    anim_start(fade);
}

animation_t *ui_toast_auto_dismiss(animator_t *animator, widget_anim_state_t *state, uint64_t duration_ms) {
    if (!animator || !state) return NULL;

    // First show the toast
    ui_toast_slide_in(animator, state, true);

    // Then create delayed hide animation
    animation_t *dummy = anim_from_to(animator, 0.0f, 1.0f, 1000, EASE_LINEAR);
    anim_set_delay(dummy, duration_ms * 1000);
    // In real implementation, callback would trigger ui_toast_slide_out
    anim_start(dummy);

    return dummy;
}

// ============================================================================
// TAB ANIMATIONS
// ============================================================================

void ui_tab_switch(animator_t *animator, widget_anim_state_t *from_tab, widget_anim_state_t *to_tab, bool left) {
    if (!animator || !from_tab || !to_tab) return;

    float direction = left ? -1.0f : 1.0f;

    // Slide out old tab
    animation_t *out_slide = anim_from_to(animator, 0.0f, -100.0f * direction, DURATION_NORMAL, EASE_IN_OUT_CUBIC);
    anim_set_target(out_slide, &from_tab->offset_x);
    anim_start(out_slide);

    animation_t *out_fade = anim_from_to(animator, 1.0f, 0.0f, DURATION_NORMAL, EASE_IN_QUAD);
    anim_set_target(out_fade, &from_tab->opacity);
    anim_start(out_fade);

    // Slide in new tab
    animation_t *in_slide = anim_from_to(animator, 100.0f * direction, 0.0f, DURATION_NORMAL, EASE_IN_OUT_CUBIC);
    anim_set_target(in_slide, &to_tab->offset_x);
    anim_start(in_slide);

    animation_t *in_fade = anim_from_to(animator, 0.0f, 1.0f, DURATION_NORMAL, EASE_OUT_QUAD);
    anim_set_target(in_fade, &to_tab->opacity);
    anim_start(in_fade);
}

void ui_tab_indicator_move(animator_t *animator, rect_f_t *indicator, rect_f_t target) {
    if (!animator || !indicator) return;

    animation_t *anim = anim_rect(animator, *indicator, target, 250000, EASE_IN_OUT_CUBIC);
    anim_set_target_rect(anim, indicator);
    anim_start(anim);
}

// ============================================================================
// SWITCH/CHECKBOX ANIMATIONS
// ============================================================================

void ui_switch_toggle(animator_t *animator, widget_anim_state_t *knob, color_t *bg_color, bool checked) {
    if (!animator || !knob) return;

    // Slide knob
    float target_x = checked ? 20.0f : 0.0f;
    animation_t *slide = anim_from_to(animator, knob->offset_x, target_x, DURATION_NORMAL, EASE_OUT_CUBIC);
    anim_set_target(slide, &knob->offset_x);
    anim_start(slide);

    // Color transition
    if (bg_color) {
        color_t on_color = { .r = 0.2f, .g = 0.6f, .b = 1.0f, .a = 1.0f };
        color_t off_color = { .r = 0.5f, .g = 0.5f, .b = 0.5f, .a = 1.0f };
        color_t target_color = checked ? on_color : off_color;

        animation_t *color_anim = anim_color(animator, *bg_color, target_color, DURATION_NORMAL, EASE_LINEAR);
        anim_set_target_color(color_anim, bg_color);
        anim_start(color_anim);
    }
}

void ui_checkbox_check(animator_t *animator, float *progress) {
    if (!animator || !progress) return;

    animation_t *anim = anim_from_to(animator, 0.0f, 1.0f, 150000, EASE_OUT_CUBIC);
    anim_set_target(anim, progress);
    anim_start(anim);
}

void ui_checkbox_uncheck(animator_t *animator, float *progress) {
    if (!animator || !progress) return;

    animation_t *anim = anim_from_to(animator, *progress, 0.0f, 150000, EASE_IN_CUBIC);
    anim_set_target(anim, progress);
    anim_start(anim);
}

// ============================================================================
// Continued in next response due to length...
// ============================================================================
