/**
 * Desktop (Workspace) Implementation
 *
 * Desktop wallpaper, icons, and context menu.
 */

#include "desktop_shell.h"
#include "../../lib/image/image.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ICON_SIZE 64
#define ICON_LABEL_HEIGHT 20
#define ICON_GRID_SIZE 96
#define ICON_PADDING 16

// ============================================================================
// DESKTOP ICON HELPERS
// ============================================================================

static const char *get_icon_name_for_type(icon_type_t type) {
    switch (type) {
        case ICON_FILE: return "document";
        case ICON_FOLDER: return "folder";
        case ICON_APP: return "terminal";
        case ICON_TRASH: return "folder";  // TODO: Create trash icon
        case ICON_VOLUME: return "folder";  // TODO: Create volume icon
        default: return "document";
    }
}

static desktop_icon_t *desktop_icon_create(const char *name, const char *path, icon_type_t type) {
    desktop_icon_t *icon = calloc(1, sizeof(desktop_icon_t));
    if (!icon) return NULL;

    strncpy(icon->name, name, sizeof(icon->name) - 1);
    strncpy(icon->path, path, sizeof(icon->path) - 1);
    icon->type = type;
    icon->selected = false;

    // Load icon texture based on type
    const char *icon_name = get_icon_name_for_type(type);
    icon_set_t *icon_set = icon_load_set(icon_name);
    if (icon_set) {
        image_t *img = icon_get_best_size(icon_set, ICON_SIZE);
        if (img) {
            // TODO: Convert image_t to texture_t when texture system is available
            // For now, store the image pointer (will need proper type when integrated)
            (void)img;
        }
        icon_free_set(icon_set);
    }

    return icon;
}

static void desktop_icon_destroy(desktop_icon_t *icon) {
    if (!icon) return;

    // TODO: Free texture
    // if (icon->icon) texture_destroy(icon->icon);

    free(icon);
}

static void desktop_icon_render(desktop_icon_t *icon, theme_t *theme) {
    if (!icon) return;

    // Render selection highlight (if selected)
    if (icon->selected) {
        rect_t highlight_rect = {
            .x = icon->bounds.x - 4,
            .y = icon->bounds.y - 4,
            .width = icon->bounds.width + 8,
            .height = icon->bounds.height + 8
        };
        // TODO: Draw rounded rect with theme->primary color (semi-transparent)
        (void)highlight_rect;
    }

    // Render icon shadow
    // TODO: Draw shadow effect

    // Render icon texture
    rect_t icon_rect = {
        .x = icon->bounds.x + (int32_t)(icon->bounds.width - ICON_SIZE) / 2,
        .y = icon->bounds.y,
        .width = ICON_SIZE,
        .height = ICON_SIZE
    };
    // TODO: Blit icon texture at icon_rect

    // Render icon label (with shadow for visibility on any wallpaper)
    rect_t label_rect = {
        .x = icon->bounds.x,
        .y = icon->bounds.y + (int32_t)ICON_SIZE + 4,
        .width = icon->bounds.width,
        .height = ICON_LABEL_HEIGHT
    };

    // TODO: Draw text with shadow
    // text: icon->name
    // color: white (or theme->text_primary)
    // shadow: black semi-transparent
    // alignment: center

    (void)theme;
    (void)icon_rect;
    (void)label_rect;
}

// ============================================================================
// DESKTOP LAYOUT
// ============================================================================

static void desktop_update_icon_layout(desktop_t *desktop) {
    if (!desktop || desktop->icon_count == 0) return;

    // Arrange icons in a grid from top-left, skipping panel area
    uint32_t grid_size = desktop->grid_size;
    int32_t x = ICON_PADDING;
    int32_t y = 32 + ICON_PADDING;  // Skip panel height

    uint32_t col = 0;
    uint32_t max_cols = (desktop->window->bounds.width - 2 * ICON_PADDING) / grid_size;

    for (uint32_t i = 0; i < desktop->icon_count; i++) {
        desktop->icons[i]->bounds.x = x + (int32_t)(col * grid_size);
        desktop->icons[i]->bounds.y = y;
        desktop->icons[i]->bounds.width = grid_size;
        desktop->icons[i]->bounds.height = ICON_SIZE + ICON_LABEL_HEIGHT;

        col++;
        if (col >= max_cols) {
            col = 0;
            y += grid_size;
        }
    }
}

// ============================================================================
// DESKTOP API
// ============================================================================

desktop_t *desktop_create(desktop_shell_t *shell) {
    if (!shell) return NULL;

    printf("[Desktop] Creating desktop\n");

    desktop_t *desktop = calloc(1, sizeof(desktop_t));
    if (!desktop) {
        fprintf(stderr, "[Desktop] ERROR: Failed to allocate desktop\n");
        return NULL;
    }

    desktop->theme = &shell->theme;
    desktop->show_icons = true;
    desktop->grid_size = ICON_GRID_SIZE;
    desktop->bg_color = color_hex(0x1E90FF);  // Default: Dodger Blue
    desktop->icon_count = 0;

    // TODO: Create fullscreen desktop window
    // desktop->window = window_create_fullscreen();

    // Add default desktop icons
    desktop_add_icon(desktop, "Home", "/home/user", ICON_FOLDER);
    desktop_add_icon(desktop, "Documents", "/home/user/Documents", ICON_FOLDER);
    desktop_add_icon(desktop, "Downloads", "/home/user/Downloads", ICON_FOLDER);
    desktop_add_icon(desktop, "Trash", "/home/user/.trash", ICON_TRASH);

    // Update layout
    desktop_update_icon_layout(desktop);

    printf("[Desktop] Desktop created with %u icons\n", desktop->icon_count);
    return desktop;
}

void desktop_destroy(desktop_t *desktop) {
    if (!desktop) return;

    printf("[Desktop] Destroying desktop\n");

    // Destroy all desktop icons
    for (uint32_t i = 0; i < desktop->icon_count; i++) {
        if (desktop->icons[i]) {
            desktop_icon_destroy(desktop->icons[i]);
        }
    }

    // TODO: Destroy wallpaper texture
    // if (desktop->wallpaper) texture_destroy(desktop->wallpaper);

    // TODO: Destroy desktop window
    // if (desktop->window) window_destroy(desktop->window);

    free(desktop);
}

void desktop_set_wallpaper(desktop_t *desktop, const char *path) {
    if (!desktop || !path) return;

    printf("[Desktop] Setting wallpaper: %s\n", path);

    // Load wallpaper image
    image_t *img = image_load(path, IMAGE_LOAD_RGBA);
    if (!img) {
        fprintf(stderr, "[Desktop] ERROR: Failed to load wallpaper: %s\n", image_get_error());
        return;
    }

    printf("[Desktop] Loaded wallpaper: %ux%u (%u channels)\n",
           img->width, img->height, img->channels);

    // TODO: Convert image_t to texture_t when texture system is available
    // For now, free the image (will integrate properly later)
    image_free(img);

    // TODO: Store texture in desktop->wallpaper
}

void desktop_add_icon(desktop_t *desktop, const char *name, const char *path, icon_type_t type) {
    if (!desktop || !name || !path) return;

    if (desktop->icon_count >= 128) {
        fprintf(stderr, "[Desktop] ERROR: Desktop is full (max 128 icons)\n");
        return;
    }

    desktop_icon_t *icon = desktop_icon_create(name, path, type);
    if (!icon) {
        fprintf(stderr, "[Desktop] ERROR: Failed to create desktop icon for %s\n", name);
        return;
    }

    desktop->icons[desktop->icon_count++] = icon;
    desktop_update_icon_layout(desktop);

    printf("[Desktop] Added icon %s (%s)\n", name, path);
}

void desktop_render(desktop_t *desktop) {
    if (!desktop || !desktop->theme) return;

    // Render wallpaper or solid color
    if (desktop->wallpaper) {
        // TODO: Blit wallpaper texture (scaled to fit screen)
        // texture_blit_scaled(desktop->wallpaper, &screen_rect);
    } else {
        // TODO: Fill screen with solid color
        // fill_rect(&screen_rect, desktop->bg_color);
    }

    // Render desktop icons (if enabled)
    if (desktop->show_icons) {
        for (uint32_t i = 0; i < desktop->icon_count; i++) {
            desktop_icon_render(desktop->icons[i], desktop->theme);
        }
    }
}
