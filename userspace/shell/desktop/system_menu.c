/**
 * System Menu Implementation
 *
 * Power menu with settings, logout, restart, shutdown options.
 */

#include "desktop_shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MENU_WIDTH 220
#define MENU_ITEM_HEIGHT 40
#define MENU_PADDING 8

// ============================================================================
// MENU CALLBACKS
// ============================================================================

static void menu_settings_callback(void *user_data) {
    printf("[SystemMenu] Opening settings\n");
    // TODO: Launch settings app
    (void)user_data;
}

static void menu_about_callback(void *user_data) {
    printf("[SystemMenu] Opening about dialog\n");
    // TODO: Show about dialog
    (void)user_data;
}

static void menu_lock_callback(void *user_data) {
    printf("[SystemMenu] Locking screen\n");
    // TODO: Lock screen
    (void)user_data;
}

static void menu_logout_callback(void *user_data) {
    printf("[SystemMenu] Logging out\n");
    // TODO: Confirm and logout
    (void)user_data;
}

static void menu_sleep_callback(void *user_data) {
    printf("[SystemMenu] Entering sleep mode\n");
    // TODO: Sleep system
    (void)user_data;
}

static void menu_restart_callback(void *user_data) {
    printf("[SystemMenu] Restarting system\n");
    // TODO: Confirm and restart
    (void)user_data;
}

static void menu_shutdown_callback(void *user_data) {
    printf("[SystemMenu] Shutting down\n");
    desktop_shell_t *shell = (desktop_shell_t *)user_data;
    // TODO: Confirm shutdown
    if (shell) {
        desktop_shell_quit(shell);
    }
}

// ============================================================================
// MENU ITEM RENDERING
// ============================================================================

static void menu_item_render(menu_item_t *item, int32_t x, int32_t y, bool hovered, theme_t *theme) {
    if (!item) return;

    rect_t bg_rect = {
        .x = x,
        .y = y,
        .width = MENU_WIDTH - MENU_PADDING * 2,
        .height = MENU_ITEM_HEIGHT
    };

    // Highlight if hovered
    if (hovered) {
        // TODO: Draw rounded rect with theme->primary (semi-transparent)
    }

    // Render label
    rect_t label_rect = {
        .x = x + MENU_PADDING,
        .y = y + (MENU_ITEM_HEIGHT - 20) / 2,
        .width = MENU_WIDTH - MENU_PADDING * 4,
        .height = 20
    };
    // TODO: Draw text (item->label, theme->text_primary)

    // Render shortcut (right side)
    if (item->shortcut[0] != '\0') {
        rect_t shortcut_rect = {
            .x = x + MENU_WIDTH - MENU_PADDING * 2 - 80,
            .y = y + (MENU_ITEM_HEIGHT - 20) / 2,
            .width = 80,
            .height = 20
        };
        // TODO: Draw text (item->shortcut, theme->text_secondary, align: right)
        (void)shortcut_rect;
    }

    // Render separator (if needed)
    if (item->separator_after) {
        rect_t sep_rect = {
            .x = x + MENU_PADDING,
            .y = y + MENU_ITEM_HEIGHT - 1,
            .width = MENU_WIDTH - MENU_PADDING * 2,
            .height = 1
        };
        // TODO: Draw horizontal line (theme->separator)
        (void)sep_rect;
    }

    (void)theme;
    (void)bg_rect;
    (void)label_rect;
}

// ============================================================================
// SYSTEM MENU API
// ============================================================================

system_menu_t *system_menu_create(desktop_shell_t *shell) {
    if (!shell) return NULL;

    printf("[SystemMenu] Creating system menu\n");

    system_menu_t *menu = calloc(1, sizeof(system_menu_t));
    if (!menu) {
        fprintf(stderr, "[SystemMenu] ERROR: Failed to allocate system menu\n");
        return NULL;
    }

    menu->theme = &shell->theme;
    menu->visible = false;
    menu->hover_index = -1;
    menu->count = 0;

    // Add menu items
    menu_item_t items[] = {
        {.label = "Settings...", .shortcut = "", .callback = menu_settings_callback, .user_data = shell, .separator_after = false},
        {.label = "About This PC", .shortcut = "", .callback = menu_about_callback, .user_data = shell, .separator_after = true},
        {.label = "Lock Screen", .shortcut = "Ctrl+Alt+L", .callback = menu_lock_callback, .user_data = shell, .separator_after = false},
        {.label = "Log Out", .shortcut = "Ctrl+Alt+Q", .callback = menu_logout_callback, .user_data = shell, .separator_after = true},
        {.label = "Sleep", .shortcut = "", .callback = menu_sleep_callback, .user_data = shell, .separator_after = false},
        {.label = "Restart...", .shortcut = "", .callback = menu_restart_callback, .user_data = shell, .separator_after = false},
        {.label = "Shut Down...", .shortcut = "", .callback = menu_shutdown_callback, .user_data = shell, .separator_after = false},
    };

    menu->count = sizeof(items) / sizeof(menu_item_t);
    for (uint32_t i = 0; i < menu->count; i++) {
        menu->items[i] = items[i];
    }

    // TODO: Create menu window
    // menu->window = window_create_popup();

    printf("[SystemMenu] System menu created with %u items\n", menu->count);
    return menu;
}

void system_menu_destroy(system_menu_t *menu) {
    if (!menu) return;

    printf("[SystemMenu] Destroying system menu\n");

    // TODO: Destroy window
    // if (menu->window) window_destroy(menu->window);

    free(menu);
}

void system_menu_open(system_menu_t *menu, int32_t x, int32_t y) {
    if (!menu) return;

    menu->visible = true;

    // Position menu at (x, y)
    // TODO: Adjust position if menu would go off-screen
    menu->window->bounds.x = x;
    menu->window->bounds.y = y;
    menu->window->bounds.width = MENU_WIDTH;
    menu->window->bounds.height = menu->count * MENU_ITEM_HEIGHT + MENU_PADDING * 2;

    printf("[SystemMenu] System menu opened at (%d, %d)\n", x, y);
}

void system_menu_close(system_menu_t *menu) {
    if (!menu) return;

    menu->visible = false;
    menu->hover_index = -1;

    printf("[SystemMenu] System menu closed\n");
}

void system_menu_render(system_menu_t *menu) {
    if (!menu || !menu->visible || !menu->theme) return;

    rect_t menu_rect = menu->window->bounds;

    // TODO: Draw semi-transparent background with shadow
    // color: menu->theme->bg_primary
    // corner_radius: menu->theme->corner_radius
    // shadow: menu->theme->shadow_opacity

    // Render menu items
    int32_t y = menu_rect.y + MENU_PADDING;
    for (uint32_t i = 0; i < menu->count; i++) {
        bool hovered = ((int32_t)i == menu->hover_index);
        menu_item_render(&menu->items[i], menu_rect.x + MENU_PADDING, y, hovered, menu->theme);
        y += MENU_ITEM_HEIGHT;
    }

    (void)menu_rect;
}
