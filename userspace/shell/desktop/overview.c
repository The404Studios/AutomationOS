/**
 * Overview (Mission Control / Activities) Implementation
 *
 * Full-screen overlay showing search bar, app grid, window thumbnails,
 * and workspace switcher.
 */

#include "desktop_shell.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define SEARCH_BAR_HEIGHT 64
#define SEARCH_BAR_WIDTH 600
#define APP_ICON_SIZE 96
#define APP_GRID_COLS 6
#define APP_GRID_SPACING 32
#define WINDOW_THUMBNAIL_WIDTH 300
#define WINDOW_THUMBNAIL_HEIGHT 200
#define WORKSPACE_HEIGHT 60

// ============================================================================
// SEARCH BAR
// ============================================================================

static search_bar_t *search_bar_create(void) {
    search_bar_t *search = calloc(1, sizeof(search_bar_t));
    if (!search) return NULL;

    search->query[0] = '\0';
    search->result_count = 0;

    return search;
}

static void search_bar_destroy(search_bar_t *search) {
    if (search) free(search);
}

static void search_bar_update(search_bar_t *search, const char *query) {
    if (!search || !query) return;

    strncpy(search->query, query, sizeof(search->query) - 1);

    // TODO: Perform actual search
    // For now, just clear results
    search->result_count = 0;

    // Simple fuzzy matching
    char query_lower[256];
    for (size_t i = 0; i < strlen(query) && i < sizeof(query_lower) - 1; i++) {
        query_lower[i] = (char)tolower((unsigned char)query[i]);
    }
    query_lower[strlen(query)] = '\0';

    // TODO: Search apps, files, settings, etc.
    // Add results to search->results[]
}

static void search_bar_render(search_bar_t *search, int32_t x, int32_t y, theme_t *theme) {
    if (!search) return;

    // Render search bar background
    rect_t bg_rect = {
        .x = x,
        .y = y,
        .width = SEARCH_BAR_WIDTH,
        .height = SEARCH_BAR_HEIGHT
    };

    // TODO: Draw rounded rect with blur
    // color: theme->bg_secondary (semi-transparent)
    // corner_radius: theme->corner_radius * 2

    // Render search icon (left side)
    rect_t icon_rect = {
        .x = x + 16,
        .y = y + (SEARCH_BAR_HEIGHT - 24) / 2,
        .width = 24,
        .height = 24
    };
    // TODO: Draw search magnifying glass icon

    // Render search text
    rect_t text_rect = {
        .x = x + 48,
        .y = y + (SEARCH_BAR_HEIGHT - 24) / 2,
        .width = SEARCH_BAR_WIDTH - 64,
        .height = 24
    };
    // TODO: Draw text (query or placeholder "Search...")

    // Render results dropdown (if any)
    if (search->result_count > 0) {
        rect_t results_rect = {
            .x = x,
            .y = y + SEARCH_BAR_HEIGHT + 8,
            .width = SEARCH_BAR_WIDTH,
            .height = search->result_count * 48
        };
        // TODO: Draw results list
    }

    (void)theme;
    (void)bg_rect;
    (void)icon_rect;
    (void)text_rect;
}

// ============================================================================
// APP GRID
// ============================================================================

static void overview_load_apps(overview_t *overview) {
    if (!overview) return;

    // TODO: Load installed apps from filesystem
    // For now, add some hardcoded apps

    const char *apps[][2] = {
        {"Files", "com.automationos.files"},
        {"Browser", "com.automationos.browser"},
        {"Terminal", "com.automationos.terminal"},
        {"Settings", "com.automationos.settings"},
        {"Calculator", "com.automationos.calculator"},
        {"Text Editor", "com.automationos.editor"},
        {"Image Viewer", "com.automationos.images"},
        {"Music", "com.automationos.music"},
        {"Videos", "com.automationos.videos"},
        {"Mail", "com.automationos.mail"},
    };

    overview->app_count = sizeof(apps) / sizeof(apps[0]);
    for (uint32_t i = 0; i < overview->app_count; i++) {
        strncpy(overview->apps[i].name, apps[i][0], sizeof(overview->apps[i].name) - 1);
        strncpy(overview->apps[i].app_id, apps[i][1], sizeof(overview->apps[i].app_id) - 1);
        // TODO: Load app icon
        // overview->apps[i].icon = texture_load_app_icon(apps[i][1]);
    }
}

static void overview_update_app_grid_layout(overview_t *overview, int32_t start_x, int32_t start_y) {
    if (!overview) return;

    uint32_t col = 0;
    uint32_t row = 0;

    for (uint32_t i = 0; i < overview->app_count; i++) {
        overview->apps[i].bounds.x = start_x + (int32_t)(col * (APP_ICON_SIZE + APP_GRID_SPACING));
        overview->apps[i].bounds.y = start_y + (int32_t)(row * (APP_ICON_SIZE + APP_GRID_SPACING));
        overview->apps[i].bounds.width = APP_ICON_SIZE;
        overview->apps[i].bounds.height = APP_ICON_SIZE + 24;  // + label height

        col++;
        if (col >= APP_GRID_COLS) {
            col = 0;
            row++;
        }
    }
}

static void overview_render_app_grid(overview_t *overview, theme_t *theme) {
    if (!overview) return;

    for (uint32_t i = 0; i < overview->app_count; i++) {
        rect_t icon_rect = {
            .x = overview->apps[i].bounds.x + (int32_t)(APP_ICON_SIZE - 64) / 2,
            .y = overview->apps[i].bounds.y,
            .width = 64,
            .height = 64
        };

        // TODO: Draw app icon
        // texture_blit(overview->apps[i].icon, &icon_rect);

        // Draw app name
        rect_t label_rect = {
            .x = overview->apps[i].bounds.x,
            .y = overview->apps[i].bounds.y + 72,
            .width = APP_ICON_SIZE,
            .height = 24
        };
        // TODO: Draw centered text
        // text: overview->apps[i].name
        // color: theme->text_primary

        (void)icon_rect;
        (void)label_rect;
    }

    (void)theme;
}

// ============================================================================
// WINDOW THUMBNAILS
// ============================================================================

static void overview_render_window_thumbnails(overview_t *overview, theme_t *theme) {
    if (!overview || overview->window_count == 0) return;

    // Render window thumbnails in a grid
    int32_t start_x = 100;
    int32_t start_y = 200;
    uint32_t col = 0;

    for (uint32_t i = 0; i < overview->window_count; i++) {
        window_t *win = overview->windows[i];
        if (!win || !win->visible) continue;

        rect_t thumbnail_rect = {
            .x = start_x + (int32_t)(col * (WINDOW_THUMBNAIL_WIDTH + 32)),
            .y = start_y,
            .width = WINDOW_THUMBNAIL_WIDTH,
            .height = WINDOW_THUMBNAIL_HEIGHT
        };

        // TODO: Draw window thumbnail (scaled down version of window surface)
        // TODO: Draw window title below thumbnail

        col++;
        if (col >= 4) {
            col = 0;
            start_y += WINDOW_THUMBNAIL_HEIGHT + 64;
        }

        (void)thumbnail_rect;
    }

    (void)theme;
}

// ============================================================================
// WORKSPACE SWITCHER
// ============================================================================

static void overview_render_workspaces(overview_t *overview, theme_t *theme) {
    if (!overview) return;

    int32_t workspace_width = 200;
    int32_t workspace_height = WORKSPACE_HEIGHT;
    int32_t spacing = 16;

    int32_t total_width = (int32_t)(overview->workspace_count * (workspace_width + spacing) - spacing);
    int32_t start_x = ((int32_t)overview->window->bounds.width - total_width) / 2;
    int32_t y = (int32_t)overview->window->bounds.height - 100;

    for (uint32_t i = 0; i < overview->workspace_count; i++) {
        rect_t ws_rect = {
            .x = start_x + (int32_t)(i * (workspace_width + spacing)),
            .y = y,
            .width = (uint32_t)workspace_width,
            .height = (uint32_t)workspace_height
        };

        // Highlight current workspace
        color_t bg_color = (i == overview->current_workspace) ?
                          theme->primary : theme->bg_secondary;

        // TODO: Draw rounded rect
        // color: bg_color
        // corner_radius: theme->corner_radius

        // Draw workspace number
        // TODO: Draw centered text
        // text: sprintf("Workspace %u", i + 1)
        // color: theme->text_primary

        (void)ws_rect;
        (void)bg_color;
    }

    (void)theme;
}

// ============================================================================
// OVERVIEW API
// ============================================================================

overview_t *overview_create(desktop_shell_t *shell) {
    if (!shell) return NULL;

    printf("[Overview] Creating overview\n");

    overview_t *overview = calloc(1, sizeof(overview_t));
    if (!overview) {
        fprintf(stderr, "[Overview] ERROR: Failed to allocate overview\n");
        return NULL;
    }

    overview->theme = &shell->theme;
    overview->active = false;
    overview->workspace_count = shell->workspace_count;
    overview->current_workspace = shell->current_workspace;
    overview->window_count = 0;
    overview->windows = NULL;

    // Create search bar
    overview->search = search_bar_create();
    if (!overview->search) {
        fprintf(stderr, "[Overview] ERROR: Failed to create search bar\n");
        overview_destroy(overview);
        return NULL;
    }

    // Load apps
    overview_load_apps(overview);

    // Update app grid layout
    int32_t grid_start_x = ((int32_t)shell->screen_width - (int32_t)(APP_GRID_COLS * (APP_ICON_SIZE + APP_GRID_SPACING))) / 2;
    int32_t grid_start_y = 150;
    overview_update_app_grid_layout(overview, grid_start_x, grid_start_y);

    // TODO: Create fullscreen overlay window
    // overview->window = window_create_overlay();

    printf("[Overview] Overview created with %u apps\n", overview->app_count);
    return overview;
}

void overview_destroy(overview_t *overview) {
    if (!overview) return;

    printf("[Overview] Destroying overview\n");

    if (overview->search) search_bar_destroy(overview->search);

    // TODO: Free app icons
    for (uint32_t i = 0; i < overview->app_count; i++) {
        // if (overview->apps[i].icon) texture_destroy(overview->apps[i].icon);
    }

    // TODO: Destroy overview window
    // if (overview->window) window_destroy(overview->window);

    free(overview);
}

void overview_open(overview_t *overview) {
    if (!overview || overview->active) return;

    printf("[Overview] Opening overview\n");
    overview->active = true;

    // TODO: Animate windows zooming out
    // TODO: Show overview window
}

void overview_close(overview_t *overview) {
    if (!overview || !overview->active) return;

    printf("[Overview] Closing overview\n");
    overview->active = false;

    // TODO: Animate windows zooming back
    // TODO: Hide overview window
}

void overview_search(overview_t *overview, const char *query) {
    if (!overview || !query) return;

    search_bar_update(overview->search, query);
}

void overview_render(overview_t *overview) {
    if (!overview || !overview->active || !overview->theme) return;

    // Render semi-transparent overlay background
    // TODO: Fill screen with black (alpha: 128)

    // Render search bar (centered at top)
    int32_t search_x = ((int32_t)overview->window->bounds.width - SEARCH_BAR_WIDTH) / 2;
    int32_t search_y = 32;
    search_bar_render(overview->search, search_x, search_y, overview->theme);

    // Render app grid
    overview_render_app_grid(overview, overview->theme);

    // Render window thumbnails
    overview_render_window_thumbnails(overview, overview->theme);

    // Render workspace switcher (at bottom)
    overview_render_workspaces(overview, overview->theme);
}
