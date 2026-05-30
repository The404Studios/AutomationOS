/**
 * Panel (Top Bar) Implementation
 *
 * The top bar containing Activities button, app title, system tray,
 * clock, and user menu.
 */

#include "desktop_shell.h"
#include "../../lib/compositor_client/compositor_client.h"
#include "../../lib/ui/draw.h"
#include "../../lib/font/font.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// Panel height in pixels
#define PANEL_HEIGHT 32
#define PANEL_PADDING 8
#define ICON_SIZE 16
#define BUTTON_WIDTH 80

// ============================================================================
// BUTTON HELPERS
// ============================================================================

static button_t *button_create(const char *label, int32_t x, int32_t y) {
    button_t *btn = calloc(1, sizeof(button_t));
    if (!btn) return NULL;

    strncpy(btn->label, label, sizeof(btn->label) - 1);
    btn->bounds.x = x;
    btn->bounds.y = y;
    btn->bounds.width = BUTTON_WIDTH;
    btn->bounds.height = PANEL_HEIGHT - 8;

    return btn;
}

static void button_destroy(button_t *btn) {
    if (btn) free(btn);
}

static void button_update(button_t *btn, int32_t mouse_x, int32_t mouse_y) {
    if (!btn) return;
    btn->hovered = rect_contains(&btn->bounds, mouse_x, mouse_y);
}

static void button_render(button_t *btn, theme_t *theme, uint32_t *fb, uint32_t fb_width, uint32_t fb_height) {
    if (!btn || !fb) return;

    // Draw button background
    uint32_t bg_color;
    if (btn->pressed) {
        bg_color = ARGB(200, theme->primary.r, theme->primary.g, theme->primary.b);
    } else if (btn->hovered) {
        bg_color = ARGB(150, theme->bg_secondary.r, theme->bg_secondary.g, theme->bg_secondary.b);
    } else {
        bg_color = ARGB(100, theme->bg_secondary.r, theme->bg_secondary.g, theme->bg_secondary.b);
    }

    draw_rounded_rect(fb, fb_width, fb_height,
                     btn->bounds.x, btn->bounds.y,
                     btn->bounds.width, btn->bounds.height,
                     4, bg_color);

    // TODO: Draw button text using font library
    (void)theme;
}

// ============================================================================
// LABEL HELPERS
// ============================================================================

static label_t *label_create(const char *text, int32_t x, int32_t y) {
    label_t *label = calloc(1, sizeof(label_t));
    if (!label) return NULL;

    strncpy(label->text, text, sizeof(label->text) - 1);
    label->bounds.x = x;
    label->bounds.y = y;
    label->bounds.height = PANEL_HEIGHT;
    label->font_size = 13;

    return label;
}

static void label_destroy(label_t *label) {
    if (label) free(label);
}

static void label_set_text(label_t *label, const char *text) {
    if (!label || !text) return;
    strncpy(label->text, text, sizeof(label->text) - 1);
}

static void label_render(label_t *label, theme_t *theme, uint32_t *fb, uint32_t fb_width, uint32_t fb_height, font_t *font) {
    if (!label || !fb) return;

    // TODO: Render text using font library when font is loaded
    (void)theme;
    (void)fb_width;
    (void)fb_height;
    (void)font;
}

// ============================================================================
// SYSTEM TRAY
// ============================================================================

static tray_t *tray_create(void) {
    tray_t *tray = calloc(1, sizeof(tray_t));
    if (!tray) return NULL;

    // Add some default tray icons
    // WiFi icon
    strncpy(tray->items[tray->count].tooltip, "WiFi: Connected",
            sizeof(tray->items[tray->count].tooltip) - 1);
    tray->items[tray->count].bounds.width = ICON_SIZE;
    tray->items[tray->count].bounds.height = ICON_SIZE;
    tray->count++;

    // Volume icon
    strncpy(tray->items[tray->count].tooltip, "Volume: 80%",
            sizeof(tray->items[tray->count].tooltip) - 1);
    tray->items[tray->count].bounds.width = ICON_SIZE;
    tray->items[tray->count].bounds.height = ICON_SIZE;
    tray->count++;

    // Battery icon
    strncpy(tray->items[tray->count].tooltip, "Battery: 85%",
            sizeof(tray->items[tray->count].tooltip) - 1);
    tray->items[tray->count].bounds.width = ICON_SIZE;
    tray->items[tray->count].bounds.height = ICON_SIZE;
    tray->count++;

    return tray;
}

static void tray_destroy(tray_t *tray) {
    if (tray) free(tray);
}

static void tray_update_layout(tray_t *tray, int32_t right_x, int32_t y) {
    if (!tray) return;

    int32_t x = right_x;
    for (uint32_t i = 0; i < tray->count; i++) {
        x -= ICON_SIZE + PANEL_PADDING;
        tray->items[i].bounds.x = x;
        tray->items[i].bounds.y = y + (PANEL_HEIGHT - ICON_SIZE) / 2;
    }
}

static void tray_render(tray_t *tray, theme_t *theme, uint32_t *fb, uint32_t fb_width, uint32_t fb_height) {
    if (!tray || !fb) return;

    for (uint32_t i = 0; i < tray->count; i++) {
        // Draw placeholder icon (colored circle)
        uint32_t icon_color = ARGB(200, theme->text_secondary.r, theme->text_secondary.g, theme->text_secondary.b);
        draw_circle(fb, fb_width, fb_height,
                   tray->items[i].bounds.x + ICON_SIZE / 2,
                   tray->items[i].bounds.y + ICON_SIZE / 2,
                   ICON_SIZE / 2, icon_color);
    }
}

// ============================================================================
// CLOCK
// ============================================================================

static clock_t *clock_create(void) {
    clock_t *clk = calloc(1, sizeof(clock_t));
    if (!clk) return NULL;

    // Initialize with current time
    time_t now = time(NULL);
    struct tm *local = localtime(&now);

    strftime(clk->time_str, sizeof(clk->time_str), "%I:%M %p", local);
    strftime(clk->date_str, sizeof(clk->date_str), "%A, %B %d", local);

    return clk;
}

static void clock_destroy(clock_t *clk) {
    if (clk) free(clk);
}

static void clock_update(clock_t *clk) {
    if (!clk) return;

    time_t now = time(NULL);
    struct tm *local = localtime(&now);

    strftime(clk->time_str, sizeof(clk->time_str), "%I:%M %p", local);
    strftime(clk->date_str, sizeof(clk->date_str), "%A, %B %d", local);
}

static void clock_render(clock_t *clk, int32_t x, int32_t y, theme_t *theme, uint32_t *fb, uint32_t fb_width, uint32_t fb_height, font_t *font) {
    if (!clk || !fb) return;

    // TODO: Render clock text using font library when font is loaded
    (void)x;
    (void)y;
    (void)theme;
    (void)fb_width;
    (void)fb_height;
    (void)font;
}

// ============================================================================
// PANEL API
// ============================================================================

panel_t *panel_create(desktop_shell_t *shell) {
    if (!shell) return NULL;

    printf("[Panel] Creating panel\n");

    panel_t *panel = calloc(1, sizeof(panel_t));
    if (!panel) {
        fprintf(stderr, "[Panel] ERROR: Failed to allocate panel\n");
        return NULL;
    }

    panel->height = PANEL_HEIGHT;
    panel->theme = &shell->theme;
    panel->autohide = false;

    // TODO: Create panel window
    // panel->window = window_create(...);

    // Create Activities button (left side)
    panel->activities = button_create("Activities", PANEL_PADDING, 4);
    if (!panel->activities) {
        fprintf(stderr, "[Panel] ERROR: Failed to create activities button\n");
        panel_destroy(panel);
        return NULL;
    }

    // Create app title label
    panel->app_title = label_create("AutomationOS",
                                    PANEL_PADDING + BUTTON_WIDTH + PANEL_PADDING,
                                    0);
    if (!panel->app_title) {
        fprintf(stderr, "[Panel] ERROR: Failed to create app title label\n");
        panel_destroy(panel);
        return NULL;
    }

    // Create system tray
    panel->system_tray = tray_create();
    if (!panel->system_tray) {
        fprintf(stderr, "[Panel] ERROR: Failed to create system tray\n");
        panel_destroy(panel);
        return NULL;
    }

    // Create clock
    panel->clock = clock_create();
    if (!panel->clock) {
        fprintf(stderr, "[Panel] ERROR: Failed to create clock\n");
        panel_destroy(panel);
        return NULL;
    }

    // Create user menu button (right side)
    int32_t user_btn_x = (int32_t)shell->screen_width - BUTTON_WIDTH - PANEL_PADDING;
    panel->user_menu = button_create("User", user_btn_x, 4);
    if (!panel->user_menu) {
        fprintf(stderr, "[Panel] ERROR: Failed to create user menu button\n");
        panel_destroy(panel);
        return NULL;
    }

    // Update tray layout
    int32_t tray_right_x = user_btn_x - PANEL_PADDING;
    tray_update_layout(panel->system_tray, tray_right_x, 0);

    printf("[Panel] Panel created successfully\n");
    return panel;
}

void panel_destroy(panel_t *panel) {
    if (!panel) return;

    printf("[Panel] Destroying panel\n");

    if (panel->user_menu) button_destroy(panel->user_menu);
    if (panel->clock) clock_destroy(panel->clock);
    if (panel->system_tray) tray_destroy(panel->system_tray);
    if (panel->app_title) label_destroy(panel->app_title);
    if (panel->activities) button_destroy(panel->activities);

    // TODO: Destroy panel window
    // if (panel->window) window_destroy(panel->window);

    free(panel);
}

void panel_update(panel_t *panel, uint64_t delta_us) {
    if (!panel) return;

    // Update clock every frame (it only changes text when needed)
    clock_update(panel->clock);

    // Update button hover states
    // TODO: Get actual mouse position
    int32_t mouse_x = 0, mouse_y = 0;
    button_update(panel->activities, mouse_x, mouse_y);
    button_update(panel->user_menu, mouse_x, mouse_y);

    (void)delta_us;
}

void panel_render(panel_t *panel) {
    if (!panel || !panel->theme) return;

    // Render panel background
    // TODO: Draw semi-transparent blurred rectangle
    // rect: (0, 0, screen_width, PANEL_HEIGHT)
    // color: panel->theme->panel_bg

    // Render Activities button
    button_render(panel->activities, panel->theme);

    // Render app title
    label_render(panel->app_title, panel->theme);

    // Render system tray icons
    tray_render(panel->system_tray, panel->theme);

    // Render clock
    if (panel->clock) {
        // Clock position: to the left of user menu button
        int32_t clock_x = panel->user_menu->bounds.x - 100 - PANEL_PADDING;
        clock_render(panel->clock, clock_x, 0, panel->theme);
    }

    // Render user menu button
    button_render(panel->user_menu, panel->theme);
}
