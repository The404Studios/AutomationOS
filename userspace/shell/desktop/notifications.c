/**
 * Notification Center Implementation
 *
 * System-wide notifications with actions, grouping, and Do Not Disturb mode.
 */

#include "desktop_shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define NOTIF_WIDTH 350
#define NOTIF_HEIGHT 80
#define NOTIF_PADDING 12
#define NOTIF_SPACING 8
#define NOTIF_MAX_DISPLAY 5

static uint32_t g_next_notification_id = 1;

// ============================================================================
// NOTIFICATION HELPERS
// ============================================================================

static notification_t *notification_create(const char *app_name, const char *summary,
                                          const char *body, notif_urgency_t urgency) {
    notification_t *notif = calloc(1, sizeof(notification_t));
    if (!notif) return NULL;

    notif->id = g_next_notification_id++;
    strncpy(notif->app_name, app_name, sizeof(notif->app_name) - 1);
    strncpy(notif->summary, summary, sizeof(notif->summary) - 1);
    strncpy(notif->body, body, sizeof(notif->body) - 1);
    notif->urgency = urgency;
    notif->timestamp = (uint64_t)time(NULL);
    notif->timeout_ms = (urgency == NOTIF_ERROR) ? 0 : 5000;  // Errors persist
    notif->action_count = 0;
    notif->dismissed = false;

    return notif;
}

static void notification_destroy(notification_t *notif) {
    if (!notif) return;

    // TODO: Free icon texture
    // if (notif->icon) texture_destroy(notif->icon);

    free(notif);
}

static void notification_render(notification_t *notif, int32_t x, int32_t y, theme_t *theme) {
    if (!notif) return;

    rect_t bg_rect = {
        .x = x,
        .y = y,
        .width = NOTIF_WIDTH,
        .height = NOTIF_HEIGHT
    };

    // Choose background color based on urgency
    color_t bg_color = theme->bg_secondary;
    if (notif->urgency == NOTIF_ERROR) {
        bg_color = color_rgba(theme->error.r, theme->error.g, theme->error.b, 200);
    } else if (notif->urgency == NOTIF_WARNING) {
        bg_color = color_rgba(theme->warning.r, theme->warning.g, theme->warning.b, 200);
    } else if (notif->urgency == NOTIF_SUCCESS) {
        bg_color = color_rgba(theme->success.r, theme->success.g, theme->success.b, 200);
    }

    // TODO: Draw rounded rect with shadow
    // color: bg_color
    // corner_radius: theme->corner_radius

    // Render app icon (left side)
    rect_t icon_rect = {
        .x = x + NOTIF_PADDING,
        .y = y + NOTIF_PADDING,
        .width = 32,
        .height = 32
    };
    // TODO: Draw icon texture or placeholder

    // Render app name (small, above summary)
    rect_t app_name_rect = {
        .x = x + NOTIF_PADDING + 40,
        .y = y + NOTIF_PADDING,
        .width = NOTIF_WIDTH - NOTIF_PADDING * 2 - 40,
        .height = 16
    };
    // TODO: Draw text (notif->app_name, theme->text_secondary, size: small)

    // Render summary (title, bold)
    rect_t summary_rect = {
        .x = x + NOTIF_PADDING + 40,
        .y = y + NOTIF_PADDING + 18,
        .width = NOTIF_WIDTH - NOTIF_PADDING * 2 - 40,
        .height = 18
    };
    // TODO: Draw text (notif->summary, theme->text_primary, size: body, weight: bold)

    // Render body (description)
    rect_t body_rect = {
        .x = x + NOTIF_PADDING + 40,
        .y = y + NOTIF_PADDING + 38,
        .width = NOTIF_WIDTH - NOTIF_PADDING * 2 - 40,
        .height = 24
    };
    // TODO: Draw text (notif->body, theme->text_secondary, size: small, wrap)

    // Render action buttons (if any)
    if (notif->action_count > 0) {
        int32_t btn_x = x + NOTIF_PADDING;
        int32_t btn_y = y + NOTIF_HEIGHT - NOTIF_PADDING - 24;
        uint32_t btn_width = (NOTIF_WIDTH - NOTIF_PADDING * (notif->action_count + 1)) / notif->action_count;

        for (uint32_t i = 0; i < notif->action_count; i++) {
            rect_t btn_rect = {
                .x = btn_x,
                .y = btn_y,
                .width = btn_width,
                .height = 24
            };
            // TODO: Draw button with notif->actions[i].label
            btn_x += (int32_t)btn_width + NOTIF_PADDING;
            (void)btn_rect;
        }
    }

    (void)theme;
    (void)bg_rect;
    (void)bg_color;
    (void)icon_rect;
    (void)app_name_rect;
    (void)summary_rect;
    (void)body_rect;
}

// ============================================================================
// NOTIFICATION CENTER API
// ============================================================================

notification_center_t *notification_center_create(desktop_shell_t *shell) {
    if (!shell) return NULL;

    printf("[Notifications] Creating notification center\n");

    notification_center_t *center = calloc(1, sizeof(notification_center_t));
    if (!center) {
        fprintf(stderr, "[Notifications] ERROR: Failed to allocate notification center\n");
        return NULL;
    }

    center->theme = &shell->theme;
    center->count = 0;
    center->do_not_disturb = false;
    center->visible = false;

    // TODO: Create notification center panel window
    // center->window = window_create_panel();

    printf("[Notifications] Notification center created\n");
    return center;
}

void notification_center_destroy(notification_center_t *center) {
    if (!center) return;

    printf("[Notifications] Destroying notification center\n");

    // Destroy all notifications
    for (uint32_t i = 0; i < center->count; i++) {
        if (center->queue[i]) {
            notification_destroy(center->queue[i]);
        }
    }

    // TODO: Destroy window
    // if (center->window) window_destroy(center->window);

    free(center);
}

uint32_t notification_send(notification_center_t *center, const char *app_name,
                          const char *summary, const char *body,
                          notif_urgency_t urgency) {
    if (!center || !app_name || !summary) return 0;

    if (center->do_not_disturb && urgency != NOTIF_ERROR) {
        // Silently drop non-error notifications in DND mode
        return 0;
    }

    if (center->count >= 100) {
        // Remove oldest notification to make space
        notification_destroy(center->queue[0]);
        for (uint32_t i = 0; i < center->count - 1; i++) {
            center->queue[i] = center->queue[i + 1];
        }
        center->count--;
    }

    notification_t *notif = notification_create(app_name, summary, body, urgency);
    if (!notif) {
        fprintf(stderr, "[Notifications] ERROR: Failed to create notification\n");
        return 0;
    }

    center->queue[center->count++] = notif;

    printf("[Notifications] New notification: %s - %s\n", app_name, summary);

    // TODO: Show notification toast
    // TODO: Play notification sound (if enabled)

    return notif->id;
}

void notification_dismiss(notification_center_t *center, uint32_t id) {
    if (!center) return;

    for (uint32_t i = 0; i < center->count; i++) {
        if (center->queue[i]->id == id) {
            notification_destroy(center->queue[i]);

            // Shift remaining notifications
            for (uint32_t j = i; j < center->count - 1; j++) {
                center->queue[j] = center->queue[j + 1];
            }

            center->count--;
            printf("[Notifications] Dismissed notification #%u\n", id);
            return;
        }
    }
}

void notification_center_render(notification_center_t *center) {
    if (!center || !center->visible || !center->theme) return;

    // Render notification center panel (right side of screen)
    rect_t panel_rect = {
        .x = (int32_t)center->window->bounds.width - NOTIF_WIDTH - 16,
        .y = 48,  // Below panel
        .width = NOTIF_WIDTH,
        .height = center->window->bounds.height - 64
    };

    // TODO: Draw semi-transparent background
    // color: center->theme->bg_primary (with alpha)

    // Render recent notifications (newest first)
    int32_t y = panel_rect.y + NOTIF_PADDING;
    uint32_t displayed = 0;

    for (int32_t i = (int32_t)center->count - 1; i >= 0 && displayed < NOTIF_MAX_DISPLAY * 3; i--) {
        notification_t *notif = center->queue[i];
        if (!notif->dismissed) {
            notification_render(notif, panel_rect.x, y, center->theme);
            y += NOTIF_HEIGHT + NOTIF_SPACING;
            displayed++;
        }
    }

    (void)panel_rect;
}
