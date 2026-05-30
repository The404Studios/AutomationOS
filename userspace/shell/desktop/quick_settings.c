/**
 * Quick Settings Panel Implementation
 *
 * Quick access to WiFi, Bluetooth, volume, brightness, and system settings.
 */

#include "desktop_shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define QS_PANEL_WIDTH 300
#define QS_PANEL_HEIGHT 400
#define QS_PADDING 16
#define QS_ITEM_HEIGHT 48
#define QS_SLIDER_HEIGHT 32

// ============================================================================
// QUICK SETTINGS HELPERS
// ============================================================================

static void quick_toggle_render(quick_toggle_t *toggle, int32_t x, int32_t y, theme_t *theme) {
    if (!toggle) return;

    rect_t bg_rect = {
        .x = x,
        .y = y,
        .width = QS_PANEL_WIDTH - QS_PADDING * 2,
        .height = QS_ITEM_HEIGHT
    };

    // Background color
    color_t bg_color = toggle->enabled ? theme->primary : theme->bg_tertiary;

    // TODO: Draw rounded rect
    // color: bg_color
    // corner_radius: theme->corner_radius

    // Render label
    rect_t label_rect = {
        .x = x + QS_PADDING,
        .y = y + (QS_ITEM_HEIGHT - 20) / 2,
        .width = QS_PANEL_WIDTH - QS_PADDING * 4,
        .height = 20
    };
    // TODO: Draw text (toggle->label, theme->text_primary)

    // Render toggle switch (right side)
    int32_t switch_x = x + (int32_t)(QS_PANEL_WIDTH - QS_PADDING * 2 - 50);
    int32_t switch_y = y + (QS_ITEM_HEIGHT - 24) / 2;
    // TODO: Draw toggle switch (iOS style)
    // width: 50, height: 24
    // background: toggle->enabled ? theme->primary : theme->bg_tertiary
    // circle position: left or right based on state

    (void)theme;
    (void)bg_rect;
    (void)bg_color;
    (void)label_rect;
    (void)switch_x;
    (void)switch_y;
}

static void quick_slider_render(quick_slider_t *slider, int32_t x, int32_t y, theme_t *theme) {
    if (!slider) return;

    // Render label
    rect_t label_rect = {
        .x = x,
        .y = y,
        .width = QS_PANEL_WIDTH - QS_PADDING * 2,
        .height = 20
    };
    // TODO: Draw text (slider->label, theme->text_secondary, size: small)

    // Render slider track
    rect_t track_rect = {
        .x = x,
        .y = y + 24,
        .width = QS_PANEL_WIDTH - QS_PADDING * 2,
        .height = 4
    };
    // TODO: Draw rounded rect (theme->bg_tertiary)

    // Render slider fill (progress)
    uint32_t fill_width = (uint32_t)((float)(QS_PANEL_WIDTH - QS_PADDING * 2) * slider->value);
    rect_t fill_rect = {
        .x = x,
        .y = y + 24,
        .width = fill_width,
        .height = 4
    };
    // TODO: Draw rounded rect (theme->primary)

    // Render slider thumb (circle)
    int32_t thumb_x = x + (int32_t)fill_width;
    int32_t thumb_y = y + 26;
    // TODO: Draw circle (radius: 12, color: white, shadow)

    (void)theme;
    (void)label_rect;
    (void)track_rect;
    (void)fill_rect;
    (void)thumb_x;
    (void)thumb_y;
}

// ============================================================================
// QUICK SETTINGS CALLBACKS
// ============================================================================

static void wifi_toggle_callback(bool enabled, void *user_data) {
    printf("[QuickSettings] WiFi %s\n", enabled ? "enabled" : "disabled");
    // TODO: Enable/disable WiFi
    (void)user_data;
}

static void bluetooth_toggle_callback(bool enabled, void *user_data) {
    printf("[QuickSettings] Bluetooth %s\n", enabled ? "enabled" : "disabled");
    // TODO: Enable/disable Bluetooth
    (void)user_data;
}

static void dnd_toggle_callback(bool enabled, void *user_data) {
    printf("[QuickSettings] Do Not Disturb %s\n", enabled ? "enabled" : "disabled");
    desktop_shell_t *shell = (desktop_shell_t *)user_data;
    if (shell && shell->notifications) {
        shell->notifications->do_not_disturb = enabled;
    }
}

static void volume_change_callback(float value, void *user_data) {
    printf("[QuickSettings] Volume: %.0f%%\n", value * 100.0f);
    // TODO: Set system volume
    (void)user_data;
}

static void brightness_change_callback(float value, void *user_data) {
    printf("[QuickSettings] Brightness: %.0f%%\n", value * 100.0f);
    // TODO: Set screen brightness
    (void)user_data;
}

static void settings_button_callback(button_t *btn, void *user_data) {
    printf("[QuickSettings] Opening system settings\n");
    // TODO: Launch settings app
    (void)btn;
    (void)user_data;
}

static void power_button_callback(button_t *btn, void *user_data) {
    printf("[QuickSettings] Opening power menu\n");
    desktop_shell_t *shell = (desktop_shell_t *)user_data;
    if (shell && shell->system_menu) {
        system_menu_open(shell->system_menu, btn->bounds.x, btn->bounds.y + (int32_t)btn->bounds.height);
    }
}

// ============================================================================
// QUICK SETTINGS API
// ============================================================================

quick_settings_t *quick_settings_create(desktop_shell_t *shell) {
    if (!shell) return NULL;

    printf("[QuickSettings] Creating quick settings\n");

    quick_settings_t *qs = calloc(1, sizeof(quick_settings_t));
    if (!qs) {
        fprintf(stderr, "[QuickSettings] ERROR: Failed to allocate quick settings\n");
        return NULL;
    }

    qs->theme = &shell->theme;
    qs->visible = false;

    // Initialize WiFi toggle
    strncpy(qs->wifi.label, "WiFi", sizeof(qs->wifi.label) - 1);
    qs->wifi.enabled = true;
    qs->wifi.on_toggle = wifi_toggle_callback;
    qs->wifi.user_data = shell;

    // Initialize Bluetooth toggle
    strncpy(qs->bluetooth.label, "Bluetooth", sizeof(qs->bluetooth.label) - 1);
    qs->bluetooth.enabled = false;
    qs->bluetooth.on_toggle = bluetooth_toggle_callback;
    qs->bluetooth.user_data = shell;

    // Initialize DND toggle
    strncpy(qs->dnd.label, "Do Not Disturb", sizeof(qs->dnd.label) - 1);
    qs->dnd.enabled = false;
    qs->dnd.on_toggle = dnd_toggle_callback;
    qs->dnd.user_data = shell;

    // Initialize volume slider
    strncpy(qs->volume.label, "Volume", sizeof(qs->volume.label) - 1);
    qs->volume.value = 0.8f;  // 80%
    qs->volume.on_change = volume_change_callback;
    qs->volume.user_data = shell;

    // Initialize brightness slider
    strncpy(qs->brightness.label, "Brightness", sizeof(qs->brightness.label) - 1);
    qs->brightness.value = 0.7f;  // 70%
    qs->brightness.on_change = brightness_change_callback;
    qs->brightness.user_data = shell;

    // Initialize settings button
    strncpy(qs->settings_btn.label, "Settings", sizeof(qs->settings_btn.label) - 1);
    qs->settings_btn.on_click = settings_button_callback;
    qs->settings_btn.user_data = shell;

    // Initialize power button
    strncpy(qs->power_btn.label, "Power", sizeof(qs->power_btn.label) - 1);
    qs->power_btn.on_click = power_button_callback;
    qs->power_btn.user_data = shell;

    // TODO: Create quick settings panel window
    // qs->window = window_create_panel();

    printf("[QuickSettings] Quick settings created\n");
    return qs;
}

void quick_settings_destroy(quick_settings_t *qs) {
    if (!qs) return;

    printf("[QuickSettings] Destroying quick settings\n");

    // TODO: Destroy window
    // if (qs->window) window_destroy(qs->window);

    free(qs);
}

void quick_settings_toggle(quick_settings_t *qs) {
    if (!qs) return;

    qs->visible = !qs->visible;
    printf("[QuickSettings] Quick settings %s\n", qs->visible ? "opened" : "closed");

    // TODO: Animate panel sliding in/out
}

void quick_settings_render(quick_settings_t *qs) {
    if (!qs || !qs->visible || !qs->theme) return;

    // Quick settings panel position (top-right, below panel)
    int32_t panel_x = (int32_t)qs->window->bounds.width - QS_PANEL_WIDTH - 16;
    int32_t panel_y = 48;  // Below panel

    rect_t panel_rect = {
        .x = panel_x,
        .y = panel_y,
        .width = QS_PANEL_WIDTH,
        .height = QS_PANEL_HEIGHT
    };

    // TODO: Draw semi-transparent rounded background
    // color: qs->theme->bg_primary (with blur)
    // corner_radius: qs->theme->corner_radius * 2

    int32_t y = panel_y + QS_PADDING;

    // Render toggles
    quick_toggle_render(&qs->wifi, panel_x + QS_PADDING, y, qs->theme);
    y += QS_ITEM_HEIGHT + 8;

    quick_toggle_render(&qs->bluetooth, panel_x + QS_PADDING, y, qs->theme);
    y += QS_ITEM_HEIGHT + 8;

    quick_toggle_render(&qs->dnd, panel_x + QS_PADDING, y, qs->theme);
    y += QS_ITEM_HEIGHT + 16;

    // Separator
    // TODO: Draw horizontal line
    y += 8;

    // Render sliders
    quick_slider_render(&qs->volume, panel_x + QS_PADDING, y, qs->theme);
    y += QS_SLIDER_HEIGHT + 16;

    quick_slider_render(&qs->brightness, panel_x + QS_PADDING, y, qs->theme);
    y += QS_SLIDER_HEIGHT + 16;

    // Separator
    y += 8;

    // Render action buttons
    uint32_t button_width = (QS_PANEL_WIDTH - QS_PADDING * 3) / 2;
    qs->settings_btn.bounds = (rect_t){
        .x = panel_x + QS_PADDING,
        .y = y,
        .width = button_width,
        .height = 40
    };
    // TODO: Render settings button

    qs->power_btn.bounds = (rect_t){
        .x = panel_x + QS_PADDING + (int32_t)button_width + QS_PADDING,
        .y = y,
        .width = button_width,
        .height = 40
    };
    // TODO: Render power button

    (void)panel_rect;
}
